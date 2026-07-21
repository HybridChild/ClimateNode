# Understanding Zephyr's sensor API and sensor shell

*A from-first-principles guide to how `sensor_sample_fetch()` turns into I²C traffic,
why readings come back as two integers instead of a float, and what the `sensor` shell
command is actually doing.*

A teaching document. It builds up from "what is a device in Zephyr" to the four lines of
sensor code in [`firmware/src/main.cpp`](../firmware/src/main.cpp), using this bench's
SCD-40 as the running example. Every code reference is real — line numbers point at
Zephyr v4.4.1 in `~/zephyr-workspace` and at this repo.

For what was *built* during sensor bring-up (overlay, Kconfig, wiring), see
[`docs/sensor-bringup.md`](../docs/sensor-bringup.md) — that's the terse reference half.
This is the concepts half. For the chip's own command codes and conversion formulas, see
`../../shared_refs/sensor/SCD4x.yaml`.

---

## 1. The problem being solved

Suppose there were no sensor API. Reading CO₂ from the SCD-40 would look like this:

```c
uint8_t cmd[2] = {0xEC, 0x05};          /* read_measurement */
i2c_write(i2c_dev, cmd, 2, 0x62);
k_sleep(K_MSEC(1));
uint8_t rx[9];
i2c_read(i2c_dev, rx, 9, 0x62);
/* rx = [co2_hi, co2_lo, crc, t_hi, t_lo, crc, rh_hi, rh_lo, crc] */
uint16_t raw_co2 = (rx[0] << 8) | rx[1];
/* ...verify three CRC-8s with polynomial 0x31, init 0xFF... */
uint16_t raw_t = (rx[3] << 8) | rx[4];
float temp_c = -45.0f + 175.0f * ((float)raw_t / 65535.0f);
```

Everything in there is **specific to this chip**: the command word `0xEC05`, the 9-byte
layout, the CRC parameters, the magic constants −45 and 175. Swap in a different CO₂
sensor and every line changes. Worse, that knowledge is now scattered through your
application code, where it doesn't belong.

Zephyr's answer is a **device-class abstraction**. All sensors — accelerometers,
thermometers, CO₂ sensors, on any bus — expose the same handful of functions. Your
application says "fetch a sample, give me the CO₂ channel." A driver, written once by
whoever knows the chip, translates that into the bus traffic above.

The payoff is concrete for this project: `firmware/src/main.cpp` contains **zero** I²C
calls, zero command codes, and zero conversion constants. Replacing the SCD-40 with a
different CO₂ sensor would be an edit to `firmware/boards/nucleo_h753zi.overlay`, not to
the application.

---

## 2. Groundwork: what a "device" is in Zephyr

Before the sensor API makes sense, one layer below it has to.

### 2.1 `struct device` is a handle

Every driver-backed thing in Zephyr — a UART, an I²C controller, a sensor — is
represented at runtime by a `const struct device *`. It bundles three things:

| Member | What it holds | Mutable? |
|---|---|---|
| `->config` | Compile-time settings from devicetree (I²C address, bus, mode) | `const`, lives in flash |
| `->data` | Runtime state (last sample, cached values) | RAM |
| `->api` | Pointer to the driver's function table | `const`, lives in flash |

For the SCD-40, `->config` holds the I²C bus spec and address `0x62`; `->data` holds the
three most recent raw 16-bit readings. You never touch either directly — only the driver
does.

### 2.2 Devicetree creates the device

Your overlay declares the hardware:

```dts
&i2c1 {
	scd40: scd40@62 {
		compatible = "sensirion,scd40";
		reg = <0x62>;
		status = "okay";
	};
};
```

Read it as a sentence: *on the I²C1 bus, there is a device at address 0x62 whose driver
is identified by the string `sensirion,scd40`, and it is present.*

At build time this becomes a C structure. In `scd4x.c:891` the driver defines a macro
that the build system instantiates once per matching node:

