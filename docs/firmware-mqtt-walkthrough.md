# Firmware MQTT client — a walkthrough

A guided reading of `firmware/src/main.cpp`, written to teach the patterns rather than
document the file. For the *concepts* underneath (what a broker is, what QoS means, how
topics work) see [`communication-guide.md`](../notes/communication-guide.md); for the *decisions*
this code implements (which topic, which QoS, and why) see
[`mqtt-design.md`](mqtt-design.md). For how the payloads themselves are encoded, see
[`protobuf-guide.md`](../notes/protobuf-guide.md). For channels, observers and why the
sensor is a separate thread, see [`zbus-guide.md`](../notes/zbus-guide.md).

`main.cpp` owns the network half only. The sensor lives in `sensor.cpp` and the two meet
on the zbus channels in `app_channels.h` — §11 reads that boundary. Together they touch
every layer of the stack: Zephyr's device model, its socket API, the MQTT library, the
sensor API, and an in-process message bus. Read them as a stack of ideas, not top to
bottom.

---

## 1. The shape of the program

**Two threads.** Each owns one clock and one job:

```
sensor.cpp   K_THREAD_DEFINE(sensor_tid, …)
  └─ forever: read the SCD-40, publish to chan_telemetry,
              wait out the sample period (or wake early for a command)

main.cpp     main()
  └─ forever: run a session, back off, retry          ← survives disconnection
      └─ run_session()      connect, then serve until the connection drops
          └─ while (connected)  wake on socket OR bus, handle it, repeat
```

Two things are worth noticing before anything else.

**The outer loop in `main()` is the one that matters.** Most introductory MQTT code is
written as "connect once, then loop publishing", with reconnection bolted on afterwards as
an `if (error) ...`. Here **disconnection is the expected control flow**. `run_session()`
has no success path to fall through to — it always ends by returning, and the only
question the caller asks is how long to wait before trying again.

**The sensor thread does not participate in any of that.** It has never heard of MQTT. It
keeps sampling at its own cadence through a connect, a disconnect, and a 30-second
backoff, and the readings it takes meanwhile simply pile up as a gap in the `sequence`
field. This is what Phase 5 bought: before the split, one loop owned both the sample clock
and the socket, so a reconnect backoff also stopped the sensor.

Both inversions are deliberate. A node that cannot survive a cable pull is not finished,
and a sensor whose cadence depends on the network's mood is not a sensor.

## 2. Zephyr fundamentals used here

### The device model

This one now lives in `sensor.cpp`, but it is the same pattern wherever you meet it:

```c
const struct device *const scd40 = DEVICE_DT_GET(DT_NODELABEL(scd40));
if (!device_is_ready(scd40)) { ... }
```

`DEVICE_DT_GET` resolves **at compile time** to the address of a `struct device` that the
build system generated from the devicetree. There is no lookup by name, no registry
search, no allocation — the linker places the struct and the macro hands you a pointer to
it. `DT_NODELABEL(scd40)` refers to the `scd40:` label in the devicetree overlay.

`device_is_ready()` is the runtime half: it reports whether the driver's init function
succeeded during boot. The pattern is always these two steps — **get the handle
statically, verify it dynamically**.

What the code does when that check *fails* is worth a moment. It does not abort. The
sensor thread keeps running and publishes a reading with `SENSOR_READING_ERROR` every
period, which crosses the bus and reaches the host as `SENSOR_STATUS_ERROR`. Going silent
would be the easier code and the worse behaviour: from the host's side, a node with a dead
sensor and a node that fell off the network look identical. An explicit error is a fact
the host can act on — and you can still reach the node over MQTT to ask it what is wrong.

### Logging

```c
LOG_MODULE_REGISTER(node, LOG_LEVEL_INF);
```

Declares a log module named `node`, which is why console lines appear as `<inf> node:`.
Each module gets its own compile-time and runtime level, so a noisy subsystem can be
turned down without silencing everything.

By default Zephyr logging is **deferred**: `LOG_INF()` packs its arguments into a ring
buffer and returns, and a background thread does the formatting and UART writes. That
means logging from a timing-sensitive path does not block on the serial port — but it
also means log output can lag reality slightly, and can be dropped under flood.

### Time and sleeping

`k_uptime_get()` returns milliseconds since boot as an `int64_t`. `k_msleep()` suspends
the calling thread and yields to the scheduler.

