/* Sensor thread: reads the Adafruit SCD-40 on its own cadence and publishes each
 * reading to the chan_telemetry zbus channel.
 *
 * This file knows nothing about MQTT, protobuf or the network — that is the
 * point of the split. Before zbus, the sensor read and the MQTT publish shared
 * one loop in main.cpp, so a reconnect backoff also stopped sampling and the
 * sample period had to be juggled against the keepalive deadline in the same
 * timeout calculation. Now each side has one job and one clock.
 *
 * The module owns both channels (see app_channels.h): the readings it produces,
 * and the commands that retune it. Owning chan_sensor_cmd is what lets the
 * period bounds be enforced here, by a zbus *validator*, instead of being
 * re-checked by every would-be publisher.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include "app_channels.h"

LOG_MODULE_REGISTER(node_sensor, LOG_LEVEL_INF);

/* Defined at the bottom of the file, next to the channels; declared here so the
 * thread below can wait on it. */
ZBUS_OBS_DECLARE(sensor_cmd_sub);

namespace {

constexpr size_t kSensorStackSize = 2048;
/* Lower priority (higher number) than main: the MQTT thread should win when
 * both are runnable, since it is the one with a keepalive deadline to meet. */
constexpr int kSensorPriority = 7;

const struct device *const scd40 = DEVICE_DT_GET(DT_NODELABEL(scd40));

/* How long to wait before initialising the sensor.
 *
 * The SCD-40 needs up to 30 ms after VDD crosses its threshold before it will
 * answer on I2C at all (`timing_specifications.power_up_time` in
 * ../../shared_refs/sensor/SCD4x.yaml). The board and the sensor share a rail,
 * so on a cold plug-in the MCU can reach the driver's POST_KERNEL init while the
 * chip is still powering up. Every one of scd4x_init()'s four transfers would
 * then NACK.
 *
 * That failure is unrecoverable, which is why it is worth avoiding rather than
 * detecting: do_device_init() sets `initialized` even when init returns an
 * error, so device_is_ready() latches false; device_init() then answers
 * -EALREADY, and device_deinit() answers -ENOTSUP because the scd4x driver
 * registers no deinit op. There is no second attempt to be had.
 *
 * So the devicetree marks the node `zephyr,deferred-init`, the boot sweep skips
 * it (kernel/init.c), and we initialise it here instead — late enough that the
 * one attempt we get is one that can succeed. 100 ms is a wide margin on 30 ms
 * and costs nothing: the first conversion is ~5 s away regardless. */
constexpr int kSensorPowerUpMs = 100;

/* Reject a command before it ever reaches the channel. zbus_chan_pub() returns
 * -ENOMSG when this returns false, and the message is not stored or delivered —
 * so the bounds cannot be bypassed, whoever publishes. */
bool sensor_cmd_valid(const void *msg, size_t msg_size)
{
	ARG_UNUSED(msg_size);

	/* The adapter; the rule itself is sensor_cmd_in_range() in
	 * app_channels.h, next to the bounds it enforces. */
	return sensor_cmd_in_range(static_cast<const struct sensor_cmd *>(msg));
}

/* Fill `out` from the sensor. A failed read is reported, not hidden: the host
 * cannot tell a broken sensor from a dead node if we simply go quiet, which is
 * why the status field exists at all. */
void read_scd40(bool device_ok, struct sensor_reading *out)
{
	if (!device_ok) {
		out->status = SENSOR_READING_ERROR;
		return;
	}

	int rc = sensor_sample_fetch(scd40);

	if (rc != 0) {
		LOG_ERR("sample_fetch failed: %d", rc);
		out->status = SENSOR_READING_ERROR;
		return;
	}

	struct sensor_value co2, temp, hum;

	sensor_channel_get(scd40, SENSOR_CHAN_CO2, &co2);
	sensor_channel_get(scd40, SENSOR_CHAN_AMBIENT_TEMP, &temp);
	sensor_channel_get(scd40, SENSOR_CHAN_HUMIDITY, &hum);

	out->co2_ppm = static_cast<uint32_t>(sensor_value_to_double(&co2));
	out->temperature_c = static_cast<float>(sensor_value_to_double(&temp));
	out->humidity_rh = static_cast<float>(sensor_value_to_double(&hum));

	/* The SCD-40 reports 0 ppm until its first conversion completes. */
	out->status = (out->co2_ppm == 0) ? SENSOR_READING_WARMING_UP : SENSOR_READING_OK;
}

void sensor_thread(void *, void *, void *)
{
	/* Deferred init: the boot sweep left this device alone, so we own the one
	 * attempt. Wait out the power-up window first — see kSensorPowerUpMs. */
	k_msleep(kSensorPowerUpMs);

	int init_rc = device_init(scd40);

	if (init_rc != 0) {
		LOG_ERR("SCD-40 init failed: %d", init_rc);
	}

	/* Deliberately read once and kept const. Re-reading it per cycle would look
	 * like resilience and provide none: readiness is latched at the first init
	 * attempt and nothing in the driver can clear it (see kSensorPowerUpMs). */
	const bool device_ok = device_is_ready(scd40);

	if (!device_ok) {
		/* Keep sampling anyway. An explicit ERROR every period says "node
		 * alive, sensor dead"; silence would be indistinguishable from a
		 * node that fell off the network. */
		LOG_ERR("SCD-40 not ready — publishing ERROR readings");
	} else {
		LOG_INF("SCD-40 online");
	}

	uint32_t sequence = 0;
	uint32_t period_ms = SAMPLE_PERIOD_DEFAULT_MS;

	while (true) {
		struct sensor_reading reading = {};

		reading.sequence = sequence++;
		reading.uptime_ms = static_cast<uint32_t>(k_uptime_get());
		read_scd40(device_ok, &reading);

		/* Copies the reading into the channel and runs its observers.
		 * Nothing here waits for, or even knows about, a consumer: if the
		 * MQTT thread is mid-reconnect the reading is simply overwritten
		 * by the next one, and the sequence gap tells the host so. */
		int rc = zbus_chan_pub(&chan_telemetry, &reading, K_MSEC(50));

		if (rc != 0) {
			LOG_WRN("chan_telemetry publish failed: %d", rc);
		}

		/* Wait out the sample period, but wake early for a command. This
		 * one blocking call is the whole event loop: both the clock and
		 * the bus can end it. Note the cadence re-bases on each sample,
		 * so a trigger shifts the following samples rather than being
		 * squeezed in between two of them. */
		const int64_t last_sample = k_uptime_get();

		while (true) {
			int64_t left = (last_sample + period_ms) - k_uptime_get();

			if (left <= 0) {
				break;
			}

			const struct zbus_channel *chan;
			struct sensor_cmd cmd;

			/* A message subscriber gets its own *copy* of every
			 * message, so two commands arriving back to back are
			 * both delivered — unlike the latest-wins read the MQTT
			 * side does on chan_telemetry. */
			if (zbus_sub_wait_msg(&sensor_cmd_sub, &chan, &cmd, K_MSEC(left)) != 0) {
				break; /* timed out: the period elapsed */
			}

			/* Exhaustive on purpose: with no `default`, a new command
			 * kind is a -Wswitch warning here rather than something
			 * that silently lands in the set-interval path. The flag
			 * is needed because a `break` inside the switch would
			 * leave the switch, not this wait loop. */
			bool sample_now = false;

			switch (cmd.kind) {
			case SENSOR_CMD_TRIGGER:
				LOG_INF("out-of-cadence measurement requested");
				sample_now = true;
				break;
			case SENSOR_CMD_SET_INTERVAL:
				/* Validated on publish, so it is in range by
				 * construction. The loop recomputes `left` from the
				 * new period, which is what makes a shortened
				 * interval take effect at once. */
				period_ms = cmd.interval_ms;
				LOG_INF("sample period set to %u ms", period_ms);
				break;
			}

			if (sample_now) {
				break;
			}
		}
	}
}

}  // namespace