```c
#define SCD4X_INIT(inst, scd4x_model)                                     \
	static struct scd4x_data scd4x_data_##scd4x_model##_##inst;       \
	static const struct scd4x_config scd4x_config_##scd4x_model##_##inst = { \
		.bus  = I2C_DT_SPEC_INST_GET(inst),                       \
		.model = scd4x_model,                                     \
		.mode = DT_INST_ENUM_IDX_OR(inst, mode, SCD4X_MODE_NORMAL), \
	};                                                                \
	SENSOR_DEVICE_DT_INST_DEFINE(inst, scd4x_init, NULL, ...,         \
	                             &scd4x_api_funcs);

#define DT_DRV_COMPAT sensirion_scd40
DT_INST_FOREACH_STATUS_OKAY_VARGS(SCD4X_INIT, SCD4X_MODEL_SCD40)
```

`DT_INST_FOREACH_STATUS_OKAY_VARGS` means: *for every devicetree node with
`compatible = "sensirion,scd40"` and `status = "okay"`, expand this macro.* One node in
your overlay → one expansion → one `struct device`. Delete the node and the driver
compiles to nothing.

Note `.mode`: the binding `sensirion,scd40.yaml` doesn't declare a `mode` property, so
`DT_INST_ENUM_IDX_OR` falls back to its default, **`SCD4X_MODE_NORMAL`** — periodic
measurement, one reading roughly every 5 s. That default matters in §8.

Two more things fall out of that macro:

- **`POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY`** — when `scd4x_init()` runs during boot.
  Sensors initialise after the kernel and after buses, so the I²C controller is ready
  when the sensor driver first talks to it.
- **`&scd4x_api_funcs`** — the function table, which is the whole subject of §3.

### 2.3 Getting the handle in your code

```c
scd40 = DEVICE_DT_GET(DT_NODELABEL(scd40));   /* main.cpp:601 */

if (!device_is_ready(scd40)) {                /* main.cpp:603 */
	/* driver init failed — the sensor did not answer on the bus */
}
```

`DT_NODELABEL(scd40)` refers to the label before the colon in the overlay.
`DEVICE_DT_GET` resolves it to the `struct device` at **compile time** — no lookup, no
string comparison, and a build error rather than a null pointer if the node is missing.

`device_is_ready()` asks whether `scd4x_init()` succeeded. It commonly fails when the
sensor is unpowered or miswired, so it's worth checking rather than assuming.

---

## 3. The API vtable

Here is the contract every sensor driver fills in
(`zephyr/include/zephyr/drivers/sensor.h:494`):

```c
__subsystem struct sensor_driver_api {
	sensor_attr_set_t     attr_set;      /* optional */
	sensor_attr_get_t     attr_get;      /* optional */
	sensor_trigger_set_t  trigger_set;   /* optional */
	sensor_sample_fetch_t sample_fetch;  /* MANDATORY */
	sensor_channel_get_t  channel_get;   /* MANDATORY */
	sensor_get_decoder_t  get_decoder;   /* optional — RTIO, see §7 */
	sensor_submit_t       submit;        /* optional — RTIO, see §7 */
};
```

If you've met C++ virtual functions, this is the same idea done manually: a struct of
function pointers, one instance per driver, reached through `dev->api`. C has no
`virtual`, so Zephyr writes the vtable out by hand.

The SCD-40 driver fills in exactly four (`scd4x.c:885`):

```c
static const struct sensor_driver_api scd4x_api_funcs = {
	.sample_fetch = scd4x_sample_fetch,
	.channel_get  = scd4x_channel_get,
	.attr_set     = scd4x_attr_set,
	.attr_get     = scd4x_attr_get,
};
```

**No `trigger_set`, no `submit`, no `get_decoder`.** Those three omissions each have
consequences later — §6 and §7.

The public function you call is a thin inline wrapper that dereferences the table
(`sensor.h:911`):

```c
static inline int z_impl_sensor_sample_fetch(const struct device *dev)
{
	const struct sensor_driver_api *api =
		(const struct sensor_driver_api *)dev->api;
	return api->sample_fetch(dev, SENSOR_CHAN_ALL);
}
```

So `sensor_sample_fetch(scd40)` compiles down to roughly one indirect call. The
abstraction costs a pointer dereference, not a layer of dispatch logic.

