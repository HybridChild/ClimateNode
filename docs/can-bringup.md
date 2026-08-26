# CAN bring-up — project reference

How the CAN link between the **Nucleo-H753ZI** gateway and the **Nucleo-F072RB** peer node is configured, clocked and verified on this bench. Terse by intent: decisions, rationale, and the facts you need when the link misbehaves. For the concepts underneath — what a differential multi-drop bus is, how arbitration makes a lower ID win, why acceptance filters exist, and what 8 bytes per frame does to a protocol design — see the companion teaching guide, [`can-guide.md`](../notes/can-guide.md).

**The link works end to end on hardware.** Two SN65HVD230 transceivers, a terminated 500 kbit/s two-node bus, heartbeats acknowledged, telemetry segmented from the peer to `node/2/telemetry`, and a command answered by the peer's own `DeviceInfo` on `node/2/ack`. *Bring-up checks* below are the procedures, in the order worth running them. Two constraints on this design are subtle enough to have their own sections and are worth reading before changing either area: *Flow control needs its own identifiers* and *The zbus pool is sized by every channel*.

Builds against the shared global Zephyr workspace — see [`toolchain.md`](toolchain.md).

## The CAN path

```
gateway/boards/nucleo_h753zi.overlay    gateway: the bitrate the board dts leaves unset
gateway/prj.conf                        gateway: CONFIG_CAN, CONFIG_ISOTP, the CAN shell
shared/can_link.h                       SHARED: address map, heartbeat frame, message type
gateway/src/relay.h                     gateway: the relay channels and liveness rule
gateway/src/relay.cpp                   gateway: the CAN threads
peer-node/boards/nucleo_f072rb.overlay  peer: can1 enabled on PA11/PA12, the same bitrate
peer-node/prj.conf                      peer: CONFIG_CAN + CONFIG_ISOTP, no shell
peer-node/src/main.cpp                  peer: the CAN session
```

`can_link.h` is the one file both boards include, and the reason it is a file rather than a comment: a link is symmetric, and neither end is in a position to be right on its own. It carries the address map, the hand-packed heartbeat layout and the one byte of message type above ISO-TP — nothing else, and nothing either side owns alone. `tests/heartbeat/` compiles it with no subsystem at all, which is the evidence that the contract has no dependencies.

### What the relay does

The gateway is a **transport hop**, not an aggregator. It moves the peer's already-encoded Protobuf between CAN and MQTT and never decodes it:

```
peer node                     gateway                              broker
─────────                     ───────                              ──────
Telemetry ── ISO-TP 0x7E8 ──▶ RX thread ──▶ chan_relay_telemetry ──▶ node/2/telemetry  QoS 0
Ack       ── ISO-TP 0x7E8 ──▶ RX thread ──▶ chan_relay_ack       ──▶ node/2/ack        QoS 1
heartbeat ── raw 0x702 ─────▶ RX filter ──▶ liveness ─▶ chan_relay_status ──▶ node/2/status  retained
Command   ◀── ISO-TP 0x7E0 ── TX thread ◀── chan_relay_command   ◀── node/2/command    QoS 1
```

The two threads are split by **what they block on**: RX only ever reads the link (a timed `isotp_recv()`, so the loop that receives also owns the liveness clock), TX only ever writes it. That is a correctness property, not tidiness — two `isotp_send()` calls on one address from different contexts interleave frames and corrupt both transfers, and confining every write to one thread enforces it without a mutex.

Two threads rather than the peer node's one, because the gateway has the harder problem: it must be receiving whenever the peer transmits *and* able to send a command that arrived from the broker at any moment, and one thread doing both would have to choose between blocking in `isotp_recv()` and blocking in `isotp_send()`. The peer has neither the second obligation nor the RAM for a second stack.

**Two alternatives were rejected, both for the RX side's timeout.** `k_poll()` on the receive context's fifo would let one thread wait on the link and the bus together — but that fifo lives inside `struct isotp_recv_ctx`, which `isotp.h` marks internal, so it would be reaching past a documented boundary. A `k_timer` for the liveness timeout would fire in ISR context, where `zbus_chan_pub()` cannot be called because it takes a mutex, so it would need a work item purely to publish. A timed `isotp_recv()` gives the same result with neither: the loop that receives also owns the clock, which is the pattern `sensor.cpp` already uses.

Decisions worth keeping:

- **The gateway reads exactly one byte of what it carries.** `[0]` is the message type from `can_link.h`; `[1..]` is forwarded verbatim. Which channel a payload lands on decides its topic, and that is the whole of the gateway's knowledge about it. `publish_relayed()` in `main.cpp` is one function long on purpose.
- **One eventfd per relay channel, three more in the same poll set.** A shared one would say only "something happened", and because `zbus_chan_read()` returns the channel's current value whether or not it is fresh, the reader would have to read all three on every wake and would republish stale telemetry as new. The descriptor is the event's identity, not merely its occurrence. The serve loop is now five descriptors and still **one deadline** — the keepalive.
- **`chan_relay_ack` is a listener, not a message subscriber**, despite an ack being an event. At most one ack is ever outstanding, because `isotp_send()` blocks until the transfer completes and the TX thread then blocks on the ack: latest-wins cannot collapse a set of one. The eventfd's counter checks that premise for free, and `main.cpp` logs a coalesced ack at **ERROR** where it logs coalesced telemetry at WARN.
- **The upward message is 164 B and the downward one 32 B**, which follows from what each direction carries and is independent of the observer kinds. It buys no RAM: every publish copies its whole message into a pool buffer regardless of who observes the channel — see *The zbus pool is sized by every channel* below.
- **One honest compromise, named.** To correlate an Ack it synthesizes for a command the peer never answered, the gateway must know that command's `sequence` — so `main.cpp` decodes the Command **envelope**, one header field, and never the payload arm. The alternative was to synthesize nothing and let the host time out, which is simpler and strictly worse: the host cannot then distinguish "the peer is gone" from "the gateway dropped it". Recorded in [`mqtt-design.md`](mqtt-design.md).

