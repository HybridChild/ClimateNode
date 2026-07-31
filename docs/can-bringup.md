# CAN bring-up — project reference

How the CAN link between the **Nucleo-H753ZI** gateway and the **Nucleo-F072RB** peer node is configured, clocked and verified on this bench. Terse by intent: decisions, rationale, and the facts you need when the link misbehaves. For the concepts underneath — what a differential multi-drop bus is, how arbitration makes a lower ID win, why acceptance filters exist, and what 8 bytes per frame does to a protocol design — see the companion teaching guide, [`can-guide.md`](../notes/can-guide.md).

**No frame has crossed a real bus yet.** The transceivers are not bought, so everything below the transceiver is unproven and the *Bring-up checks* here run in internal loopback on one board. What exists above the frame is the F072RB node's firmware — heartbeat, ISO-TP send and receive, the shared command handling — which builds and links but has never been run against a peer. The gateway's `relay.cpp` does not exist at all.

Builds against the shared global Zephyr workspace — see [`toolchain.md`](toolchain.md).

## The CAN path

```
firmware/boards/nucleo_h753zi.overlay   gateway: the bitrate the board dts leaves unset
firmware/prj.conf                       gateway: CONFIG_CAN + the CAN shell
firmware/src/can_link.h                 SHARED: address map, heartbeat frame, message type
sensor-node/boards/nucleo_f072rb.overlay  peer: can1 enabled on PA11/PA12, the same bitrate
sensor-node/prj.conf                    peer: CONFIG_CAN + CONFIG_ISOTP, no shell
sensor-node/src/main.cpp                peer: the CAN session
```

No application code on the *gateway* talks to the controller yet, which is deliberate: the CAN shell can drive the link on its own, so the peripheral gets proven before anything is layered on it. Debugging a silent bus and debugging a segmentation bug at the same time is the failure mode that ordering avoids.

`can_link.h` is the one file both boards include, and the reason it is a file rather than a comment: a link is symmetric, and neither end is in a position to be right on its own. It carries the address map, the hand-packed heartbeat layout and the one byte of message type above ISO-TP — nothing else, and nothing either side owns alone. `tests/heartbeat/` compiles it with no subsystem at all, which is the evidence that the contract has no dependencies.

### The address map

Three 11-bit identifiers per peer, `id` being the node id and `n = id - 2`:

| Identifier | Direction | Carries |
|---|---|---|
| `0x700 + id` | peer → gateway | heartbeat, one raw frame at 1 Hz |
| `0x7E0 + n` | gateway → peer | commands, ISO-TP |
| `0x7E8 + n` | peer → gateway | telemetry and acks, ISO-TP |

For node 2 that is `0x702`, `0x7E0`, `0x7E8`. The ranges are borrowed rather than invented — `0x700 + id` is CANopen's heartbeat convention and `0x7E0`/`0x7E8` is the UDS request/response pair — so a trace reads familiarly to anyone who has met a CAN bus, at no cost. Neither protocol is implemented.

The ordering is load-bearing, though, and not decorative: a lower identifier wins arbitration, so heartbeats outrank every ISO-TP frame and liveness cannot be starved by a long segmented transfer. `tests/heartbeat/` asserts that relationship rather than trusting it to survive an edit.

**One byte of protocol sits above ISO-TP**: `[0]` is the message type (telemetry / ack / command), `[1..]` is the protobuf payload verbatim. ISO-TP has no equivalent of an MQTT topic, and [`mqtt-design.md`](mqtt-design.md)'s rule still holds — the type is not on the wire, so something outside the payload must assert it. On MQTT the topic does that for free; here it costs a byte. The gateway will read that byte, strip it and forward the rest untouched, which is the whole of what "the gateway does not decode the peer's payload" means in practice.

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

**2. Devicetree — the one thing we must add.** `firmware/boards/nucleo_h753zi.overlay`:

```dts
&fdcan1 {
	bitrate = <500000>;
};
```

This is the trap worth understanding, because everything above it looks correct when you get it wrong. The board dts sets no `bitrate` property, so without this stanza the controller silently takes `CONFIG_CAN_DEFAULT_BITRATE`, which is **125000** (`drivers/can/Kconfig:26-28`). The F072RB peer node runs at 500 kbit/s, and a bitrate mismatch is a completely dead bus in which the pinmux, the driver, the filters and the application are all fine.

The rate lives in the overlay rather than `prj.conf` because it describes **the wire both nodes share**, not this app; the F072RB's overlay states the same number for the same reason. Verify it landed rather than assuming — see *Bring-up checks* step 1, and note that `can show` will **not** tell you.