> **Aside — `__syscall` and `z_impl_`.** The odd naming exists because Zephyr supports
> user-mode threads with memory protection. In that build the call becomes a real system
> call with argument validation; in a normal single-privilege build like ours the macro
> collapses to a direct call to `z_impl_sensor_sample_fetch`. You can ignore it.

---

## 4. The fetch/get split — the central design decision

This is the piece worth understanding properly, because it looks redundant until you see
why.

Reading a sensor is **two calls, not one**:

```c
sensor_sample_fetch(scd40);                                /* bus traffic */
sensor_channel_get(scd40, SENSOR_CHAN_CO2, &co2);          /* memory read */
sensor_channel_get(scd40, SENSOR_CHAN_AMBIENT_TEMP, &temp);/* memory read */
sensor_channel_get(scd40, SENSOR_CHAN_HUMIDITY, &hum);     /* memory read */
```

That's `main.cpp:471-479`. The division of labour:

- **`sample_fetch`** performs the I²C transaction and stores raw values into `dev->data`.
  It converts nothing.
- **`channel_get`** touches no bus at all. It reads `dev->data` and applies the
  conversion maths.

You can see both halves in the driver. `scd4x_read_sample` (`scd4x.c:125`) does the bus
work and stores three raw 16-bit numbers:

```c
scd4x_write_command(dev, SCD4X_CMD_READ_MEASUREMENT);
scd4x_read_reg(dev, rx_data, sizeof(rx_data));   /* 9 bytes, CRC checked inside */

data->co2_sample  = sys_get_be16(rx_data);
data->temp_sample = sys_get_be16(&rx_data[3]);
data->humi_sample = sys_get_be16(&rx_data[6]);
```

And `scd4x_channel_get` (`scd4x.c:629`) converts, with no bus access anywhere in it:

```c
case SENSOR_CHAN_AMBIENT_TEMP:
	tmp_val = data->temp_sample * SCD4X_MAX_TEMP;          /* 175 */
	val->val1 = (int32_t)(tmp_val / 0xFFFF) + SCD4X_MIN_TEMP;  /* −45 */
	val->val2 = ((tmp_val % 0xFFFF) * 1000000) / 0xFFFF;
	break;
```

That is the datasheet formula `T = −45 + 175 × (raw / 65535)`, done in integer
arithmetic. It's the fifth line of §1's hand-rolled version, now living where it belongs.

### Why split it

**Coherence.** The SCD-40 returns CO₂, temperature and humidity in *one* 9-byte response.
Three `channel_get` calls decode three slices of that single snapshot, so all three
readings describe the same instant. If reading were one fused call per channel, you'd
either do three bus round-trips — three different moments — or need a special
"read-everything" API that every driver would have to implement differently.

**Cost visibility.** One function is slow and can fail on the bus; the other is a few
integer operations. Keeping them separate makes it obvious where the I²C traffic is —
which is what made it safe to move the sensor read onto its own thread.

**Selective reads.** `sensor_sample_fetch_chan()` lets a driver fetch only one channel
when the hardware supports partial reads, without changing how you retrieve values.

The header states the guarantee explicitly (`sensor.h:957`): two successive
`channel_get` calls return the same value unless you `sample_fetch` in between. Values
are latched, not live.

---

## 5. `struct sensor_value` — why not just a float

```c
struct sensor_value {
	int32_t val1;  /* integer part */
	int32_t val2;  /* millionths */
};
```

The value is `val1 + val2 × 10⁻⁶`. So 22.41 °C is `{22, 410000}`.

### Why

Zephyr targets range from Cortex-M0 upward, and **most of that range has no FPU**. On
those parts every float operation is a software routine — tens of cycles, plus the
library linked into flash. Doubles are worse. A sensor API built on `float` would impose
that cost on every driver on every target.

Two `int32_t`s are free everywhere. This is fixed-point arithmetic with a fixed scale of
10⁻⁶ — enough precision for any physical sensor, and range up to ±2 billion in the
integer part.

Your board (STM32H753, Cortex-M7) *does* have an FPU, so this is a cost you personally
aren't paying. The API is designed for the whole family, not for the best case.

