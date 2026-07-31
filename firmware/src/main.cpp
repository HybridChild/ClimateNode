/* MQTT client node: publishes SCD-40 readings to a Mosquitto broker on the
 * Raspberry Pi, and answers commands from it.
 *
 * This file owns the MQTT session and nothing else. The application is four
 * translation units, each with one responsibility:
 *
 *   sensor.cpp     acquisition — owns the SCD-40, the sample period, the bounds
 *   protocol.cpp   the wire format — the only place internal types meet protobuf
 *   commands.cpp   command semantics — dispatch, duplicate suppression, identity
 *   main.cpp       this file: connect, poll, publish, reconnect
 *
 * sensor.cpp and main.cpp meet on the zbus channels declared in app_channels.h —
 * see that header for why the two channels use different observer styles.
 *
 * The split is what makes the logic testable without hardware: protocol.cpp and
 * commands.cpp carry no socket or device dependency, so tests/ compiles them on
 * their own. See notes/testing-guide.md.
 *
 * Implements the design in docs/mqtt-design.md:
 *
 *   node/1/telemetry   node -> host, QoS 0, every 5 s by default (SetInterval retunes)
 *   node/1/command     host -> node, QoS 1   (subscribed)
 *   node/1/ack         node -> host, QoS 1, one per Command received
 *   node/1/status      node -> host, QoS 1, RETAINED — "online" on connect,
 *                      "offline" published by the BROKER via the Last Will if we
 *                      drop without a clean DISCONNECT
 *
 * Telemetry, Command and Ack payloads are nanopb-encoded Protobuf, generated from
 * proto/node.proto at build time. `status` stays plain ASCII: the broker itself
 * writes it as the will, so firmware cannot encode it.
 *
 * Structure: main() owns a forever loop of "connect, serve until dropped, back
 * off, retry". Reconnect is not error handling bolted on the side — it is the
 * shape of the program, because a node that cannot survive a cable pull is not
 * finished (see the README's learning goals).
 *
 * The interface itself needs no code: net_config applies the static IPv4 address
 * at boot from prj.conf.
 *
 * C++ app (see notes/language-cpp.md): the MQTT, sensor and socket APIs are C APIs
 * called directly from C++. `main` is never name-mangled, so it needs no
 * extern "C"; the event callback is a plain static function used as a C function
 * pointer.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/zvfs/eventfd.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "app_channels.h"
#include "commands.h"
#include "protocol.h"

LOG_MODULE_REGISTER(node, LOG_LEVEL_INF);

/* This app's half of the identity seam commands.h declares. At global scope and
 * externally linked on purpose: commands.cpp refers to it by name, and internal
 * linkage would stop the two from meeting (notes/language-cpp.md §6). The topic
 * literals below have to agree with it -- both say node 1. */
const char *const kNodeClientId = "nucleo-1";

