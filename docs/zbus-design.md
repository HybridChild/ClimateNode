# zbus design — project reference

The channels this firmware runs on, who observes each and why, and the two Kconfig numbers that hold the arrangement up. Terse by intent: decisions, rationale, and the facts you need when the bus misbehaves. For the concepts underneath — what a channel and an observer are, why a multi-threaded firmware wants a bus at all, and how to wait on one alongside a socket — see the companion teaching guide, [`zbus-guide.md`](../notes/zbus-guide.md).

Part of this reference is inline, deliberately: the header comments in [`../shared/app_channels.h`](../shared/app_channels.h) and [`../gateway/src/relay.h`](../gateway/src/relay.h) carry each channel's rationale next to the declaration it constrains, where an edit cannot miss it. This page is what neither header can state alone — the inventory across both, and a pool constraint that spans every channel in the image.

## The channels

Six on the gateway, defined across two translation units. The first four carry data *toward* the network, the last two carry commands *away* from it, and the observer kinds sort themselves the same way:

| Channel | Defined in | Message | Observer | Why that kind |
|---|---|---|---|---|
| `chan_telemetry` | `sensor.cpp` | `sensor_reading`, 44 B | listener | state; a superseded reading is one the host is better off not getting |
| `chan_relay_telemetry` | `relay.cpp` | `relay_up`, 164 B | listener | same, for a reading this node did not take |
| `chan_relay_ack` | `relay.cpp` | `relay_up`, 164 B | listener | an event, but at most one is ever outstanding — see below |
| `chan_relay_status` | `relay.cpp` | `relay_status`, 2 B | listener | liveness is state, and latest-wins is what a retained topic means |
| `chan_sensor_cmd` | `sensor.cpp` | `sensor_cmd`, 8 B | message subscriber | an event with no successor; validated |
| `chan_relay_command` | `relay.cpp` | `relay_down`, 32 B | message subscriber | an event with no successor |

Struct sizes read off the ELF with `arm-zephyr-eabi-gdb -ex 'print sizeof(struct …)'`. Why the split falls out along direction rather than being imposed, and why it reaches the same answer as QoS 0 vs QoS 1 does on the wire, is [`zbus-guide.md`](../notes/zbus-guide.md) §4 and §9.

**`chan_relay_ack` is the one row that needed thinking about.** An ack is an event, so the rule above says message subscriber. It is a listener because at most one ack is ever outstanding — `relay.cpp`'s TX thread calls `isotp_send()` with a null completion callback, which blocks until the transfer completes, then blocks again on the ack — so latest-wins cannot collapse a set of one. That is a premise rather than a proof, so it is checked: the eventfd counter says how many times it was signalled, and `main.cpp` logs a coalesced ack at **ERROR** where it logs coalesced telemetry at WARN.

**The peer node runs two of the same channels** off the same `shared/app_channels.h` — `chan_telemetry` from its sensor thread to its CAN session, and `chan_sensor_cmd` for the commands the gateway relays to it. Both are defined in `peer-node/src/sensor.cpp`, and its single listener is in `peer-node/src/main.cpp`.

## Where each piece is defined

- **Each module defines the channels it owns.** `sensor.cpp` defines the two sensor channels, `relay.cpp` the four relay ones. The sensor module owns the readings it produces *and* the sample period commands adjust, so `SAMPLE_PERIOD_*` and the validator sit together in `app_channels.h`; the relay owns the link, so it owns what crosses it.
- **The observers live with the consumer.** All four `ZBUS_LISTENER_DEFINE`s are in `gateway/src/main.cpp`, because the network side owns the observers that feed the network. The two `ZBUS_MSG_SUBSCRIBER_DEFINE`s sit with the threads that wait on them, in `sensor.cpp` and `relay.cpp`.
- **Definitions sit at global scope**, outside each file's anonymous namespace: `ZBUS_CHAN_DEFINE` emits symbols `ZBUS_CHAN_DECLARE` names from another translation unit, and internal linkage would break the match. See [`language-cpp.md`](../notes/language-cpp.md) §6.
- **`ZBUS_CHAN_DECLARE` expands to `extern`**, so whoever links supplies the definition — `sensor.cpp` in the firmware, the suite itself in `tests/commands/`. That is the seam the tests use, and it is the boundary the design already had rather than one carved for them; see [`test-strategy.md`](test-strategy.md).