### The sign rule — the one real trap

**Both fields carry the sign.** From the header comment (`sensor.h:42`):

```
 0.5  →  val1 =  0, val2 =  500000
−0.5  →  val1 =  0, val2 = −500000
−1.0  →  val1 = −1, val2 =  0
−1.5  →  val1 = −1, val2 = −500000
```

−1.5 is **not** `{−2, 500000}`. Don't normalise it like a borrow-carry — the magnitude
lives in `|val1| + |val2|×10⁻⁶` and the sign is replicated. This will matter the first
time this bench sees a sub-zero temperature.

### Getting back to a normal number

```c
double d = sensor_value_to_double(&temp);   /* sensor.h:1396 */
float  f = sensor_value_to_float(&temp);    /* sensor.h:1407 */
```

`main.cpp:481` uses these because the protobuf schema declares `float` fields. That's the
right place to leave fixed-point behind: at the boundary where the value stops being a
sensor reading and starts being wire data. See
[`protobuf-guide.md`](protobuf-guide.md) for what happens to it next.

There are also unit helpers in the same region of the header — `sensor_ms2_to_g()`,
`sensor_rad_to_degrees()` and friends — irrelevant here but the same pattern.

---

## 6. Channels, attributes, triggers

### 6.1 Channels — *what* you're measuring

A channel is a named physical quantity **with a fixed unit fixed by the API, not by the
driver**. `SENSOR_CHAN_AMBIENT_TEMP` is always °C. `SENSOR_CHAN_HUMIDITY` is always %RH.
`SENSOR_CHAN_CO2` is always ppm. Every driver in the tree obeys this, which is what makes
sensors interchangeable — a different CO₂ sensor reports the same channel in the same
unit.

`scd4x_channel_get` handles its three and returns `-ENOTSUP` for everything else. Asking
an SCD-40 for `SENSOR_CHAN_ACCEL_X` is not a crash; it's an error code.

`SENSOR_CHAN_ALL` is the wildcard passed to `sample_fetch` meaning "refresh everything."

### 6.2 Attributes — knobs that aren't measurements

`attr_set`/`attr_get` cover configuration: sample rate, thresholds, calibration. Generic
ones exist (`SENSOR_ATTR_SAMPLING_FREQUENCY`, `SENSOR_ATTR_FULL_SCALE`), and drivers may
define their own. The SCD-40 defines a full set (`scd4x.c:682`):

| Attribute | Effect |
|---|---|
| `SENSOR_ATTR_SCD4X_TEMPERATURE_OFFSET` | Compensates the sensor's own self-heating |
| `SENSOR_ATTR_SCD4X_SENSOR_ALTITUDE` | Altitude for pressure compensation |
| `SENSOR_ATTR_SCD4X_AMBIENT_PRESSURE` | Direct pressure compensation (overrides altitude) |
| `SENSOR_ATTR_SCD4X_AUTOMATIC_CALIB_ENABLE` | Automatic self-calibration on/off |
| `SENSOR_ATTR_SCD4X_SELF_CALIB_INITIAL_PERIOD` | ASC initial period |
| `SENSOR_ATTR_SCD4X_SELF_CALIB_STANDARD_PERIOD` | ASC standard period |

Pressure compensation is real physics, not a nicety: NDIR CO₂ measurement depends on gas
density, so the reading drifts with altitude and weather.

These live in `enum sensor_attribute_scd4x` and the driver casts the generic enum to it
(`scd4x.c:669`) — the standard escape hatch for vendor-specific attributes. Values start
above the generic range so they can't collide.

### 6.3 Triggers — and the one this driver doesn't have

`sensor_trigger_set()` registers a callback fired by a hardware event: data-ready,
threshold crossed, tap detected. It's how you get interrupt-driven reads instead of
polling — you stop asking "is there data yet" and let the sensor say so.

**The in-tree `scd4x` driver does not implement it.** There is no `.trigger_set` in its
API table.

