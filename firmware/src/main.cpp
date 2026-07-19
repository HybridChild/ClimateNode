/* MQTT client node: reads the Adafruit SCD-40 over I2C and publishes the readings
 * to a Mosquitto broker on the Raspberry Pi.
 *
 * Implements the design in docs/mqtt-design.md:
 *
 *   node/1/telemetry   node -> host, QoS 0, ~every 5 s
 *   node/1/command     host -> node, QoS 1   (subscribed)
 *   node/1/status      node -> host, QoS 1, RETAINED — "online" on connect,
 *                      "offline" published by the BROKER via the Last Will if we
 *                      drop without a clean DISCONNECT
 *
 * Payload is plain text for now; nanopb-encoded Protobuf replaces it in Phase 4,
 * and a zbus channel decouples sensor from publisher in Phase 5.
 *
 * Structure: main() owns a forever loop of "connect, serve until dropped, back
 * off, retry". Reconnect is not error handling bolted on the side — it is the
 * shape of the program, because a node that cannot survive a cable pull is not
 * finished (see the README's learning goals).
 *
 * The interface itself needs no code: net_config applies the static IPv4 address
 * at boot from prj.conf.
 *
 * C++ app (see docs/language-cpp.md): the MQTT, sensor and socket APIs are C APIs
 * called directly from C++. `main` is never name-mangled, so it needs no
 * extern "C"; the event callback is a plain static function used as a C function
 * pointer.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Generated from proto/node.proto at build time into the build directory.
 * It is C, with extern "C" guards, so it includes cleanly here. */
#include <node.pb.h>
#include <pb_decode.h>
#include <pb_encode.h>

LOG_MODULE_REGISTER(node, LOG_LEVEL_INF);

namespace {

/* ---- configuration -------------------------------------------------------- */

constexpr const char *kBrokerAddr = CONFIG_NET_CONFIG_PEER_IPV4_ADDR;
constexpr uint16_t kBrokerPort = 1883;

constexpr const char *kClientId = "nucleo-1";
constexpr const char *kTopicTelemetry = "node/1/telemetry";
constexpr const char *kTopicCommand = "node/1/command";
constexpr const char *kTopicAck = "node/1/ack";
constexpr const char *kTopicStatus = "node/1/status";

/* Bumped only when a change to proto/node.proto breaks old readers. Additive
 * field changes do not touch it — protobuf handles those on its own. */
constexpr uint32_t kSchemaVersion = 1;

constexpr const char *kFirmwareVersion = "0.4.0";
constexpr const char *kBoardName = CONFIG_BOARD;

/* Keepalive bounds how long the broker waits before declaring us dead
 * (1.5x keepalive) and firing the will. We publish every 5 s, so PINGREQ rarely
 * fires on a healthy link — this value is really a failure-detection deadline.
 * Lower it temporarily (e.g. 10 s) when testing the will by hand; see
 * "Testing the Last Will" in docs/mqtt-design.md. */
constexpr uint16_t kKeepaliveSec = 60;

/* Plain milliseconds rather than a k_timeout_t: this value is only ever used in
 * arithmetic against k_uptime_get(), never handed to a kernel API, and it is
 * now runtime-adjustable via the SetInterval command. The bounds keep a bad
 * command from either flooding the broker or stalling telemetry entirely. */
constexpr uint32_t kSamplePeriodDefaultMs = 5000;
constexpr uint32_t kSamplePeriodMinMs = 1000;
constexpr uint32_t kSamplePeriodMaxMs = 300000;

/* Reconnect backoff: start at 1 s, double up to 30 s, so a broker that is down
 * (or a cable left unplugged) is retried patiently instead of in a hot loop. */
constexpr int kBackoffMinMs = 1000;
constexpr int kBackoffMaxMs = 30000;

/* ---- state ---------------------------------------------------------------- */

uint8_t rx_buffer[256];
uint8_t tx_buffer[256];
struct mqtt_client client;
struct sockaddr_in broker;

/* Set from the MQTT event callback, read by the serve loop. */
volatile bool connected;
volatile bool connect_failed;

uint32_t sequence;

/* Adjustable by SetInterval; survives reconnects deliberately, since the host's
 * last instruction should outlive a dropped TCP connection. */
uint32_t sample_period_ms = kSamplePeriodDefaultMs;

/* Set by TriggerMeasurement to force a publish on the next loop pass. */
bool sample_now;

/* QoS 1 is at-least-once, so a redelivered command must not be executed twice.
 * We ack duplicates (the broker needs the PUBACK) but skip the side effect. */
bool have_last_command;
uint32_t last_command_sequence;

/* ---- MQTT plumbing -------------------------------------------------------- */

/* Defined below, after the client struct it operates on. */
int publish(const char *topic, const uint8_t *payload, size_t payload_len,
	    mqtt_qos qos, bool retain);

/* Encode and publish an Ack. Best effort: a failure here is logged, not
 * propagated, because the command itself may already have taken effect. */
void send_ack(uint32_t seq, node_AckStatus status, const char *detail,
	      const node_DeviceInfo *info)
{
	node_Ack ack = node_Ack_init_zero;
	uint8_t buf[node_Ack_size];

	ack.schema_version = kSchemaVersion;
	ack.sequence = seq;
	ack.status = status;

	if (detail != nullptr) {
		/* Bounded by max_size:48 in node.options. Truncating the
		 * diagnostic is fine; hosts branch on `status`, not this text. */
		strncpy(ack.detail, detail, sizeof(ack.detail) - 1);
	}
	if (info != nullptr) {
		ack.has_device_info = true;
		ack.device_info = *info;
	}

	pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));

	if (!pb_encode(&stream, node_Ack_fields, &ack)) {
		LOG_ERR("ack encode failed: %s", PB_GET_ERROR(&stream));
		return;
	}
	publish(kTopicAck, buf, stream.bytes_written, MQTT_QOS_1_AT_LEAST_ONCE, false);
}

