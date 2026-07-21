# zbus from first principles

What an in-process message bus is, why a firmware app wants one, and how Zephyr's zbus
expresses it. Written to be portable beyond this repo — the concepts apply to any
event-driven embedded system. For how *this* project wires it up, see
[`firmware-mqtt-walkthrough.md`](../docs/firmware-mqtt-walkthrough.md) §11 and the header
comment in `firmware/src/app_channels.h`.

The single most important thing up front: **zbus has nothing to do with the network.**
It sits next to "MQTT" in this project's concept list, and the vocabulary is identical —
publish, subscribe, channels, observers — but it never touches a wire. It is threads
inside one MCU talking to each other. Getting that straight early saves a lot of
confusion.

---

## 1. The problem it solves

Start from the code this project had *before* zbus. One loop did everything:

```c
while (connected) {
    timeout = MIN(time_until_next_sample, time_until_keepalive);
    if (poll(socket, timeout) > 0) { handle_incoming(); }
    mqtt_live();
    if (time_to_sample()) { read_sensor(); publish(); }
}
```

That works, and for a long time it is the right amount of structure. But look at what is
entangled:

- **The sensor cannot sample unless the network loop is running.** During a reconnect
  backoff — up to 30 s here — nothing is read at all. The sensor's cadence is hostage to
  the broker's availability.
- **A slow sensor read delays the keepalive.** The SCD-40 is an I²C device; a fetch takes
  milliseconds, but a device that stretches the clock or a bus that needs a retry could
  eat into the deadline that keeps the MQTT session alive.
- **Every new deadline makes the timeout expression harder.** Two deadlines already
  required `MIN(MAX(until_sample, 0), keepalive)`, where the `MAX` guards against a
  negative value that `poll()` would read as *block forever*. Add a third concern and the
  edge cases multiply.
- **Adding a second consumer means editing the producer.** Want to also log readings to
  flash, or drive a display? Both go inside the same `if (time_to_sample())` block.

Each of those is a symptom of one cause: **acquisition and transport are the same code**,
so they share a clock, a thread, and a failure mode.

## 2. The obvious fix, and why it is not enough

Split them into two threads. Now the sensor samples on its own clock. But threads that
never talk are useless, so how does a reading get from one to the other?

The naive answer is a shared global plus a mutex:

```c
struct reading latest;      /* guarded by latest_mutex */
```

This works and is genuinely fine for small systems. What it does not give you:

| Missing | Why it matters |
|---|---|
| **Notification** | The reader must poll the variable, or you bolt a semaphore alongside it and keep the two in sync by hand |
| **A second consumer** | Every new reader needs its own signalling, added to the writer |
| **Validation** | Nothing stops a caller storing nonsense; the rules end up duplicated at each writer |
| **Discoverability** | The coupling is invisible — nothing declares that these two threads share anything |

A message bus is that pattern, generalised and named: **shared storage + notification +
a registry of who cares**, declared in one place.

## 3. zbus vocabulary

Three nouns.

**Channel** — a named piece of storage holding exactly one message of a fixed type, plus
a lock and a list of observers. Defined statically:

```c
ZBUS_CHAN_DEFINE(chan_telemetry, struct sensor_reading,
                 NULL,                              /* validator  */
                 NULL,                              /* user data  */
                 ZBUS_OBSERVERS(telemetry_listener),
                 ZBUS_MSG_INIT(0));
```

Note **exactly one message**. A channel is not a queue. Publishing overwrites. This is the
single most important property to internalise, and §5 is about its consequences.

**Publishing** — `zbus_chan_pub(&chan, &msg, timeout)` takes the channel lock, copies the
message in, runs every observer, and releases. It is a *copy*, so the publisher's local
variable can go out of scope immediately.

**Observer** — something registered to be told when a channel changes. zbus offers three
kinds, and choosing between them is the real design decision:

| Kind | How it is notified | Gets the message? |
|---|---|---|
| **Listener** | Callback, run synchronously inside `zbus_chan_pub()` | No — reads the channel itself |
| **Subscriber** | A `k_msgq` receives a *channel pointer* | No — reads the channel itself |
| **Message subscriber** | A queue receives a *copy of the message* | Yes, its own private copy |