This bears directly on the open question in `CLAUDE.md` — whether telemetry should be
triggered by the SCD-40's data-ready signal instead of a timed poll. The chip does have
that capability (`get_data_ready_status`, and the driver calls it internally at
`scd4x.c:604`), but it isn't exposed through the trigger API. Using it would mean either
polling the status yourself over I²C or adding `trigger_set` to the upstream driver.

So the timed poll isn't a shortcut past an available feature — **the feature isn't
plumbed through**. Worth recording if that decision gets revisited.

---

## 7. The shell — and the RTIO detour

### 7.1 What you get

`CONFIG_SENSOR_SHELL=y` registers one root command (`sensor_shell.c:1147`) with these
subcommands:

| Command | Purpose |
|---|---|
| `sensor get <dev> [chan...]` | Read channels; **all** channels if none named |
| `sensor attr_set <dev> <chan> <attr> <value>` | Write an attribute |
| `sensor attr_get <dev> [<chan> <attr>...]` | Read attributes |
| `sensor info` | Vendor/model for all sensors (needs `CONFIG_SENSOR_INFO`) |
| `sensor trig <dev> <on\|off> <trigger>` | Enable a trigger — `data_ready` only |
| `sensor stream <dev> ...` | Continuous FIFO-backed streaming (needs `CONFIG_SENSOR_SHELL_STREAM`) |

On this bench, over `./scripts/console.sh`:

```
uart:~$ sensor get scd40@62 co2 ambient_temp humidity
```

Each channel prints one line in this shape (`sensor_shell.c:487`, format
`PRIsensor_q31_data` from `sensor_data_types.h:143`):

```
channel type=<n>(<name>) index=0 shift=<s> num_samples=1 value=<timestamp>ns (<reading>)
```

Note the layout: the number before `ns` is a **timestamp**, not the measurement — the
reading is the value in parentheses. `shift` is the q31 fixed-point exponent the decoder
used. Both fields come from the RTIO representation described in §7.2, not from anything
the SCD-40 reports. *(Shape taken from the format strings, not captured from this bench —
run it to see live values.)*

The device name `scd40@62` is the **devicetree node name** — label `scd40`, unit address
`0x62`. Not the label alone. Both device and channel names tab-complete; the command
table wires up dynamic completion via `&dsub_device_name` (`sensor_shell.c:1131`).

Channel names are the enum names lowercased with `SENSOR_CHAN_` stripped:
`SENSOR_CHAN_AMBIENT_TEMP` → `ambient_temp`. The table is at `sensor_shell.c:52`.

Since `attr_set` is implemented, you can also tune the chip live:

```
uart:~$ sensor attr_set scd40@62 co2 scd4x_sensor_altitude 50
```

### 7.2 Why your `prj.conf` comment mentions RTIO

The comment on `CONFIG_SENSOR_SHELL` in [`firmware/prj.conf`](../firmware/prj.conf) flags
something non-obvious. Here's the full story.

Zephyr is migrating sensors toward a second, asynchronous API built on **RTIO** — a
submit/complete queue model, like `io_uring` for peripherals. Instead of a blocking
`sample_fetch`, you submit a read request; the result lands in a buffer, and a **decoder**
interprets the raw bytes later. It exists for high-rate sensors with hardware FIFOs, where
blocking per-sample is untenable.

The classic API (`sample_fetch`/`channel_get`) and the RTIO API (`submit`/`get_decoder`)
coexist today. Drivers may implement either or both.

Now the chain:

```
CONFIG_SENSOR_SHELL=y
    ↓  select SENSOR_ASYNC_API          (drivers/sensor/Kconfig:37)
    ↓  select CBPRINTF_FP_SUPPORT       (drivers/sensor/Kconfig:36)
```

`select` in Kconfig is a *forced* enable — you get RTIO whether or not you asked for it,
because **the shell is written exclusively against the RTIO path**. There is no classic
code path in `sensor_shell.c`.

But `scd4x` implements neither `submit` nor `get_decoder`. So how does `sensor get` work?

The bridge is `drivers/sensor/default_rtio_sensor.c`. When a driver lacks RTIO ops, the
subsystem substitutes `__sensor_default_decoder` and a default submit handler that calls
`sample_fetch` and `channel_get` underneath, packing results into an RTIO buffer for the
shell to decode.

