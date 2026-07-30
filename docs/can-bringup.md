# CAN bring-up — project reference

How the CAN controller on the **Nucleo-H753ZI** is configured, clocked and verified on this bench. Terse by intent: decisions, rationale, and the facts you need when the link misbehaves. For the concepts underneath — what a differential multi-drop bus is, how arbitration makes a lower ID win, why acceptance filters exist, and what 8 bytes per frame does to a protocol design — see the companion teaching guide, [`can-guide.md`](../notes/can-guide.md).

This page stops at the **frame**. Getting a frame onto the wire and back off it is all that is built so far: there is no ISO-TP segmentation, no relay, and no second node. The seam is exact — this page ends where `can_send()` and an RX filter callback hand over to code that does not exist yet.

Builds against the shared global Zephyr workspace — see [`toolchain.md`](toolchain.md).

## The CAN path

Two files, and that is the whole of it today:

```
firmware/boards/nucleo_h753zi.overlay   the bitrate, which the board dts leaves unset
firmware/prj.conf                       CONFIG_CAN + the CAN shell
```

No application code talks to the controller yet, which is deliberate: the shell can drive the link on its own, so the peripheral gets proven before anything is layered on it. Debugging a silent bus and debugging a segmentation bug at the same time is the failure mode that ordering avoids.

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

**4. Console.** Unchanged — `zephyr,console = &usart3` on the ST-LINK VCP at **115200 8N1**, opened with `scripts/console.sh`. With both Nucleos attached the glob matches two ports and the script refuses to guess; pass one explicitly or set `PORT=`.

## Driver behaviour worth knowing

- **The shell addresses the controller as `can@4000a000`, not `fdcan1`.** Device names come from the devicetree node's full name, not its label. Tab completion after `can mode ` enumerates them, which is faster than remembering the address.
- **Controllers come up stopped.** Nothing transmits or receives until `can_start()`. The failure mode is the nasty kind: every log line looks healthy and no frame moves. Mode changes also require the stopped state — `can_set_mode()` returns `-EBUSY` otherwise — so the order is always stop → mode → start.
- **Filters are not deduplicated, and matching stops at the first match.** `can_mcan_add_rx_filter_std()` (`drivers/can/can_mcan.c:1094`) allocates the lowest free slot and registers a callback; adding the same ID three times yields three slots and three callbacks. But the M_CAN hardware walks the standard filter list in order and stops at the first match, so **only one callback fires** — observed on the bench in loopback: three identical filters for `0x702`, one printed frame. `include/zephyr/drivers/can.h:1335` states the general rule: where filters overlap, match priority is hardware dependent.
- **Consequently `can dump` can show nothing while frames are arriving normally.** `cmd_can_dump()` installs a catch-all (`id = 0, mask = 0`, `drivers/can/can_shell.c:481`) through the same lowest-free-slot allocator. Any specific filter installed earlier — by application code at boot, say — occupies a lower slot and wins every match, leaving the shell's catch-all permanently silent. Use `can filter add <dev> <id>` for a known ID instead, and do not read an empty `can dump` as a dead bus.
- **`transceiver: passive/none` is permanent here, and is not a presence indicator.** Zephyr reports a transceiver only when a `can-transceiver` node exists to control. The SN65HVD230 breakouts are always-on parts with no standby pin wired, so there is nothing to describe and nothing to drive. That line will read the same with the transceivers attached and working.
- **Bus-off recovery is automatic.** `CONFIG_CAN_MANUAL_RECOVERY_MODE` is off, so the controller recovers on its own and the shell has no `can recover` command — visible in the `can` help output, where `recover` is a conditional command that simply is not there.
- **No CAN-FD.** `can show` reports capabilities `normal loopback listen-only` with no `fd`, because `CONFIG_CAN_FD_MODE` is unset. Deliberate — see *What is deliberately not here*.

## Build & flash

```sh
scripts/build.sh -p                  # pristine (required after devicetree/Kconfig edits)
scripts/flash.sh                     # forces -r openocd; both boards default to the uninstalled cube runner
scripts/console.sh /dev/cu.usbmodemXXX   # 115200; quit with Ctrl-A then K
```

Both scripts take `-a <app>` and default to `firmware`. With two probes attached, `STLINK_SERIAL=` pins `flash.sh` to one of them; without it openocd takes whichever it finds first.

Enabling CAN cost **+16 392 B flash** (219 460 → 235 852) and **+908 B RAM** (52 000 → 52 908), on a part with 2 MB and 512 KB. `drivers/can` is 10 804 B of that, split `can_mcan.c` 4392, **`can_shell.c` 4356**, `can_common.c` 1098, with the STM32H7 glue making up the rest — so roughly 40 % of the CAN footprint is a debugging aid. Worth knowing before the F072RB, which has 16 KB of RAM total and will not be carrying a shell. Read these off `scripts/build.sh -t rom_report` rather than trusting the numbers here; they move with every Kconfig change.

## Bring-up checks

No wiring is needed for any of this — internal loopback proves the controller without a transceiver, and stays the way to answer "is it my firmware or my wiring?" later.

**1. The bitrate actually applied.** `can show` deliberately does *not* print the configured bitrate — it prints the controller's *maximum*, which on this part is 1 Mbit/s and will look reassuring while the bus is misconfigured. Check the generated devicetree instead:

```sh
grep P_bitrate firmware/build/zephyr/include/generated/zephyr/devicetree_generated.h
```

Expect `#define DT_N_S_soc_S_can_4000a000_P_bitrate 500000`. A `125000` here, or no line at all, means the overlay did not apply — rebuild with `-p`, since devicetree changes need a pristine build.

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

**4. The rest of the app is undisturbed.** Telemetry should keep publishing throughout — `<inf> node: published telemetry seq=N` every ~5 s on the console, and readings still arriving on the Pi. CAN shares no code with the network path, and this check exists to notice if that ever stops being true.

A two-node check against the real bus belongs here, and is deliberately absent: it has not been run, and a procedure nobody has executed is not a bring-up check. It arrives with the transceivers.

## What is deliberately not here

- **No CAN-FD.** The peer node is an STM32F072 with **bxCAN**, which is classic CAN 2.0B only, so the link is classic at 8 bytes per frame regardless of what the H7's FDCAN could do. This is a feature rather than a concession: the 8-byte limit is what forces real segmentation, which is one of the things this phase exists to learn. `CONFIG_CAN_FD_MODE` stays unset, and `can show` correctly omits `fd`.
- **No transceiver node.** Nothing to control — see *Driver behaviour worth knowing*.
- **No ISO-TP, no relay, no second MQTT identity.** All of that is the next step, and none of it exists yet. When it does, the address map, the heartbeat frame layout and the gateway's opacity boundary will be recorded here.
- **No manual bus-off recovery.** Automatic recovery is the default and is what a two-node bench bus wants; `CONFIG_CAN_MANUAL_RECOVERY_MODE` would only be interesting if the app needed to observe or delay recovery.
- **CAN is not a network interface.** Zephyr can run 6LoCAN and expose CAN through the networking stack; this project deliberately treats CAN as a raw frame transport with ISO-TP above it, because the point is the framing problem MQTT hides, not another IP link.