/* Execute a decoded command. Returns the status to report, and may fill
 * `detail` and `info` for the Ack. */
node_AckStatus apply_command(const node_Command &cmd, char *detail, size_t detail_len,
			     node_DeviceInfo *info, bool *has_info)
{
	switch (cmd.which_payload) {
	case node_Command_set_interval_tag: {
		uint32_t ms = cmd.payload.set_interval.interval_ms;

		if (ms < kSamplePeriodMinMs || ms > kSamplePeriodMaxMs) {
			snprintf(detail, detail_len, "interval %u outside [%u,%u]", ms,
				 kSamplePeriodMinMs, kSamplePeriodMaxMs);
			return node_AckStatus_ACK_STATUS_INVALID_ARGUMENT;
		}
		sample_period_ms = ms;
		LOG_INF("sample period set to %u ms", ms);
		return node_AckStatus_ACK_STATUS_OK;
	}

	case node_Command_trigger_measurement_tag:
		sample_now = true;
		LOG_INF("single-shot measurement requested");
		return node_AckStatus_ACK_STATUS_OK;

	case node_Command_get_device_info_tag:
		*info = node_DeviceInfo_init_zero;
		strncpy(info->firmware_version, kFirmwareVersion,
			sizeof(info->firmware_version) - 1);
		strncpy(info->board, kBoardName, sizeof(info->board) - 1);
		strncpy(info->client_id, kClientId, sizeof(info->client_id) - 1);
		*has_info = true;
		return node_AckStatus_ACK_STATUS_OK;

	default:
		/* A oneof member this firmware does not know: the host is newer
		 * than the node. Protobuf decoded it fine — the gap is in our
		 * handlers, which is exactly what UNSUPPORTED reports. */
		snprintf(detail, detail_len, "unknown command tag %u",
			 static_cast<unsigned>(cmd.which_payload));
		return node_AckStatus_ACK_STATUS_UNSUPPORTED;
	}
}