Kconfig this cost, all in `gateway/prj.conf` and all previously defaults nobody had set: `CONFIG_ISOTP=y`, `CONFIG_ZVFS_EVENTFD_MAX=4` (a hard count — the fourth `zvfs_eventfd()` just fails without it), `CONFIG_ZVFS_POLL_MAX=5`, `CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_STATIC_DATA_SIZE` 16 → 164 (sized by `relay_up`; see below), `CONFIG_MAIN_STACK_SIZE` 2048 → 3072, `CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE` 1024 → 2048 (ISO-TP drives every segmented transfer from `k_work_submit()`, which is the *shared* system workqueue). The whole gateway image builds at **248 352 B flash and 62 012 B RAM** on a part with 2 MB and 512 KB, so none of this is close to a constraint. Read the current numbers off `scripts/build.sh -t rom_report` rather than trusting these.

### The address map

Five 11-bit identifiers per peer, `id` being the node id and `n = id - 2`:

| Identifier | Direction | Carries |
|---|---|---|
| `0x700 + id` | peer → gateway | heartbeat, one raw frame at 1 Hz |
| `0x7E0 + n` | gateway → peer | commands, ISO-TP data |
| `0x7E4 + n` | peer → gateway | flow control answering the above |
| `0x7E8 + n` | peer → gateway | telemetry and acks, ISO-TP data |
| `0x7EC + n` | gateway → peer | flow control answering the above |

For node 2 that is `0x702`, `0x7E0`, `0x7E4`, `0x7E8`, `0x7EC`. The ranges are borrowed rather than invented — `0x700 + id` is CANopen's heartbeat convention and `0x7E0`/`0x7E8` is the UDS request/response pair — so a trace reads familiarly to anyone who has met a CAN bus, at no cost. Neither protocol is implemented.

The two flow-control identifiers are *not* borrowed, and they are the one place this map departs from the UDS shape it otherwise imitates: UDS carries a direction's data and its flow control on the same identifier pair, which is not usable on either of these controllers. Why, and what it costs, is *Flow control needs its own identifiers* below.

The ordering is load-bearing, though, and not decorative: a lower identifier wins arbitration, so heartbeats outrank every ISO-TP frame and liveness cannot be starved by a long segmented transfer. `tests/heartbeat/` asserts that relationship rather than trusting it to survive an edit.

**One byte of protocol sits above ISO-TP**: `[0]` is the message type (telemetry / ack / command), `[1..]` is the protobuf payload verbatim. ISO-TP has no equivalent of an MQTT topic, and [`mqtt-design.md`](mqtt-design.md)'s rule still holds — the type is not on the wire, so something outside the payload must assert it. On MQTT the topic does that for free; here it costs a byte. The gateway will read that byte, strip it and forward the rest untouched, which is the whole of what "the gateway does not decode the peer's payload" means in practice.

## The zbus pool is sized by every channel, not every message subscriber

`CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_STATIC_DATA_SIZE` is **164** on the gateway and **44** on the peer, and both numbers are the largest message on *any* channel in that application — not the largest on a message-subscriber channel, which is what the option's name suggests.

**Why.** With `CONFIG_ZBUS_MSG_SUBSCRIBER=y`, `_zbus_vded_exec()` allocates a pool buffer and copies the entire message into it on **every** `zbus_chan_pub()`, before it examines a single observer (`subsys/zbus/zbus.c:244-256`). That block is guarded by the Kconfig symbol alone, never by the channel's observer kinds. A listener channel skips the *delivery*, not the copy — so putting large messages on listeners buys nothing here, and every channel's message must fit a slot.

The message sizes, read off the ELF with `arm-zephyr-eabi-gdb -ex 'print sizeof(struct …)'`:

| Struct | Size | Channels |
|---|---|---|
| `relay_up` | 164 B | `chan_relay_telemetry`, `chan_relay_ack` (listeners) |
| `sensor_reading` | 44 B | `chan_telemetry` (listener) |
| `relay_down` | 32 B | `chan_relay_command` (message subscriber) |
| `sensor_cmd` | 8 B | `chan_sensor_cmd` (message subscriber) |
| `relay_status` | 2 B | `chan_relay_status` (listener) |

**The failure mode is why this is worth a section.** Undersize the pool and `net_buf_add_mem()` writes past a fixed slot in a contiguous `uint8_t[count][size]` array. There is no error: zbus's own `__ASSERT` for it (`zbus.c:46`) is compiled out unless `CONFIG_ASSERT=y`, which neither app sets. A small overrun lands inside the next buffer of the same pool, which is about to be reused anyway, and can run indefinitely with no visible symptom. A large one escapes the array and corrupts whatever follows, surfacing much later as a fault in an unrelated thread — typically an imprecise bus fault inside `__aeabi_memcpy4` with the return address in `_zbus_vded_exec`, in whichever thread next published something. If you ever see that signature, this option is the first thing to check.

`static_assert`s in [`shared/app_channels.h`](../shared/app_channels.h) and [`gateway/src/relay.h`](../gateway/src/relay.h) tie every channel's message size to the Kconfig value, so adding a channel or growing a message fails the build instead. Enabling `CONFIG_ASSERT=y` for bench builds — as `peer-node/debug.conf` layers a shell — is the other half of the defence, and turns a class of silent corruption into a named message.

## Flow control needs its own identifiers

The address map gives each ISO-TP context an identifier nothing else on that node filters. That costs two identifiers per peer over the UDS-style two-identifier scheme, and it is not optional on this hardware.

**The mechanism.** Zephyr's ISO-TP installs a separate CAN acceptance filter per context: a bound receive context filters on the identifier it receives data on (`add_ff_sf_filter`, `subsys/canbus/isotp/isotp.c:606`), and a send context filters on the identifier it expects Flow Control on (`add_fc_filter`, `:1185`). Give both the same identifier — as UDS does, answering a transfer on `0x7E0` with FC on `0x7E8` — and the node installs two filters for it.