**The net effect:** the shell works; the same two driver functions run; you pay some flash
for a compatibility shim you never asked for. That's what the `prj.conf` comment records —
so a future reader doesn't wonder why an RTIO symbol appears in a build that uses none of
it.

It also explains the shell's verbose output format (`shift`, `num_samples`, that odd
`value=...ns`): those fields come from the RTIO decoder's generic representation, not from
anything the SCD-40 reports.

### 7.3 What it costs

`CBPRINTF_FP_SUPPORT` pulls floating-point formatting into `printf`; RTIO adds its own
machinery. Fine on an STM32H753 with 2 MB of flash — worth remembering on a tighter part.

### 7.4 Why keep it enabled

The shell gives you a **ground truth independent of your application logic**. When
telemetry looks wrong on the Pi, one console command settles where the fault is:

```
uart:~$ sensor get scd40@62 co2 ambient_temp humidity
```

Plausible reading → sensor and I²C are fine, so the bug is downstream: protobuf encode,
MQTT publish, or host decode. Error or nonsense → the fault is at or below the driver.

One command bisects the entire pipeline. That's worth the flash, and it became *more*
valuable once zbus inserted another hop between the sensor read and the publish: the
shell reads the driver directly, bypassing the bus, so a healthy `sensor get` alongside
absent telemetry points at the channel rather than the chip.

---

## 8. Gotchas worth knowing

### 8.1 `sample_fetch` can succeed without new data

This is the sharpest edge in the whole path. In `SCD4X_MODE_NORMAL` — your mode, per §2.2
— `scd4x_sample_fetch` checks readiness first (`scd4x.c:604`):

```c
ret = scd4x_data_ready(dev, &is_data_ready);
if (ret < 0) { ... return ret; }
if (!is_data_ready) {
	return 0;          /* ← success, but nothing was read */
}
```

**It returns 0 — success — without refreshing `dev->data`.** Your subsequent
`channel_get` calls then hand back the *previous* sample, with no indication anything is
stale.

Concretely: the SCD-40 in periodic mode produces a reading about every 5 s. Poll it every
2 s and roughly three in five polls return the previous value while reporting success.
Your current 5 s interval sits right at the sensor's own cadence, so it mostly lines up —
but `SetInterval` can retune that interval at runtime, and dropping it below ~5 s
silently produces duplicate readings that look like a sensor stuck at a constant value.

Two ways to handle it, if it matters:

- Keep the publish interval at or above the sensor's ~5 s period.
- Track whether values changed, or read `get_data_ready_status` explicitly.

Either way this is a **driver-level surprise**, not something the sensor API's contract
would have told you. It's a good argument for reading a driver's `sample_fetch` before
depending on its timing.

### 8.2 The zero-CO₂ warm-up

`main.cpp:486` has:

```c
/* The SCD-40 reports 0 ppm until its first conversion completes. */
msg.sensor_status = (msg.co2_ppm == 0)
			? node_SensorStatus_SENSOR_STATUS_WARMING_UP
			: node_SensorStatus_SENSOR_STATUS_OK;
```

The first periodic measurement takes ~5 s after power-up, and the chip reports 0 ppm until
then. 0 ppm is physically impossible in air (outdoor baseline is ~420 ppm), so it's a safe
sentinel — but note it's *this application's* interpretation, not something the sensor API
defines.

### 8.3 Bare `sensor get` is noisy

With no channel names, `cmd_get_sensor` loops over **every channel type in the enum**
(`sensor_shell.c:575`) and asks the device for each. The SCD-40 answers three and returns
`-ENOTSUP` for the rest, so errors scroll past. Name the channels explicitly.

### 8.4 One shell read at a time

`cmd_get_sensor` takes a mutex with `K_NO_WAIT` (`sensor_shell.c:554`):

```
Another sensor reading in progress
```

It fails fast rather than blocking. Expect this if a shell read overlaps a triggered or
streaming read.

### 8.5 The application ignores `channel_get` return values