/* Read a PUBLISH payload out of the socket and decode it. The MQTT_EVT_PUBLISH
 * event carries only the *length*; the bytes must be read explicitly.
 *
 * Every byte of the payload MUST leave the socket, even the ones we do not want.
 * The library resumes parsing at whatever follows, so bytes left behind are
 * decoded as the next packet's fixed header and desync the connection — an
 * oversized command would corrupt the stream, not merely be truncated. So we
 * keep the first sizeof(payload)-1 bytes and drain the remainder. */
void handle_incoming_publish(struct mqtt_client *c,
			     const struct mqtt_publish_param *pub)
{
	uint8_t payload[128];
	uint8_t discard[64];
	uint32_t remaining = pub->message.payload.len;
	uint32_t kept = MIN(remaining, sizeof(payload));

	int rc = mqtt_readall_publish_payload(c, payload, kept);

	if (rc < 0) {
		LOG_ERR("failed reading publish payload: %d", rc);
		return;
	}
	remaining -= kept;

	bool oversized = (remaining > 0);

	if (oversized) {
		LOG_WRN("command payload too large: %u bytes dropped", remaining);
	}
	/* Drain into a separate buffer: reusing `payload` would overwrite the
	 * bytes we just kept. */
	while (remaining > 0) {
		uint32_t chunk = MIN(remaining, sizeof(discard));

		rc = mqtt_readall_publish_payload(c, discard, chunk);
		if (rc < 0) {
			LOG_ERR("failed draining publish payload: %d", rc);
			return;
		}
		remaining -= chunk;
	}

	/* Commands are QoS 1, so the broker expects a PUBACK from us. Without it
	 * the broker redelivers (with DUP set) until it gets one. Send it before
	 * doing any work: the PUBACK is a transport-level "I have the bytes", not
	 * an application-level "I executed it" — that is what the Ack is for. */
	if (pub->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
		struct mqtt_puback_param puback = {};

		puback.message_id = pub->message_id;
		mqtt_publish_qos1_ack(c, &puback);
	}

	node_Command cmd = node_Command_init_zero;
	pb_istream_t stream = pb_istream_from_buffer(payload, kept);

	/* A truncated payload might still decode into something plausible, so
	 * reject oversize outright rather than acting on a partial command. */
	if (oversized || !pb_decode(&stream, node_Command_fields, &cmd)) {
		LOG_ERR("command decode failed: %s",
			oversized ? "payload too large" : PB_GET_ERROR(&stream));
		/* sequence 0: we could not read one, so there is nothing to
		 * correlate against. The host learns the message was garbage. */
		send_ack(0, node_AckStatus_ACK_STATUS_MALFORMED,
			 oversized ? "payload too large" : "decode failed", nullptr);
		return;
	}

	LOG_INF("command seq=%u tag=%u schema=%u", cmd.sequence,
		static_cast<unsigned>(cmd.which_payload), cmd.schema_version);

	/* Duplicate suppression. QoS 1 redelivers after a lost PUBACK, so the
	 * same command can arrive twice; re-running "set interval" is harmless
	 * but "trigger measurement" is not. Ack again so the host still gets a
	 * reply, but skip the side effect. */
	if (have_last_command && cmd.sequence == last_command_sequence) {
		LOG_WRN("duplicate command seq=%u ignored", cmd.sequence);
		send_ack(cmd.sequence, node_AckStatus_ACK_STATUS_OK,
			 "duplicate ignored", nullptr);
		return;
	}

	char detail[48] = {};
	node_DeviceInfo info = node_DeviceInfo_init_zero;
	bool has_info = false;

	node_AckStatus status =
		apply_command(cmd, detail, sizeof(detail), &info, &has_info);

	have_last_command = true;
	last_command_sequence = cmd.sequence;

	send_ack(cmd.sequence, status, detail[0] != '\0' ? detail : nullptr,
		 has_info ? &info : nullptr);
}

