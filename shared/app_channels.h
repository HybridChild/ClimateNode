/* zbus channels shared between the sensor thread and the transport thread.
 *
 * zbus is an *in-process* pub/sub bus between threads on the MCU. Nothing here
 * touches a network or a transport -- it is the internal decoupling that lets
 * the sensor be read on its own cadence while the transport is busy connecting,
 * blocked in poll(), or backing off after a dropped link.
 *
 * This header is the decisions half of the zbus documentation; the concepts from
 * first principles are in notes/zbus-guide.md.
 *
 * The two channels deliberately use different observer styles, for the same
 * reason the two MQTT topics use different QoS (docs/mqtt-design.md):
 *
 *   chan_telemetry   sensor -> transport.  A LISTENER that only signals; the
 *                    reader then takes the channel's *current* value. State, so
 *                    latest wins and `sequence` makes the loss visible.
 *
 *   chan_sensor_cmd  transport -> sensor.  A MESSAGE SUBSCRIBER, which receives
 *                    a *copy* of every message in order. A command has no next
 *                    one coming, so none may be collapsed.
 *
 * Both channels are DECLARED here and DEFINED by whoever links this header --
 * each app's own sensor.cpp, or the test in tests/commands/. That seam is why
 * two applications share this file: the H753ZI gateway and the F072RB peer node
 * both include it, each for its own sensor. Neither app owns it, and nothing in
 * it may depend on one.
 *
 * The SCD-40 measures CO2, temperature and humidity; the BME280 temperature,
 * humidity and pressure. Neither is a subset of the other, which is why struct
 * sensor_reading carries a presence flag per measurement rather than a value
 * that has to mean "none" -- notes/protobuf-guide.md §5 for the wire half.
 *
 * What is NOT here: anything to do with a transport. The CAN address map and
 * heartbeat frame are in can_link.h, a contract between two *boards*; the
 * gateway's relay channels are in relay.h, and exist precisely to carry a
 * transport's bytes.
 */
#ifndef APP_CHANNELS_H_
#define APP_CHANNELS_H_

#include <zephyr/zbus/zbus.h>

#include <stdint.h>

/* Sample period bounds. Enforced by the chan_sensor_cmd validator, which is the
 * single source of truth for them; commands.cpp only names them in the Ack
 * detail text it sends back to the host. The bounds keep a bad command from either
 * flooding the broker or stalling telemetry entirely. */
#define SAMPLE_PERIOD_DEFAULT_MS 5000U
#define SAMPLE_PERIOD_MIN_MS 1000U
#define SAMPLE_PERIOD_MAX_MS 300000U

/* Note on the minimum: the SCD-40 produces one conversion per ~5 s, and
 * sensor_sample_fetch() returns 0 without updating anything when none is ready.
 * Polling faster than the sensor converts therefore republishes the previous
 * measurements under a fresh sequence and uptime. Known and deliberately not
 * fixed -- see "Accepted limitation" in docs/sensor-bringup.md before changing
 * this value or reading anything into a 1 s cadence. */

/* Deliberately NOT the generated node_Telemetry struct. zbus is internal, the
 * protobuf types are the wire format, and keeping them separate means a schema
 * change stops in protocol.cpp -- the one translation unit that maps between
 * them -- instead of rippling into the sensor thread. */
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

	/* The measurements, each paired with a presence flag. A node fills in
	 * what it can measure and leaves the rest false; nothing has to lie with
	 * a 0. The SCD-40 node here sets the first three and never has_pressure_pa,
	 * and a BME280 node is the mirror image of that.
	 *
	 * This mirrors -- deliberately, not accidentally -- the `optional` fields
	 * in proto/node.proto, which exist for the same reason: a measurement of
	 * zero and no measurement at all are different facts, and a bare scalar
	 * cannot tell them apart. The flags are the internal half of that
	 * distinction, and protocol.cpp is where the two halves meet. */
	bool has_co2_ppm;
	uint32_t co2_ppm;
	bool has_temperature_c;
	float temperature_c;
	bool has_humidity_rh;
	float humidity_rh;
	bool has_pressure_pa;
	uint32_t pressure_pa;

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

/* Every message on every channel must fit one buffer of the zbus message
 * subscriber pool -- including the ones only listeners observe, which is not
 * what the option's name suggests and is the whole point of these asserts.
 * Undersizing it overruns a fixed slot with no diagnostic. The mechanism and the
 * failure signature are in docs/can-bringup.md *The zbus pool is sized by every
 * channel*; adding a channel or growing a message means revisiting these.
 *
 * Guarded because the test suites enable CONFIG_ZBUS without the message
 * subscriber, so the symbol does not exist there -- and nothing in them
 * publishes. */
#ifdef CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_STATIC_DATA_SIZE
static_assert(sizeof(struct sensor_reading) <=
		      CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_STATIC_DATA_SIZE,
	      "raise CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_STATIC_DATA_SIZE to sizeof(struct "
	      "sensor_reading)");
static_assert(sizeof(struct sensor_cmd) <= CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_STATIC_DATA_SIZE,
	      "raise CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_STATIC_DATA_SIZE to sizeof(struct "
	      "sensor_cmd)");
#endif

/* The rule the chan_sensor_cmd validator enforces, stated next to the bounds it
 * compares against rather than in sensor.cpp -- the constants and the comparison
 * drifting apart is exactly what a single definition prevents.
 * sensor_cmd_valid() in each sensor.cpp is the zbus *adapter* around this; being
 * an ordinary function, the rule itself can be tested directly. */
static inline bool sensor_cmd_in_range(const struct sensor_cmd *cmd)
{
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

ZBUS_CHAN_DECLARE(chan_telemetry);
ZBUS_CHAN_DECLARE(chan_sensor_cmd);

#endif /* APP_CHANNELS_H_ */