`main.cpp:477-479` doesn't check the return of the three `channel_get` calls. That's
defensible here — the channels are known-supported and the call can't touch the bus — but
it does mean an unexpected `-ENOTSUP` would leave `co2`/`temp`/`hum` uninitialised. Worth
knowing it's a deliberate simplification rather than an oversight.

---

## 9. The whole path, end to end

What actually happens when `main.cpp` reads the sensor:

```
main.cpp:471   sensor_sample_fetch(scd40)
                 └─ sensor.h:911   api->sample_fetch(dev, SENSOR_CHAN_ALL)
                      └─ scd4x.c:571   scd4x_sample_fetch()
                           ├─ scd4x_data_ready()      → I²C: cmd 0xE4B8
                           │    └─ not ready? return 0, data unchanged   (§8.1)
                           └─ scd4x_read_sample()     → I²C: cmd 0xEC05, read 9 B
                                ├─ verify three CRC-8s
                                └─ store 3 raw uint16 into dev->data

main.cpp:477   sensor_channel_get(scd40, SENSOR_CHAN_CO2, &co2)
                 └─ sensor.h:977   api->channel_get(dev, chan, val)
                      └─ scd4x.c:629   scd4x_channel_get()
                           └─ read dev->data, apply datasheet formula
                              → struct sensor_value {val1, val2}     (no bus access)

main.cpp:481   sensor_value_to_double(&co2)  → double  → protobuf field
```

Three layers, each with one job: **application** (what to do with a reading), **subsystem**
(a uniform vocabulary of channels and units), **driver** (this chip's commands, CRCs and
constants).

---

## 10. Cheat sheet

```c
/* Get the device (compile-time; errors at build if the node is missing) */
const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(scd40));
if (!device_is_ready(dev)) { /* driver init failed */ }

/* Read: one fetch, N gets */
int rc = sensor_sample_fetch(dev);            /* bus traffic; check this */
struct sensor_value v;
sensor_channel_get(dev, SENSOR_CHAN_CO2, &v); /* memory only */

/* Convert out at the boundary */
double ppm = sensor_value_to_double(&v);      /* v.val1 + v.val2 * 1e-6 */

/* Configure */
struct sensor_value alt = { .val1 = 50, .val2 = 0 };
sensor_attr_set(dev, SENSOR_CHAN_CO2,
                (enum sensor_attribute)SENSOR_ATTR_SCD4X_SENSOR_ALTITUDE, &alt);
```

```sh
# On the console (./scripts/console.sh — quit with Ctrl-A then K)
sensor get scd40@62 co2 ambient_temp humidity   # name channels; bare get is noisy
sensor attr_get scd40@62 co2 scd4x_sensor_altitude
sensor info                                      # needs CONFIG_SENSOR_INFO
```

| Remember | Because |
|---|---|
| Fetch once, get many | One bus read; all channels share one instant |
| `channel_get` never touches the bus | It only converts what `sample_fetch` latched |
| Both `sensor_value` fields carry the sign | −1.5 is `{−1, −500000}`, not `{−2, 500000}` |
| Channel units are fixed by the API | °C, %RH, ppm — the same across every driver |
| `sample_fetch` may return 0 with stale data | Periodic mode + no new sample (§8.1) |
| `scd4x` has no `trigger_set` | Data-ready interrupts aren't available through the API |
| `SENSOR_SHELL` forces RTIO on | It's shell-only; the SCD-40 path still runs classic ops |

---

## Where to go next

- **[`docs/sensor-bringup.md`](../docs/sensor-bringup.md)** — the reference half: what was
  built, the overlay, the Kconfig, verified bench facts.
- **`../../shared_refs/sensor/SCD4x.yaml`** — chip-level truth: command codes, conversion
  formulas, CRC-8 parameters. Reach for it when debugging the sensor path.
- **[`protobuf-guide.md`](protobuf-guide.md)** — what happens to the reading
  after `sensor_value_to_double`.
- **[`zbus-guide.md`](zbus-guide.md)** — the sensor read now lives on its own thread and
  reaches the publisher over a channel. §4's fetch/get separation is exactly the seam
  that split runs along.