void mqtt_evt_handler(struct mqtt_client *const c, const struct mqtt_evt *evt)
{
	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->result != 0) {
			LOG_ERR("CONNACK refused: %d", evt->result);
			connect_failed = true;
			break;
		}
		LOG_INF("connected to broker");
		connected = true;
		break;

	case MQTT_EVT_DISCONNECT:
		LOG_WRN("disconnected: %d", evt->result);
		connected = false;
		break;

	case MQTT_EVT_PUBLISH:
		handle_incoming_publish(c, &evt->param.publish);
		break;

	case MQTT_EVT_PUBACK:
		LOG_DBG("PUBACK id %u", evt->param.puback.message_id);
		break;

	case MQTT_EVT_SUBACK:
		LOG_INF("subscribed to %s", kTopicCommand);
		break;

	case MQTT_EVT_PINGRESP:
		LOG_DBG("PINGRESP");
		break;

	default:
		break;
	}
}

void client_setup(void)
{
	mqtt_client_init(&client);

	broker = {};
	broker.sin_family = AF_INET;
	broker.sin_port = htons(kBrokerPort);
	zsock_inet_pton(AF_INET, kBrokerAddr, &broker.sin_addr);

	client.broker = &broker;
	client.evt_cb = mqtt_evt_handler;
	client.client_id.utf8 = reinterpret_cast<uint8_t *>(const_cast<char *>(kClientId));
	client.client_id.size = strlen(kClientId);
	client.user_name = nullptr;
	client.password = nullptr;
	client.protocol_version = MQTT_VERSION_3_1_1;
	client.keepalive = kKeepaliveSec;
	/* Clean session: we want no server-side queue replayed on reconnect —
	 * stale telemetry is worse than none (docs/mqtt-design.md). */
	client.clean_session = 1;

	client.rx_buf = rx_buffer;
	client.rx_buf_size = sizeof(rx_buffer);
	client.tx_buf = tx_buffer;
	client.tx_buf_size = sizeof(tx_buffer);
	client.transport.type = MQTT_TRANSPORT_NON_SECURE;

	/* Last Will: registered at CONNECT, held by the broker, and published on
	 * our behalf if we vanish without a clean DISCONNECT. This is how the host
	 * learns about a crash or cable pull — no firmware runs in that path. */
	static struct mqtt_topic will_topic;
	static struct mqtt_utf8 will_message;

	will_topic.topic.utf8 = reinterpret_cast<uint8_t *>(const_cast<char *>(kTopicStatus));
	will_topic.topic.size = strlen(kTopicStatus);
	will_topic.qos = MQTT_QOS_1_AT_LEAST_ONCE;
	will_message.utf8 = reinterpret_cast<uint8_t *>(const_cast<char *>("offline"));
	will_message.size = strlen("offline");

	client.will_topic = &will_topic;
	client.will_message = &will_message;
	client.will_retain = 1;
}

/* Payload is a length-delimited byte range, not a C string: protobuf output is
 * binary and routinely contains NUL bytes, so strlen() would truncate it. */
int publish(const char *topic, const uint8_t *payload, size_t payload_len,
	    mqtt_qos qos, bool retain)
{
	struct mqtt_publish_param param = {};

	param.message.topic.topic.utf8 = reinterpret_cast<uint8_t *>(const_cast<char *>(topic));
	param.message.topic.topic.size = strlen(topic);
	param.message.topic.qos = qos;
	param.message.payload.data = const_cast<uint8_t *>(payload);
	param.message.payload.len = payload_len;
	/* message_id is only meaningful for QoS 1/2 — at QoS 0 there is no PUBACK
	 * to correlate, and the field is not even put on the wire. A counter is
	 * enough here: it only has to be non-zero and distinct among *in-flight*
	 * messages, and we never have more than one outstanding. */
	static uint16_t next_message_id;

	if (qos == MQTT_QOS_0_AT_MOST_ONCE) {
		param.message_id = 0;
	} else {
		next_message_id++;
		if (next_message_id == 0) {
			next_message_id = 1;
		}
		param.message_id = next_message_id;
	}
	param.dup_flag = 0;
	param.retain_flag = retain ? 1 : 0;

	return mqtt_publish(&client, &param);
}