A frame then reaches exactly **one** of them. bxCAN reports a single filter-match index per frame and the driver invokes that one callback (`drivers/can/can_stm32_bxcan.c:129-143`); M_CAN walks the filter list in order and stops at the first match. Either way the bind, installed at boot, occupies the lower slot and wins. Every FC frame is handed to the receive context, which has no case for an FC PCI and discards it (`isotp.c:456`), while the sender waits out its timeout for flow control that arrived and was thrown away.

**What that looks like on a bench**, since the symptom is misleading: heartbeats work perfectly and nothing segmented completes. Single frames never involve flow control, so the link appears healthy — `peer node 2 is online` stays true — while the peer logs `Got unexpected frame. Ignore` followed by `Reception of next FC has timed out`, and the gateway logs `Timeout while waiting for CF` and `isotp_recv: -3`. Reading only one console makes it look like a wiring fault, which it is not.

**No emulated controller can catch a colliding map.** `zephyr,can-loopback` walks its whole filter table and invokes **every** filter a frame matches, with no break (`drivers/can/can_loopback.c:93-100`). All-match, where real silicon is first-match — so `tests/isotp_loopback/` passes either way and cannot distinguish them. The property is asserted on the constants alone in `tests/heartbeat/`'s `test_no_node_listens_to_one_identifier_twice`, which needs no controller and therefore cannot inherit a driver's semantics. [`test-strategy.md`](test-strategy.md) has the general form of that argument.

**The cost.** Four identifiers spaced four apart leave room for four peers (node ids 2–5, `kMaxPeerNodeId`) rather than eight. Nothing else: no filters beyond the ones ISO-TP already installs, no extra bus traffic, and no change to the framing.

## How it fits together

**1. Devicetree — mostly inherited.** Unlike the SCD-40, there is **no node to add**. The board's own `nucleo_h753zi.dts` already does the work:

```dts
&fdcan1 {
	clocks = <&rcc STM32_CLOCK(APB1_2, 8)>,
		 <&rcc STM32_SRC_PLL2_Q FDCAN_SEL(2)>;
	pinctrl-0 = <&fdcan1_rx_pd0 &fdcan1_tx_pd1>;
	pinctrl-names = "default";
	status = "okay";
};
```

- `status = "okay"` already, on **PD0 (RX) / PD1 (TX)** — and the same dts sets `chosen { zephyr,canbus = &fdcan1; }`, so both `DT_CHOSEN(zephyr_canbus)` and `DT_NODELABEL(fdcan1)` resolve without an overlay. `nucleo_h753zi.yaml` lists `can` among the board's supported features, so this is a first-class, upstream-tested configuration rather than something coaxed into working.
- The controller is clocked from **PLL2_Q at 80 MHz** — the board dts sets `div-q = <3>` with the comment *"gives 80MHz to the FDCAN"*. `can show` reports that number back, which is the cheapest confirmation that the clock tree is what the dts claims.
- The board doc's pin table states `CAN/CANFD : PD0, PD1`. Read the physical header position off **UM2407** before soldering; the Zephyr board files name the pins, not the connector.

**2. Devicetree — the one thing we must add.** `gateway/boards/nucleo_h753zi.overlay`:

```dts
&fdcan1 {
	bitrate = <500000>;
};
```

This is the trap worth understanding, because everything above it looks correct when you get it wrong. The board dts sets no `bitrate` property, so without this stanza the controller silently takes `CONFIG_CAN_DEFAULT_BITRATE`, which is **125000** (`drivers/can/Kconfig:26-28`). The F072RB peer node runs at 500 kbit/s, and a bitrate mismatch is a completely dead bus in which the pinmux, the driver, the filters and the application are all fine.

The rate lives in the overlay rather than `prj.conf` because it describes **the wire both nodes share**, not this app; the F072RB's overlay states the same number for the same reason. Verify it landed rather than assuming — see *Bring-up checks* step 1, and note that `can show` will **not** tell you.

**3. Kconfig — `gateway/prj.conf`.** The CAN share of it:

```
CONFIG_CAN=y            # the CAN subsystem and controller API
CONFIG_CAN_SHELL=y      # `can` console commands — see Bring-up checks
```

Note what's *absent*, exactly as with the sensor driver: we never set `CONFIG_CAN_MCAN` or `CONFIG_CAN_STM32H7_FDCAN`. Both are `default y` gated on the devicetree, so **the board's existing `fdcan1` node auto-selects the driver**. Verified in the build: `CONFIG_CAN_MCAN=y` and `CONFIG_CAN_STM32H7_FDCAN=y` appear in `build/zephyr/.config` without us asking. Same devicetree→Kconfig bridge described in [`build-system-overview.md`](build-system-overview.md).

**4. The peer's devicetree — everything must be added.** `peer-node/boards/nucleo_f072rb.overlay` is the opposite situation to the gateway's. `can1` arrives from `stm32f072.dtsi` as `status = "disabled"` with no pinctrl, `can` is absent from `nucleo_f072rb.yaml`'s supported list, and nothing sets `chosen { zephyr,canbus }`. So the overlay supplies all four: pinctrl, bitrate, status and the chosen node.

```dts
&can1 {
	pinctrl-0 = <&can_rx_pa11 &can_tx_pa12>;
	pinctrl-names = "default";
	bitrate = <500000>;
	status = "okay";
};
```

