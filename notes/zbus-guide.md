# zbus from first principles

What an in-process message bus is, why a firmware app wants one, and how Zephyr's zbus expresses it. Written to be portable beyond this repo — the concepts apply to any event-driven embedded system. For how *this* project wires it up, see [`firmware-mqtt-walkthrough.md`](../docs/firmware-mqtt-walkthrough.md) §11 and the header comments in `shared/app_channels.h` (the two sensor channels) and `gateway/src/relay.h` (the four relay channels).

The single most important thing up front: **zbus has nothing to do with the network.** It sits next to "MQTT" in this project's concept list, and the vocabulary is identical — publish, subscribe, channels, observers — but it never touches a wire. It is threads inside one MCU talking to each other. Getting that straight early saves a lot of confusion.

**The shape of this document:**

- **§1–§2** — the problem a bus solves, and why the obvious fixes fall short.
- **§3–§4** — the vocabulary, and the one design decision that actually matters: which kind of observer.
- **§5–§7** — the three consequences that surprise people: latest-wins, validators, and waiting on a bus and a socket at the same time.
- **§8–§9** — what it costs, and what this project wired up with it.
- **§10** — exercises on the running node.
- **§11–§12** — the whole model in a paragraph, and where to go next.

---

## 1. The problem it solves

Start from the design you reach for first. A node that samples a sensor and publishes the result over the network has one obvious shape — a single loop that does both:

```c
while (connected) {
    timeout = MIN(time_until_next_sample, time_until_keepalive);
    if (poll(socket, timeout) > 0) { handle_incoming(); }
    mqtt_live();
    if (time_to_sample()) { read_sensor(); publish(); }
}
```

One call in there does the waiting, and the whole guide turns on it, so it is worth being precise about. **`poll()` sleeps until one of a set of things is ready to be read, or until a timeout expires** — whichever happens first. You hand it a list and it hands back which entries woke it. The things in that list are **file descriptors**: small integers the kernel uses to name anything you can read from or write to — a socket, a serial port, a file. That is the whole idea, and §7 is about the fact that a zbus channel is *not* one of them.

The loop above therefore blocks efficiently — no busy-waiting, no fixed tick. It works, and for a long time it is the right amount of structure. But look at what is entangled:

- **The sensor cannot sample unless the network loop is running.** During a reconnect backoff — up to 30 s on this bench — nothing is read at all. The sensor's cadence is hostage to the broker's availability.
- **A slow sensor read delays the keepalive.** The SCD-40 is an I²C device; a fetch takes milliseconds, but a device that stretches the clock or a bus that needs a retry could eat into the deadline that keeps the MQTT session alive.
- **Every new deadline makes the timeout expression harder.** Two deadlines already required `MIN(MAX(until_sample, 0), keepalive)`, where the `MAX` guards against a negative value that `poll()` would read as *block forever*. Add a third concern and the edge cases multiply.
- **Adding a second consumer means editing the producer.** Want to also log readings to flash, or drive a display? Both go inside the same `if (time_to_sample())` block.

Each of those is a symptom of one cause: **acquisition and transport are the same code**, so they share a clock, a thread, and a failure mode.

## 2. The obvious fix, and why it is not enough

Split them into two threads. Now the sensor samples on its own clock. But threads that never talk are useless, so how does a reading get from one to the other?

The naive answer is a shared global plus a mutex:

```c
struct reading latest;      /* guarded by latest_mutex */
```

(A **mutex** is a lock exactly one thread can hold at a time. It exists because a struct this size is not written atomically: without one, a reader can catch `latest` half-updated — a new CO₂ value beside a stale timestamp — and nothing in the language warns you.)

This works and is genuinely fine for small systems. What it does not give you:

| Missing | Why it matters |
|---|---|
| **Notification** | The reader must poll the variable, or you bolt a semaphore alongside it and keep the two in sync by hand |
| **A second consumer** | Every new reader needs its own signalling, added to the writer |
| **Validation** | Nothing stops a caller storing nonsense; the rules end up duplicated at each writer |
| **Discoverability** | The coupling is invisible — nothing declares that these two threads share anything |

A message bus is that pattern, generalised and named: **shared storage + notification + a registry of who cares**, declared in one place.

## 3. zbus vocabulary

Three nouns.

**Channel** — a named piece of storage holding exactly one message of a fixed type, plus a lock and a list of observers. Defined statically:

```c
ZBUS_CHAN_DEFINE(chan_telemetry, struct sensor_reading,
                 NULL,                              /* validator  */
                 NULL,                              /* user data  */
                 ZBUS_OBSERVERS(telemetry_listener),
                 ZBUS_MSG_INIT(0));
```

