/* zbus channels shared between the sensor thread and the MQTT thread.
 *
 * zbus is an *in-process* pub/sub bus between threads on the MCU. Nothing here
 * touches the network — it is the internal decoupling that lets the sensor be
 * read on its own cadence while the MQTT client is busy connecting, blocked in
 * poll(), or backing off after a dropped link.
 *
 * This header is the decisions half of the zbus documentation; the concepts
 * from first principles — channels, the three observer kinds, why latest-wins
 * is a feature, validators, and waiting on a bus and a socket at once — are in
 * notes/zbus-guide.md.
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
 *
 * ---------------------------------------------------------------------------
 * Two applications now share this header
 * ---------------------------------------------------------------------------
 *
 * The H753ZI gateway (firmware/) and the F072RB peer node (sensor-node/) both
 * include it, and both DEFINE these two channels — each in its own sensor.cpp,
 * for its own sensor. ZBUS_CHAN_DECLARE expands to `extern`, so the header
 * declares and whoever links supplies the definition; tests/commands/ has been
 * using that same seam for chan_sensor_cmd all along, and commands.h now uses
 * it for the node identity.
 *
 * That is what makes the description above hold for both nodes without a word
 * of it being about either one. The SCD-40 fills in CO2, temperature and
 * humidity; the BME280 fills in temperature, humidity and pressure. Neither is
 * a subset of the other, which is why struct sensor_reading carries a presence
 * flag per measurement rather than a value that has to mean "none" — see the
 * comment on it below, and notes/protobuf-guide.md §5 for the wire half.
 *
 * What is NOT here: anything to do with CAN. The link's address map, heartbeat
 * frame and message-type byte live in can_link.h, because they are a contract
 * between two *boards* rather than between two threads; the gateway's relay
 * channels live in relay.h. This header's whole claim is that nothing in it
 * touches a transport, and the relay channels exist precisely to carry one.
 *
 * Worth reading the two side by side, because they avoid generated Protobuf
 * types for opposite reasons. Here, the internal types exist so a schema change
 * stops at protocol.cpp instead of reaching the sensor thread. There, the relay
 * carries raw bytes so a schema change on the *peer* does not reach the gateway
 * at all — it never decodes what it forwards. Same goal, approached from either
 * end: a schema change that does not ripple.
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

/* The rule the chan_sensor_cmd validator enforces, stated next to the bounds it
 * compares against rather than in sensor.cpp -- the constants and the
 * comparison drifting apart is exactly the failure a single definition
 * prevents. sensor_cmd_valid() in sensor.cpp is the zbus *adapter* around this
 * (it takes a const void *, as the bus requires); this is the rule itself, and
 * being an ordinary function it can be tested directly. */
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