- **PA11 (RX) / PA12 (TX)**, on ST morpho CN10-14 and CN10-12. The pinctrl labels are `can_*`, **not** `can1_*` — they follow the peripheral's name in the SoC dtsi, which on this part is `can`, unlike the H7's `fdcan1_*`. Verified present in `modules/hal/stm32/dts/st/f0/stm32f072r(8-b)tx-pinctrl.dtsi`.
- The `can_rx_pb8` / `can_tx_pb9` remap exists and is **not** usable here: PB8/PB9 are `i2c1`'s pins on this board, which is where the BME280 lives. The choice of CAN pins is really a choice about the sensor bus.
- **PA11/PA12 are also USB_DM/USB_DP.** That is not a conflict to resolve but one that cannot arise: on STM32F0 the USB and CAN peripherals share the same 1 KB packet-buffer SRAM (RM0091), so they are mutually exclusive in hardware. This node uses no USB.
- The bitrate is stated here for the same reason as on the gateway, and the two numbers must match. Verify it landed the same way — see *Bring-up checks* step 1, substituting `can_40006400` for the H7's `can_4000a000`.

**5. The peer's Kconfig — `peer-node/prj.conf`.** `CONFIG_CAN=y` plus `CONFIG_ISOTP=y`, and **no shell in the default image**, so its console is output-only: log lines out, nothing in. `CONFIG_ISOTP_USE_TX_BUF` stays off, so `isotp_send()` is called with a null completion callback and blocks until the transfer finishes — see *Driver behaviour worth knowing*.

The shell is a **build variant**, not a permanent absence. `peer-node/debug.conf` layers a trimmed one on top:

```sh
./scripts/build.sh -a peer-node --debug -p       # -DEXTRA_CONF_FILE=debug.conf
```

The measurements that put it there rather than in `prj.conf`, all pristine builds, RAM out of 16 KB:

| Configuration | RAM | Flash |
|---|---|---|
| no shell (`prj.conf` alone) | 10 696 B — 65 % | 67 384 B |
| shell core, stock settings | 15 632 B — 95 % | 97 008 B |
| + `CONFIG_CAN_SHELL`, stock | 16 092 B — 98 % | 106 268 B |
| + `CONFIG_SENSOR_SHELL`, stock | **overflows by 5856 B** | — |
| `debug.conf` (trimmed, with `can`) | 13 532 B — 83 % | 98 680 B |

A stock Zephyr shell takes 95 % of this part's RAM before a single command set is added, which is why the trimming in `debug.conf` — no history, no tab completion, no VT100, 1 KB stack — is what makes it usable rather than a tidy-up. `CONFIG_SENSOR_SHELL` cannot be had at all: it selects `SENSOR_ASYNC_API`, which pulls in RTIO and its pools. Read the BME280 through the telemetry stream instead, which is the path that has to work anyway.

Worth building when a real bus is being brought up for the first time: `can show` on *each* node reports that node's own state and error counters, so "which end is unhappy" stops being a guess. Toggling `--debug` changes Kconfig, so always pair it with `-p`.

**6. Console.** The gateway is `zephyr,console = &usart3`, the peer is `&usart2`, both on their ST-LINK VCP at **115200 8N1**, both opened with `scripts/console.sh -a <app>`. With both Nucleos attached the glob matches two ports and the script refuses to guess — `-a` is how you say which, and `scripts/probe.sh` prints the table it uses. Expect very different things at the far end: the gateway prompts `uart:~$` and has the `can` and `net` commands, while the peer logs at boot and is then silent, because it has no shell. A quiet console on the peer is not a hung peer.

## Driver behaviour worth knowing

- **The shell addresses the controller as `can@4000a000`, not `fdcan1`.** Device names come from the devicetree node's full name, not its label. Tab completion after `can mode ` enumerates them, which is faster than remembering the address.
- **Controllers come up stopped.** Nothing transmits or receives until `can_start()`. The failure mode is the nasty kind: every log line looks healthy and no frame moves. Mode changes also require the stopped state — `can_set_mode()` returns `-EBUSY` otherwise — so the order is always stop → mode → start.
- **Filters are not deduplicated, and matching stops at the first match.** `can_mcan_add_rx_filter_std()` (`drivers/can/can_mcan.c:1094`) allocates the lowest free slot and registers a callback; adding the same ID three times yields three slots and three callbacks. But the M_CAN hardware walks the standard filter list in order and stops at the first match, so **only one callback fires** — observed on the bench in loopback: three identical filters for `0x702`, one printed frame. bxCAN does the same by a different route: it reports one filter-match index per frame and the driver calls that single callback (`drivers/can/can_stm32_bxcan.c:129-143`). `include/zephyr/drivers/can.h:1335` states the general rule: where filters overlap, match priority is hardware dependent. **This is not only a debugging curiosity** — it is what forces the address map's shape, since two ISO-TP contexts sharing an identifier means two filters and the loser is starved silently. See *Flow control needs its own identifiers*.
- **Consequently `can dump` can show nothing while frames are arriving normally.** `cmd_can_dump()` installs a catch-all (`id = 0, mask = 0`, `drivers/can/can_shell.c:481`) through the same lowest-free-slot allocator. Any specific filter installed earlier — by application code at boot, say — occupies a lower slot and wins every match, leaving the shell's catch-all permanently silent. Use `can filter add <dev> <id>` for a known ID instead, and do not read an empty `can dump` as a dead bus.
- **`can dump` takes the console over entirely.** It ends by installing a shell bypass (`shell_set_bypass()`, `drivers/can/can_shell.c:526`) whose callback recognises exactly one byte, `0x03` (`:447`). While a dump runs there is no prompt and no parser, so no other command — `can send` included — can be issued from that console. Ctrl+C restores the shell and removes the dump's own two filters, and leaves the controller running if the dump did not start it (`cmd_can_dump()` remembers whether its `can_start()` returned `-EALREADY`). Consequence for loopback work: the shell cannot both dump and generate frames, so the frame source has to be a peer.
- **`can_send()` with a NULL callback blocks, and the timeout argument does not bound it.** `z_impl_can_send()` (`drivers/can/can_common.c:57-71`) installs its own callback and does `k_sem_take(&ctx.done, K_FOREVER)` when you pass none, so the call returns only once the frame is actually transmitted. The `k_timeout_t` you pass bounds something else entirely — the wait for a free transmit mailbox (`can_stm32_bxcan.c:800-808`) — so `K_NO_WAIT` does **not** make the call non-blocking, which is the natural misreading and the one this project made. Combined with the two bullets below, that means a lone transmitter blocks for as long as the link is down. Pass a real completion callback whenever the caller must stay responsive; `peer-node/src/main.cpp`'s heartbeat is the worked example, and the symptom of getting it wrong is a thread that goes silent for an entire outage and resumes without ever noticing one happened.
- **Automatic retransmission is on, so an unacknowledged frame is retried forever.** The bxCAN driver clears `NART` at init (`can_stm32_bxcan.c:658`), so the hardware re-arbitrates a failed frame indefinitely and `RQCP` — the flag the TX interrupt and therefore the completion callback wait on — is never set. Single-shot exists (`CAN_MODE_ONE_SHOT` sets `NART`, `:518`) but is per-controller, so it would also strip ISO-TP of the retransmission its reliability rests on. Not a good trade for this link.
- **Automatic bus-off recovery is on, so bus-off does not end the retrying either.** `ABOM` is set at init (`:664`), so the controller recovers on its own after 128×11 recessive bits and the aborted transmission can be attempted again. Together with the bullet above there is no state a lone transmitter reaches where the hardware gives up — which is correct behaviour for CAN, and exactly why a blocking `can_send()` has no bound.
- **`transceiver: passive/none` is permanent here, and is not a presence indicator.** Zephyr reports a transceiver only when a `can-transceiver` node exists to control. The SN65HVD230 breakouts are always-on parts with no standby pin wired, so there is nothing to describe and nothing to drive. That line will read the same with the transceivers attached and working.
- **Bus-off recovery is automatic.** `CONFIG_CAN_MANUAL_RECOVERY_MODE` is off, so the controller recovers on its own and the shell has no `can recover` command — visible in the `can` help output, where `recover` is a conditional command that simply is not there.
- **No CAN-FD.** `can show` reports capabilities `normal loopback listen-only` with no `fd`, because `CONFIG_CAN_FD_MODE` is unset. Deliberate — see *What is deliberately not here*.

