/* Sensor thread: reads the Adafruit BME280 on its own cadence and publishes each
 * reading to the chan_telemetry zbus channel.
 *
 * The gateway's firmware/src/sensor.cpp is the same file for a different sensor,
 * and the two are deliberately NOT shared. What they have in common -- the
 * channels, the period bounds, the reading struct -- is already shared, in
 * app_channels.h; what differs is every line below, because acquisition is
 * exactly the part that is per-sensor. Merging them would mean one file with two
 * sensors in it and a build-time switch, which is more coupling than either node
 * has reason to carry.
 *
 * The interesting difference is what each node can measure. The SCD-40 gives
 * CO2, temperature and humidity; the BME280 gives temperature, humidity and
 * pressure. Neither is a subset of the other, and neither has to lie about the
 * gap: struct sensor_reading carries a presence flag per measurement, and those
 * become proto3 `optional` fields at the wire boundary. See notes/protobuf-guide.md §5.
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

/* Smaller than the gateway's 2048: this thread's deepest frame is three
 * struct sensor_value plus a struct sensor_reading, and there is 16 KB of RAM
 * on this part in total. Raise it, do not guess, if a stack canary ever fires
 * (CONFIG_THREAD_ANALYZER is the tool). */
constexpr size_t kSensorStackSize = 1024;
/* Lower priority (higher number) than main, which owns the CAN link and has a
 * heartbeat deadline to meet. Same argument as the gateway, where main owns the
 * MQTT keepalive. */
constexpr int kSensorPriority = 7;

const struct device *const bme280 = DEVICE_DT_GET(DT_NODELABEL(bme280));

/* Fill `out` from the sensor. A failed read is reported, not hidden: the
 * gateway cannot tell a broken sensor from a dead node if this node simply goes
 * quiet, and the heartbeat would keep insisting it is fine. */
void read_bme280(bool device_ok, struct sensor_reading *out)
{
	if (!device_ok) {
		out->status = SENSOR_READING_ERROR;
		return;
	}

	int rc = sensor_sample_fetch(bme280);

	if (rc != 0) {
		LOG_ERR("sample_fetch failed: %d", rc);
		out->status = SENSOR_READING_ERROR;
		return;
	}

	struct sensor_value temp, hum, press;

	sensor_channel_get(bme280, SENSOR_CHAN_AMBIENT_TEMP, &temp);
	sensor_channel_get(bme280, SENSOR_CHAN_HUMIDITY, &hum);
	sensor_channel_get(bme280, SENSOR_CHAN_PRESS, &press);

	out->temperature_c = static_cast<float>(sensor_value_to_double(&temp));
	out->has_temperature_c = true;
	out->humidity_rh = static_cast<float>(sensor_value_to_double(&hum));
	out->has_humidity_rh = true;

	/* Pressure is converted by hand rather than through
	 * sensor_value_to_double(), and the reason is this part: the M0 has no
	 * FPU, so every float operation is a soft-float library call. The
	 * conversion is exact integer arithmetic instead.
	 *
	 * SENSOR_CHAN_PRESS is documented as kilopascals, with val2 the
	 * micro-kPa fraction (include/zephyr/drivers/sensor.h). So one whole
	 * pascal is val1 * 1000, and val2 / 1000 is the rest of it, with no
	 * rounding anywhere -- 1 uKPa is 1 mPa, well below the sensor's own
	 * accuracy. That exactness is also the argument for `uint32 pressure_pa`
	 * in the schema instead of a float: nothing in this path ever needed
	 * one. Atmospheric pressure is roughly 30 000-110 000 Pa, so a uint32 is
	 * ample and cannot overflow on anything the sensor can report. */
	out->pressure_pa = static_cast<uint32_t>(press.val1) * 1000U +
			   static_cast<uint32_t>(press.val2) / 1000U;
	out->has_pressure_pa = true;

	/* Never set: this node has no CO2 sensor, and has_co2_ppm stays false so
	 * the gateway's host sees an absence rather than a confident 0 ppm.
	 *
	 * Note also what is missing compared to the SCD-40: no warming-up state.
	 * The BME280 in normal mode has a conversion ready within milliseconds
	 * of init, so there is no window in which it answers with a placeholder,
	 * and SENSOR_READING_WARMING_UP is a status this node never publishes. */
	out->status = SENSOR_READING_OK;
}

