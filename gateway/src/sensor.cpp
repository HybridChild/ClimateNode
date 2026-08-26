/* Sensor thread: reads the Adafruit SCD-40 on its own cadence and publishes each
 * reading to the chan_telemetry zbus channel.
 *
 * This file knows nothing about MQTT, protobuf or the network. Sample the sensor
 * from the same loop that serves the socket and the two clocks become one: a
 * reconnect backoff would stop sampling. Across a channel, each side has one job
 * and one clock.
 *
 * The module owns both channels (see app_channels.h) -- the readings it produces
 * and the commands that retune it -- which is what lets the period bounds be
 * enforced by a zbus validator rather than re-checked by every publisher.
 * notes/sensor-api-guide.md and docs/sensor-bringup.md cover the SCD-40 itself.
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
 * The SCD-40 will not answer on I2C until up to 30 ms after its rail comes up,
 * and the driver gets exactly one attempt: a failed init latches
 * device_is_ready() false forever, with no deinit to retry through. So the
 * devicetree marks the node `zephyr,deferred-init`, the boot sweep skips it, and
 * this thread initialises it late enough for that attempt to succeed. 100 ms is
 * a wide margin and costs nothing -- the first conversion is ~5 s away anyway.
 * docs/sensor-bringup.md has the full reasoning. */
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

	uint32_t co2_ppm = static_cast<uint32_t>(sensor_value_to_double(&co2));

	/* The SCD-40 reports 0 ppm until its first conversion completes. */
	if (co2_ppm == 0) {
		out->status = SENSOR_READING_WARMING_UP;
		return;
	}

	/* All three come from one conversion, so they are valid together or not at
	 * all -- which is why the presence flags are set here and nowhere else. A
	 * warming-up or failed read leaves all four false, and the host sees "no
	 * measurement" rather than a confident 0 it cannot question. Note what this
	 * node never sets: has_pressure_pa. The SCD-40 has no barometer, and saying
	 * nothing is the honest report. */
	out->co2_ppm = co2_ppm;
	out->has_co2_ppm = true;
	out->temperature_c = static_cast<float>(sensor_value_to_double(&temp));
	out->has_temperature_c = true;
	out->humidity_rh = static_cast<float>(sensor_value_to_double(&hum));
	out->has_humidity_rh = true;

	out->status = SENSOR_READING_OK;
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

	/* Read once and kept const. Re-reading per cycle would look like resilience
	 * and provide none: readiness latches at the first init attempt. */
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

		/* Wait out the sample period, but wake early for a command. This one
		 * blocking call is the whole event loop: both the clock and the bus
		 * can end it. The cadence re-bases on each sample, so a trigger
		 * shifts the following samples rather than squeezing one in. */
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

			/* Redundant today: sensor_cmd_sub observes one channel, so
			 * `chan` is always &chan_sensor_cmd. Kept as the shape a
			 * second observed channel would need, since all of them feed
			 * one queue and no type tag travels with the message. */
			if (chan != &chan_sensor_cmd) {
				LOG_WRN("ignoring message from %s",
					zbus_chan_name(chan));
				continue; /* re-wait on the rest of the period */
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
 * linkage than the declaration. notes/language-cpp.md §6.
 *
 * The macros' arguments are laid out in notes/zbus-guide.md §3. */

ZBUS_MSG_SUBSCRIBER_DEFINE(sensor_cmd_sub);

/* Sensor -> MQTT. A listener, and no validator: every reading is legal,
 * including an ERROR one -- that is a fact to report, not a value to reject. */
ZBUS_CHAN_DEFINE(chan_telemetry, struct sensor_reading,
		 nullptr,
		 nullptr,
		 ZBUS_OBSERVERS(telemetry_listener), /* defined in main.cpp */
		 ZBUS_MSG_INIT(0));

/* MQTT -> sensor. The validator is what makes the period bounds unbypassable:
 * zbus_chan_pub() returns -ENOMSG and stores nothing when it rejects, so
 * commands.cpp never re-checks them. The message subscriber is what stops two
 * commands arriving back to back from being collapsed.
 *
 * ZBUS_MSG_INIT is only ever the channel's *initial contents* -- defining a
 * channel notifies nobody, so the sensor thread never receives this. It matches
 * the period the thread starts from so neither misleads a reader of the other. */
ZBUS_CHAN_DEFINE(chan_sensor_cmd, struct sensor_cmd,
		 sensor_cmd_valid,
		 nullptr,
		 ZBUS_OBSERVERS(sensor_cmd_sub),
		 ZBUS_MSG_INIT(SENSOR_CMD_SET_INTERVAL, SAMPLE_PERIOD_DEFAULT_MS));

/* Started during kernel boot. The SCD-40's power-up wait is *inside* the thread
 * (kSensorPowerUpMs) rather than expressed as a start delay here, so it holds up
 * only this thread and not the boot sweep. */
K_THREAD_DEFINE(sensor_tid, kSensorStackSize, sensor_thread, NULL, NULL, NULL,
		kSensorPriority, 0, 0);
