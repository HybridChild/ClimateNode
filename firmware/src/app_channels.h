/* zbus channels shared between the sensor thread and the MQTT thread.
 *
 * zbus is an *in-process* pub/sub bus between threads on the MCU. Nothing here
 * touches the network — it is the internal decoupling that lets the sensor be
 * read on its own cadence while the MQTT client is busy connecting, blocked in
 * poll(), or backing off after a dropped link.
 *
 * The two channels deliberately use different observer styles, for the same
 * reasons the two MQTT topics use different QoS (docs/mqtt-design.md):
 *
 *   chan_telemetry   sensor -> MQTT.  Observed by a LISTENER that only signals;
 *                    the reader then takes the channel's *current* value. A
 *                    channel stores exactly one message, so a reading produced
 *                    while the reader is busy is overwritten — latest wins.
 *                    That is correct here: a stale reading is worse than a
 *                    missing one, and `sequence` makes the loss visible. Same
 *                    argument as telemetry being QoS 0 on the wire.
 *
 *   chan_sensor_cmd  MQTT -> sensor.  Observed by a MESSAGE SUBSCRIBER, which
 *                    receives a *copy* of every message in order. A command has
 *                    no next one coming, so none may be collapsed. Same
 *                    argument as command/ack being QoS 1 on the wire.
 *
 * Both channels are defined in sensor.cpp: the sensor module owns the readings
 * it produces *and* the sample period the commands adjust, so the bounds and
 * the validator live with the code they constrain.
 */
#ifndef APP_CHANNELS_H_
#define APP_CHANNELS_H_

#include <zephyr/zbus/zbus.h>

#include <stdint.h>

/* Sample period bounds. Enforced by the chan_sensor_cmd validator, which is the
 * single source of truth for them; main.cpp only names them in the Ack detail
 * text it sends back to the host. The bounds keep a bad command from either
 * flooding the broker or stalling telemetry entirely. */
#define SAMPLE_PERIOD_DEFAULT_MS 5000U
#define SAMPLE_PERIOD_MIN_MS 1000U
#define SAMPLE_PERIOD_MAX_MS 300000U

/* Deliberately NOT the generated node_Telemetry struct. zbus is internal, the
 * protobuf types are the wire format, and keeping them separate means a schema
 * change stops at the one function in main.cpp that maps between them instead
 * of rippling into the sensor thread. */
enum sensor_reading_status {
	SENSOR_READING_OK,
	/* The SCD-40 needs a warm-up before its first valid conversion. */
	SENSOR_READING_WARMING_UP,
	SENSOR_READING_ERROR,
};

struct sensor_reading {
	/* Counts *readings taken*, not messages published. A gap therefore tells
	 * the host either that QoS 0 dropped a publish, or that the node took a
	 * reading it could not deliver (disconnected, or the bus overwrote it).
	 * All three are "you missed one", which is what the field is for. */
	uint32_t sequence;
	uint32_t uptime_ms;
	uint32_t co2_ppm;
	float temperature_c;
	float humidity_rh;
	enum sensor_reading_status status;
};

enum sensor_cmd_kind {
	SENSOR_CMD_SET_INTERVAL,
	SENSOR_CMD_TRIGGER,
};

struct sensor_cmd {
	enum sensor_cmd_kind kind;
	/* Only meaningful for SENSOR_CMD_SET_INTERVAL. */
	uint32_t interval_ms;
};

ZBUS_CHAN_DECLARE(chan_telemetry);
ZBUS_CHAN_DECLARE(chan_sensor_cmd);

#endif /* APP_CHANNELS_H_ */