Note **exactly one message**. A channel is not a queue. Publishing overwrites. This is the single most important property to internalise, and §5 is about its consequences.

**Publishing** — `zbus_chan_pub(&chan, &msg, timeout)` takes the channel lock, copies the message in, runs every observer, and releases. It is a *copy*, so the publisher's local variable can go out of scope immediately.

**Observer** — something registered to be told when a channel changes. zbus offers three kinds, and choosing between them is the real design decision:

| Kind | How it is notified | Gets the message? |
|---|---|---|
| **Listener** | Callback, run synchronously inside `zbus_chan_pub()` | No — reads the channel itself |
| **Subscriber** | A `k_msgq` receives a *channel pointer* | No — reads the channel itself |
| **Message subscriber** | A queue receives a *copy of the message* | Yes, its own private copy |

## 4. Choosing an observer type

This is where the interesting thinking lives. The question to ask is: **if two messages arrive before I handle the first, what should happen?**

**Listeners run in the publisher's context.** The callback executes on whichever thread called `zbus_chan_pub()`, inside the call, with the channel locked. That has sharp edges: it must not block, must not do slow work, and must not try to take the same channel's lock. It is for signalling and trivial bookkeeping, nothing more. In exchange it is instant and allocates nothing.

**Subscribers and message subscribers run in their own thread**, which waits on a queue. They can block, take their time, and do real work.

The difference between the two queue-based kinds is what the queue carries. A plain **subscriber** gets a pointer to the channel and then reads it — so by the time it looks, the value may have been overwritten twice. It learns that *something* changed, not *what* it was. A **message subscriber** gets a private copy taken at publish time, so nothing is ever collapsed.

So the choice maps onto the semantics of the data:

- **State** — "what is the current temperature?" Latest wins. Missing an intermediate value costs nothing, because a newer, better one already replaced it. Use a listener or a subscriber.
- **Events** — "the user pressed the button", "set the interval to 2000". Each one is a distinct fact with no successor. Collapsing two is data loss. Use a message subscriber.

If that distinction feels familiar, it should: it is exactly the argument behind MQTT QoS levels. Telemetry is state, so this project ships it at QoS 0 and observes it with a listener. Commands are events, so they go at QoS 1 and are observed with a message subscriber. **The same reasoning applies inside the chip and across the wire**, which is the most transferable idea in this guide.

## 5. Latest-wins is a feature, not a limitation

Because a channel holds one message, a slow reader loses data. That sounds like a defect until you ask what the alternative does.

Suppose telemetry were queued instead. A node disconnected for a minute at a 5 s cadence accumulates twelve readings. On reconnect it publishes all twelve — and eleven of them describe air that no longer exists. The host's "live" display shows a minute-old value and catches up in a burst. Worse, the queue has to be sized for the longest outage you are willing to survive, and overflowing it is a hard failure rather than a graceful one.

Overwriting gives you the newest reading, bounded memory, and no failure mode at all. The data loss is real, but it is the *right* data to lose.

What you owe the consumer is **visibility into the loss**. A sequence number in the message does it: the reader sees `56` then `69` and knows twelve are gone. Silent loss is a bug; accounted loss is a design.

## 6. Validators

A channel can carry a predicate, run before a message is stored:

```c
bool sensor_cmd_valid(const void *msg, size_t msg_size)
{
    const struct sensor_cmd *cmd = msg;
    return cmd->interval_ms >= MIN_MS && cmd->interval_ms <= MAX_MS;
}
```

If it returns false, `zbus_chan_pub()` returns `-ENOMSG` and **nothing is stored and no observer runs**. That last part is what makes it useful rather than decorative: a rejected publish is atomic. The caller knows with certainty that the system did not change.

The design value is about *where the rule lives*. Without a validator, every publisher range-checks the interval, and those copies drift apart the moment there are two of them — one from MQTT, one from a shell command, one from a config file. With a validator, the module that owns the data owns the rule, and publishers just report what the bus told them.

## 7. Waiting on a bus and something else at the same time

A consumer thread that only watches the bus is easy: block in `zbus_sub_wait_msg()`.

The hard case is a thread that must watch the bus **and** something unrelated — a socket, a UART, a timer. This project has exactly that: the MQTT thread must wake for broker traffic *and* for fresh readings. There is no combined primitive.

Three ways out, in increasing order of quality:

**Poll the bus on a short timeout.** Block on the socket for, say, 100 ms; on timeout, check the bus non-blockingly; repeat. Works, costs a wakeup ten times a second to usually find nothing, and adds up to 100 ms of latency. Acceptable, unambitious.