## 4. Choosing an observer type

This is where the interesting thinking lives. The question to ask is: **if two messages
arrive before I handle the first, what should happen?**

**Listeners run in the publisher's context.** The callback executes on whichever thread
called `zbus_chan_pub()`, inside the call, with the channel locked. That has sharp edges:
it must not block, must not do slow work, and must not try to take the same channel's
lock. It is for signalling and trivial bookkeeping, nothing more. In exchange it is
instant and allocates nothing.

**Subscribers and message subscribers run in their own thread**, which waits on a queue.
They can block, take their time, and do real work.

The difference between the two queue-based kinds is what the queue carries. A plain
**subscriber** gets a pointer to the channel and then reads it — so by the time it looks,
the value may have been overwritten twice. It learns *that* something changed, not *what*
it was. A **message subscriber** gets a private copy taken at publish time, so nothing is
ever collapsed.

So the choice maps onto the semantics of the data:

- **State** — "what is the current temperature?" Latest wins. Missing an intermediate
  value costs nothing, because a newer, better one already replaced it. Use a listener or
  a subscriber.
- **Events** — "the user pressed the button", "set the interval to 2000". Each one is a
  distinct fact with no successor. Collapsing two is data loss. Use a message subscriber.

If that distinction feels familiar, it should: it is exactly the argument behind MQTT QoS
levels. Telemetry is state, so this project ships it at QoS 0 and observes it with a
listener. Commands are events, so they go at QoS 1 and are observed with a message
subscriber. **The same reasoning applies inside the chip and across the wire**, which is
the most transferable idea in this guide.

## 5. Latest-wins is a feature, not a limitation

Because a channel holds one message, a slow reader loses data. That sounds like a defect
until you ask what the alternative does.

Suppose telemetry were queued instead. A node disconnected for a minute at a 5 s cadence
accumulates twelve readings. On reconnect it publishes all twelve — and eleven of them
describe air that no longer exists. The host's "live" display shows a minute-old value
and catches up in a burst. Worse, the queue has to be sized for the longest outage you
are willing to survive, and overflowing it is a hard failure rather than a graceful one.

Overwriting gives you the newest reading, bounded memory, and no failure mode at all. The
data loss is real, but it is the *right* data to lose.

What you owe the consumer is **visibility into the loss**. A sequence number in the
message does it: the reader sees `56` then `69` and knows twelve are gone. Silent loss is
a bug; accounted loss is a design.

## 6. Validators

A channel can carry a predicate, run before a message is stored:

```c
bool sensor_cmd_valid(const void *msg, size_t msg_size)
{
    const struct sensor_cmd *cmd = msg;
    return cmd->interval_ms >= MIN_MS && cmd->interval_ms <= MAX_MS;
}
```

If it returns false, `zbus_chan_pub()` returns `-ENOMSG` and **nothing is stored and no
observer runs**. That last part is what makes it useful rather than decorative: a rejected
publish is atomic. The caller knows with certainty that the system did not change.

The design value is about *where the rule lives*. Without a validator, every publisher
range-checks the interval, and those copies drift apart the moment there are two of them —
one from MQTT, one from a shell command, one from a config file. With a validator, the
module that owns the data owns the rule, and publishers just report what the bus told
them.

## 7. Waiting on a bus and something else at the same time

A consumer thread that only watches the bus is easy: block in `zbus_sub_wait_msg()`.

The hard case is a thread that must watch the bus **and** something unrelated — a socket,
a UART, a timer. This project has exactly that: the MQTT thread must wake for broker
traffic *and* for fresh readings. There is no combined primitive.

Three ways out, in increasing order of quality:

**Poll the bus on a short timeout.** Block on the socket for, say, 100 ms; on timeout,
check the bus non-blockingly; repeat. Works, costs a wakeup ten times a second to usually
find nothing, and adds up to 100 ms of latency. Acceptable, unambitious.

