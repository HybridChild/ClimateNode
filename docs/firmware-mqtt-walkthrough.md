# Firmware MQTT client — a walkthrough

A guided reading of `firmware/src/main.cpp`, written to teach the patterns rather than
document the file. For the *concepts* underneath (what a broker is, what QoS means, how
topics work) see [`communication-guide.md`](communication-guide.md); for the *decisions*
this code implements (which topic, which QoS, and why) see
[`mqtt-design.md`](mqtt-design.md).

The file is ~400 lines and touches every layer of the stack: Zephyr's device model, its
socket API, the MQTT library, and the sensor API. Read it as a stack of ideas, not
top to bottom.

---

## 1. The shape of the program

Three loops nested inside each other. Recognising them makes the rest fall into place.

```
main()                     forever: run a session, back off, retry
 └─ run_session()          connect, then serve until the connection drops
     └─ while (connected)  wake, handle I/O, maybe publish, sleep again
```

The outer loop is the one that matters. Most introductory MQTT code is written as
"connect once, then loop publishing", with reconnection bolted on afterwards as an
`if (error) ...`. Here **disconnection is the expected control flow**. `run_session()`
has no success path to fall through to — it always ends by returning, and the only
question the caller asks is how long to wait before trying again.

That inversion is deliberate. A node that cannot survive a cable pull, a broker restart,
or a Wi-Fi hiccup is not finished, so the retry structure is designed in from the start
instead of patched on.

## 2. Zephyr fundamentals used here

### The device model

```c
scd40 = DEVICE_DT_GET(DT_NODELABEL(scd40));
if (!device_is_ready(scd40)) { ... }
```

`DEVICE_DT_GET` resolves **at compile time** to the address of a `struct device` that the
build system generated from the devicetree. There is no lookup by name, no registry
search, no allocation — the linker places the struct and the macro hands you a pointer to
it. `DT_NODELABEL(scd40)` refers to the `scd40:` label in the devicetree overlay.

`device_is_ready()` is the runtime half: it reports whether the driver's init function
succeeded during boot. The pattern is always these two steps — **get the handle
statically, verify it dynamically**.

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
`k_msleep()`, both of which take the thread off the run queue. On a real product this is
what lets the CPU drop into a low-power idle state between events.

## 3. Where did the socket go?

Nothing in this file calls `socket()`, `connect()`, `send()`, or `recv()`. That surprises
people coming from Berkeley-sockets examples.

`mqtt_connect()` does all of it: it creates the TCP socket, connects it to the address in
`client.broker`, and sends the MQTT `CONNECT` packet. **The library owns the file
descriptor.**

You touch it in exactly one place:

```c
fds[0].fd = client.transport.tcp.sock;
fds[0].events = ZSOCK_POLLIN;
int rc = zsock_poll(fds, 1, timeout_ms);
```

You borrow the descriptor purely so `zsock_poll()` can tell you *"there are bytes
waiting."* You never read those bytes — `mqtt_input()` does that. The division of
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

## 6. The heart: computing the poll timeout

This is the most instructive part of the file.

```c
int64_t until_sample = next_sample - k_uptime_get();
int keepalive_ms = mqtt_keepalive_time_left(&client);
int timeout = MIN(MAX(until_sample, 0), keepalive_ms);
```

The thread has **three** reasons to wake up:

1. Bytes arrived from the broker (an incoming command)
2. It is time to publish the next telemetry sample
3. Keepalive is due — a `PINGREQ` must go out or the broker will declare us dead

Reason 1 is handled by `zsock_poll()` returning early. Reasons 2 and 3 are **deadlines**,
and the sleep must not overshoot *either* of them — so the timeout is the minimum of the
two. Sleeping past the sample deadline stalls telemetry; sleeping past the keepalive
deadline gets the connection torn down by the broker.

`MAX(until_sample, 0)` clamps the sample deadline at zero. Without it, an already-expired
deadline yields a negative number, which `poll()` interprets as **block forever** — a
subtle and total hang.

This "sleep until the nearest deadline, then work out which one fired" pattern is how
every single-threaded event loop operates, from `select()`-based network servers to
embedded superloops. The alternative — one thread per concern plus synchronisation —
buys complexity that a constrained target does not need.

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
uint32_t kept = MIN(remaining, sizeof(payload) - 1);
mqtt_readall_publish_payload(c, payload, kept);
remaining -= kept;

while (remaining > 0) {
    uint32_t chunk = MIN(remaining, sizeof(payload));
    mqtt_readall_publish_payload(c, payload, chunk);   /* discard */
    remaining -= chunk;
}
```

Reusing `payload` as the drain buffer is intentional — the bytes are being thrown away, so
no additional storage is needed.

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

## 11. What is deliberately missing

- **No Protobuf.** Payloads are plain text (`seq=1 co2=812 temp=22.41 rh=41.3`). Phase 4
  replaces `format_telemetry()` with nanopb encoding against the schema in `proto/`.
- **No zbus.** `run_session()` reads the sensor directly. Phase 5 introduces a zbus channel
  so the sensor task publishes readings and the MQTT task consumes them, decoupling
  acquisition from transport.
- **No TLS, no credentials.** `MQTT_TRANSPORT_NON_SECURE` and anonymous access — bench
  only. A real deployment uses `MQTT_TRANSPORT_SECURE` plus a credential set.
- **No `node/<id>/ack` publishing.** The subscribe path logs commands but does not answer
  yet; that arrives with the Protobuf `Command` / `Ack` pair.