**3. Kconfig — `firmware/prj.conf`.** The CAN share of it:

```
CONFIG_CAN=y            # the CAN subsystem and controller API
CONFIG_CAN_SHELL=y      # `can` console commands — see Bring-up checks
```

Note what's *absent*, exactly as with the sensor driver: we never set `CONFIG_CAN_MCAN` or `CONFIG_CAN_STM32H7_FDCAN`. Both are `default y` gated on the devicetree, so **the board's existing `fdcan1` node auto-selects the driver**. Verified in the build: `CONFIG_CAN_MCAN=y` and `CONFIG_CAN_STM32H7_FDCAN=y` appear in `build/zephyr/.config` without us asking. Same devicetree→Kconfig bridge described in [`build-system-overview.md`](build-system-overview.md).

**4. The peer's devicetree — everything must be added.** `sensor-node/boards/nucleo_f072rb.overlay` is the opposite situation to the gateway's. `can1` arrives from `stm32f072.dtsi` as `status = "disabled"` with no pinctrl, `can` is absent from `nucleo_f072rb.yaml`'s supported list, and nothing sets `chosen { zephyr,canbus }`. So the overlay supplies all four: pinctrl, bitrate, status and the chosen node.

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

**5. The peer's Kconfig — `sensor-node/prj.conf`.** `CONFIG_CAN=y` plus `CONFIG_ISOTP=y`, and **no shell in the default image**, so its console is output-only: log lines out, nothing in. `CONFIG_ISOTP_USE_TX_BUF` stays off, so `isotp_send()` is called with a null completion callback and blocks until the transfer finishes — see *Driver behaviour worth knowing*.

The shell is a **build variant**, not a permanent absence. `sensor-node/debug.conf` layers a trimmed one on top:

```sh
./scripts/build.sh -a sensor-node --debug -p     # -DEXTRA_CONF_FILE=debug.conf
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
- **Filters are not deduplicated, and matching stops at the first match.** `can_mcan_add_rx_filter_std()` (`drivers/can/can_mcan.c:1094`) allocates the lowest free slot and registers a callback; adding the same ID three times yields three slots and three callbacks. But the M_CAN hardware walks the standard filter list in order and stops at the first match, so **only one callback fires** — observed on the bench in loopback: three identical filters for `0x702`, one printed frame. `include/zephyr/drivers/can.h:1335` states the general rule: where filters overlap, match priority is hardware dependent.
- **Consequently `can dump` can show nothing while frames are arriving normally.** `cmd_can_dump()` installs a catch-all (`id = 0, mask = 0`, `drivers/can/can_shell.c:481`) through the same lowest-free-slot allocator. Any specific filter installed earlier — by application code at boot, say — occupies a lower slot and wins every match, leaving the shell's catch-all permanently silent. Use `can filter add <dev> <id>` for a known ID instead, and do not read an empty `can dump` as a dead bus.
- **`can dump` takes the console over entirely.** It ends by installing a shell bypass (`shell_set_bypass()`, `drivers/can/can_shell.c:526`) whose callback recognises exactly one byte, `0x03` (`:447`). While a dump runs there is no prompt and no parser, so no other command — `can send` included — can be issued from that console. Ctrl+C restores the shell and removes the dump's own two filters, and leaves the controller running if the dump did not start it (`cmd_can_dump()` remembers whether its `can_start()` returned `-EALREADY`). Consequence for loopback work: the shell cannot both dump and generate frames, so the frame source has to be a peer.
- **`transceiver: passive/none` is permanent here, and is not a presence indicator.** Zephyr reports a transceiver only when a `can-transceiver` node exists to control. The SN65HVD230 breakouts are always-on parts with no standby pin wired, so there is nothing to describe and nothing to drive. That line will read the same with the transceivers attached and working.
- **Bus-off recovery is automatic.** `CONFIG_CAN_MANUAL_RECOVERY_MODE` is off, so the controller recovers on its own and the shell has no `can recover` command — visible in the `can` help output, where `recover` is a conditional command that simply is not there.
- **No CAN-FD.** `can show` reports capabilities `normal loopback listen-only` with no `fd`, because `CONFIG_CAN_FD_MODE` is unset. Deliberate — see *What is deliberately not here*.

## Build & flash

```sh
scripts/build.sh -p                       # the gateway; pristine, required after devicetree/Kconfig edits
scripts/build.sh -a sensor-node -p        # the peer node
scripts/flash.sh -a sensor-node           # forces -r openocd; both boards default to the uninstalled cube runner
scripts/console.sh -a sensor-node         # 115200; quit with Ctrl-A then K
scripts/probe.sh                          # which ST-LINK and which port is which app
```

Every wrapper takes `-a <app>` (or `APP=`) and defaults to `firmware`; the board follows from the app, so `-b` is never needed. **Use `-a` whenever both boards are attached**, which from here on is the normal case: `flash.sh` resolves the app's ST-LINK through `scripts/probes.conf` and passes `--serial`, and refuses rather than flashing at random if it cannot. That refusal is the point — an openocd that picks the wrong probe writes the wrong image to the wrong part and reports success, and on two boards running the same two-node experiment that is a genuinely confusing hour. `STLINK_SERIAL=` still overrides.

Enabling CAN on the **gateway** cost **+16 392 B flash** (219 460 → 235 852) and **+908 B RAM** (52 000 → 52 908), on a part with 2 MB and 512 KB. `drivers/can` is 10 804 B of that, split `can_mcan.c` 4392, **`can_shell.c` 4356**, `can_common.c` 1098, with the STM32H7 glue making up the rest — so roughly 40 % of the CAN footprint is a debugging aid.

The **peer node** is the interesting budget, because it is the constrained one: the whole application — CAN, ISO-TP, nanopb, zbus, the BME280 driver, two threads and the shared command handling — is **67 384 B of 128 KB flash (51 %)** and **10 696 B of 16 KB RAM (65 %)**. Comfortable, and it is the absent shell that makes it so. The largest single line item that is *not* the application is deferred logging, at +8748 B flash and +1640 B RAM over `CONFIG_LOG_MODE_MINIMAL` — bought deliberately, because on a node with no shell the console is the only diagnostic there is and minimal mode interleaves concurrent messages within a line. `sensor-node/prj.conf` has the working. Read these off `scripts/build.sh -a sensor-node -t rom_report` rather than trusting the numbers here; they move with every Kconfig change.

## Bring-up checks

No wiring is needed for any of this — internal loopback proves the controller without a transceiver, and stays the way to answer "is it my firmware or my wiring?" later.

**1. The bitrate actually applied, on both boards.** `can show` deliberately does *not* print the configured bitrate — it prints the controller's *maximum*, which on the H7 is 1 Mbit/s and will look reassuring while the bus is misconfigured. And the peer node has no shell at all. Check the generated devicetree instead, which is the one place the answer is unambiguous for both:

```sh
grep P_bitrate firmware/build/zephyr/include/generated/zephyr/devicetree_generated.h
grep P_bitrate sensor-node/build/zephyr/include/generated/zephyr/devicetree_generated.h
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