There is no busy-waiting anywhere in this file. Every wait is either `zsock_poll()` or
`k_msleep()`, both of which take the thread off the run queue.

When no thread is runnable the kernel's idle thread executes `WFI` and the Cortex-M7 core
clock gates off until the next interrupt. That needs no configuration, and
`CONFIG_TICKLESS_KERNEL` (already default `y` here) means there is no periodic tick
dragging the core awake — it sleeps until the next real deadline.

WFI is as far as it goes on this target, though. Deeper STM32 states (STOP, STANDBY) need
`CONFIG_PM` plus a SoC implementation and `power-states` devicetree nodes, and the
STM32H7 has neither in Zephyr v4.4.1 — `soc/st/stm32/stm32h7x/` ships no `power.c`, unlike
its low-power-oriented siblings. Nor would it help much: a node holding a TCP connection
open keeps the Ethernet MAC and PHY clocked continuously, which costs far more than the
idle core. Genuinely low-power nodes wake, publish, disconnect, and sleep — a different
architecture from this one.

## 3. Where did the socket go?

Nothing in this file calls `socket()`, `connect()`, `send()`, or `recv()`. That surprises
people coming from Berkeley-sockets examples.

`mqtt_connect()` does all of it: it creates the TCP socket, connects it to the address in
`client.broker`, and sends the MQTT `CONNECT` packet. **The library owns the file
descriptor.**

You touch it only to wait on it:

```c
fds[0].fd = client.transport.tcp.sock;
fds[0].events = ZSOCK_POLLIN;
int rc = zsock_poll(fds, 1, timeout_ms);
```

You borrow the descriptor purely so `zsock_poll()` can tell you *"there are bytes
waiting."* You never read those bytes — `mqtt_input()` does that. (This is
`wait_for_input()`, used while waiting for CONNACK. The serve loop waits on the same
descriptor plus one more — §6.) The division of
responsibility is worth stating plainly:

> **You decide when to wait and for how long. The library performs the actual I/O.**

`zsock_poll()` blocks until one of: the socket becomes readable, the timeout expires, or
an error occurs. Return value `> 0` means "readable", `0` means "timed out", `< 0` means
error.

### Why the `zsock_` prefix

Zephyr's native socket API is namespaced with `zsock_`. Zephyr can *also* expose the
plain POSIX names (`poll`, `socket`, `recv`) when `CONFIG_POSIX_API=y`. Using the
prefixed forms avoids any collision with C library symbols and makes it obvious at a
glance that this is Zephyr's network stack, not the host's.

## 4. `struct mqtt_client` is a form you fill in

`client_setup()` does not *do* anything — it populates a configuration record. Every
field is read later, when `mqtt_connect()` serialises the `CONNECT` packet.

| Field | What it becomes |
|---|---|
| `client_id` | Identifies this session to the broker |
| `keepalive` | A 16-bit seconds field in the CONNECT packet |
| `clean_session` | A flag bit: discard any prior server-side state for this client id |
| `protocol_version` | Selects MQTT 3.1.1 framing |
| `will_topic` / `will_message` / `will_retain` | The Last Will, **stored by the broker** |
| `rx_buf` / `tx_buf` | Where the library assembles packets |
| `transport.type` | Plain TCP vs. TLS |

Three of these deserve more than a table row.

### The buffers are yours

```c
uint8_t rx_buffer[256];
uint8_t tx_buffer[256];
```

Zephyr's MQTT library performs **no dynamic allocation**. You hand it statically allocated
buffers and those sizes are the hard ceiling on packet size. This is characteristic of
embedded libraries generally: the caller supplies the memory, so the footprint is visible
in the map file rather than hidden in a heap.

Practical consequence: a topic + payload larger than `tx_buf_size` cannot be published,
and `mqtt_publish()` will fail rather than grow the buffer.

### Everything you hand the library must outlive the connection

This is the most transferable lesson in the file, and it follows directly from the
no-allocation rule above. **The library stores your pointers; it never copies your data.**
`rx_buf`, `tx_buf`, `broker`, `will_topic`, `will_message` are all dereferenced throughout
the life of the connection — long after `client_setup()` has returned.

So `client_setup()` looks like it is building local objects, but nothing it registers is
local:

```c
namespace {
uint8_t rx_buffer[256];          /* namespace scope: static storage duration */
uint8_t tx_buffer[256];
struct mqtt_client client;
struct sockaddr_in broker;
...

void client_setup(void)
{
    broker = {};                 /* assignment to the file-scope object, NOT a declaration */
    ...
    static struct mqtt_topic will_topic;    /* explicit `static` for the same reason */
    static struct mqtt_utf8 will_message;
    client.will_topic = &will_topic;
}
```

The `static` on `will_topic` and `will_message` is load-bearing. Drop it and they become
stack locals; `client.will_topic` then dangles the instant the function returns, and the
CONNECT packet is built from whatever has since been written over that stack.

Two points worth being precise about:

- **A pointer to a dead local does not become null.** It keeps pointing at reclaimed stack.
  That is worse than null: a null dereference faults immediately and loudly, whereas the
  stale bytes usually survive untouched until the next call overwrites them — so the code
  appears to work on the bench and fails when an interrupt lands at the wrong moment.
- **`broker` is reused across sessions.** `run_session()` calls `client_setup()` on every
  reconnect attempt, so the same struct is re-initialised each time. That is why line
  `broker = {}` re-zeroes it first: without it, fields from a previous session would
  silently carry over.

The `namespace { ... }` wrapper is a separate concern — it gives *internal linkage*, the
C++ replacement for file-scope `static`, keeping these symbols out of other translation
units. It does not affect lifetime; namespace-scope variables already have static storage
duration.

### `keepalive` is a wire field, not a Zephyr timeout

`client.keepalive` is a `uint16_t` **because MQTT 3.1.1 defines Keep Alive as a 16-bit
big-endian count of seconds** in the CONNECT packet. It is not a scheduling value and not
a `k_timeout_t`; its type and unit come from the protocol specification.

What it actually controls is the broker's patience: if the broker hears nothing from this
client for 1.5 × keepalive, it declares the client dead and fires the will. Since this
node publishes every ~5 s, `PINGREQ` rarely fires on a healthy link — the value is really
a **failure-detection deadline**, not a heartbeat interval.

### The will is *registered*, not sent

The will topic and payload travel inside the CONNECT packet and the broker **holds onto
them**. If the connection dies without a clean `DISCONNECT`, the broker publishes the will
on the client's behalf.

No code in this file runs in that path — which is exactly the point, because the scenario
it covers is "the node is dead." You cannot write firmware that reliably announces its own
crash; delegating that to the broker is the whole idea.

Note the matching asymmetry at the end of `run_session()`:

```c
mqtt_disconnect(&client, nullptr);
```

A clean `DISCONNECT` **suppresses** the will. That is MQTT semantics, not a Zephyr detail:
disconnecting deliberately means "I am leaving on purpose", so `offline` should only be
published when the node dies *unexpectedly*.

## 5. Two directions, two mechanisms

Outbound and inbound work completely differently. This is the core client pattern and it
generalises well beyond MQTT.

**Outbound is a direct call.** `publish()` fills a `struct mqtt_publish_param` and calls
`mqtt_publish()`, which serialises the packet and writes it to the socket immediately.

**Inbound is a callback.** `mqtt_evt_handler()` receives `MQTT_EVT_CONNACK`,
`MQTT_EVT_PUBLISH`, `MQTT_EVT_PUBACK`, `MQTT_EVT_SUBACK`, `MQTT_EVT_PINGRESP`, and so on.

But here is the part that is easy to get wrong:

> **The callback does not fire on its own.** It is invoked *synchronously, from inside
> `mqtt_input()`*, on your own thread. There is no MQTT thread. If you stop calling
> `mqtt_input()`, no events ever arrive.

This explains a block that otherwise looks strange:

```c
int rc = mqtt_connect(&client);
...
for (int waited = 0; !connected && !connect_failed && waited < 5000; waited += 100) {
    if (wait_for_input(100) > 0) {
        mqtt_input(&client);
    }
}
```

`mqtt_connect()` returning `0` only means *"TCP is up and the CONNECT packet was sent."*
The CONNACK is a **reply** and arrives later. So the code has to pump `mqtt_input()` in a
loop until the callback flips the `connected` flag. The 5-second cap converts "broker
never answers" from a hang into a reportable failure.

### A note on `volatile`

`connected` and `connect_failed` are declared `volatile`, which suggests concurrent
access. In fact the callback runs on the same thread as the loop reading them, so
`volatile` is not buying anything here. It is harmless, but do not carry away the idea
that `volatile` provides thread safety — it does not. It prevents the compiler from
caching a value in a register; it says nothing about atomicity or memory ordering between
CPUs. Genuine cross-thread sharing in Zephyr wants `atomic_t`, a mutex, or a message
queue.