**Do the work in the callback.** Let the listener publish the MQTT message directly. This is wrong for a subtle reason: the callback runs on the *sensor* thread, so now two threads call into an `mqtt_client` that is not thread-safe. You would need a mutex around the whole MQTT client, and the sensor thread would block on network I/O.

**Signal a file descriptor.** `poll()` understands descriptors, so give it one: the listener writes to an **eventfd**, which is a counter with a descriptor attached. Add it to the poll set next to the socket and the thread blocks on both with one call, waking only when something genuinely happened.

```c
/* in the listener, on the producer's thread */
zvfs_eventfd_write(evt_fd, 1);

/* in the consumer's event loop */
fds[0].fd = socket;   fds[1].fd = evt_fd;
zsock_poll(fds, 2, timeout);
```

This is not a zbus idea — it is the classic **self-pipe trick** from Unix network servers, where a signal handler writes one byte to a pipe so the main `select()` loop can notice it safely. eventfd is the modern, cheaper form: a 64-bit counter instead of a byte stream.

An eventfd also hands you free instrumentation. Reading it returns the accumulated count and resets it to zero, so the value *is* "how many times was I signalled since I last looked". Anything above one means the channel coalesced messages — the reader can report its own data loss without tracking any extra state.

## 8. What zbus costs

Honest accounting, because "add a bus" is not free:

- **RAM** — each channel holds one message plus a semaphore and bookkeeping; each message subscriber needs a queue and a buffer pool.
- **A copy per publish** — messages are copied in, and copied again for each message subscriber. Keep them small; put a pointer in the channel if the payload is large (and then you own its lifetime again, which is most of what the bus was saving you from).
- **Indirection** — "who handles this?" is answered by a `ZBUS_OBSERVERS()` list rather than by a function call you can follow. Enable `CONFIG_ZBUS_CHANNEL_NAME` and `CONFIG_ZBUS_OBSERVER_NAME` so at least the logs name things.
- **New failure modes** — publish timeouts, queue-full conditions, and callbacks that block the producer.

For the two threads and two channels this project started with, that is arguably more machinery than a mutex and a semaphore would have needed. The payoff is at the *next* consumer: adding one is a line in an observers list, with no edit to the producer at all. Whether that trade is worth it depends on whether you expect a third participant.

This project got to find out. A later phase added a CAN relay — two more threads, four more channels, a second MQTT identity, and a whole second node's traffic passing through the same firmware. `sensor.cpp` was not touched by any of it, and the one edit to the header both sides include changed nothing but a comment. The thread that reads the SCD-40 still publishes one reading to one channel and knows nothing about a peer node, a CAN bus or a second broker connection, because the only thing it was ever coupled to was the channel. The consumer side did change substantially, which is the honest half of the accounting: `main.cpp` grew four listeners and the publish paths behind them. But that is the direction the design promised to make cheap, and the promise held. §9 is what it grew into.

## 9. What this project wires up

Everything above is general. Here is the whole of this firmware's bus — six channels across three translation units, with four threads publishing to them. That is more than it was when the guide was written, and the interesting part is what did *not* have to change to accommodate it.

```
   sensor.cpp                  main.cpp                    relay.cpp
   ──────────                  ────────                    ─────────
   sensor_thread()             the MQTT sessions           rx_thread()
   reads the SCD-40            one thread, one wait,       reads the CAN link
                               six descriptors             tx_thread()
                                                           writes it

 upward — what the network must publish. All LISTENERS, one eventfd each.
 ┌──────────────────────┐
 │ chan_telemetry       │◀── sensor_thread()    our own reading
 ├──────────────────────┤
 │ chan_relay_telemetry │◀── rx_thread()        the peer's, opaque bytes
 ├──────────────────────┤
 │ chan_relay_ack       │◀── rx_thread()        the peer's, opaque bytes
 ├──────────────────────┤
 │ chan_relay_status    │◀── rx_thread()        the peer's liveness
 └──────────┬───────────┘
            │   four listeners, four eventfds, one zsock_poll() in main()
            ▼
      publish to node/1/... and node/2/...

 downward — commands off the wire. All MESSAGE SUBSCRIBERS, a private copy each.
 ┌──────────────────────┐
 │ chan_sensor_cmd      │──▶ sensor_thread()    validator: sensor_cmd_valid
 ├──────────────────────┤
 │ chan_relay_command   │──▶ tx_thread()        then out over ISO-TP
 └──────────┬───────────┘
            ▲
            │   published by main(), on a Command arriving off the socket
```