namespace {

/* ---- configuration -------------------------------------------------------- */

constexpr const char *kBrokerAddr = CONFIG_NET_CONFIG_PEER_IPV4_ADDR;
constexpr uint16_t kBrokerPort = 1883;

constexpr const char *kTopicTelemetry = "node/1/telemetry";
constexpr const char *kTopicCommand = "node/1/command";
constexpr const char *kTopicAck = "node/1/ack";
constexpr const char *kTopicStatus = "node/1/status";

/* Keepalive bounds how long the broker waits before declaring us dead
 * (1.5x keepalive) and firing the will. We publish every 5 s, so PINGREQ rarely
 * fires on a healthy link — this value is really a failure-detection deadline.
 * Lower it temporarily (e.g. 10 s) when testing the will by hand; see
 * "Testing the Last Will" in docs/mqtt-design.md. */
constexpr uint16_t kKeepaliveSec = 60;

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

/* How the sensor thread wakes this one.
 *
 * The serve loop must wait on two unrelated things at once: bytes from the
 * broker, and a fresh reading from the bus. zsock_poll() only understands file
 * descriptors, and a zbus channel is not one — so the zbus listener writes to an
 * eventfd, which *is* a descriptor and can sit in the same poll set as the
 * socket. That keeps the loop fully blocking: no timed wakeups just to check
 * whether the bus has something.
 *
 * -1 until main() creates it. The sensor thread starts at boot and may publish
 * before then; the listener checks, and a reading lost before the network is
 * even up is of no consequence. */
int telemetry_evt_fd = -1;

/* ---- MQTT plumbing -------------------------------------------------------- */

/* Defined below, after the client struct it operates on. */
int publish(const char *topic, const uint8_t *payload, size_t payload_len,
	    mqtt_qos qos, bool retain);

/* Encode and publish an Ack. Best effort: a failure here is logged, not
 * propagated, because the command itself may already have taken effect. */
void send_ack(uint32_t seq, node_AckStatus status, const char *detail,
	      const node_DeviceInfo *info)
{
	uint8_t buf[node_Ack_size];
	size_t len = encode_ack(seq, status, detail, info, buf, sizeof(buf));

	if (len == 0) {
		return;
	}
	publish(kTopicAck, buf, len, MQTT_QOS_1_AT_LEAST_ONCE, false);
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

	/* From here the transport is done with the bytes: protocol.cpp decides
	 * whether they are a Command, commands.cpp decides what it means, and
	 * this layer only reports the outcome back to the host. */
	node_Command cmd;

	if (!decode_command(payload, kept, oversized, &cmd)) {
		/* sequence 0: we could not read one, so there is nothing to
		 * correlate against. The host learns the message was garbage. */
		send_ack(0, node_AckStatus_ACK_STATUS_MALFORMED,
			 oversized ? "payload too large" : "decode failed", nullptr);
		return;
	}

	struct command_result result;

	handle_command(cmd, &result);

	send_ack(cmd.sequence, result.status, result.detail[0] != '\0' ? result.detail : nullptr,
		 result.has_info ? &result.info : nullptr);
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
	client.client_id.utf8 = reinterpret_cast<uint8_t *>(const_cast<char *>(kNodeClientId));
	client.client_id.size = strlen(kNodeClientId);
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

/* ---- telemetry: from the bus to the wire ---------------------------------- */

/* zbus listener for chan_telemetry.
 *
 * Listener callbacks run **in the publisher's context** — this executes on the
 * sensor thread, inside zbus_chan_pub(), with the channel locked. So it does the
 * one thing that cannot block: bump the eventfd. The actual reading is left in
 * the channel for the serve loop to pick up.
 *
 * That split is deliberate. The eventfd carries "something new happened"; the
 * channel carries the value. Because a channel stores exactly one message, the
 * serve loop always gets the *latest* reading, never a backlog of stale ones. */
void on_telemetry(const struct zbus_channel *chan)
{
	ARG_UNUSED(chan);

	if (telemetry_evt_fd >= 0) {
		zvfs_eventfd_write(telemetry_evt_fd, 1);
	}
}

/* Drain the eventfd, read the newest reading off the bus, encode it, publish it.
 * Returns an mqtt_publish() result, or 0 if there was nothing to send. */
int publish_pending_telemetry(void)
{
	zvfs_eventfd_t signalled = 0;

	/* Non-blocking, and reading resets the counter to zero. Its value is the
	 * number of readings taken since we last looked: more than one means the
	 * channel overwrote some, which the host will see as a sequence gap. */
	if (zvfs_eventfd_read(telemetry_evt_fd, &signalled) != 0) {
		return 0;
	}
	if (signalled > 1) {
		LOG_WRN("%llu readings coalesced into one publish",
			static_cast<unsigned long long>(signalled - 1));
	}

	struct sensor_reading reading;
	int rc = zbus_chan_read(&chan_telemetry, &reading, K_MSEC(50));

	if (rc != 0) {
		LOG_ERR("chan_telemetry read failed: %d", rc);
		return 0;
	}

	uint8_t payload[node_Telemetry_size];
	size_t len = encode_telemetry(reading, payload, sizeof(payload));

	if (len == 0) {
		return 0;
	}

	rc = publish(kTopicTelemetry, payload, len, MQTT_QOS_0_AT_MOST_ONCE, false);
	if (rc == 0) {
		LOG_INF("published telemetry seq=%u (%zu bytes)", reading.sequence, len);
	}
	return rc;
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

	while (connected) {
		/* Two descriptors, one wait. Before zbus this loop also owned the
		 * sample clock, so the timeout had to be the minimum of "next
		 * sample due" and "keepalive due". Now the sensor keeps its own
		 * cadence and tells us via the eventfd, leaving exactly one
		 * deadline here: the keepalive. */
		struct zsock_pollfd fds[2] = {};

		fds[0].fd = client.transport.tcp.sock;
		fds[0].events = ZSOCK_POLLIN;
		fds[1].fd = telemetry_evt_fd;
		fds[1].events = ZSOCK_POLLIN;

		if (zsock_poll(fds, 2, mqtt_keepalive_time_left(&client)) < 0) {
			LOG_ERR("poll: %d", errno);
			break;
		}

		if (fds[0].revents & ZSOCK_POLLIN) {
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

		/* A command handled by mqtt_input() above may have triggered a
		 * measurement; the sensor thread runs and signals the eventfd, so
		 * the next pass picks it up. Nothing here needs to know that. */
		if (fds[1].revents & ZSOCK_POLLIN) {
			rc = publish_pending_telemetry();
			if (rc != 0) {
				LOG_ERR("publish: %d", rc);
				break;
			}
		}
	}

	/* Best-effort clean teardown. A clean DISCONNECT deliberately suppresses
	 * the will — we only want "offline" published when we die unexpectedly. */
	mqtt_disconnect(&client, nullptr);
	return true;
}

}  // namespace

/* At global scope: the observation records ZBUS_CHAN_DEFINE emits in sensor.cpp
 * refer to this symbol by name. */
ZBUS_LISTENER_DEFINE(telemetry_listener, on_telemetry);

int main(void)
{
	/* Counter starts at 0 and never blocks a reader, so a poll on it is
	 * simply "has the sensor thread published since I last looked". */
	telemetry_evt_fd = zvfs_eventfd(0, ZVFS_EFD_NONBLOCK);

	if (telemetry_evt_fd < 0) {
		LOG_ERR("eventfd: %d", errno);
		return 0;
	}
	LOG_INF("broker %s:%u", kBrokerAddr, kBrokerPort);

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
