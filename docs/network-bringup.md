# Network bring-up — project reference

How the on-board Ethernet comes up on this bench: the LAN8742 PHY, the STM32H7 MAC, and the static IPv4 address — all with **no application code touching the interface**. Terse by intent: decisions, rationale, and the facts you need when the link misbehaves. For the concepts underneath — what the MAC and PHY each are (the digital frame engine vs. the analog line driver), what MDIO and RMII carry, what a `net_if` is, what an L2 does, why the socket API never names an interface, and how hard it would be to swap Ethernet for USB or Wi-Fi — see the companion teaching guide, [`network-stack-guide.md`](../notes/network-stack-guide.md) (§2 defines the hardware terms).

This doc stops at the socket. Everything **above** it — MQTT lifecycle, topics, QoS — is in [`mqtt-design.md`](mqtt-design.md) and the [`firmware-mqtt-walkthrough.md`](firmware-mqtt-walkthrough.md). The seam is exact: this page ends where `mqtt_connect()` calls `zsock_socket()`.

Builds against the shared global Zephyr workspace — see [`toolchain.md`](toolchain.md).

## The network path

Unlike the sensor, **no `.cpp` in this repo appears here.** The whole pipe is devicetree + Kconfig + one Zephyr subsystem that runs before `main()`:

```
dts/arm/st/h7/stm32h7.dtsi          the MAC + MDIO nodes (in Zephyr, status="disabled")
boards/st/nucleo_h753zi.dts         enables them + the LAN8742 PHY + RMII pins (in Zephyr)
gateway/prj.conf                    CONFIG_NET_* — which layers compile in, and the address
subsys/net/lib/config/init.c        net_config: applies the address at boot (in Zephyr)
```

The first, second, and fourth are **Zephyr's**, cited by `file:line` against v4.4.1. The only file this repo writes is `prj.conf` — and even there, the networking block is mostly addresses, not code.

## How it fits together

**1. Devicetree — the MAC, the MDIO bus, the PHY.** The SoC `.dtsi` ships the peripherals *off*:

```dts
/* dts/arm/st/h7/stm32h7.dtsi:1184 */
mac: ethernet {
	compatible = "st,stm32h7-ethernet", "st,stm32-ethernet";
	interrupts = <61 0>;
	memory-regions = <&sram2>;      /* DMA descriptors live in SRAM2 (see below) */
	status = "disabled";
};
mdio: mdio {
	compatible = "st,stm32-mdio";
	status = "disabled";
};
```

The **board** `.dts` turns them on and wires the physical layer — this is the whole "connect the pipe to the hardware" step, and it already ships with Zephyr for this board:

```dts
/* boards/st/nucleo_h753zi/nucleo_h753zi.dts:210 */
&mac {
	status = "okay";
	pinctrl-0 = <&eth_rxd0_pc4 &eth_rxd1_pc5 &eth_ref_clk_pa1
		     &eth_crs_dv_pa7 &eth_tx_en_pg11 &eth_txd0_pg13 &eth_txd1_pb13>;
	pinctrl-names = "default";
	phy-connection-type = "rmii";   /* 7 pins, not MII's 16 — the Nucleo is RMII */
	phy-handle = <&eth_phy>;        /* which PHY this MAC drives */
};
&mdio {
	status = "okay";
	pinctrl-0 = <&eth_mdio_pa2 &eth_mdc_pc1>;    /* the 2-wire management bus */
	pinctrl-names = "default";
	eth_phy: ethernet-phy@0 {
		compatible = "ethernet-phy";   /* generic clause-22 PHY → phy_mii.c */
		reg = <0x00>;                  /* PHY address on the MDIO bus */
	};
};
```

Three things worth reading off this:

- **The PHY is a devicetree node on its own bus.** The MDIO bus (2 wires: `MDC` clock, `MDIO` data) is how the MAC *configures and polls* the PHY — reads link status, sets speed. The RMII pins (`eth_*`) are the separate high-speed path the actual frames travel on. Two buses, two jobs.
- **`compatible = "ethernet-phy"` is deliberately generic.** It binds to Zephyr's `phy_mii.c` (`DT_DRV_COMPAT ethernet_phy`), the standard IEEE-802.3 clause-22 driver — *not* the LAN8742-specific `phy_microchip_lan8742.c`. The generic driver reads the PHY's ID registers over MDIO and drives it through the standard register set, so it works for the LAN8742 without naming it. (A part needing vendor quirks would use its specific compatible.)
- **This repo's overlay does none of this.** `gateway/boards/nucleo_h753zi.overlay` adds only the SCD-40 on I²C; the Ethernet nodes above come entirely from the board definition. We inherit a working link and never edit it.