## 6. The heart: one wait, two descriptors

This is the most instructive part of the file, and the part Phase 5 changed most.

```c
struct zsock_pollfd fds[2] = {};

fds[0].fd = client.transport.tcp.sock;   /* bytes from the broker  */
fds[1].fd = telemetry_evt_fd;            /* a reading from the bus */
fds[0].events = fds[1].events = ZSOCK_POLLIN;

zsock_poll(fds, 2, mqtt_keepalive_time_left(&client));
```

The thread has three reasons to wake up:

1. Bytes arrived from the broker (an incoming command)
2. The sensor thread produced a reading
3. Keepalive is due — a `PINGREQ` must go out or the broker will declare us dead

Reasons 1 and 2 are **descriptors**; reason 3 is a **deadline**. Because the sample clock
now belongs to the sensor thread, exactly one deadline is left, and the timeout is simply
`mqtt_keepalive_time_left()` — no minimum to compute, no clamp to get wrong.

### What this replaced, and why it is better

The previous version owned both clocks and had to reconcile them by hand:

```c
/* the old code */
int64_t until_sample = next_sample - k_uptime_get();
int timeout = MIN(MAX(until_sample, 0), mqtt_keepalive_time_left(&client));
```

That `MAX(until_sample, 0)` was not decoration. An already-expired deadline yields a
negative number, and `poll()` reads a negative timeout as **block forever** — a subtle and
total hang. Every additional deadline folded into one loop adds another such edge. Moving
the sample clock out removed the whole class.

### Why the bus needs a file descriptor at all

`zsock_poll()` understands file descriptors and nothing else. A zbus channel is not one,
so on its own it cannot be waited for alongside a socket. The options are then:

| Approach | Cost |
|---|---|
| Poll the bus on a short timeout | Wakes the thread constantly to usually find nothing; adds latency equal to the tick |
| Publish MQTT directly from the zbus callback | Runs on the sensor thread, so two threads touch a non-thread-safe `mqtt_client` |
| **Signal an eventfd from the callback** | One extra descriptor; the loop stays fully blocking |

The third is what the code does, and it is the standard answer to "wait on a socket and an
internal event at once" — the same trick as the self-pipe in Unix servers. §12 covers the
mechanics.

This "block until any source is ready, then work out which fired" pattern is how every
event loop operates, from `select()`-based network servers to embedded superloops. The
difference from the classic single-threaded version is only *where the deadlines live*.

### `mqtt_live()`

```c
rc = mqtt_live(&client);
if (rc != 0 && rc != -EAGAIN) { ... }
```

Called unconditionally on every pass. It is the library's housekeeping tick: it checks
whether keepalive is due and sends `PINGREQ` if so. `-EAGAIN` means "nothing needed
doing", which is why that value is explicitly excluded from the error check — treating it
as an error would tear down a perfectly healthy connection every loop iteration.

## 7. Receiving: the payload arrives in two pieces

`handle_incoming_publish()` demonstrates a genuinely non-obvious API contract. The
`MQTT_EVT_PUBLISH` event carries the topic and the payload **length** — but not the
bytes. You call `mqtt_readall_publish_payload()` to pull them out.

Why the split? Because the payload may not have arrived yet. TCP is a **byte stream**, not
a message stream: the library has parsed a complete fixed header telling it a 4 KB payload
is coming, but perhaps only 200 bytes are in the socket buffer so far. Rather than force
the library to buffer arbitrary payloads — with memory it does not have — it reports the
length and lets the application read at its own pace into its own storage.

### Every byte must leave the socket

This is the trap. The library resumes parsing wherever the payload ends, so **bytes left
unread are decoded as the next packet's fixed header**. An oversized message would not
simply be truncated; it would desynchronise the connection and produce nonsense errors
afterwards.

So the code keeps what fits and explicitly drains the rest:

```c
uint32_t kept = MIN(remaining, sizeof(payload));
mqtt_readall_publish_payload(c, payload, kept);
remaining -= kept;

bool oversized = (remaining > 0);

while (remaining > 0) {
    uint32_t chunk = MIN(remaining, sizeof(discard));
    mqtt_readall_publish_payload(c, discard, chunk);   /* thrown away */
    remaining -= chunk;
}
```