## Build & flash

```sh
scripts/build.sh -p                       # the gateway; pristine, required after devicetree/Kconfig edits
scripts/build.sh -a peer-node -p        # the peer node
scripts/flash.sh -a peer-node           # forces -r openocd; both boards default to the uninstalled cube runner
scripts/console.sh -a peer-node         # 115200; quit with Ctrl-A then K
scripts/probe.sh                          # which ST-LINK and which port is which app
```

Every wrapper takes `-a <app>` (or `APP=`) and defaults to `gateway`; the board follows from the app, so `-b` is never needed. **Use `-a` whenever both boards are attached**, which from here on is the normal case: `flash.sh` resolves the app's ST-LINK through `scripts/probes.conf` and passes `--serial`, and refuses rather than flashing at random if it cannot. That refusal is the point — an openocd that picks the wrong probe writes the wrong image to the wrong part and reports success, and on two boards running the same two-node experiment that is a genuinely confusing hour. `STLINK_SERIAL=` still overrides.

Enabling CAN on the **gateway** cost **+16 392 B flash** (219 460 → 235 852) and **+908 B RAM** (52 000 → 52 908), on a part with 2 MB and 512 KB. `drivers/can` is 10 804 B of that, split `can_mcan.c` 4392, **`can_shell.c` 4356**, `can_common.c` 1098, with the STM32H7 glue making up the rest — so roughly 40 % of the CAN footprint is a debugging aid.

The **peer node** is the interesting budget, because it is the constrained one: the whole application — CAN, ISO-TP, nanopb, zbus, the BME280 driver, two threads and the shared command handling — is **67 496 B of 128 KB flash (51 %)** and **10 696 B of 16 KB RAM (65 %)**. Comfortable, and it is the absent shell that makes it so. The largest single line item that is *not* the application is deferred logging, at +8748 B flash and +1640 B RAM over `CONFIG_LOG_MODE_MINIMAL` — bought deliberately, because on a node with no shell the console is the only diagnostic there is and minimal mode interleaves concurrent messages within a line. `peer-node/prj.conf` has the working. Read these off `scripts/build.sh -a peer-node -t rom_report` rather than trusting the numbers here; they move with every Kconfig change.

## Bring-up checks

No wiring is needed for steps 1–5 — internal loopback proves the controller without a transceiver, and stays the way to answer "is it my firmware or my wiring?" once there is wiring to doubt. Step 6 is the first that needs the bus built.

**Before any of it, run `./scripts/test.sh`.** `tests/isotp_loopback/` applies the same technique with no board at all: a devicetree overlay gives `qemu_cortex_m3` an emulated `zephyr,can-loopback` controller, and the suite drives the address map, the heartbeat frame and a full 161-byte ISO-TP transfer through it. If the framing is wrong, that says so in thirty seconds without a board attached, and the checks below can then be read as being about *this* hardware rather than about the code. See [`test-strategy.md`](test-strategy.md) for what it covers and what it cannot.

**1. The bitrate actually applied, on both boards.** `can show` deliberately does *not* print the configured bitrate — it prints the controller's *maximum*, which on the H7 is 1 Mbit/s and will look reassuring while the bus is misconfigured. And the peer node has no shell at all. Check the generated devicetree instead, which is the one place the answer is unambiguous for both:

```sh
grep P_bitrate gateway/build/zephyr/include/generated/zephyr/devicetree_generated.h
grep P_bitrate peer-node/build/zephyr/include/generated/zephyr/devicetree_generated.h
```

Expect `DT_N_S_soc_S_can_4000a000_P_bitrate 500000` from the first and `DT_N_S_soc_S_can_40006400_P_bitrate 500000` from the second — different addresses, **the same number**, which is the thing being checked. A `125000`, or no line at all, means the overlay did not apply; rebuild with `-p`, since devicetree changes need a pristine build.

While you are in the peer's generated header, `grep DT_CHOSEN_zephyr_canbus` there should resolve to `DT_N_S_soc_S_can_40006400`. Nothing on that board sets it but the overlay, and `main.cpp` gets its device from it rather than from a node label.