/* Convenience for the one topic that stays plain ASCII: `status` is also written
 * by the broker as our Last Will, so it cannot be protobuf (docs/mqtt-design.md). */
int publish_text(const char *topic, const char *text, mqtt_qos qos, bool retain)
{
	return publish(topic, reinterpret_cast<const uint8_t *>(text), strlen(text),
		       qos, retain);
}

int subscribe_to_commands(void)
{
	struct mqtt_topic topic = {};
	struct mqtt_subscription_list list = {};

	topic.topic.utf8 = reinterpret_cast<uint8_t *>(const_cast<char *>(kTopicCommand));
	topic.topic.size = strlen(kTopicCommand);
	topic.qos = MQTT_QOS_1_AT_LEAST_ONCE;

	list.list = &topic;
	list.list_count = 1U;
	list.message_id = 1U;

	return mqtt_subscribe(&client, &list);
}

/* Block until the socket has data or the timeout expires. Returns the poll
 * result; the MQTT library owns the socket, we only wait on it. */
int wait_for_input(int timeout_ms)
{
	struct zsock_pollfd fds[1];

	fds[0].fd = client.transport.tcp.sock;
	fds[0].events = ZSOCK_POLLIN;

	int rc = zsock_poll(fds, 1, timeout_ms);

	if (rc < 0) {
		LOG_ERR("poll: %d", errno);
	}
	return rc;
}

/* ---- sensor --------------------------------------------------------------- */

const struct device *scd40;

/* Encode the current reading as a node.Telemetry message.
 *
 * A failed sensor read is still published, with sensor_status = ERROR and the
 * measurement fields left at zero. Silence would be ambiguous — the host cannot
 * tell a broken sensor from a dead node — whereas an explicit ERROR is a fact
 * the host can act on. This is why the status enum exists.
 *
 * Returns the encoded length, or 0 on encode failure. */
size_t encode_telemetry(uint8_t *out, size_t out_len)
{
	node_Telemetry msg = node_Telemetry_init_zero;
	struct sensor_value co2, temp, hum;

	msg.schema_version = kSchemaVersion;
	msg.sequence = sequence;
	msg.uptime_ms = static_cast<uint32_t>(k_uptime_get());

	int rc = sensor_sample_fetch(scd40);

	if (rc != 0) {
		LOG_ERR("sample_fetch failed: %d", rc);
		msg.sensor_status = node_SensorStatus_SENSOR_STATUS_ERROR;
	} else {
		sensor_channel_get(scd40, SENSOR_CHAN_CO2, &co2);
		sensor_channel_get(scd40, SENSOR_CHAN_AMBIENT_TEMP, &temp);
		sensor_channel_get(scd40, SENSOR_CHAN_HUMIDITY, &hum);

		msg.co2_ppm = static_cast<uint32_t>(sensor_value_to_double(&co2));
		msg.temperature_c = static_cast<float>(sensor_value_to_double(&temp));
		msg.humidity_rh = static_cast<float>(sensor_value_to_double(&hum));

		/* The SCD-40 reports 0 ppm until its first conversion completes. */
		msg.sensor_status = (msg.co2_ppm == 0)
					    ? node_SensorStatus_SENSOR_STATUS_WARMING_UP
					    : node_SensorStatus_SENSOR_STATUS_OK;
	}

	/* An output stream writing into a caller-supplied buffer. nanopb never
	 * allocates: if the message does not fit, encoding fails rather than
	 * growing anything. node_Telemetry_size (36) is the generated upper
	 * bound, so a 256-byte buffer cannot overflow here. */
	pb_ostream_t stream = pb_ostream_from_buffer(out, out_len);

	if (!pb_encode(&stream, node_Telemetry_fields, &msg)) {
		LOG_ERR("telemetry encode failed: %s", PB_GET_ERROR(&stream));
		return 0;
	}
	return stream.bytes_written;
}

/* ---- the session ---------------------------------------------------------- */