**Do the work in the callback.** Let the listener publish the MQTT message directly. This
is wrong for a subtle reason: the callback runs on the *sensor* thread, so now two threads
call into an `mqtt_client` that is not thread-safe. You would need a mutex around the
whole MQTT client, and the sensor thread would block on network I/O.

**Signal a file descriptor.** `poll()` understands descriptors, so give it one: the
listener writes to an **eventfd**, which is a counter with a descriptor attached. Add it
to the poll set next to the socket and the thread blocks on both with one call, waking
only when something genuinely happened.

```c
/* in the listener, on the producer's thread */
zvfs_eventfd_write(evt_fd, 1);

/* in the consumer's event loop */
fds[0].fd = socket;   fds[1].fd = evt_fd;
zsock_poll(fds, 2, timeout);
```

This is not a zbus idea — it is the classic **self-pipe trick** from Unix network servers,
where a signal handler writes one byte to a pipe so the main `select()` loop can notice it
safely. eventfd is the modern, cheaper form: a 64-bit counter instead of a byte stream.

An eventfd also hands you free instrumentation. Reading it returns the accumulated count
and resets it to zero, so the value *is* "how many times was I signalled since I last
looked". Anything above one means the channel coalesced messages — the reader can report
its own data loss without tracking any extra state.

## 8. What zbus costs

Honest accounting, because "add a bus" is not free:

- **RAM** — each channel holds one message plus a semaphore and bookkeeping; each message
  subscriber needs a queue and a buffer pool.
- **A copy per publish** — messages are copied in, and copied again for each message
  subscriber. Keep them small; put a pointer in the channel if the payload is large (and
  then you own its lifetime again, which is most of what the bus was saving you from).
- **Indirection** — "who handles this?" is answered by a `ZBUS_OBSERVERS()` list rather
  than by a function call you can follow. Enable `CONFIG_ZBUS_CHANNEL_NAME` and
  `CONFIG_ZBUS_OBSERVER_NAME` so at least the logs name things.
- **New failure modes** — publish timeouts, queue-full conditions, and callbacks that
  block the producer.

For two threads and two channels this is arguably more machinery than a mutex and a
semaphore would need. The payoff is at the *next* consumer: adding one is a line in an
observers list, with no edit to the producer at all. Whether that trade is worth it
depends on whether you expect a third participant. In a learning project, building it
once and feeling the shape is the point.

## 9. What this project verified on the bench

2026-07-21, against the real hardware:

- **Decoupling holds.** The sensor thread sampled through a 66 s broker outage and a
  30 s reconnect backoff. Before the split, the same outage would have stopped sampling
  entirely.
- **Latest-wins behaves as designed and is observable.** On reconnect the node logged
  `12 readings coalesced into one publish` and jumped from `seq=56` to `seq=69` —
  12 discarded plus 1 published equals the 13 readings the wall clock predicts.
- **The eventfd kept the loop fully blocking.** No timed wakeups to check the bus, and a
  triggered measurement still appeared immediately, because the descriptor was already
  readable by the time `poll()` was re-entered.
- **A validator rejection is atomic and reportable.** `interval 100` returned `-ENOMSG`
  from `zbus_chan_pub()`, which the MQTT layer mapped straight to
  `ACK_STATUS_INVALID_ARGUMENT` — the sample period was provably unchanged, because the
  message never reached the channel.

## 10. Where to go next

- **A third observer.** Add a listener to `chan_telemetry` that keeps a running min/max,
  and notice that `sensor.cpp` does not change. That is the whole argument for a bus, in
  one commit.
- **`zbus_chan_add_obs()`** — runtime observers, for consumers that come and go.
- **Priority boost** (`CONFIG_ZBUS_PRIORITY_BOOST`) — zbus can temporarily raise a
  publisher to its highest observer's priority to bound inversion. Worth reading about
  before a bus carries anything time-critical.
- **The Zephyr samples** in `samples/subsys/zbus/` — `hello_world`, `msg_subscriber`,
  `priority_boost` and `runtime_obs_registration` each isolate one idea. `benchmark`
  quantifies the copy cost from §8 on your own target.