| Channel | Direction | Message | Observer | Why that kind |
|---|---|---|---|---|
| `chan_telemetry` | sensor → MQTT | `sensor_reading` | **listener** | state; a superseded reading is one the host is better off not getting |
| `chan_relay_telemetry` | CAN → MQTT | `relay_up` | **listener** | same argument, for a reading this node did not take |
| `chan_relay_ack` | CAN → MQTT | `relay_up` | **listener** | an event — but at most one is ever outstanding (below) |
| `chan_relay_status` | CAN → MQTT | `relay_status` | **listener** | liveness is state, and latest-wins is what a retained topic means |
| `chan_sensor_cmd` | MQTT → sensor | `sensor_cmd` | **message subscriber** | an event with no successor; validated (§6) |
| `chan_relay_command` | MQTT → CAN | `relay_down` | **message subscriber** | an event with no successor |

**The table sorts itself by direction, and that is the finding.** Every channel carrying data *toward* the network is a listener; every channel carrying a command *away* from it is a message subscriber. Nobody imposed that rule — the relay's four channels were chosen one at a time on §4's question ("if two arrive before I handle the first, what should happen?") and landed on the same split the original two had. Data flowing up is state, which has a next one coming. Commands flowing down are events, which do not. And the same split holds one layer out, where telemetry is QoS 0 and command/ack are QoS 1: the argument is about the data, so it reaches the same answer inside the chip and across the wire.

**The one row that needed thinking about is `chan_relay_ack`.** An ack is plainly an event, so §4 says message subscriber — yet it is a listener. The justification is not "acks are unimportant" but that **at most one ack is ever outstanding**, guaranteed structurally rather than hoped for: `relay.cpp`'s TX thread calls `isotp_send()` with a null completion callback, which blocks until the whole segmented transfer finishes, and then blocks again waiting for the ack. Command round trips are serialised by construction, and latest-wins cannot collapse a set of one. What makes this honest rather than clever is that the premise is *checked*: §7's eventfd counter returns how many times it was signalled, so a count above one means the invariant broke, and `main.cpp` logs that at ERROR — as against the WARN on the telemetry fd, where coalescing is expected and accounted.

**Four eventfds, not one.** A single shared descriptor would say only "something happened", and since `zbus_chan_read()` hands back the channel's current value whether or not it is fresh, the reader would have to read all four channels on every wake and would republish stale ones. `CONFIG_ZVFS_EVENTFD_MAX` is a hard count — the fourth `zvfs_eventfd()` simply fails without it — so this is one of the few places where the design decision shows up directly as a Kconfig number.

**The observer kinds also decide the RAM, which is §8's cost made concrete.** The message-subscriber net_buf pool is a *single* pool shared across every message-subscriber channel, sized by the largest message on any of them; listener channels store their message in the channel itself and never touch it. `relay_up` is 164 bytes and `relay_down` is 32, so the big upward messages riding listeners is what lets `CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_STATIC_DATA_SIZE` stay at 32 rather than 164 — about 130 bytes per buffer. Worth being clear about the direction of causation: the observer kinds were picked on their merits and the RAM saving is the reward, not the reason. Had the argument gone the other way, the right move would have been to pay the 130 bytes.

Three details worth noticing in the code:

- **Each module defines the channels it owns.** `sensor.cpp` defines the two sensor channels, `relay.cpp` defines the four relay ones. The sensor module owns the readings it produces *and* the sample period the commands adjust, so the bounds and the validator live with the code they constrain — §6's argument about where a rule belongs. The relay owns the link, so it owns what crosses it.
- **The observers live with the consumer, not the producer.** All four `ZBUS_LISTENER_DEFINE`s sit in `main.cpp`, because the network side owns the observers that feed the network. A channel definition and its observers being in different files is normal and is most of what the bus is for.
- **The definitions sit at global scope**, outside each file's anonymous namespace. `ZBUS_CHAN_DEFINE` emits symbols that `ZBUS_CHAN_DECLARE` names from another translation unit; internal linkage would break the match. See [`language-cpp.md`](language-cpp.md) §6.

The reference half of this section is split the same way the code is: the header comment in `shared/app_channels.h` records the decisions behind the two sensor channels, and the one in `gateway/src/relay.h` does the same for the four relay channels — including why they are deliberately *not* in `app_channels.h`, whose stated contract is that nothing in it touches a transport.

## 10. Exercising the bus

Every claim above is observable on the running node. Three exercises, each isolating one property. You need the console on the Mac (`./scripts/console.sh`, quit with **Ctrl-A** then **K**) and, on the Pi, the harness:

```sh
host/.venv/bin/python host/monitor.py          # terminal A, on the Pi
```

### Exercise 1 — Decoupling and latest-wins, in one shot

*Demonstrates §1 (the sensor's cadence is not hostage to the network) and §5 (latest-wins, accounted for).*

Take the broker away for about a minute while watching the **console**, not the Pi:

```sh
sudo systemctl stop mosquitto
#   ... wait ~60 s ...
sudo systemctl start mosquitto
```

During the outage the console keeps logging sensor activity while the MQTT side backs off (`reconnecting in 1000 ms`, doubling to 30000). On reconnect, one line reports the damage:

```
<wrn> node: 12 readings coalesced into one publish
```

and `monitor.py` shows `sequence` jumping by that count plus one — e.g. `seq=56` straight to `seq=69`, which is 12 discarded plus 1 published, matching what the wall clock predicts for a 66 s outage at a 5 s cadence.

**Proves:** the sensor thread sampled straight through an outage *and* a 30 s backoff — the single-loop design of §1 would have stopped sampling entirely. It also proves the loss is **accounted**, not silent: the eventfd counter (§7) and the sequence number agree, so the node reports its own data loss without tracking any extra state.

### Exercise 2 — A validator rejection is atomic

*Demonstrates §6: nothing is stored and no observer runs.*

`SAMPLE_PERIOD_MIN_MS` is 1000, so ask for something below it:

```sh
host/.venv/bin/python host/command.py interval 100
```

```
<- node/1/ack  seq=...  ACK_STATUS_INVALID_ARGUMENT
   detail: interval 100 outside [1000,300000]
```

Then watch `monitor.py`: telemetry keeps arriving at the **previous** cadence, unchanged.

**Proves:** `zbus_chan_pub()` returned `-ENOMSG`, which the MQTT layer maps straight to `ACK_STATUS_INVALID_ARGUMENT`. The period was provably untouched because the message never reached the channel — the rejection is atomic, not a partial application that was rolled back. Note also *where* the rule lives: `main.cpp` never range-checks anything, it only reports what the bus told it.

Now try a legal one and watch the cadence visibly change:

```sh
host/.venv/bin/python host/command.py interval 2000
host/.venv/bin/python host/command.py interval 5000    # restore
```

### Exercise 3 — The loop really is fully blocking

*Demonstrates §7: an eventfd in the poll set, not a polling timeout.*

```sh
host/.venv/bin/python host/command.py trigger
```

A telemetry message appears **immediately**, not on the next 5 s boundary.

**Proves:** the command arrived on the socket, the sensor thread woke early and published, the listener bumped the eventfd, and the descriptor was already readable when `poll()` was re-entered — so the reading went out with no timed wakeup anywhere in the path. Had the MQTT thread been polling the bus on a short timeout instead, this would have shown up to a tick of latency; had the listener published MQTT directly, two threads would be inside a non-thread-safe `mqtt_client` at once.

## 11. The model in one paragraph

A zbus channel is **one message of a fixed type, a lock, and a list of observers**, declared statically — shared storage, notification, and a registry of who cares, which are exactly the three things a bare global-plus-mutex leaves you to build by hand. Publishing copies the message in and runs the observers. Because a channel is not a queue, publishing **overwrites**: right for state, wrong for events, which is why picking the observer kind is the real design decision (§4). A **listener** runs synchronously on the publisher's thread and may only signal; a **message subscriber** gets a private copy on its own thread, so nothing is collapsed. A **validator** puts the rule next to the data it guards and makes rejection atomic. When a consumer must wait on the bus *and* a socket, the answer is an **eventfd** in the poll set rather than a polling timeout. The cost is RAM, a copy per publish, and indirection; the payoff arrives at the *next* consumer, which is a line in an observers list instead of an edit to the producer.

## 12. Where to go next

- **A third observer.** Add a listener to `chan_telemetry` that keeps a running min/max, and notice that `sensor.cpp` does not change. That is the whole argument for a bus, in one commit.
- **`zbus_chan_add_obs()`** — runtime observers, for consumers that come and go.
- **Priority boost** (`CONFIG_ZBUS_PRIORITY_BOOST`) — zbus can temporarily raise a publisher to its highest observer's priority to bound inversion. Worth reading about before a bus carries anything time-critical.
- **The Zephyr samples** in `samples/subsys/zbus/` — `hello_world`, `msg_subscriber`, `priority_boost` and `runtime_obs_registration` each isolate one idea. `benchmark` quantifies the copy cost from §8 on your own target.