The drain deliberately targets a **separate** `discard` buffer. Reusing `payload` looks
like a free optimisation — the bytes are being thrown away, so why allocate more stack? —
but it overwrites the prefix that was just kept.

That bug is unusually well camouflaged, and it was caught on hardware rather than by the
compiler. When the payload is a uniform test string the overwritten data is identical to
what it replaced, so the only symptom was a few bytes of stack garbage where the string
terminator had been. With real content the log would have shown the *discarded tail* as
though it were the message. Worth remembering whenever one scratch buffer serves both a
"keep" and a "discard" path.

Now that payloads are Protobuf, `payload` is a `uint8_t` array with no terminator, and
`oversized` is carried forward so the message can be **rejected outright** rather than
decoded from a truncated buffer — a partial message can decode into something plausible.
See [`protobuf-guide.md`](../notes/protobuf-guide.md) §8.

### The QoS 1 obligation

```c
if (pub->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
    struct mqtt_puback_param ack = {};
    ack.message_id = pub->message_id;
    mqtt_publish_qos1_ack(c, &ack);
}
```

If a message arrives at QoS 1, the receiver **must** send a `PUBACK`, or the broker
redelivers it — with the `DUP` flag set — indefinitely. This is "at least once" seen from
the receiving end, and it is precisely why the design requires commands to carry a
`sequence` field the node can deduplicate on. See the QoS rationale in
[`mqtt-design.md`](mqtt-design.md).

## 8. Publishing and message ids

```c
static uint16_t next_message_id;

if (qos == MQTT_QOS_0_AT_MOST_ONCE) {
    param.message_id = 0;
} else {
    next_message_id++;
    if (next_message_id == 0) { next_message_id = 1; }
    param.message_id = next_message_id;
}
```

The packet identifier correlates a `PUBLISH` with its `PUBACK`. At QoS 0 there is no
acknowledgement to correlate and the field is not even placed on the wire, so `0` is
correct there.

For QoS 1 the identifier only has to be **non-zero and distinct among messages currently
in flight**. Since this node never has more than one outstanding, a simple incrementing
counter is sufficient and fully deterministic. The wrap check exists because `0` is
reserved. A client with many concurrent in-flight messages would need to track which ids
are still awaiting acknowledgement and avoid reuse.

## 9. Reconnect and backoff

```c
bool was_connected = run_session();

if (was_connected) {
    backoff = kBackoffMinMs;
}
LOG_WRN("reconnecting in %d ms", backoff);
k_msleep(backoff);
if (!was_connected) {
    backoff = MIN(backoff * 2, kBackoffMaxMs);
}
```

`run_session()` returns whether the session ever reached CONNACK, and that distinction
drives the policy:

- **Reached CONNACK, then dropped** — the broker is demonstrably reachable, so this was a
  transient fault. Reset to the 1 s minimum for a fast recovery.
- **Never connected** — the broker is down, or the cable is unplugged. Keep doubling up to
  30 s so a dead endpoint is retried patiently instead of hammered in a hot loop.

Exponential backoff without the reset is a classic bug: after a handful of unrelated
disconnects the delay is pinned at maximum, and a node that recovers instantly still waits
30 s to notice.

## 10. C APIs from C++

The repeated

```c
reinterpret_cast<uint8_t *>(const_cast<char *>(kTopicCommand))
```

is noise with a real cause. Zephyr's MQTT API describes strings as
`struct mqtt_utf8 { uint8_t *utf8; uint32_t size; }` — a **non-const** `uint8_t *`. String
literals in C++ are `const char[]`. Bridging the two requires dropping `const` and
reinterpreting the character type.

This is safe *here* because the library only reads those bytes, but the casts are
genuinely unpleasant and would be worth hiding behind a small helper if more topics are
added:

```cpp
constexpr mqtt_utf8 utf8_of(const char *s);
```

Related: `main` is never name-mangled, so it needs no `extern "C"`. The event handler is a
plain `static` function used as a C function pointer, which works because it is not a
non-static member function and therefore has ordinary C calling convention.

## 11. The seam between the two threads

Everything above concerns one file. This section reads the join. For channels and
observers from first principles, see [`zbus-guide.md`](../notes/zbus-guide.md).

### Going out: a reading becomes a publish

`sensor.cpp` calls `zbus_chan_pub(&chan_telemetry, &reading, …)`. That copies the reading
into the channel and synchronously runs its observers — of which there is one:

```cpp
void on_telemetry(const struct zbus_channel *chan)
{
    if (telemetry_evt_fd >= 0) {
        zvfs_eventfd_write(telemetry_evt_fd, 1);
    }
}
```

Three things are packed into those two lines.

**It runs on the sensor thread.** A zbus *listener* callback executes in the publisher's
context, inside `zbus_chan_pub()`, with the channel locked. So it must not block and must
not do work — anything slow here stalls the sensor thread and holds the channel lock
against the reader. Bumping a counter is about the most it should ever do.

**The signal and the value travel separately.** The eventfd carries "something happened";
the reading stays in the channel. The MQTT thread picks it up with `zbus_chan_read()`,
which always returns the channel's *current* contents. A channel stores exactly one
message, so if two readings are produced before the reader gets there, the reader sees the
newer one and the older is simply gone. That is the intended semantics for telemetry — a
stale reading is worse than a missing one — and it is the same argument that makes
telemetry QoS 0 on the wire.

**The `>= 0` guard is not defensive padding.** `K_THREAD_DEFINE` starts the sensor thread
at boot, before `main()` has created the descriptor, so the first reading can genuinely
arrive with no fd to signal. Losing a reading taken before the network exists costs
nothing.

### The counter is free instrumentation

```cpp
zvfs_eventfd_read(telemetry_evt_fd, &signalled);   /* non-blocking; resets to 0 */
if (signalled > 1) {
    LOG_WRN("%llu readings coalesced into one publish", signalled - 1);
}
```

An eventfd holds a 64-bit counter, and reading it returns the accumulated total and clears
it. So the value is exactly "how many readings happened since I last looked", and anything
above 1 means the channel overwrote some. The node reports its own data loss, for free,
with no extra state.

On the bench this closed the loop precisely. A 66 s broker outage at a 5 s cadence
produced `12 readings coalesced into one publish` and a jump from `seq=56` to `seq=69` —
12 discarded plus 1 published equals the 13 the wall clock predicts.

### Coming back: a command becomes a bus message

`apply_command()` no longer changes anything itself. It fills a `struct sensor_cmd` and
hands it over:

```cpp
int rc = zbus_chan_pub(&chan_sensor_cmd, &sc, K_MSEC(100));
```

The interesting part is the error mapping. `chan_sensor_cmd` is defined with a
**validator**, and when a validator rejects a message `zbus_chan_pub()` returns `-ENOMSG`
and the message is never stored or delivered. So `-ENOMSG` means precisely "out of range,
and nothing changed" — which is exactly `ACK_STATUS_INVALID_ARGUMENT`:

```cpp
case -ENOMSG:
    snprintf(detail, detail_len, "interval %u outside [%u,%u]", …);
    return node_AckStatus_ACK_STATUS_INVALID_ARGUMENT;
```

Before this, `main.cpp` range-checked the interval itself. Now the bounds live in
`sensor.cpp`, next to the thread that actually obeys them, and the wire layer cannot drift
from them — it only reports what the bus told it. `interval 100` still comes back
`ACK_STATUS_INVALID_ARGUMENT` with `detail: interval 100 outside [1000,300000]`; the
difference is that the rule now has one home.

Note also that `chan_sensor_cmd` uses a **message subscriber**, not a listener: it
delivers a private copy of every message, in order, and collapses nothing. Commands have
no next one coming, so none may be dropped — the QoS 1 argument, applied internally.

## 12. What is deliberately missing

- **No TLS, no credentials.** `MQTT_TRANSPORT_NON_SECURE` and anonymous access — bench
  only. A real deployment uses `MQTT_TRANSPORT_SECURE` plus a credential set.
- **No persistence.** The sample period survives reconnects but not reboots; a
  `SetInterval` is lost on power cycle. Zephyr's settings subsystem is the usual answer.
- **Single-slot command dedupe.** `last_command_sequence` remembers only the most recent
  command, so back-to-back duplicates are caught but an interleaved `A, B, A` is not. A
  deliberate simplification for a node with one command source.
- **No backpressure from the bus.** The sensor thread publishes regardless of whether the
  MQTT thread is keeping up, and never learns that a reading was discarded — only the
  reader sees the coalesce count. Fine for latest-wins telemetry; wrong for anything that
  must not be lost.
- **The backoff can outlast the outage.** A broker that returns after 2 s may still wait
  out a 30 s delay. See *Settled since* in [`mqtt-design.md`](mqtt-design.md).
