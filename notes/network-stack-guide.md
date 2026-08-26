# How Zephyr turns `prj.conf` into a working network, with no app code

*A from-first-principles guide to the layer the communication guide black-boxes: how a handful of `CONFIG_NET_*` lines and a devicetree node become a live IP interface before `main()` runs — and how much would change if the wire were USB or Wi-Fi instead of Ethernet.*

This is a teaching document, not project documentation. [`communication-guide.md`](communication-guide.md) draws its stack and then says *"everything below MQTT, Zephyr implements for you"* — and stops there, on purpose. This guide lifts that floor: it explains **how** Zephyr implements it, and why the app can open a socket without ever mentioning Ethernet, an IP address, or an interface. Its reference half — the concrete nodes, Kconfig, and boot steps *this* board uses — is [`network-bringup.md`](../docs/network-bringup.md); read them together, this for the "why", that for the "what".

**Prerequisites:** the [`zephyr-build-system-guide.md`](zephyr-build-system-guide.md) (what devicetree and Kconfig *are*), and enough of [`communication-guide.md`](communication-guide.md) to know that TCP gives a byte stream and MQTT rides on it. You do **not** need to have read the MQTT walkthrough.

**The shape of this document:**

- **§1** — the puzzle: three files, no code, a working socket.
- **§2** — the physical pieces: what MAC, PHY, MDIO, and RMII actually are, in hardware.
- **§3** — the one idea that makes it work: everything routes through a `net_if`.
- **§4** — the stack as Zephyr's own layers (L2, net_if, socket), and where each is chosen.
- **§5** — the boot sequence, step by step, that ends with an address on the interface.
- **§6** — why the socket API never names an interface (and when that breaks).
- **§7** — the payoff: swapping Ethernet for USB, Wi-Fi, or Bluetooth — what moves and what doesn't.
- **§8** — a runnable lab on the board's `net` shell.
- **§9–§10** — the model in a paragraph, and where to go next.

---

## 1. The puzzle

Here is everything the application does to get on the network:

```
(nothing)
```

There is no `net_if_up()`, no `socket()` for the interface, no address assignment, no link setup anywhere in `main.cpp`. The first networking call the app makes is `mqtt_connect()`, and by then a fully configured IPv4 interface already exists. Compared to the sensor, which `sensor.cpp` explicitly initialises with `device_init()` — the network needs no equivalent.

What produces that interface is three inert-looking things:

1. a **devicetree node** (`&mac { status = "okay"; … }`) that the board already ships,
2. a dozen **`CONFIG_NET_*` lines** in `prj.conf`, most of them just addresses,
3. one Zephyr subsystem — **`net_config`** — that runs automatically before `main()`.

The question this guide answers is how those three become a socket you can connect through. The short version: **Zephyr's network stack is built around one hardware-agnostic object, the `net_if`, and the whole job of bring-up is (a) making one exist and (b) putting an address on it.** Everything else is the build system and one `SYS_INIT` hook.

---

## 2. The physical pieces: MAC, PHY, MDIO, RMII

Before the software abstraction, the hardware — because §1's answer hinges on a `net_if` that a *driver* registers, and that driver is talking to two distinct chips wired together in a standard way. The terms the reference doc ([`network-bringup.md`](../docs/network-bringup.md)) throws around (MAC, PHY, MDIO, RMII) name the parts of that arrangement, and they are worth pinning down once, because they are the same on every wired-Ethernet MCU you will meet — nothing here is STM32-specific.

**Ethernet is two halves.** The IEEE-802.3 standard splits a wired link into a *digital* half and an *analog* half, and real silicon splits along the same line — usually into two separate chips:

```
   STM32H753 (the MCU die)                       LAN8742 (a separate chip)
 ┌───────────────────────────┐                 ┌──────────────────────┐        ┌───────┐
 │ CPU ─ AHB bus ─ MAC       │══ RMII (data) ══│  PHY                 │─ pair ─│ RJ45  │═ cable ═▶
 │     ("ETH" peripheral)    │── MDIO (mgmt) ──│  (transceiver)       │─ pair ─│  +    │
 │                           │                 │                      │        │ magn. │
 └───────────────────────────┘                 └──────────────────────┘        └───────┘
    digital: frames & DMA       two buses         analog: volts on copper
```