**2. The controller is present and sanely clocked.**

```
uart:~$ can show can@4000a000
```

Expect `core clock: 80000000 Hz`, `max bitrate: 1000000 bps`, `max std filters: 28`, `max ext filters: 8`, `capabilities: normal loopback listen-only`, and `state: stopped`. The core clock is the load-bearing line: 80 MHz confirms the PLL2_Q path the board dts describes.

**3. A frame goes out and comes back.**

```
uart:~$ can mode can@4000a000 loopback
uart:~$ can start can@4000a000
uart:~$ can filter add can@4000a000 0x702
uart:~$ can send can@4000a000 0x702 01 02 03 04 05 06 07 08
```

Expect `filter ID: 0` from the third command, then the fourth to report the frame enqueued and sent, followed by the frame arriving back:

```
can@4000a000       702   [8]  01 02 03 04 05 06 07 08
```

Note the subcommand is `can filter add`, not `can add`; a bare `can add` prints the whole help text instead of erroring usefully. If `can mode` returns `-EBUSY`, the controller is already started — `can stop` first.

**Proves:** bit-timing configuration accepted at 500 kbit/s, the clock tree, the pinctrl and driver binding, filter installation, and the transmit and receive paths — everything except the physical layer. This is the bisect line: with loopback passing and the real bus silent, the fault is wiring, termination or grounding, not firmware.

**4. The rest of the gateway app is undisturbed.** Telemetry should keep publishing throughout — `<inf> node: published telemetry seq=N` every ~5 s on the console, and readings still arriving on the Pi. CAN shares no code with the network path, and this check exists to notice if that ever stops being true.

**5. The peer node boots and finds its sensor.** Run this with the peer **alone** — the other board unpowered or the CANH/CANL pair unplugged — because that is what makes the link errors below predictable rather than confusing. Flash it and open its console, then **press the black RESET button (B2)**, because `screen` cannot be attached before the board starts and the interesting lines are all at boot.

```
*** Booting Zephyr OS build v4.4.1 ***
[00:00:00.009,000] <inf> node: node 2 on CAN: heartbeat 0x702, isotp in 0x7e0 (fc out 0x7e4), out 0x7e8 (fc in 0x7ec)
[00:00:00.009,000] <inf> node_sensor: BME280 online
[00:00:00.010,000] <wrn> node: gateway unreachable (heartbeat failed: -5) — backing off, still sampling
[00:00:01.009,000] <inf> node: sensor readings OK (seq 0)
```

Then silence.

The timestamps are worth a second look, because two of them confirm design decisions rather than just marking time. Everything to do with bring-up lands inside **10 ms** — the BME280 needs no power-up window, unlike the SCD-40 on the gateway, which is why this node has no `zephyr,deferred-init` and no wait. And the first reading is reported at **1.009 s**, not at 9 ms: the sensor thread published it almost immediately, but `main` was parked in `isotp_recv()` until the heartbeat came due. That one-second gap *is* the up-to-one-beat telemetry latency `src/main.cpp` documents, visible on the console. The node beats once a second and publishes every five, and none of that is logged once the states above stop changing — every remaining message is emitted on a *transition*. There is no shell here to ask it anything, which is the trade `peer-node/prj.conf` documents.

Line by line, because each one is a check:

- **The identifiers are read back out of the running firmware.** All five come from `can_link.h` via `heartbeat_id()` and friends rather than from literals, so seeing `0x702` and the two `in`/`out` pairs is confirmation that this board computed the same map the gateway did. The gateway logs the same four with `in` and `out` swapped, and reading the two consoles side by side is the cheapest check that the ends agree.
- **`BME280 online`** means `device_is_ready()` succeeded, so the driver bound and the chip answered its ID register. `BME280 not ready` instead means it did not answer at `0x77` — try `0x76`, which most non-Adafruit breakouts use.
- **`gateway unreachable` is the expected result for a lone node**, not a fault: with nothing else on the bus there is nothing to acknowledge a frame, so the transmit error counter climbs and the controller reports first `-EIO` and then, once it passes 255, `-ENETUNREACH` (bus-off). Its *absence* is what step 6 checks. See the error table below.
- **`sensor readings OK (seq 0)`** is the one that says the telemetry has real content in it. Without it — or with `sensor reads are failing` in its place — the encoded `Telemetry` is about **11 bytes** rather than **25**, because every measurement is absent rather than zero. That length difference is worth memorising: it is the cheapest way to spot a dead sensor in an ISO-TP log line.

**Proves:** the I²C path, the driver binding, the CAN device resolving through `chosen`, `isotp_bind()` finding a free filter, and the sensor→zbus→encode path end to end. Not proven, and not provable from one board: that any of those frames leave the pin.

**What the CAN errors mean with no peer attached**, which is what step 5's console shows and what any single-board run will show:

| Code | Name | Meaning |
|---|---|---|
| `-5` | `-EIO` | a transmit failed; error counters climbing, still error-active |
| `-114` | `-ENETUNREACH` | `CAN_ESR_BOFF` is set — the controller is **bus-off** (`can_stm32_bxcan.c:794`) |
| `-2` | `ISOTP_N_TIMEOUT_BS` | the first frame went out; no flow control came back inside `CONFIG_ISOTP_BS_TIMEOUT` (1000 ms) |

All three are the predicted behaviour of a node alone on a bus, not defects: nothing drives the ACK slot, so every frame is unacknowledged, TEC passes 255, and the controller takes itself off the bus. Recovery is automatic, so it recovers and repeats. [`can-guide.md`](../notes/can-guide.md) §5 and §6 are the mechanism. The firmware treats it as a link state rather than an error — one line on the transition, then quiet, with telemetry backing off 2 s → 30 s while the heartbeat keeps trying.

**6. Two nodes on a real bus, and the heartbeat crosses it.** The first check that needs the wire built. Two SN65HVD230 breakouts, one per board:

| Transceiver pin | Gateway (H753ZI) | Peer (F072RB) |
|---|---|---|
| `3V3` / `VCC` | 3.3 V | 3.3 V |
| `GND` | GND | GND |
| `D` / `CTX` (driver in) | **PD1** (CAN TX) | **PA12** (CAN TX) |
| `R` / `CRX` (receiver out) | **PD0** (CAN RX) | **PA11** (CAN RX) |

No crossover: the module's TX pin takes the MCU's TX, because the labels are already written from the MCU's point of view. Between the two modules, three wires — `CANH`↔`CANH`, `CANL`↔`CANL`, `GND`↔`GND`. The ground link is not optional; two boards on separate USB supplies have no common-mode reference without it. Termination is **120 Ω across CANH/CANL at each of the two ends**, which on a two-node bus means one per module — most SN65HVD230 breakouts carry it onboard, so check for the resistor or its jumper rather than adding a third. If the module exposes `R_S`, it must be at GND (directly or through ~10 kΩ); pulled to 3.3 V it is in standby and deaf.

Then flash both, open both consoles, and press **RESET (B2)** on the peer:

```sh
./scripts/flash.sh -a gateway
./scripts/flash.sh -a peer-node
./scripts/console.sh -a gateway      # one terminal
./scripts/console.sh -a peer-node    # another
```

On the gateway, from `relay.cpp`:

```
[00:00:01.652,000] <inf> relay: relay up: peer 2, heartbeat 0x702, isotp in 0x7e8 (fc out 0x7ec), out 0x7e0 (fc in 0x7e4)
[00:00:01.902,000] <inf> relay: peer node 2 is online
```

On the peer, step 5's boot lines **without** the `gateway unreachable` warning.

**Proves the entire physical layer at once**, and this is why it is the check worth running before any other two-node work: a node alone on a CAN bus cannot complete a transmission at all, because nothing drives the acknowledgement slot and the transmit error counter climbs to bus-off. So a heartbeat that *succeeds* — reported by silence on the peer and by `peer node 2 is online` on the gateway 250 ms later — can only mean the differential pair, the common ground, the termination and the bitrate are all correct together. There is no partial credit and nothing to interpret.

**Not proven:** anything segmented. The heartbeat is one raw 8-byte frame and never touches ISO-TP.

**7. The segmented path, both directions, end to end.** Everything above proves frames move. This proves the *transport* does, and it is the last check that needs hardware rather than a test suite.

Run the harness **on the Pi**, which is the only machine that can reach the broker. Regenerate the Python bindings first — they are gitignored, so a Pi that has not run this since the last schema change is silently stale:

```sh
./host/generate.sh
host/.venv/bin/python host/monitor.py
```

Worth knowing what stale looks like, because the message does not point at the cause: `DECODE FAILED (Field node.Telemetry.co2_ppm does not have presence.)` on **every** topic, including `node/1`. That is `HasField()` in `monitor.py` refusing a field that the *installed* `node_pb2.py` still declares as a bare proto3 scalar — a host that predates the `optional` change, not a firmware fault. The raw bytes in that error line are the way to check: decode them by hand and the payload will be perfectly well formed.

With current bindings, both nodes on one screen:

```
node/2/telemetry  seq=0  co2=   -- ppm  temp=23.66 C  rh=54.2 %  p=101108 Pa  up=  0.0s  SENSOR_STATUS_OK
node/1/telemetry  seq=0  co2=   -- ppm  temp=   -- C  rh=  -- %  p=    -- Pa  up=  2.3s  SENSOR_STATUS_WARMING_UP
node/1/telemetry  seq=1  co2= 1030 ppm  temp=28.39 C  rh=44.2 %  p=    -- Pa  up=  7.3s  SENSOR_STATUS_OK
node/2/telemetry  seq=1  co2=   -- ppm  temp=23.65 C  rh=54.1 %  p=101106 Pa  up=  5.1s  SENSOR_STATUS_OK
```

**Read the `--` columns, not the numbers.** They are the check. Node 1 has an SCD-40 and no barometer; node 2 has a BME280 and no CO₂ sensor; each reports the measurements it cannot take as *absent* rather than as a confident zero. Node 1's `seq=0` shows the same thing for a different reason — `WARMING_UP`, so every measurement is absent and the payload is about 7 bytes rather than 22. Two sensors with different capabilities, one schema, and a gateway that decoded neither payload. This is the exercise `proto/node.proto`'s `optional` measurements exist for, and it cannot be demonstrated with one node.

Then the downward direction. `info` is the right command, and `trigger` is not, because **the gateway synthesizes an Ack when the peer does not answer** — so a reply on `node/2/ack` proves nothing on its own:

```sh
host/.venv/bin/python host/command.py --node 1 info   # control: no CAN involved
host/.venv/bin/python host/command.py --node 2 info   # the actual test
```

```
-> node/2/command  seq=227368785 info (9 bytes)
<- node/2/ack  seq=227368785 ACK_STATUS_OK
   firmware: 0.5.0
   board:    nucleo_f072rb
   clientid: nucleo-2
```

| Reply | Meaning |
|---|---|
| `ACK_STATUS_OK` with `board: nucleo_f072rb` | **Genuine round trip.** That string is `CONFIG_BOARD` compiled into the F072RB image; it exists nowhere in the gateway, so it can only have crossed the bus. |
| `ACK_STATUS_FAILED`, `no response over CAN` | The command reached the peer's identifier but nothing came back — suspect the `0x7E4` flow control. |
| `ACK_STATUS_FAILED`, `no route to node over CAN` | `isotp_send()` failed outright; the transfer never left the gateway. |

**Proves:** ISO-TP segmentation and reassembly in both directions over real silicon with separate flow-control identifiers; the relay's two threads and their RX/TX rendezvous; the liveness clock driven by a peer's actual cadence; both MQTT sessions publishing concurrently; and the gateway forwarding two different nodes' payloads without decoding either. Between them, every part of the relay path that a host test cannot reach.