**2. Kconfig — which layers compile in.** The networking block of `gateway/prj.conf`:

```
CONFIG_NETWORKING=y
CONFIG_NET_L2_ETHERNET=y      # the Ethernet link layer (frames ⇄ the MAC driver)
CONFIG_NET_IPV4=y             # IP addressing
CONFIG_NET_IPV6=n
CONFIG_NET_SOCKETS=y          # the BSD-style socket API mqtt_lib builds on
CONFIG_NET_TCP=y              # TCP (MQTT rides on it)
CONFIG_NET_CONFIG_SETTINGS=y  # the net_config helper + the address settings below
CONFIG_NET_CONFIG_NEED_IPV4=y
CONFIG_NET_CONFIG_MY_IPV4_ADDR="192.168.10.2"
CONFIG_NET_CONFIG_MY_IPV4_NETMASK="255.255.255.0"
CONFIG_NET_CONFIG_PEER_IPV4_ADDR="192.168.10.1"   # the broker; kept here so all IPs live in one file
```

Note what's **absent**, exactly as with the sensor driver: we never set the MAC driver's own Kconfig. `ETH_STM32_HAL` is `default y` gated on `DT_HAS_ST_STM32_ETHERNET_ENABLED` (`drivers/ethernet/Kconfig.stm32_hal:9`), so **enabling `&mac` in the devicetree auto-selects the driver** — the same devicetree→Kconfig bridge that pulls in `CONFIG_SCD4X` (see [`build-system-overview.md`](build-system-overview.md)). It also `select`s `NOCACHE_MEMORY` on the H7 Cortex-M7, because the Ethernet DMA descriptors must live in non-cached RAM (the `memory-regions = <&sram2>` above). None of that appears in `prj.conf`.

`CONFIG_NET_CONFIG_PEER_IPV4_ADDR` is the odd one out: it configures *nothing* on the interface — it is a convenience string the app reads to know where the broker is. It lives in this block only so every address sits in one file.

**3. The boot sequence — where the interface actually comes up.** Three Zephyr mechanisms fire in order, none of them app code:

| When | What | Effect |
|---|---|---|
| `POST_KERNEL` (driver init) | `ETH_NET_DEVICE_DT_INST_DEFINE(0, eth_initialize, …)` (`eth_stm32_hal_common.c:406`) | MAC driver inits and **registers a `net_if`** bound to the Ethernet L2 |
| PHY link-up (async, interrupt/poll) | driver calls `net_eth_carrier_on(iface)` (`eth_stm32_hal_common.c:245`) | interface goes *data-up* — it can carry frames only once the cable has carrier |
| `APPLICATION`, prio 95 | `SYS_INIT(init_app, APPLICATION, …)` in `net_config` (`init.c:565`) | applies the static IPv4 address (below) |

The last step is the "no app code" magic, and it is worth reading the actual mechanism, because it explains the one surprising default. `net_config`'s auto-init picks an interface *by searching*, not by name:

```c
/* subsys/net/lib/config/init.c — net_config_init_app(), abridged */
net_if_foreach(iface_find_cb, &iface);   /* first iface without NET_IF_NO_AUTO_START */
...
setup_ipv4(iface);                       /* → net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0) */
```

So with **one** interface it needs no configuration to find it — it takes the only one. The address string from `prj.conf` is parsed by `net_addr_pton` and added as a `NET_ADDR_MANUAL` address; the netmask is set the same way. This whole dance runs from a `SYS_INIT` at `APPLICATION` level, which is *after* every `POST_KERNEL` driver, so the `net_if` is guaranteed to exist by the time `net_config` looks for it.

**`CONFIG_NET_CONFIG_AUTO_INIT` is `default y`** (`init.c:565` is compiled in) — which is why `main.cpp` never calls `net_config_init()`. That default has one exception that matters for the swap question; see [`network-stack-guide.md`](../notes/network-stack-guide.md) §7.