**5. The peer node boots and finds its sensor.** Flash it and open its console — then **press the black RESET button (B2)**, because `screen` cannot be attached before the board starts and the interesting lines are all at boot.

```
*** Booting Zephyr OS build v4.4.1 ***
[00:00:00.009,000] <inf> node: node 2 on CAN: heartbeat 0x702, isotp rx 0x7e0 tx 0x7e8
[00:00:00.009,000] <inf> node_sensor: BME280 online
[00:00:00.010,000] <wrn> node: gateway unreachable (heartbeat failed: -5) — backing off, still sampling
[00:00:01.009,000] <inf> node: sensor readings OK (seq 0)
```

Then silence.

The timestamps are worth a second look, because two of them confirm design decisions rather than just marking time. Everything to do with bring-up lands inside **10 ms** — the BME280 needs no power-up window, unlike the SCD-40 on the gateway, which is why this node has no `zephyr,deferred-init` and no wait. And the first reading is reported at **1.009 s**, not at 9 ms: the sensor thread published it almost immediately, but `main` was parked in `isotp_recv()` until the heartbeat came due. That one-second gap *is* the up-to-one-beat telemetry latency `src/main.cpp` documents, visible on the console. The node beats once a second and publishes every five, and none of that is logged once the states above stop changing — every remaining message is emitted on a *transition*. There is no shell here to ask it anything, which is the trade `sensor-node/prj.conf` documents.

Line by line, because each one is a check:

- **The identifiers are read back out of the running firmware.** They come from `can_link.h` via `heartbeat_id()` and friends rather than from a literal, so seeing `0x702 / 0x7e0 / 0x7e8` is confirmation that this board computed the same map the gateway will.
- **`BME280 online`** means `device_is_ready()` succeeded, so the driver bound and the chip answered its ID register. `BME280 not ready` instead means it did not answer at `0x77` — try `0x76`, which most non-Adafruit breakouts use.
- **`gateway unreachable` is the expected result today**, not a fault: with no transceiver there is nothing to acknowledge a frame, so the transmit error counter climbs and the controller reports first `-EIO` and then, once it passes 255, `-ENETUNREACH` (bus-off). See *Bring-up checks* note below.
- **`sensor readings OK (seq 0)`** is the one that says the telemetry has real content in it. Without it — or with `sensor reads are failing` in its place — the encoded `Telemetry` is about **11 bytes** rather than **25**, because every measurement is absent rather than zero. That length difference is worth memorising: it is the cheapest way to spot a dead sensor in an ISO-TP log line.

**Proves:** the I²C path, the driver binding, the CAN device resolving through `chosen`, `isotp_bind()` finding a free filter, and the sensor→zbus→encode path end to end. Not proven, and not provable from one board: that any of those frames leave the pin.

**What the CAN errors mean with no peer attached**, since this is what the console will show until the transceivers arrive:

| Code | Name | Meaning |
|---|---|---|
| `-5` | `-EIO` | a transmit failed; error counters climbing, still error-active |
| `-114` | `-ENETUNREACH` | `CAN_ESR_BOFF` is set — the controller is **bus-off** (`can_stm32_bxcan.c:794`) |
| `-2` | `ISOTP_N_TIMEOUT_BS` | the first frame went out; no flow control came back inside `CONFIG_ISOTP_BS_TIMEOUT` (1000 ms) |

All three are the predicted behaviour of a node alone on a bus, not defects: nothing drives the ACK slot, so every frame is unacknowledged, TEC passes 255, and the controller takes itself off the bus. Recovery is automatic, so it recovers and repeats. [`can-guide.md`](../notes/can-guide.md) §5 and §6 are the mechanism. The firmware treats it as a link state rather than an error — one line on the transition, then quiet, with telemetry backing off 2 s → 30 s while the heartbeat keeps trying.

A two-node check against a real bus belongs here and is deliberately absent — it has not been run, and a procedure nobody has executed is not a bring-up check. So is the loopback check of the peer's own ISO-TP path, which is possible in principle and has not been done. Both arrive with the transceivers.

## What is deliberately not here

- **No CAN-FD.** The peer node is an STM32F072 with **bxCAN**, which is classic CAN 2.0B only, so the link is classic at 8 bytes per frame regardless of what the H7's FDCAN could do. This is a feature rather than a concession: the 8-byte limit is what forces real segmentation, which is one of the things this phase exists to learn. `CONFIG_CAN_FD_MODE` stays unset, and `can show` correctly omits `fd`.
- **No transceiver node.** Nothing to control — see *Driver behaviour worth knowing*.
- **No relay, and no second MQTT identity.** ISO-TP now exists on the peer, but only the peer: the gateway has no `relay.cpp`, no relay zbus channels and one MQTT connection. Until that is built, nothing the peer transmits has anywhere to go. Both the gateway's opacity boundary and the second identity are the next step.
- **No flow-control throttling.** The peer advertises `bs = 0, stmin = 0` — send the whole transfer, no minimum gap — because the only thing that reaches it is a 20-byte `Command`, which is three frames. Block size exists so a slow receiver can throttle a fast sender mid-transfer; throttling three frames costs a round trip per block and buys nothing. A node receiving a firmware image would answer differently.
- **No ISO-TP TX buffering.** `CONFIG_ISOTP_USE_TX_BUF` and `CONFIG_ISOTP_ENABLE_CONTEXT_BUFFERS` are both off. `isotp_send()` with a null completion callback blocks until the transfer completes, so the caller's own buffer stays valid throughout and there is nothing for the library to copy into — which is also what makes the peer's single-writer rule enforceable without a mutex.
- **No second thread on the peer.** `main()` does the receiving, the sending and the heartbeat. Two `isotp_send()` calls on one address from different contexts interleave their frames and corrupt both transfers, so single-writer is a correctness property rather than a simplification. The visible cost is that a reading can wait up to one heartbeat period before it is transmitted.
- **No manual bus-off recovery.** Automatic recovery is the default and is what a two-node bench bus wants; `CONFIG_CAN_MANUAL_RECOVERY_MODE` would only be interesting if the app needed to observe or delay recovery.
- **CAN is not a network interface.** Zephyr can run 6LoCAN and expose CAN through the networking stack; this project deliberately treats CAN as a raw frame transport with ISO-TP above it, because the point is the framing problem MQTT hides, not another IP link.