## The pool is sized by every channel, not every message subscriber

`CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_STATIC_DATA_SIZE` is **164** on the gateway and **44** on the peer, and both numbers are the largest message on *any* channel in that application — not the largest on a message-subscriber channel, which is what the option's name suggests.

**Why.** With `CONFIG_ZBUS_MSG_SUBSCRIBER=y`, `_zbus_vded_exec()` allocates a pool buffer and copies the entire message into it on **every** `zbus_chan_pub()`, before it examines a single observer (`subsys/zbus/zbus.c:244-256`). That block is guarded by the Kconfig symbol alone, never by the channel's observer kinds. A listener channel skips the *delivery*, not the copy — so putting large messages on listeners buys nothing here, and every channel's message must fit a slot.

**The failure mode is why this is worth a section.** Undersize the pool and `net_buf_add_mem()` writes past a fixed slot in a contiguous `uint8_t[count][size]` array. There is no error: zbus's own `__ASSERT` for it (`zbus.c:46`) is compiled out unless `CONFIG_ASSERT=y`, which neither app sets. A small overrun lands inside the next buffer of the same pool, which is about to be reused anyway, and can run indefinitely with no visible symptom. A large one escapes the array and corrupts whatever follows, surfacing much later as a fault in an unrelated thread — typically an imprecise bus fault inside `__aeabi_memcpy4` with the return address in `_zbus_vded_exec`, in whichever thread next published something. If you ever see that signature, this option is the first thing to check.

`static_assert`s in [`../shared/app_channels.h`](../shared/app_channels.h) and [`../gateway/src/relay.h`](../gateway/src/relay.h) tie every channel's message size to the Kconfig value, so adding a channel or growing a message fails the build instead. Enabling `CONFIG_ASSERT=y` for bench builds — as `peer-node/debug.conf` layers a shell — is the other half of the defence, and turns a class of silent corruption into a named message.

`CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_POOL_SIZE` is **4** on the gateway and **2** on the peer: the count of slots, not their size, and sized by how many publishes can be in flight at once rather than by any message.

## The poll set

Four eventfds, one per channel carrying data toward the network, so that `main()` can wait on the bus and both sockets in a single `zsock_poll()`. Not one shared descriptor: `zbus_chan_read()` returns a channel's current value whether or not it is fresh, so a reader woken by a shared signal could not tell which channel had news and would republish stale telemetry as new. The descriptor is the event's identity, not merely its occurrence.

Two Kconfig counts follow directly, and both are hard limits rather than hints:

- **`CONFIG_ZVFS_EVENTFD_MAX=4`** — the fourth `zvfs_eventfd()` simply fails without it.
- **`CONFIG_ZVFS_POLL_MAX=6`** — two MQTT sockets plus the four eventfds. The serve loop is six descriptors and still **one deadline**, the nearest across every session, because no producer's clock lives in that loop.

Both are sized for exactly two nodes; a third peer would move them. See [`can-bringup.md`](can-bringup.md) *What is deliberately not here*.

## The validator

`chan_sensor_cmd` carries `sensor_cmd_valid()`, the zbus adapter in `sensor.cpp` around `sensor_cmd_in_range()` in `app_channels.h`, which is stated next to the `SAMPLE_PERIOD_*` bounds it compares against. A validator rejection returns `-ENOMSG` from `zbus_chan_pub()` and stores nothing, so the rejection is atomic — which is what lets `commands.cpp` map `-ENOMSG` straight to `ACK_STATUS_INVALID_ARGUMENT` without range-checking anything itself. One rule, one definition, reachable from both applications and from `tests/commands/`.