**MAC — Media Access Control.** The *digital* half, and on this board it is a peripheral *inside* the STM32 (the block the SoC devicetree calls `ethernet`, and ST's docs call "ETH"). It thinks in **frames**: it builds the Ethernet header, appends the CRC, hands finished frames to DMA, and on receive does the reverse. It owns the interface's **MAC address** — `02:80:E1:9C:A7:DE` here — and enforces the rules for *who may transmit when*, which is the "media access control" its name is short for. It is the *controller*, and it never touches a wire. In Zephyr this is the `eth_stm32_hal` driver, bound to the `&mac` devicetree node.

**PHY — the physical-layer transceiver.** The *analog* half, and a **separate chip** on the board (the Microchip LAN8742). It thinks in **signals**: it turns the MAC's bits into the precise voltages the twisted-pair cable carries, and back — and it is the part that actually **auto-negotiates** link speed and duplex with whatever sits at the far end of the cable. It has no idea what a frame *means*; it moves symbols. It is the *transceiver*. In Zephyr it is a PHY driver bound to the `ethernet-phy` node (§4).

Why two chips at all? Because the analog job — line drivers, signalling matched to the RJ45's magnetics, negotiation — is a different silicon process from the digital MAC, and decoupling them lets one MCU pair with any standards-compliant PHY. Which raises how the two talk, and it is *two* connections, not one — the point that most often trips people up:

- **RMII — the data path.** Reduced Media-Independent Interface: the fast bus the actual frame bytes cross between MAC and PHY, clocked at 50 MHz over 7 signal wires. "Reduced" because the full MII uses 16; the Nucleo routes the smaller set, which is what `phy-connection-type = "rmii"` in the devicetree declares. This is the pipe your telemetry bytes physically travel down on the way off-chip.
- **MDIO — the management path.** Management Data Input/Output: a slow, 2-wire (`MDC` clock + `MDIO` data) *control* bus, entirely separate from RMII. The MAC uses it to **configure and poll** the PHY — read its ID registers, ask "is the link up?", set the speed. No frame data ever crosses it. It is its own devicetree bus (`&mdio`) with the PHY as a child node, precisely because it *is* a distinct bus with its own two pins.

The one-sentence version to carry into the rest of the guide: **the MAC is the on-chip digital frame engine, the PHY is the off-chip analog line driver, RMII is the fast data road between them, and MDIO is the slow control road — and "Ethernet"/"ETH" names the MAC end of that pair.** Everything from here up stops caring which of these is which: §3 abstracts the whole column behind one `net_if`.

---

## 3. The one idea: everything routes through a `net_if`

A `net_if` (network interface) is Zephyr's in-memory handle for "a way to send and receive packets." It is the pivot of the entire stack, and it is deliberately **ignorant of hardware**. Above it, the socket and IP code deal only in `net_if`s and addresses; below it, a driver deals in frames and registers. The `net_if` is the seam that lets those two never know about each other.

```
      your code          zsock_socket / zsock_connect / mqtt_connect
         │                        (names addresses, never interfaces)
   ──────┼──────────────────────────────────────────────────────────
    IP / TCP / sockets     "which net_if reaches 192.168.10.1?"  → routing
         │
    ┌────▼────┐
    │ net_if  │   ← the hardware-agnostic handle: one address, one L2, one driver
    └────┬────┘
         │            net_if.l2 = the Ethernet L2  (frames ⇄ the API below)
    ─────┼──────────────────────────────────────────────────────────
    L2 (link layer)   NET_L2_ETHERNET: add/strip Ethernet headers, ARP, MAC
         │
    device driver     eth_stm32_hal: DMA rings, the MAC peripheral, MDIO to the PHY
         │
    ─────┼──────  the wire  ──────────────────────────────────────────
      STM32 MAC → RMII → LAN8742 PHY → RJ45
```

Two properties of this picture are the whole reason the app is empty:

- **The socket layer picks the `net_if` for you, by address.** When the app connects to `192.168.10.1`, the IP layer asks "which interface can reach that address?" and finds the one interface whose subnet matches. The app never selects it. (§6 is this in detail.)
- **Every layer below the `net_if` is chosen at *build* time, by Kconfig and devicetree — not by code.** Which L2 (`NET_L2_ETHERNET`), which driver (`ETH_STM32_HAL`, auto-selected by the devicetree node): all decided before the program runs. There is nothing left for `main()` to wire up because the wiring happened in the build.

So "no app code" is not magic; it is the payoff of pushing every hardware choice into configuration and giving the runtime exactly one address-to-set. Hold onto the `net_if` as the center of gravity — §7's swap question is entirely "keep the `net_if` and its whole stack above; replace what sits below it."

---

## 4. The stack as Zephyr's layers, and where each is chosen

The [communication guide's](communication-guide.md) stack (Ethernet → IP → TCP → MQTT) is the *protocol* view. Zephyr's *implementation* has the same shape but names its layers differently, and — the point of this section — each layer is turned on by a different mechanism:

| Zephyr layer | What it does | Turned on by | In this repo |
|---|---|---|---|
| **socket API** | the `zsock_*` calls the app (and `mqtt_lib`) makes | `CONFIG_NET_SOCKETS` | `prj.conf` |
| **TCP / IPv4** | reliable stream; addressing & routing | `CONFIG_NET_TCP`, `CONFIG_NET_IPV4` | `prj.conf` |
| **L2 (link layer)** | frames ⇄ driver: Ethernet headers, ARP | `CONFIG_NET_L2_ETHERNET` | `prj.conf` |
| **MAC driver** | the STM32 Ethernet peripheral + DMA | `CONFIG_ETH_STM32_HAL` — **auto** | devicetree `&mac` |
| **PHY driver** | configures/polls the LAN8742 over MDIO | the `ethernet-phy` node | board devicetree |

The **L2** is the layer worth dwelling on, because it is the one the communication guide never names and the one §7 swaps. "L2" is Zephyr's abstraction for *the link layer* — the code that turns an outgoing IP packet into whatever frame the medium below expects, and back. For Ethernet that means: prepend the 14-byte Ethernet header, resolve the next-hop IP to a MAC via ARP, hand the frame to the driver. `NET_L2_ETHERNET` is that code.

Crucially, **the L2 is an interface, not Ethernet-specific machinery.** Zephyr ships several implementations of it — `subsys/net/l2/` holds `ethernet`, `ppp`, `ieee802154`, `wifi`, `virtual`, and more — and a `net_if` binds to exactly one. The IP layer above calls the same L2 entry points regardless of which one it is. That single fact is what makes the swap in §7 tractable: **change the L2 (and the driver under it), keep everything above the `net_if`.**

The **MAC driver row is the auto-selected one**, and it mirrors the sensor exactly. You never write `CONFIG_ETH_STM32_HAL=y`; it is `default y` gated on `DT_HAS_ST_STM32_ETHERNET_ENABLED`, so the *devicetree* node enabling `&mac` is what pulls the driver in. Same devicetree→Kconfig bridge as `CONFIG_SCD4X` (see [`build-system-overview.md`](../docs/build-system-overview.md)). The driver, in turn, is what *creates the `net_if`* — via a registration macro, next.

---

## 5. The boot sequence: from `SYS_INIT` to an address on the wire

Zephyr boots by running a fixed sequence of init levels — `PRE_KERNEL_1`, `PRE_KERNEL_2`, `POST_KERNEL`, `APPLICATION` — and *then* calling `main()`. Both the driver and `net_config` hook into that sequence with `SYS_INIT`/device macros, which is how all of this happens before your code. Three events, in order:

**(1) `POST_KERNEL` — the driver registers a `net_if`.** The STM32 Ethernet driver ends with:

```c
/* drivers/ethernet/eth_stm32_hal_common.c:406 */
ETH_NET_DEVICE_DT_INST_DEFINE(0, eth_initialize, NULL, &eth0_data, &eth0_config,
                              CONFIG_ETH_INIT_PRIORITY, &eth_api, ETH_STM32_HAL_MTU);
```

`ETH_NET_DEVICE_*` is `NET_DEVICE_*` plus "bind me to the Ethernet L2." It does two things at build time — creates a `struct device` for the MAC *and* a `net_if` pointing at it and at `NET_L2_ETHERNET` — then runs `eth_initialize` at boot to bring the peripheral up. After this step a `net_if` exists, bound to its L2 and driver. It has **no IP address yet.**

**(2) PHY link-up — the interface gains carrier.** Asynchronously (the driver polls/gets an interrupt from the LAN8742 over MDIO), when the cable has link the driver calls `net_eth_carrier_on(iface)`. Only now can the interface actually move frames. Pull the cable and `net_if_carrier_off` fires — the address stays assigned, but the interface is not operational. This is why a socket connect *fails fast* with no cable rather than hanging.

**(3) `APPLICATION` — `net_config` assigns the address.** This is the "no app code" step made concrete. `net_config` registers an init hook:

```c
/* subsys/net/lib/config/init.c:565 */
SYS_INIT(init_app, APPLICATION, CONFIG_NET_CONFIG_INIT_PRIO);   /* prio 95 */
```

`APPLICATION` runs after every `POST_KERNEL` driver, so the `net_if` from step (1) is guaranteed to exist. The hook does exactly what the app would otherwise have to:

```c
/* net_config_init_app(), abridged */
net_if_foreach(iface_find_cb, &iface);   /* the first auto-started net_if */
setup_ipv4(iface);                        /* net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0) */
```

Two details carry the whole design:

- **It finds the interface by *searching*, not by name.** `net_if_foreach` walks every registered `net_if` and takes the first that isn't flagged `NET_IF_NO_AUTO_START`. With one interface — this board — it needs zero configuration to pick the right one. This is the deep reason the setup scales down to "no code": with a single `net_if`, there is nothing to *choose*.
- **The address is your `prj.conf` string, parsed at boot.** `CONFIG_NET_CONFIG_MY_IPV4_ADDR` is run through `net_addr_pton` and added as a manual address; the netmask likewise. No gateway string is set, so no default route is installed (this cable is on-link only).

`CONFIG_NET_CONFIG_AUTO_INIT` being `default y` is what compiles that `SYS_INIT` in at all. Turn it off and the interface still *exists* after step (1) — but it has no address until the app calls `net_config_init()` itself. Which is exactly the situation §7 hits with USB.

By the time `main()` runs: interface up, address set, routing implicit. `mqtt_connect()` → `zsock_socket()` → `zsock_connect(192.168.10.1)` just works.

---

## 6. Why the socket never names an interface

Here is the socket call at the bottom of `mqtt_connect()`, conceptually:

```c
int s = zsock_socket(AF_INET, SOCK_STREAM, 0);
zsock_connect(s, (struct sockaddr *)&broker, sizeof(broker));   /* broker = 192.168.10.1:1883 */
```

Nowhere does it say "use the Ethernet interface." It can't, and shouldn't — that is the Berkeley-sockets bargain the whole industry runs on: **the application names a *destination*, and the stack chooses the *path*.** When `connect()` fires, the IP layer runs a route lookup: *which `net_if` has a subnet or route that reaches `192.168.10.1`?* The Ethernet interface owns `192.168.10.0/24`, so it wins; the source address `192.168.10.2` is filled in from it.

This is why the app is transport-agnostic *for free*. Swap Ethernet for USB (§7) and this code does not change one character: the new interface owns the matching subnet, and the same route lookup selects it instead. The socket layer is the ceiling of "hardware-agnostic" and the app lives above it.

**Where it breaks:** the moment there are *two* interfaces that could both reach a destination — say Ethernet and Wi-Fi both on-link — the automatic choice is no longer obvious, and you either need routing metrics, a bound source address, or `SO_BINDTODEVICE` to force one. This node has exactly one interface, so it never faces that; but it is the reason `net_config` searching for "the first auto-started interface" (§5) is only sufficient in the single-`net_if` case. More interfaces, more configuration — the empty-app property is a property of *one wire*.

---

## 7. The payoff: replacing the wire

Now the question worth the whole guide: **how hard is it to run this same MQTT client over USB, Wi-Fi, or Bluetooth instead of Ethernet?** The answer falls straight out of §3's picture — keep the `net_if` and everything above it, replace what sits below. But *how much* below changes differs sharply by medium, and one of the three isn't available at all.

### The invariant

Nothing from the `net_if` up moves. IPv4, TCP, sockets, `mqtt_lib`, every line of `main.cpp`, every topic and QoS decision — all untouched, because none of them ever named Ethernet (§6). What changes is always some subset of: **the devicetree node, the L2 Kconfig, the driver, and whether bring-up stays automatic.**

### USB (CDC-ECM / CDC-NCM) — the near drop-in

This is the surprising one: **USB networking still uses the Ethernet L2.** The USB CDC-ECM and CDC-NCM classes present the host↔device link *as an Ethernet interface* — both call `ethernet_init(iface)` internally (`subsys/usb/device_next/class/usbd_cdc_ecm.c:612`, `usbd_cdc_ncm.c:1159`). To the IP layer it is indistinguishable from a real NIC. So:

| Layer | Change |
|---|---|
| App / MQTT / sockets / IP | **none** |
| L2 | **none** — still `CONFIG_NET_L2_ETHERNET` |
| Driver | swap the STM32 MAC for the USB device stack + a CDC-ECM/NCM class |
| Devicetree | drop `&mac`; add a `zephyr,cdc-ecm-ethernet` node under the USB device |
| Kconfig | `CONFIG_USB_DEVICE_STACK`/`USB_DEVICE_STACK_NEXT` + the CDC class + net config |
| **Bring-up** | **now manual** — see below |

The one real catch is bring-up. A USB network interface does not exist until the *host* enumerates the device, so it cannot be ready at `APPLICATION`-level init the way the MAC is. Zephyr encodes this precisely: `CONFIG_NET_CONFIG_AUTO_INIT` is

```
default y if !(USB_DEVICE_NETWORK || USBD_CDC_ECM_CLASS || USBD_CDC_NCM_CLASS)
```

— i.e. auto-init flips **off** the moment you enable USB networking. The app must now bring USB up and then call `net_config_init()` (or its own address setup) itself, after enumeration. That is a genuine handful of app code — the first time this project would need any — but it is *bring-up* code, not *protocol* code: the socket and MQTT layers still never change.

### Wi-Fi — new L2, new driver, app mostly unchanged

Wi-Fi has its **own L2** (`CONFIG_NET_L2_WIFI_MGMT`) and, on most parts, an *offloaded* driver — the TCP/IP or at least the MAC runs on a companion chip (`offloaded_netdev` in `subsys/net/l2/`). Structurally:

| Layer | Change |
|---|---|
| App / MQTT / sockets | **none** (the route lookup finds the Wi-Fi `net_if`) |
| L2 | Ethernet L2 → Wi-Fi L2 |
| Driver | a Wi-Fi driver for the specific module (e.g. an ESP32 co-processor) |
| Hardware | the H753ZI has no radio — needs an add-on module |
| Extra app code | **association**: SSID + credentials, via a `net_mgmt` connect request |

The IP-and-up stack is still untouched, but Wi-Fi adds a step Ethernet never had — you must *associate* with an access point (SSID, PSK) before there is a link, and that is an explicit `net_mgmt(NET_REQUEST_WIFI_CONNECT, …)` call in app code. So: same "socket layer is hardware-agnostic" payoff, but more bring-up than USB, and it needs hardware this board lacks.

### Bluetooth — not a drop-in on this Zephyr

IP-over-Bluetooth (IPSP / 6LoWPAN-over-BLE) **is not available in Zephyr v4.4.1** — the Bluetooth IP L2 was removed; `subsys/net/l2/` has no `bluetooth` entry, and there is no `NET_L2_BT` symbol. So you cannot point this MQTT client at a BLE link the way you can at USB. The realistic Bluetooth options are all *different architectures*, not swaps:

- **802.15.4 + OpenThread** (`NET_L2_OPENTHREAD`) gives a genuine IPv6 mesh the socket layer *can* run over — but that is Thread, not classic/BLE Bluetooth, and needs an 802.15.4 radio.
- **BLE GATT without IP** — talk MQTT-SN or a custom protocol over a GATT characteristic, with a gateway translating to real MQTT. Here the whole "sockets → TCP → MQTT" stack of this project is *gone*; only the ideas transfer.

The honest summary: USB is a near drop-in (same L2, +bring-up code), Wi-Fi is a clean L2 swap (+association, +hardware), and Bluetooth is a different project.

### The one-line answer

**Everything above the `net_if` is portable across media for free; the cost of a swap is entirely (a) the driver + L2 below it and (b) how much bring-up that medium needs before an address can be assigned.** Ethernet needs none — which is the entire reason this app has no network code — and every other medium needs some.

---

## 8. Lab: watch the interface come up

The board's `net` shell (enabled by `CONFIG_NET_SHELL=y`) exposes the exact objects §3–§5 describe. This needs the board flashed and on the console (`scripts/console.sh`); the bring-up procedure itself lives in [`network-bringup.md`](../docs/network-bringup.md)'s *Bring-up checks* — this lab is about *seeing the layers*, not first-time verification.

### Exercise 1 — the `net_if` and its stack

```
uart:~$ net iface
```

Read the output against §3's diagram: the interface is `Ethernet`, which names its **L2**; it carries the **IPv4 address** `net_config` assigned in step (3); it shows the derived **MAC** (the driver, step 1); and its state (`up`/`dormant`) is the **carrier** of step (2). One command, the whole vertical slice.

**Proves:** the `net_if` is real and singular, and everything below the socket the app never touched is nonetheless present and configured.

### Exercise 2 — the route lookup, made visible

```
uart:~$ net route
```

There is **no default route** (§5: no gateway string), yet §6 claims `connect(192.168.10.1)` finds a path. That path is the interface's own on-link `/24`, not a route entry — which is exactly why an off-link address would fail. Confirm the on-link half works:

```
uart:~$ net ping 192.168.10.1
```

**Proves:** the socket layer's "name a destination, the stack picks the path" (§6) resolves here purely from the interface's subnet — no routing table needed for one on-link peer.

### Exercise 3 — carrier vs. address

With the console open, pull the Ethernet cable, then:

```
uart:~$ net iface
```

The IPv4 address is **still listed** — but the interface state has dropped (§5, step 2). Plug it back in and it recovers. This separates the two things people conflate: *having an address* (set once, at boot) and *having carrier* (moment to moment, from the PHY).

**Proves:** address assignment and link liveness are independent, which is why the MQTT reconnect loop (a *keepalive*-driven concern, one layer up) is what actually notices the outage — the IP layer keeps its address the whole time.

---

## 9. The model in one paragraph

Zephyr's network stack pivots on one hardware-agnostic object, the `net_if`. A driver *registers* a `net_if` at boot (`POST_KERNEL`) and binds it to an L2 — for this board, the STM32 Ethernet driver, auto-selected by the devicetree `&mac` node, bound to `NET_L2_ETHERNET`. A separate subsystem, `net_config`, runs a `SYS_INIT` hook at `APPLICATION` level that finds the one auto-started interface and puts the `prj.conf` static address on it — which is the entire reason the application contains no network bring-up code. Above the `net_if`, the socket layer names only *destinations* and lets a route lookup pick the interface, so the app is transport-agnostic for free: swapping Ethernet for USB keeps the very same Ethernet L2 and changes nothing above the socket (only the driver, the devicetree node, and — because a USB interface isn't ready until the host enumerates it — the one place bring-up stops being automatic); Wi-Fi is a clean L2 swap plus association and a radio; and IP-over-Bluetooth simply isn't in v4.4.1. The cost of a wire, in this design, is exactly the driver beneath the `net_if` plus however much bring-up that medium needs before an address can land — and Ethernet needs none.

## 10. Where to go next

- **[`network-bringup.md`](../docs/network-bringup.md)** — the reference half: the exact devicetree nodes, the `CONFIG_NET_*` block line by line, the boot-order table, and the hardware bring-up checks.
- **[`communication-guide.md`](communication-guide.md)** — the layer this guide sits *under*: once there is a socket, what MQTT and the broker add on top of it. This guide is the answer to that guide's "everything below MQTT, Zephyr implements for you."
- **[`firmware-mqtt-walkthrough.md`](../docs/firmware-mqtt-walkthrough.md)** §3 — where the socket this guide produces is first *used*: `mqtt_connect()` creating and owning the file descriptor, and the app only borrowing it to `poll()`.
- **[`zephyr-build-system-guide.md`](zephyr-build-system-guide.md)** §6 — the devicetree→Kconfig bridge that auto-selects the MAC driver from the `&mac` node, the same mechanism seen here and in the sensor path.