/* Definitions live at global scope: ZBUS_CHAN_DECLARE in the header declares
 * these same symbols, and an anonymous namespace would give them a different
 * linkage than the declaration. */

/* The command channel's observer. The single argument is just the symbol name;
 * it expands to a k_fifo, the observer struct, and an enabled flag (this form
 * defaults to enabled — ZBUS_MSG_SUBSCRIBER_DEFINE_WITH_ENABLE takes it
 * explicitly).
 *
 * A message subscriber is handed its own *copy* of every message, which is what
 * lets zbus_sub_wait_msg() above return a command that arrived while this thread
 * was busy sampling. Those copies come from a fixed net_buf pool sized in
 * prj.conf (CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_POOL_SIZE and _STATIC_DATA_SIZE),
 * not the heap — same no-allocator policy as nanopb. */
ZBUS_MSG_SUBSCRIBER_DEFINE(sensor_cmd_sub);

/* Sensor -> MQTT. ZBUS_CHAN_DEFINE takes six arguments, in order:
 *
 *   1 name       the symbol. ZBUS_CHAN_DECLARE in app_channels.h declares this
 *                same one, which is why the definition is at global scope.
 *   2 type       the message type. The channel stores exactly one instance,
 *                statically: publishing *copies* into it, so there is no queue
 *                and no allocation, and an undelivered reading is overwritten
 *                rather than backing up. Latest wins, deliberately.
 *   3 validator  bool (*)(const void *msg, size_t msg_size), called inside
 *                zbus_chan_pub() before the message is stored.
 *   4 user data  a void * carried on the channel; zbus never reads it. It is
 *                for observers shared between channels that need to tell which
 *                one they were invoked for.
 *   5 observers  who is notified, in the order listed — that order *is* the
 *                notification priority.
 *   6 init val   the channel's contents before anything is published.
 */
