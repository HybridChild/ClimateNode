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

/* Reject a command before it ever reaches the channel. zbus_chan_pub() returns
 * -ENOMSG when this returns false, and the message is not stored or delivered —
 * so the bounds cannot be bypassed, whoever publishes. */
bool sensor_cmd_valid(const void *msg, size_t msg_size)
{
	ARG_UNUSED(msg_size);
	const struct sensor_cmd *cmd = static_cast<const struct sensor_cmd *>(msg);

	switch (cmd->kind) {
	case SENSOR_CMD_SET_INTERVAL:
		return cmd->interval_ms >= SAMPLE_PERIOD_MIN_MS &&
		       cmd->interval_ms <= SAMPLE_PERIOD_MAX_MS;
	case SENSOR_CMD_TRIGGER:
		return true;
	default:
		return false;
	}
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

			if (cmd.kind == SENSOR_CMD_TRIGGER) {
				LOG_INF("out-of-cadence measurement requested");
				break;
			}

			/* Validated on publish, so it is in range by construction.
			 * The loop recomputes `left` from the new period, which is
			 * what makes a shortened interval take effect at once. */
			period_ms = cmd.interval_ms;
			LOG_INF("sample period set to %u ms", period_ms);
		}
	}
}

}  // namespace

/* Definitions live at global scope: ZBUS_CHAN_DECLARE in the header declares
 * these same symbols, and an anonymous namespace would give them a different
 * linkage than the declaration. */

ZBUS_MSG_SUBSCRIBER_DEFINE(sensor_cmd_sub);

ZBUS_CHAN_DEFINE(chan_telemetry, struct sensor_reading,
		 nullptr,                            /* no validator: any reading is legal */
		 nullptr,                            /* user data */
		 ZBUS_OBSERVERS(telemetry_listener), /* defined in main.cpp */
		 ZBUS_MSG_INIT(0));

ZBUS_CHAN_DEFINE(chan_sensor_cmd, struct sensor_cmd, sensor_cmd_valid, nullptr,
		 ZBUS_OBSERVERS(sensor_cmd_sub),
		 /* Never delivered — just the channel's initial contents. */
		 ZBUS_MSG_INIT(SENSOR_CMD_SET_INTERVAL, SAMPLE_PERIOD_DEFAULT_MS));

K_THREAD_DEFINE(sensor_tid, kSensorStackSize, sensor_thread, NULL, NULL, NULL,
		kSensorPriority, 0, 0);