void sensor_thread(void *, void *, void *)
{
	/* No deferred init and no power-up wait, unlike the gateway's SCD-40:
	 * this device came up in the boot sweep, so by the time this thread runs
	 * the answer is already known. */
	const bool device_ok = device_is_ready(bme280);

	if (!device_ok) {
		LOG_ERR("BME280 not ready — publishing ERROR readings");
	} else {
		LOG_INF("BME280 online");
	}

	uint32_t sequence = 0;
	uint32_t period_ms = SAMPLE_PERIOD_DEFAULT_MS;

	while (true) {
		struct sensor_reading reading = {};

		reading.sequence = sequence++;
		reading.uptime_ms = static_cast<uint32_t>(k_uptime_get());
		read_bme280(device_ok, &reading);

		int rc = zbus_chan_pub(&chan_telemetry, &reading, K_MSEC(50));

		if (rc != 0) {
			LOG_WRN("chan_telemetry publish failed: %d", rc);
		}

		/* Wait out the sample period, but wake early for a command --
		 * the same loop as the gateway's sensor thread, and for the same
		 * reason: one blocking call in which both the clock and the bus
		 * can end the wait. The commands arrive over CAN here instead of
		 * MQTT, but nothing below this line knows that, which is the
		 * point of the channel sitting between them. */
		const int64_t last_sample = k_uptime_get();

		while (true) {
			int64_t left = (last_sample + period_ms) - k_uptime_get();

			if (left <= 0) {
				break;
			}

			const struct zbus_channel *chan;
			struct sensor_cmd cmd;

			if (zbus_sub_wait_msg(&sensor_cmd_sub, &chan, &cmd, K_MSEC(left)) != 0) {
				break; /* timed out: the period elapsed */
			}

			if (chan != &chan_sensor_cmd) {
				continue;
			}

			bool sample_now = false;

			switch (cmd.kind) {
			case SENSOR_CMD_TRIGGER:
				LOG_INF("out-of-cadence measurement requested");
				sample_now = true;
				break;
			case SENSOR_CMD_SET_INTERVAL:
				/* In range by construction: the channel
				 * validator rejected anything else before it
				 * was stored. */
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

/* The zbus adapter around the shared rule in app_channels.h. The gateway's
 * sensor.cpp has these same two lines; what must not be duplicated is the
 * *rule*, and that lives in one place for both nodes. */
bool sensor_cmd_valid_impl(const void *msg, size_t msg_size)
{
	ARG_UNUSED(msg_size);
	return sensor_cmd_in_range(static_cast<const struct sensor_cmd *>(msg));
}

}  // namespace

/* Definitions at global scope: ZBUS_CHAN_DECLARE in app_channels.h declares
 * these same symbols, and an anonymous namespace would give them a different
 * linkage than the declaration. notes/language-cpp.md §6. */

ZBUS_MSG_SUBSCRIBER_DEFINE(sensor_cmd_sub);

/* Sensor -> CAN. A LISTENER, exactly as on the gateway: the channel stores one
 * reading and the newest overwrites the last, which is what "latest wins" means
 * and what telemetry wants. If the CAN link is mid-transfer when a sample lands,
 * the sample is dropped and the sequence gap says so.
 *
 * The observer differs from the gateway's, though, and the difference is
 * instructive. There it signals an eventfd, because the reader is parked in
 * zsock_poll() on a socket and a bus notification has to become a file
 * descriptor to be waited on alongside it. Here the reader is parked in
 * isotp_recv(), which takes a k_timeout and no descriptor, so a semaphore is
 * the whole bridge. Same channel, same observer kind, different signal -- the
 * transport below is what decides which. */
ZBUS_CHAN_DEFINE(chan_telemetry, struct sensor_reading,
		 nullptr,                            /* no validator: an ERROR reading is a
						      * fact to report, not a value to reject */
		 nullptr,                            /* no user data */
		 ZBUS_OBSERVERS(telemetry_listener), /* defined in main.cpp */
		 ZBUS_MSG_INIT(0));

/* CAN -> sensor. A MESSAGE SUBSCRIBER, so two commands arriving back to back are
 * both delivered rather than collapsed; a command has no next one coming. The
 * validator is what makes the period bounds unbypassable -- commands.cpp, which
 * both nodes share, never re-checks them. */
ZBUS_CHAN_DEFINE(chan_sensor_cmd, struct sensor_cmd,
		 sensor_cmd_valid_impl,
		 nullptr,
		 ZBUS_OBSERVERS(sensor_cmd_sub),
		 ZBUS_MSG_INIT(SENSOR_CMD_SET_INTERVAL, SAMPLE_PERIOD_DEFAULT_MS));

K_THREAD_DEFINE(sensor_tid, kSensorStackSize, sensor_thread, NULL, NULL, NULL,
		kSensorPriority, 0, 0);