ZBUS_CHAN_DEFINE(chan_telemetry, struct sensor_reading,
		 nullptr,                            /* no validator: every reading is legal,
						      * including an ERROR one — that is a fact
						      * to report, not a value to reject */
		 nullptr,                            /* no user data */
		 ZBUS_OBSERVERS(telemetry_listener), /* defined in main.cpp */
		 ZBUS_MSG_INIT(0));                  /* zeroed; the first sample overwrites it */

/* MQTT -> sensor. Same six arguments; the two that differ carry the weight:
 *
 *   validator  sensor_cmd_valid — the reason the period bounds cannot be
 *              bypassed. zbus_chan_pub() returns -ENOMSG and stores nothing
 *              when it returns false, so commands.cpp never re-checks them.
 *   observers  sensor_cmd_sub, the message subscriber above, so two commands
 *              arriving back to back are both delivered rather than collapsed.
 *
 * ZBUS_MSG_INIT's own arguments are ordinary positional initialisers for the
 * message struct — {.kind, .interval_ms} here — not a zbus concept. This value
 * is only ever the channel's *initial contents*: defining a channel notifies
 * nobody, so the sensor thread never receives it. It matches the period the
 * thread starts from on purpose, so a reader of either one is not misled about
 * the other. */
ZBUS_CHAN_DEFINE(chan_sensor_cmd, struct sensor_cmd, sensor_cmd_valid, nullptr,
		 ZBUS_OBSERVERS(sensor_cmd_sub),
		 ZBUS_MSG_INIT(SENSOR_CMD_SET_INTERVAL, SAMPLE_PERIOD_DEFAULT_MS));

/* Starts the sensor thread during kernel boot. K_THREAD_DEFINE's nine
 * arguments, in order:
 *
 *   sensor_tid        the thread id symbol (a k_tid_t), for later k_thread_*
 *                     calls. Nothing needs it here; the thread runs untouched.
 *   kSensorStackSize  stack size in bytes, reserved statically.
 *   sensor_thread     the entry function.
 *   NULL, NULL, NULL  its three void * parameters, unused — this thread takes
 *                     all of its input from the channels instead.
 *   kSensorPriority   a preemptible priority (>= 0), lower than main's by
 *                     design; see kSensorPriority for why.
 *   0                 thread options — K_ESSENTIAL, K_FP_REGS and friends.
 *                     None apply: this thread does no floating-point work in
 *                     an ISR, and its death should not panic the kernel.
 *   0                 start delay in ms. Zero means the kernel starts it during
 *                     boot; K_FOREVER would leave it suspended for an explicit
 *                     k_thread_start(). The SCD-40's power-up wait is *inside*
 *                     the thread (kSensorPowerUpMs) rather than expressed here,
 *                     so it delays only this thread and not the boot sweep.
 */
K_THREAD_DEFINE(sensor_tid, kSensorStackSize, sensor_thread, NULL, NULL, NULL,
		kSensorPriority, 0, 0);