**8. The peer goes away, and comes back.** Tests the *other* mechanism that writes `node/2/status offline` — the gateway's own liveness timeout, which is firmware-published and has nothing to do with the Last Will (see [`mqtt-design.md`](mqtt-design.md) for that distinction, and do not confuse the two).

With everything running, unplug CANH/CANL between the transceivers and watch the Pi:

```
18:23:19  node/2/telemetry  seq=10  ... up=  50.1s
18:23:30  node/2/status     offline
   ... node/1 continues undisturbed throughout ...
18:25:32  node/2/status     online
18:25:32  node/2/telemetry  seq=36  ... up= 180.2s
```

**Proves:** the relay's liveness clock times the peer out after `kHeartbeatTimeoutMs` (3.5 s) and publishes `offline` from firmware; the gateway's own `node/1` path is unaffected by a dead peer, which is the isolation the two-session design is for; and the link recovers with no reset when the bus returns.

**Check the sequence numbers, not just the topics.** `seq` jumped 10 → 36 across a 130.1 s outage: 26 readings at the 5 s cadence, which is exactly 130.1 / 5. The peer's sensor thread keeps sampling into the zbus channel while the link is down and latest-wins discards all but the newest, so the *gap* is the count of what the outage cost. A gap that does not match the outage means the sensor thread stopped too, which is a different fault entirely.

**A silent peer here means a blocked one.** The peer must log the transition within about two seconds. If it says nothing for the whole outage and then resumes without a word, `main` is parked inside `can_send()` rather than looping — see the `can_send()` bullet in *Driver behaviour worth knowing*, which is the trap that produces exactly that signature. The sequence gap tells you the same thing from the host side: a peer that was blocked still shows the full gap, because its sensor thread kept publishing into a channel nobody was draining.

**Two different lines are correct here, and which one you get is timing.** `gateway unreachable (heartbeat failed: -16)` is `-EBUSY` — the previous beat never completed, which is the 1 Hz detector doing its job. `gateway unreachable (telemetry failed: -2)` is `ISOTP_N_TIMEOUT_BS` instead, and appears when the unplug lands while a telemetry transfer is in flight: a failing `isotp_send()` blocks for the full `CONFIG_ISOTP_BS_TIMEOUT` (1 s) and the heartbeat cannot run inside that window, so telemetry gets there first. `note_link()` logs only the transition, so whichever detector notices first is the only one you see. Both are the link going down; neither is a fault. Expect `gateway reachable again` on replug, and possibly one `isotp_recv failed: -3` beside it — a transfer that was physically cut mid-reassembly, timing out.

**Commands sent during the outage are refused, not queued.** The host will see `ACK_STATUS_FAILED` with detail `no route to node over CAN` within about a second, and nothing is delivered when the bus returns. That is deliberate: the relay has no store-and-forward, because a command delivered a minute late is frequently worse than one never delivered — a `trigger_measurement` is meaningless by then, and a `set_interval` applied after the operator has moved on is actively wrong. The host holds the intent, so the host owns the retry, and `Command.sequence` plus the node's duplicate suppression is what makes retrying safe. See the QoS discussion in [`mqtt-design.md`](mqtt-design.md).

**Not checked here, but checked elsewhere:** the Last Will on `node/2/status` when the *gateway* dies is the complementary failure and lives in [`mqtt-design.md`](mqtt-design.md) — it is a broker behaviour, not a CAN one, and confusing it with step 8 below is easy. **Still undone anywhere:** duplicate suppression against the peer specifically, and a loopback check of the peer's own ISO-TP path, which remains possible.

## What is deliberately not here

- **No CAN-FD.** The peer node is an STM32F072 with **bxCAN**, which is classic CAN 2.0B only, so the link is classic at 8 bytes per frame regardless of what the H7's FDCAN could do. This is a feature rather than a concession: the 8-byte limit is what forces real segmentation, which is one of the things this link exists to exercise. `CONFIG_CAN_FD_MODE` stays unset, and `can show` correctly omits `fd`.
- **No transceiver node.** Nothing to control — see *Driver behaviour worth knowing*.
- **No third node.** The address helpers in `can_link.h` all take a node id and `main.cpp`'s sessions are an array, so a third is a row and a pair of ISO-TP contexts rather than a redesign — but nothing has been built or sized for one. `CONFIG_ZVFS_POLL_MAX` and `CONFIG_ZVFS_EVENTFD_MAX` are both set to exactly what two nodes need.
- **No flow-control throttling.** The peer advertises `bs = 0, stmin = 0` — send the whole transfer, no minimum gap — because the only thing that reaches it is a 20-byte `Command`, which is three frames. Block size exists so a slow receiver can throttle a fast sender mid-transfer; throttling three frames costs a round trip per block and buys nothing. A node receiving a firmware image would answer differently.
- **No ISO-TP TX buffering.** `CONFIG_ISOTP_USE_TX_BUF` and `CONFIG_ISOTP_ENABLE_CONTEXT_BUFFERS` are both off. `isotp_send()` with a null completion callback blocks until the transfer completes, so the caller's own buffer stays valid throughout and there is nothing for the library to copy into — which is also what makes the peer's single-writer rule enforceable without a mutex.
- **No second thread on the peer.** `main()` does the receiving, the sending and the heartbeat. Two `isotp_send()` calls on one address from different contexts interleave their frames and corrupt both transfers, so single-writer is a correctness property rather than a simplification. The visible cost is that a reading can wait up to one heartbeat period before it is transmitted.
- **No manual bus-off recovery.** Automatic recovery is the default and is what a two-node bench bus wants; `CONFIG_CAN_MANUAL_RECOVERY_MODE` would only be interesting if the app needed to observe or delay recovery.
- **CAN is not a network interface.** Zephyr can run 6LoCAN and expose CAN through the networking stack; this project deliberately treats CAN as a raw frame transport with ISO-TP above it, because the point is the framing problem MQTT hides, not another IP link.