**4. No gateway, on purpose.** `CONFIG_NET_CONFIG_MY_IPV4_GW` is unset. `setup_ipv4()` only adds a default route when that string is non-empty (`init.c:199`), so **no route is installed** — deliberately. The node (`.2`) and the Pi (`.1`) share `192.168.10.0/24`, so every peer is on-link and reachable by ARP alone; a gateway would only be needed to reach *off*-link addresses, and there are none on this direct cable. See [`mqtt-design.md`](mqtt-design.md).

## Driver behaviour worth knowing

- **The MAC address is derived, not fixed.** The STM32 Ethernet driver hashes the chip's 96-bit unique ID into a locally-administered MAC — `02:80:E1:9C:A7:DE` on this board (the `02` low nibble marks it locally-administered). `CONFIG_NET_LOG=y` prints it at boot. It is stable per board but not globally unique; only relevant if a second node ever shares the link. See [`mqtt-design.md`](mqtt-design.md).
- **The interface tracks carrier.** Link-up calls `net_eth_carrier_on`, cable-pull calls `net_if_carrier_off` (`eth_stm32_hal_common.c:245,269`). The static IP stays *assigned* across a cable pull, but the interface is not *operational* without carrier — so a socket connect fails immediately rather than hanging when the cable is out. (This is the firmware side of the observation in [`mqtt-design.md`](mqtt-design.md) that holding the board in reset drops the PHY.)
- **DMA descriptors live in non-cached RAM.** The H7's Ethernet DMA and the Cortex-M7 data cache would otherwise disagree about descriptor contents; `NOCACHE_MEMORY` + the `memory-regions = <&sram2>` binding place them where the cache does not cover. Auto-selected; nothing to configure, but it is why the map file shows Ethernet buffers in SRAM2.
- **RMII, not MII.** `phy-connection-type = "rmii"` uses 7 signal pins clocked at 50 MHz instead of MII's 16 — the reduced pin count the Nucleo routes. It caps at 100 Mbit, which is all the LAN8742 does anyway.

## Bring-up checks

Verified on hardware. The link needs no sensor and no broker — it comes up on power alone, so these checks run first when anything network-shaped misbehaves. Direct cable Nucleo↔Pi; the Pi holds `192.168.10.1/24` on `eth0` (see the [bench notes](../notes/README.md) and [`mqtt-design.md`](mqtt-design.md)).

**1. The interface exists and has the address.** At the console:

```
uart:~$ net iface
```

Expect one Ethernet interface, `Ethernet <up>` (with the cable in), `IPv4 address: 192.168.10.2`, netmask `255.255.255.0`, and the derived MAC. `net iface` reading `dormant` or `down` with the cable seated points at carrier/PHY — check the RJ45 link LEDs before the firmware.

**Proves:** the devicetree link bound, the driver registered a `net_if`, and `net_config` applied the address — the entire chain of this page, below the socket.

**2. The node can reach the Pi.** ICMP outbound from the board:

```
uart:~$ net ping 192.168.10.1
```

Expect three replies with round-trip times. Silence means either no carrier (check `net iface` first) or the Pi's `eth0` is down — `ip -br addr show eth0` on the Pi should read `UP` with `192.168.10.1/24`.

**3. The Pi can reach the node.** From the Pi: `ping -c3 192.168.10.2`. Both directions proving is the point — a one-way ping can hide an ARP or route asymmetry.

**Proves (2 + 3):** IPv4 is live end to end on the cable. Everything from here up — opening a socket, the TCP handshake, the MQTT `CONNECT` — is the socket layer and above, so a failure past this point is in [`firmware-mqtt-walkthrough.md`](firmware-mqtt-walkthrough.md)'s territory, not this page's. This is the bisect line.

## What is deliberately not here

- **No DHCP.** A static address on a two-host cable is simpler and has nothing to lease from. `CONFIG_NET_DHCPV4` + a call to `net_dhcpv4_start()` would be the alternative, and would move address assignment *into* app code — the opposite of what this setup demonstrates.
- **No IPv6.** `CONFIG_NET_IPV6=n`. One address family is enough to carry MQTT; leaving v6 off trims the stack.
- **No gateway / no DNS.** On-link only, broker addressed by IP. Both would be needed the moment a peer lived off this cable.
- **No connection manager.** `CONFIG_NET_CONNECTION_MANAGER` would give the app L4-connectivity events instead of the MQTT layer discovering a dead link via keepalive. Not used — the reconnect loop in `main.cpp` handles it at the MQTT layer instead.
