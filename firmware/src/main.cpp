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

LOG_MODULE_REGISTER(node, LOG_LEVEL_INF);

namespace {

/* ---- configuration -------------------------------------------------------- */

constexpr const char *kBrokerAddr = CONFIG_NET_CONFIG_PEER_IPV4_ADDR;
constexpr uint16_t kBrokerPort = 1883;

constexpr const char *kClientId = "nucleo-1";
constexpr const char *kTopicTelemetry = "node/1/telemetry";
constexpr const char *kTopicCommand = "node/1/command";
constexpr const char *kTopicStatus = "node/1/status";

/* Keepalive bounds how long the broker waits before declaring us dead
 * (1.5x keepalive) and firing the will. We publish every 5 s, so PINGREQ rarely
 * fires on a healthy link — this value is really a failure-detection deadline.
 * Lower it temporarily (e.g. 10 s) when testing the will by hand; see
 * "Testing the Last Will" in docs/mqtt-design.md. */
constexpr uint16_t kKeepaliveSec = 60;
constexpr k_timeout_t kSamplePeriod = K_SECONDS(5);

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

/* ---- MQTT plumbing -------------------------------------------------------- */

/* Read a PUBLISH payload out of the socket and log it. The MQTT_EVT_PUBLISH
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
	char payload[128];
	uint8_t discard[64];
	uint32_t remaining = pub->message.payload.len;
	uint32_t kept = MIN(remaining, sizeof(payload) - 1);

	int rc = mqtt_readall_publish_payload(c, reinterpret_cast<uint8_t *>(payload), kept);

	if (rc < 0) {
		LOG_ERR("failed reading publish payload: %d", rc);
		return;
	}
	payload[kept] = '\0';
	remaining -= kept;

	if (remaining > 0) {
		LOG_WRN("command payload truncated: %u bytes dropped",
			remaining);
	}
	/* Drain into a separate buffer: reusing `payload` would overwrite the
	 * bytes we just kept, terminator included. */
	while (remaining > 0) {
		uint32_t chunk = MIN(remaining, sizeof(discard));

		rc = mqtt_readall_publish_payload(c, discard, chunk);
		if (rc < 0) {
			LOG_ERR("failed draining publish payload: %d", rc);
			return;
		}
		remaining -= chunk;
	}

	LOG_INF("command received: \"%s\"", payload);

	/* Commands are QoS 1, so the broker expects a PUBACK from us. Without it
	 * the broker redelivers (with DUP set) until it gets one. */
	if (pub->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
		struct mqtt_puback_param ack = {};

		ack.message_id = pub->message_id;
		mqtt_publish_qos1_ack(c, &ack);
	}

	/* Phase 4 decodes this as a Protobuf Command and answers on node/1/ack. */
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

int publish(const char *topic, const char *payload, mqtt_qos qos, bool retain)
{
	struct mqtt_publish_param param = {};

	param.message.topic.topic.utf8 = reinterpret_cast<uint8_t *>(const_cast<char *>(topic));
	param.message.topic.topic.size = strlen(topic);
	param.message.topic.qos = qos;
	param.message.payload.data = reinterpret_cast<uint8_t *>(const_cast<char *>(payload));
	param.message.payload.len = strlen(payload);
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

/* Format the current reading. Returns false if the sensor read failed. */
bool format_telemetry(char *out, size_t out_len)
{
	struct sensor_value co2, temp, hum;
	int rc = sensor_sample_fetch(scd40);

	if (rc != 0) {
		LOG_ERR("sample_fetch failed: %d", rc);
		return false;
	}

	sensor_channel_get(scd40, SENSOR_CHAN_CO2, &co2);
	sensor_channel_get(scd40, SENSOR_CHAN_AMBIENT_TEMP, &temp);
	sensor_channel_get(scd40, SENSOR_CHAN_HUMIDITY, &hum);

	snprintf(out, out_len, "seq=%u co2=%.0f temp=%.2f rh=%.1f", sequence,
		 sensor_value_to_double(&co2), sensor_value_to_double(&temp),
		 sensor_value_to_double(&hum));
	return true;
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
	publish(kTopicStatus, "online", MQTT_QOS_1_AT_LEAST_ONCE, true);
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

		if (k_uptime_get() >= next_sample) {
			char payload[96];

			if (format_telemetry(payload, sizeof(payload))) {
				rc = publish(kTopicTelemetry, payload,
					     MQTT_QOS_0_AT_MOST_ONCE, false);
				if (rc != 0) {
					LOG_ERR("publish: %d", rc);
					break;
				}
				LOG_INF("published: %s", payload);
				sequence++;
			}
			next_sample = k_uptime_get() + k_ticks_to_ms_floor64(kSamplePeriod.ticks);
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