/* Connect, then serve the connection until it drops. Returns when disconnected
 * for any reason; the caller backs off and calls again.
 *
 * Returns true if the session ever reached CONNACK. The caller uses that to
 * reset its backoff: a link that worked and then dropped deserves a fast retry,
 * while one that never connected at all should keep backing off. */
bool run_session(void)
{
	client_setup();
	connected = false;
	connect_failed = false;

	int rc = mqtt_connect(&client);

	if (rc != 0) {
		/* No TCP connection: broker down, or 192.168.10.1 unreachable. */
		LOG_ERR("mqtt_connect: %d", rc);
		return false;
	}

	/* mqtt_connect() only sends CONNECT — the CONNACK arrives asynchronously,
	 * so pump input until the callback flips `connected`. */
	for (int waited = 0; !connected && !connect_failed && waited < 5000; waited += 100) {
		if (wait_for_input(100) > 0) {
			mqtt_input(&client);
		}
	}

	if (!connected) {
		LOG_ERR("no CONNACK — aborting");
		mqtt_abort(&client);
		return false;
	}

	/* Announce ourselves. Retained, so a harness starting later immediately
	 * learns we are up; the will (also retained) overwrites it if we die. */
	publish_text(kTopicStatus, "online", MQTT_QOS_1_AT_LEAST_ONCE, true);
	subscribe_to_commands();

	int64_t next_sample = k_uptime_get();

	while (connected) {
		/* Wake for whichever comes first: the next telemetry publish, or
		 * the keepalive deadline. Sleeping past either would stall
		 * publishing or let the broker time us out. */
		int64_t until_sample = next_sample - k_uptime_get();
		int keepalive_ms = mqtt_keepalive_time_left(&client);
		int timeout = MIN(MAX(until_sample, 0), keepalive_ms);

		if (wait_for_input(timeout) > 0) {
			rc = mqtt_input(&client);
			if (rc != 0) {
				LOG_ERR("mqtt_input: %d", rc);
				break;
			}
		}

		/* Sends PINGREQ when due, and surfaces a dead connection. */
		rc = mqtt_live(&client);
		if (rc != 0 && rc != -EAGAIN) {
			LOG_ERR("mqtt_live: %d", rc);
			break;
		}

		if (sample_now || k_uptime_get() >= next_sample) {
			uint8_t payload[node_Telemetry_size];
			size_t len = encode_telemetry(payload, sizeof(payload));

			sample_now = false;

			if (len > 0) {
				rc = publish(kTopicTelemetry, payload, len,
					     MQTT_QOS_0_AT_MOST_ONCE, false);
				if (rc != 0) {
					LOG_ERR("publish: %d", rc);
					break;
				}
				LOG_INF("published telemetry seq=%u (%zu bytes)",
					sequence, len);
				sequence++;
			}
			next_sample = k_uptime_get() + sample_period_ms;
		}
	}

	/* Best-effort clean teardown. A clean DISCONNECT deliberately suppresses
	 * the will — we only want "offline" published when we die unexpectedly. */
	mqtt_disconnect(&client, nullptr);
	return true;
}

}  // namespace

int main(void)
{
	scd40 = DEVICE_DT_GET(DT_NODELABEL(scd40));

	if (!device_is_ready(scd40)) {
		LOG_ERR("SCD-40 not ready");
		return 0;
	}
	LOG_INF("SCD-40 online; broker %s:%u", kBrokerAddr, kBrokerPort);

	int backoff = kBackoffMinMs;

	while (true) {
		bool was_connected = run_session();

		/* Reached only when the session ended. A session that reached
		 * CONNACK proves the broker is reachable, so start over from the
		 * short delay; otherwise keep doubling so a down broker (or an
		 * unplugged cable) is retried patiently instead of in a hot loop. */
		if (was_connected) {
			backoff = kBackoffMinMs;
		}

		LOG_WRN("reconnecting in %d ms", backoff);
		k_msleep(backoff);

		if (!was_connected) {
			backoff = MIN(backoff * 2, kBackoffMaxMs);
		}
	}
	return 0;
}
