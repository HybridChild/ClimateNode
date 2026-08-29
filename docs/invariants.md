# Cross-cutting invariants — project reference

The contracts that no single file can state on its own, because two or more components have to agree on them. Each one is **silent when violated**: nothing logs, nothing returns an error, and the symptom appears somewhere other than the code that broke it. This page is the inventory — one entry each, with what enforces it and where the full treatment lives. It is deliberately short; nothing here replaces the page it links to.

Why a reference with no teaching guide of its own: each of these belongs to a different topic, and the guides cover them in their own contexts. What the guides cannot do is put them in one place to be re-read before a change.

## The CAN link

**No two contexts on a node may listen to the same identifier.** Zephyr installs one CAN filter per ISO-TP context, and a real controller fires only the lowest-numbered match — so the second context is starved with no diagnostic whatsoever. This is why data and flow control have separate identifier ranges in [`can_link.h`](../shared/can_link.h) rather than sharing one. *Enforced by* `tests/heartbeat/`'s `test_no_node_listens_to_one_identifier_twice`, which deliberately runs with no controller attached: an emulated one is all-match and would accept a colliding map. *Full treatment:* [`can-bringup.md`](can-bringup.md) *Flow control needs its own identifiers*.

**The heartbeat must sort below the ISO-TP ranges.** `0x700 + id` beats `0x7E0 + n` in arbitration, so liveness cannot be starved by a segmented transfer in progress. Reversing that ordering would make the gateway declare a peer offline precisely when the peer is busiest. *Enforced by* `test_address_map`. *Full treatment:* [`can-bringup.md`](can-bringup.md) *The address map*.

**The address map caps the bus at `kMaxPeerNodeId`.** The four identifier ranges are spaced four apart, so `n = 0..3`; a fifth peer would be handed an identifier another node already filters on, and the symptom would be exactly the silently-swallowed flow control above. A fifth peer is not a tight fit, it is a wrong one — the map has to grow first. *Enforced by* `tests/heartbeat/`, on the constants alone.

**`can_send()` with a NULL callback blocks with no bound, and the timeout argument does not change that.** The synchronous path ends in `k_sem_take(&ctx.done, K_FOREVER)` (`drivers/can/can_common.c:69`); the `k_timeout_t` bounds only the wait for a free TX mailbox, not the wait for completion. The bxCAN driver clears `NART` and sets `ABOM`, so an unacknowledged frame is retried forever and never permanently fails — a lone transmitter on a dead link blocks for as long as the link is down, taking its whole thread with it. **Pass a callback in anything that must stay responsive**, as [`peer-node/src/main.cpp`](../peer-node/src/main.cpp) does. *Full treatment:* [`can-bringup.md`](can-bringup.md) *Driver behaviour worth knowing*.

## The internal bus

**The zbus pool is sized by every channel, not every message subscriber.** With `CONFIG_ZBUS_MSG_SUBSCRIBER=y`, **every** `zbus_chan_pub()` copies its whole message into a pool buffer before any observer is consulted — listener-only channels included. `CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_STATIC_DATA_SIZE` must therefore cover the largest message on *any* channel, and undersizing it corrupts memory with no diagnostic: neither app sets `CONFIG_ASSERT=y`, so zbus's own check is compiled out. **Adding a channel or growing a message means re-checking it.** *Enforced by* `static_assert`s in [`app_channels.h`](../shared/app_channels.h) (lines 123, 127) and [`relay.h`](../gateway/src/relay.h) (lines 79, 82, 85). *Full treatment:* [`zbus-design.md`](zbus-design.md) *The pool is sized by every channel*.

**`kRelayUpMax` must stay above the largest encoded message.** The relay carries the peer's payloads without decoding them, so its buffers are sized by a plain number (160) that `relay.h` states without including the generated header. A schema change that outgrows it would truncate an `Ack` on the wire. *Enforced by* `static_assert(node_Ack_size <= kRelayUpMax)` and its two siblings at [`relay.cpp:36`](../gateway/src/relay.cpp) — `relay.cpp` is the one translation unit allowed to include `node.pb.h`, which is why the check lives there and not beside the constant.

## The wire format

**`proto/node.proto` is the only source of truth, and `schema_version` bumps only on a breaking change.** Firmware and host both generate from that file; additive changes do not touch the version, because Protobuf already handles them. Hand-editing generated code, or restating the field rules in prose, re-introduces exactly the drift the generator exists to prevent. *Enforced by* `tests/protocol/`, which runs the previous schema against the current one in both directions. *Full treatment:* [`../proto/node.proto`](../proto/node.proto), which carries its evolution rules inline.

**`shared/` may not depend on an application, and no application may depend on another.** The rule is one definition, no drift: `gateway/`, `peer-node/` and `tests/` compile the same files by relative path, which is what makes a passing suite a test of what the boards actually run. A `#include` reaching the wrong way compiles fine in one app and breaks the other two. *Full treatment:* the `CLAUDE.md` section *What this is*.

## The sensor

**A poll faster than the SCD-40's conversion republishes the last reading.** In periodic mode `sensor_sample_fetch()` returns 0 without updating the values when no fresh sample is ready, so a sub-5 s cadence yields duplicate readings with a fresh `sequence` — a known, documented, deliberately unfixed limitation. Read it before touching `SAMPLE_PERIOD_MIN_MS` or trusting a fast cadence. *Full treatment:* [`sensor-bringup.md`](sensor-bringup.md) *Accepted limitation*.
