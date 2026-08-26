/* MQTT client node: publishes SCD-40 readings to a Mosquitto broker on the
 * Raspberry Pi, answers commands from it, and relays a peer node's traffic.
 *
 * This file owns the MQTT sessions and nothing else. Acquisition is sensor.cpp,
 * the wire format protocol.cpp, command semantics commands.cpp, the CAN side
 * relay.cpp. That split is what keeps the middle two free of any socket or
 * device dependency, so tests/ can compile them on their own.
 *
 * Two sessions, one per node identity, stepped as a state machine over a single
 * poll loop: each is independently backing off, awaiting CONNACK, or serving.
 * The second identity exists because MQTT allows one Last Will per connection,
 * and node/2/status needs one of its own.
 *
 * A guided reading of this file is docs/firmware-mqtt-walkthrough.md. The
 * topics, QoS levels and will semantics it implements are decided in
 * docs/mqtt-design.md; the channels it meets the other threads on are described
 * in app_channels.h and relay.h.
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
#include "can_link.h"
#include "commands.h"
#include "protocol.h"
#include "relay.h"

LOG_MODULE_REGISTER(node, LOG_LEVEL_INF);

/* This app's half of the identity seam commands.h declares. Externally linked
 * on purpose: commands.cpp refers to it by name (notes/language-cpp.md §6). */
const char *const kNodeClientId = "nucleo-1";

namespace {

/* ---- configuration -------------------------------------------------------- */

constexpr const char *kBrokerAddr = CONFIG_NET_CONFIG_PEER_IPV4_ADDR;
constexpr uint16_t kBrokerPort = 1883;

constexpr const char *kTopicTelemetry = "node/1/telemetry";
constexpr const char *kTopicCommand = "node/1/command";
constexpr const char *kTopicAck = "node/1/ack";
constexpr const char *kTopicStatus = "node/1/status";

/* The peer node's topics. Its payloads are produced by the F072RB and only
 * carried here; nothing in MQTT requires a topic's publisher to be the thing the
 * topic names, and the peer has no IP stack at all. kPeerClientId is the
 * gateway's name FOR the peer — the peer defines the same string for itself in
 * peer-node/src/main.cpp, and neither can check the other. */
constexpr const char *kPeerClientId = "nucleo-2";

constexpr const char *kPeerTopicTelemetry = "node/2/telemetry";
constexpr const char *kPeerTopicCommand = "node/2/command";
constexpr const char *kPeerTopicAck = "node/2/ack";
constexpr const char *kPeerTopicStatus = "node/2/status";

/* The broker declares us dead after 1.5x this and fires the will, so it is a
 * failure-detection deadline rather than a heartbeat interval — we publish every
 * 5 s, so PINGREQ rarely fires. Lower it when testing the will by hand; see
 * "Testing the Last Will" in docs/mqtt-design.md. */
constexpr uint16_t kKeepaliveSec = 60;

/* Reconnect backoff: start at 1 s, double up to 30 s, so a broker that is down
 * (or a cable left unplugged) is retried patiently instead of in a hot loop. */
constexpr int kBackoffMinMs = 1000;
constexpr int kBackoffMaxMs = 30000;

/* How long a CONNECT may go unanswered before we give up on it. */
constexpr int kConnackTimeoutMs = 5000;

/* ---- the session ---------------------------------------------------------- */

enum session_state {
	/* Not connected; waiting for `retry_at`. Holds no socket, so it
	 * contributes nothing to the poll set. */
	SESSION_IDLE,
	/* CONNECT sent, CONNACK outstanding, deadline in `connack_due`. */
	SESSION_CONNECTING,
	/* CONNACK received. The only state in which anything may be published. */
	SESSION_SERVING,
};

/* One client's entire world, so that two of them cannot share any of it.
 *
 * The will structures are why this is a struct rather than four parallel arrays.
 * mqtt_connect() does not copy them — client_setup() hands the library pointers
 * and it reads through them when it serialises CONNECT — so anything shared or
 * shorter-lived than the session gives one status topic two wills and the other
 * none. Nothing reports that, because a will is only observable when a node
 * actually dies. `next_message_id` is per-session because MQTT packet ids are
 * scoped to a connection. */
struct node_session {
	/* Configuration, fixed at startup. */
	const char *client_id;
	const char *topic_telemetry;
	const char *topic_ack;
	const char *topic_status;
	/* Command topics this session subscribes to. More than one because a
	 * single connection can legitimately serve several nodes' command
	 * topics — which is exactly what this gateway does today. */
	const char *command_topics[2];
	uint8_t command_topic_count;

	/* MQTT state. */
	struct mqtt_client client;
	struct sockaddr_in broker;
	uint8_t rx[256];
	uint8_t tx[256];
	struct mqtt_topic will_topic;
	struct mqtt_utf8 will_message;
	uint16_t next_message_id;

	/* Written by the event callback, read by the loop. */
	volatile bool connected;
	volatile bool connect_failed;

	enum session_state state;
	int backoff_ms;
	int64_t retry_at;
	int64_t connack_due;
};

/* Two connections to one broker, presenting two client ids. The second buys
 * exactly one thing: MQTT 3.1.1 permits one Last Will per connection, so only a
 * client that *is* node 2 can have the broker publish `node/2/status offline`
 * when the GATEWAY dies. It buys no knowledge of the peer's own liveness — that
 * is the relay's heartbeat timeout, covering a different failure. The failure
 * table is in docs/mqtt-design.md.
 *
 * The second session speaks for a node that is not this device, so its
 * `topic_telemetry` is never used: the relay publishes by explicit topic. It is
 * the identity that matters, not the plumbing. */
node_session sessions[] = {
	{
		.client_id = kNodeClientId,
		.topic_telemetry = kTopicTelemetry,
		.topic_ack = kTopicAck,
		.topic_status = kTopicStatus,
		.command_topics = {kTopicCommand},
		.command_topic_count = 1,
	},
	{
		.client_id = kPeerClientId,
		.topic_telemetry = kPeerTopicTelemetry,
		.topic_ack = kPeerTopicAck,
		.topic_status = kPeerTopicStatus,
		.command_topics = {kPeerTopicCommand},
		.command_topic_count = 1,
	},
};

constexpr size_t kSessionCount = ARRAY_SIZE(sessions);

/* Which session carries which node's topics. */
constexpr uint8_t kOwnSession = 0;
constexpr uint8_t kPeerSession = 1;

/* ---- state ---------------------------------------------------------------- */

/* How the other threads wake this one.
 *
 * zsock_poll() understands file descriptors and a zbus channel is not one, so
 * each listener writes to an eventfd that can sit in the same poll set as the
 * sockets. That is what keeps the loop fully blocking, with no timed wakeups
 * just to check whether a bus has something. notes/zbus-guide.md §7.
 *
 * -1 until main() creates them. The producer threads start at boot and may
 * publish before then; a reading lost before the network is up costs nothing. */
int telemetry_evt_fd = -1;

/* One eventfd per relay channel, not one shared. zbus_chan_read() returns the
 * channel's current value whether or not it is fresh, so a single descriptor
 * would force the reader to check all three on every wake and republish stale
 * telemetry as new. The descriptor is the identity of the event. */
int relay_telemetry_evt_fd = -1;
int relay_ack_evt_fd = -1;
int relay_status_evt_fd = -1;

/* ---- MQTT plumbing -------------------------------------------------------- */

/* Defined below, after the session struct it operates on. */
int publish(node_session *s, const char *topic, const uint8_t *payload, size_t payload_len,
	    mqtt_qos qos, bool retain);

/* Does an MQTT topic (length-delimited, not NUL-terminated) equal this literal?
 * A length check first, so a topic that merely starts the same cannot match. */
bool topic_is(const struct mqtt_utf8 *topic, const char *literal)
{
	size_t n = strlen(literal);

	return topic->size == n && memcmp(topic->utf8, literal, n) == 0;
}

/* Encode and publish an Ack. Best effort: a failure here is logged, not
 * propagated, because the command itself may already have taken effect. */
void publish_ack(node_session *s, const char *topic, uint32_t seq, node_AckStatus status,
		 const char *detail, const node_DeviceInfo *info)
{
	uint8_t buf[node_Ack_size];
	size_t len = encode_ack(seq, status, detail, info, buf, sizeof(buf));

	if (len == 0) {
		return;
	}
	publish(s, topic, buf, len, MQTT_QOS_1_AT_LEAST_ONCE, false);
}

/* Hand a command addressed to the peer node to the relay, unmodified.
 *
 * `sequence` is the entire extent of the gateway's knowledge of this message,
 * taken from the Command *envelope* so that a command the peer never answers can
 * still be acked with a correlatable number. That is the one deliberate
 * exception to the gateway's opacity, priced in docs/mqtt-design.md. */
void forward_to_peer(const uint8_t *payload, size_t len, uint32_t sequence)
{
	struct relay_down down = {};

	if (len > sizeof(down.bytes)) {
		/* Cannot happen while node_Command_size fits (relay.cpp
		 * static_asserts it), but the buffer is not the schema's to
		 * guarantee. */
		publish_ack(&sessions[kPeerSession], kPeerTopicAck, sequence,
			    node_AckStatus_ACK_STATUS_FAILED, "command too large to relay",
			    nullptr);
		return;
	}

	down.node_id = kFirstPeerNodeId;
	down.len = static_cast<uint8_t>(len);
	down.sequence = sequence;
	memcpy(down.bytes, payload, len);

	/* A message subscriber, so this does not block on the relay thread being
	 * ready — the message is copied into the pool and delivered in order. */
	int rc = zbus_chan_pub(&chan_relay_command, &down, K_MSEC(100));

	if (rc != 0) {
		LOG_ERR("chan_relay_command publish failed: %d", rc);
		publish_ack(&sessions[kPeerSession], kPeerTopicAck, sequence,
			    node_AckStatus_ACK_STATUS_FAILED, "relay busy", nullptr);
	}
}

/* Read a PUBLISH payload out of the socket and decode it: MQTT_EVT_PUBLISH
 * carries the *length*, not the bytes.
 *
 * Every byte MUST leave the socket, wanted or not. The library resumes parsing
 * at whatever follows, so bytes left behind are decoded as the next packet's
 * fixed header and desync the connection — an oversized command would corrupt
 * the stream, not merely be truncated. So: keep what fits, drain the rest. */
void handle_incoming_publish(node_session *s, struct mqtt_client *c,
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

	/* Commands are QoS 1, so the broker redelivers with DUP set until it gets
	 * a PUBACK. Sent before any work, because it is a transport-level "I have
	 * the bytes" and not an application-level "I executed it" — that is the
	 * Ack's job. On a relayed command the distinction stops being academic:
	 * this says the *gateway* has the bytes and nothing about whether the peer
	 * saw them. QoS is hop-by-hop; the Ack is end to end. */
	if (pub->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
		struct mqtt_puback_param puback = {};

		puback.message_id = pub->message_id;
		mqtt_publish_qos1_ack(c, &puback);
	}

	/* Which node was this addressed to? The topic is the only thing that
	 * distinguishes them — nothing inside a Command says which node it is for.
	 * notes/protobuf-guide.md §4 on the topic being part of the contract. */
	bool for_peer = topic_is(&pub->message.topic.topic, kPeerTopicCommand);

	/* From here the transport is done with the bytes: protocol.cpp decides
	 * whether they are a Command, commands.cpp decides what it means. */
	node_Command cmd;

	if (!decode_command(payload, kept, oversized, &cmd)) {
		/* sequence 0: we could not read one, so there is nothing to
		 * correlate against. Answered by the gateway even when addressed
		 * to the peer — bytes that are not a Command cannot be forwarded
		 * as one, and only the gateway is in a position to say so. */
		publish_ack(s, for_peer ? kPeerTopicAck : s->topic_ack, 0,
			    node_AckStatus_ACK_STATUS_MALFORMED,
			    oversized ? "payload too large" : "decode failed", nullptr);
		return;
	}

	if (for_peer) {
		forward_to_peer(payload, kept, cmd.sequence);
		return;
	}

	struct command_result result;

	handle_command(cmd, &result);

	publish_ack(s, s->topic_ack, cmd.sequence, result.status,
		    result.detail[0] != '\0' ? result.detail : nullptr,
		    result.has_info ? &result.info : nullptr);
}

/* One callback for every session, so it has to recover which one it was invoked
 * for. mqtt_client's `user_data` is a void * the library never touches, set to
 * the owning session in client_setup(): one cast, and no dependence on the
 * session struct's layout. */
void mqtt_evt_handler(struct mqtt_client *const c, const struct mqtt_evt *evt)
{
	node_session *s = static_cast<node_session *>(c->user_data);

	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->result != 0) {
			LOG_ERR("[%s] CONNACK refused: %d", s->client_id, evt->result);
			s->connect_failed = true;
			break;
		}
		LOG_INF("[%s] connected to broker", s->client_id);
		s->connected = true;
		break;

	case MQTT_EVT_DISCONNECT:
		LOG_WRN("[%s] disconnected: %d", s->client_id, evt->result);
		s->connected = false;
		break;

	case MQTT_EVT_PUBLISH:
		handle_incoming_publish(s, c, &evt->param.publish);
		break;

	case MQTT_EVT_PUBACK:
		LOG_DBG("[%s] PUBACK id %u", s->client_id, evt->param.puback.message_id);
		break;

	case MQTT_EVT_SUBACK:
		LOG_INF("[%s] subscribed", s->client_id);
		break;

	case MQTT_EVT_PINGRESP:
		LOG_DBG("[%s] PINGRESP", s->client_id);
		break;

	default:
		break;
	}
}

void client_setup(node_session *s)
{
	mqtt_client_init(&s->client);

	s->broker = {};
	s->broker.sin_family = AF_INET;
	s->broker.sin_port = htons(kBrokerPort);
	zsock_inet_pton(AF_INET, kBrokerAddr, &s->broker.sin_addr);

	s->client.broker = &s->broker;
	s->client.evt_cb = mqtt_evt_handler;
	s->client.client_id.utf8 = reinterpret_cast<uint8_t *>(const_cast<char *>(s->client_id));
	s->client.client_id.size = strlen(s->client_id);
	s->client.user_name = nullptr;
	s->client.password = nullptr;
	s->client.protocol_version = MQTT_VERSION_3_1_1;
	s->client.keepalive = kKeepaliveSec;
	/* Clean session: we want no server-side queue replayed on reconnect —
	 * stale telemetry is worse than none (docs/mqtt-design.md). */
	s->client.clean_session = 1;

	/* How the shared event callback finds its way back here. */
	s->client.user_data = s;

	s->client.rx_buf = s->rx;
	s->client.rx_buf_size = sizeof(s->rx);
	s->client.tx_buf = s->tx;
	s->client.tx_buf_size = sizeof(s->tx);
	s->client.transport.type = MQTT_TRANSPORT_NON_SECURE;

	/* Last Will: registered at CONNECT, held by the broker, and published on
	 * our behalf if we vanish without a clean DISCONNECT — no firmware runs in
	 * that path, which is the point. Per session; see struct node_session. */
	s->will_topic.topic.utf8 =
		reinterpret_cast<uint8_t *>(const_cast<char *>(s->topic_status));
	s->will_topic.topic.size = strlen(s->topic_status);
	s->will_topic.qos = MQTT_QOS_1_AT_LEAST_ONCE;
	s->will_message.utf8 = reinterpret_cast<uint8_t *>(const_cast<char *>("offline"));
	s->will_message.size = strlen("offline");

	s->client.will_topic = &s->will_topic;
	s->client.will_message = &s->will_message;
	s->client.will_retain = 1;
}

/* Payload is a length-delimited byte range, not a C string: protobuf output is
 * binary and routinely contains NUL bytes, so strlen() would truncate it. */
int publish(node_session *s, const char *topic, const uint8_t *payload, size_t payload_len,
	    mqtt_qos qos, bool retain)
{
	struct mqtt_publish_param param = {};

	param.message.topic.topic.utf8 = reinterpret_cast<uint8_t *>(const_cast<char *>(topic));
	param.message.topic.topic.size = strlen(topic);
	param.message.topic.qos = qos;
	param.message.payload.data = const_cast<uint8_t *>(payload);
	param.message.payload.len = payload_len;
	/* message_id is only meaningful for QoS 1/2; at QoS 0 there is no PUBACK
	 * to correlate and the field is not put on the wire. It only has to be
	 * non-zero and distinct among *in-flight* messages, and we never have more
	 * than one outstanding, so a counter is enough. */
	if (qos == MQTT_QOS_0_AT_MOST_ONCE) {
		param.message_id = 0;
	} else {
		s->next_message_id++;
		if (s->next_message_id == 0) {
			s->next_message_id = 1;
		}
		param.message_id = s->next_message_id;
	}
	param.dup_flag = 0;
	param.retain_flag = retain ? 1 : 0;

	return mqtt_publish(&s->client, &param);
}

/* Convenience for the one topic that stays plain ASCII: `status` is also written
 * by the broker as our Last Will, so it cannot be protobuf (docs/mqtt-design.md). */
int publish_text(node_session *s, const char *topic, const char *text, mqtt_qos qos, bool retain)
{
	return publish(s, topic, reinterpret_cast<const uint8_t *>(text), strlen(text), qos,
		       retain);
}

/* A session subscribes to its own command topics, in one SUBSCRIBE. A wildcard
 * `node/+/command` would also work and is deliberately not used: it would
 * silently accept a node/9/command this gateway has no route for. */
int subscribe_to_commands(node_session *s)
{
	struct mqtt_topic topics[ARRAY_SIZE(s->command_topics)] = {};
	struct mqtt_subscription_list list = {};

	for (uint8_t i = 0; i < s->command_topic_count; i++) {
		topics[i].topic.utf8 =
			reinterpret_cast<uint8_t *>(const_cast<char *>(s->command_topics[i]));
		topics[i].topic.size = strlen(s->command_topics[i]);
		topics[i].qos = MQTT_QOS_1_AT_LEAST_ONCE;
	}

	list.list = topics;
	list.list_count = s->command_topic_count;
	list.message_id = 1U;

	return mqtt_subscribe(&s->client, &list);
}

/* ---- telemetry: from the bus to the wire ---------------------------------- */

/* zbus listener for chan_telemetry. Runs **in the publisher's context** — on the
 * sensor thread, inside zbus_chan_pub(), with the channel locked — so it does
 * the one thing that cannot block and leaves the reading in the channel. The
 * eventfd carries "something new happened"; the channel carries the value. */
void on_telemetry(const struct zbus_channel *chan)
{
	ARG_UNUSED(chan);

	if (telemetry_evt_fd >= 0) {
		zvfs_eventfd_write(telemetry_evt_fd, 1);
	}
}

/* Drain the eventfd, read the newest reading off the bus, encode it, publish it.
 * Returns an mqtt_publish() result, or 0 if there was nothing to send. */
int publish_pending_telemetry(node_session *s)
{
	zvfs_eventfd_t signalled = 0;

	/* Non-blocking, and reading resets the counter to zero. Its value is the
	 * number of readings taken since we last looked: more than one means the
	 * channel overwrote some, which the host sees as a sequence gap. */
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

	rc = publish(s, s->topic_telemetry, payload, len, MQTT_QOS_0_AT_MOST_ONCE, false);
	if (rc == 0) {
		LOG_INF("published telemetry seq=%u (%zu bytes)", reading.sequence, len);
	}
	return rc;
}

/* ---- the relay: from CAN to the wire --------------------------------------- */

/* Three listeners, three descriptors. Same shape as on_telemetry() above, and
 * under the same constraint — each runs in the relay RX thread's context with
 * the channel locked, so it signals and returns. */
void on_relay_telemetry(const struct zbus_channel *chan)
{
	ARG_UNUSED(chan);
	if (relay_telemetry_evt_fd >= 0) {
		zvfs_eventfd_write(relay_telemetry_evt_fd, 1);
	}
}

void on_relay_ack(const struct zbus_channel *chan)
{
	ARG_UNUSED(chan);
	if (relay_ack_evt_fd >= 0) {
		zvfs_eventfd_write(relay_ack_evt_fd, 1);
	}
}

void on_relay_status(const struct zbus_channel *chan)
{
	ARG_UNUSED(chan);
	if (relay_status_evt_fd >= 0) {
		zvfs_eventfd_write(relay_status_evt_fd, 1);
	}
}

/* Publish whatever the relay left on an upward channel.
 *
 * The payload is republished byte for byte: this function does not decode it and
 * could not tell a Telemetry from an Ack. The channel it arrived on decides the
 * topic, and that is the whole of the gateway's knowledge about it. */
int publish_relayed(node_session *s, int fd, const struct zbus_channel *chan, const char *topic,
		    mqtt_qos qos, const char *what)
{
	zvfs_eventfd_t signalled = 0;

	if (zvfs_eventfd_read(fd, &signalled) != 0) {
		return 0;
	}

	/* Coalescing means different things on the two channels. On telemetry it
	 * is expected — latest-wins, and the sequence gap tells the host. On acks
	 * it is an invariant violation: the relay serialises command round trips,
	 * so more than one outstanding ack means that premise broke (relay.h). */
	if (signalled > 1) {
		if (chan == &chan_relay_ack) {
			LOG_ERR("%llu relayed acks coalesced — round trips are not serialised",
				static_cast<unsigned long long>(signalled));
		} else {
			LOG_WRN("%llu relayed %s coalesced into one publish",
				static_cast<unsigned long long>(signalled - 1), what);
		}
	}

	struct relay_up up;
	int rc = zbus_chan_read(chan, &up, K_MSEC(50));

	if (rc != 0) {
		LOG_ERR("relay channel read failed: %d", rc);
		return 0;
	}

	rc = publish(s, topic, up.bytes, up.len, qos, false);
	if (rc == 0) {
		LOG_INF("relayed %s from node %u (%u bytes)", what, up.node_id, up.len);
	}
	return rc;
}

/* The peer's liveness, as plain ASCII on a retained topic — the same shape the
 * gateway's own status uses, so a host need not care which node published it. */
int publish_relayed_status(node_session *s)
{
	zvfs_eventfd_t signalled = 0;

	if (zvfs_eventfd_read(relay_status_evt_fd, &signalled) != 0) {
		return 0;
	}

	struct relay_status st;
	int rc = zbus_chan_read(&chan_relay_status, &st, K_MSEC(50));

	if (rc != 0) {
		LOG_ERR("chan_relay_status read failed: %d", rc);
		return 0;
	}

	return publish_text(s, kPeerTopicStatus, st.online ? "online" : "offline",
			    MQTT_QOS_1_AT_LEAST_ONCE, true);
}

/* ---- session lifecycle ----------------------------------------------------- */

/* Give up on a session and schedule the next attempt. `reached_connack` decides
 * the backoff: a link that worked and then dropped deserves a fast retry, while
 * one that never connected should keep doubling. */
void session_drop(node_session *s, bool reached_connack, const char *why)
{
	if (s->state == SESSION_SERVING) {
		/* A clean DISCONNECT deliberately suppresses the will — we only
		 * want "offline" published when we die unexpectedly. */
		mqtt_disconnect(&s->client, nullptr);
	} else if (s->state == SESSION_CONNECTING) {
		mqtt_abort(&s->client);
	}

	s->backoff_ms = reached_connack ? kBackoffMinMs
					: MIN(s->backoff_ms * 2, kBackoffMaxMs);
	s->state = SESSION_IDLE;
	s->connected = false;
	s->retry_at = k_uptime_get() + s->backoff_ms;

	LOG_WRN("[%s] %s — reconnecting in %d ms", s->client_id, why, s->backoff_ms);
}

/* Send CONNECT. The CONNACK arrives asynchronously, so this only moves the
 * session to CONNECTING and arms a deadline; the main loop pumps input. */
void session_connect(node_session *s)
{
	client_setup(s);
	s->connected = false;
	s->connect_failed = false;

	int rc = mqtt_connect(&s->client);

	if (rc != 0) {
		/* No TCP connection: broker down, or 192.168.10.1 unreachable. */
		s->state = SESSION_IDLE;
		session_drop(s, false, "mqtt_connect failed");
		return;
	}

	s->state = SESSION_CONNECTING;
	s->connack_due = k_uptime_get() + kConnackTimeoutMs;
}

/* CONNACK arrived: announce ourselves and subscribe. Retained status, so a
 * harness starting later immediately learns we are up. */
void session_serving(node_session *s)
{
	s->state = SESSION_SERVING;
	s->backoff_ms = kBackoffMinMs;

	/* The gateway's own session may say "online" unconditionally; it is
	 * evidently up. The peer's may not — it speaks for a node this device
	 * merely relays, which may well be dead right now, and a false "online" on
	 * a retained topic would survive until the next heartbeat corrected it. So
	 * it publishes what the relay believes, and PEER_UNKNOWN publishes nothing
	 * at all — see liveness_step() in relay.h for why that state exists. */
	if (s == &sessions[kPeerSession] && kPeerSession != kOwnSession) {
		struct relay_status st;

		if (zbus_chan_read(&chan_relay_status, &st, K_MSEC(50)) == 0 && st.node_id != 0) {
			publish_text(s, s->topic_status, st.online ? "online" : "offline",
				     MQTT_QOS_1_AT_LEAST_ONCE, true);
		}
	} else {
		publish_text(s, s->topic_status, "online", MQTT_QOS_1_AT_LEAST_ONCE, true);
	}

	subscribe_to_commands(s);
}

/* The nearest deadline across every session, or -1 for "no deadline at all".
 *
 * mqtt_keepalive_time_left() returns -1 when keepalive is 0, meaning "never";
 * folding that into a minimum would turn *never* into *immediately* and spin the
 * loop, so negatives are skipped rather than compared. */
int next_deadline_ms(int64_t now)
{
	int64_t best = -1;

	for (size_t i = 0; i < kSessionCount; i++) {
		node_session *s = &sessions[i];
		int64_t due;

		switch (s->state) {
		case SESSION_IDLE:
			due = s->retry_at - now;
			break;
		case SESSION_CONNECTING:
			due = s->connack_due - now;
			break;
		case SESSION_SERVING: {
			int left = mqtt_keepalive_time_left(&s->client);

			if (left < 0) {
				continue;
			}
			due = left;
			break;
		}
		default:
			continue;
		}

		if (due < 0) {
			due = 0;
		}
		if (best < 0 || due < best) {
			best = due;
		}
	}
	return static_cast<int>(best);
}

}  // namespace

/* At global scope: the observation records ZBUS_CHAN_DEFINE emits in sensor.cpp
 * and relay.cpp refer to these symbols by name. All four live here rather than
 * beside their channels — the network side owns the observers that feed it. */
ZBUS_LISTENER_DEFINE(telemetry_listener, on_telemetry);
ZBUS_LISTENER_DEFINE(relay_telemetry_listener, on_relay_telemetry);
ZBUS_LISTENER_DEFINE(relay_ack_listener, on_relay_ack);
ZBUS_LISTENER_DEFINE(relay_status_listener, on_relay_status);

int main(void)
{
	/* Counters start at 0 and never block a reader, so a poll on one is
	 * simply "has that producer published since I last looked". */
	telemetry_evt_fd = zvfs_eventfd(0, ZVFS_EFD_NONBLOCK);
	relay_telemetry_evt_fd = zvfs_eventfd(0, ZVFS_EFD_NONBLOCK);
	relay_ack_evt_fd = zvfs_eventfd(0, ZVFS_EFD_NONBLOCK);
	relay_status_evt_fd = zvfs_eventfd(0, ZVFS_EFD_NONBLOCK);

	if (telemetry_evt_fd < 0 || relay_telemetry_evt_fd < 0 || relay_ack_evt_fd < 0 ||
	    relay_status_evt_fd < 0) {
		/* CONFIG_ZVFS_EVENTFD_MAX is a hard count, and the fourth one
		 * failing is what you get for adding a channel without raising it. */
		LOG_ERR("eventfd: %d (raise CONFIG_ZVFS_EVENTFD_MAX?)", errno);
		return 0;
	}

	/* Two sessions presenting the same client id make the broker perform a
	 * takeover: each CONNECT kicks the other off, forever, and it reads exactly
	 * like a network fault. Costs nothing to rule out. */
	for (size_t i = 0; i < kSessionCount; i++) {
		for (size_t j = i + 1; j < kSessionCount; j++) {
			__ASSERT(strcmp(sessions[i].client_id, sessions[j].client_id) != 0,
				 "duplicate MQTT client id");
		}
		sessions[i].state = SESSION_IDLE;
		sessions[i].backoff_ms = kBackoffMinMs;
		sessions[i].retry_at = 0; /* connect immediately */
	}

	LOG_INF("broker %s:%u, %zu session(s)", kBrokerAddr, kBrokerPort, kSessionCount);

	while (true) {
		int64_t now = k_uptime_get();

		/* Start anything whose backoff has elapsed. A session that is down
		 * does not stop the others — the whole reason this is a state
		 * machine rather than a blocking connect-serve-disconnect. */
		for (size_t i = 0; i < kSessionCount; i++) {
			if (sessions[i].state == SESSION_IDLE && now >= sessions[i].retry_at) {
				session_connect(&sessions[i]);
			}
		}

		/* Assemble the poll set from what currently exists. A session
		 * without a socket contributes nothing, and an eventfd is watched
		 * only while the session that would publish it is serving —
		 * otherwise a producer signalling into a dead session spins this
		 * loop at the rate of its own cadence. */
		struct zsock_pollfd fds[kSessionCount + 4] = {};
		int sock_slot[kSessionCount];
		int n = 0;

		for (size_t i = 0; i < kSessionCount; i++) {
			if (sessions[i].state == SESSION_IDLE) {
				sock_slot[i] = -1;
				continue;
			}
			sock_slot[i] = n;
			fds[n].fd = sessions[i].client.transport.tcp.sock;
			fds[n].events = ZSOCK_POLLIN;
			n++;
		}

		const bool own_serving = sessions[kOwnSession].state == SESSION_SERVING;
		const bool peer_serving = sessions[kPeerSession].state == SESSION_SERVING;

		int own_telemetry_slot = -1;
		int relay_telemetry_slot = -1;
		int relay_ack_slot = -1;
		int relay_status_slot = -1;

		if (own_serving) {
			own_telemetry_slot = n;
			fds[n].fd = telemetry_evt_fd;
			fds[n].events = ZSOCK_POLLIN;
			n++;
		}
		if (peer_serving) {
			relay_telemetry_slot = n;
			fds[n].fd = relay_telemetry_evt_fd;
			fds[n].events = ZSOCK_POLLIN;
			n++;
			relay_ack_slot = n;
			fds[n].fd = relay_ack_evt_fd;
			fds[n].events = ZSOCK_POLLIN;
			n++;
			relay_status_slot = n;
			fds[n].fd = relay_status_evt_fd;
			fds[n].events = ZSOCK_POLLIN;
			n++;
		}

		/* One wait, one deadline, with every producer keeping its own clock
		 * and saying so through a descriptor. Each new source of work is
		 * one more fd and one more `if`, never another timeout to reconcile
		 * against the keepalive. */
		if (zsock_poll(fds, n, next_deadline_ms(now)) < 0) {
			LOG_ERR("poll: %d", errno);
			k_msleep(kBackoffMinMs);
			continue;
		}

		/* Input first, so a CONNACK or a DISCONNECT is visible to the
		 * per-session handling below in the same pass. */
		for (size_t i = 0; i < kSessionCount; i++) {
			node_session *s = &sessions[i];

			if (sock_slot[i] < 0 || !(fds[sock_slot[i]].revents & ZSOCK_POLLIN)) {
				continue;
			}
			if (mqtt_input(&s->client) != 0) {
				session_drop(s, s->state == SESSION_SERVING, "mqtt_input failed");
			}
		}

		now = k_uptime_get();

		for (size_t i = 0; i < kSessionCount; i++) {
			node_session *s = &sessions[i];

			switch (s->state) {
			case SESSION_CONNECTING:
				if (s->connected) {
					session_serving(s);
				} else if (s->connect_failed) {
					session_drop(s, false, "CONNACK refused");
				} else if (now >= s->connack_due) {
					session_drop(s, false, "no CONNACK");
				}
				break;

			case SESSION_SERVING: {
				if (!s->connected) {
					session_drop(s, true, "connection lost");
					break;
				}
				/* Sends PINGREQ when due, and surfaces a dead
				 * connection. */
				int rc = mqtt_live(&s->client);

				if (rc != 0 && rc != -EAGAIN) {
					session_drop(s, true, "mqtt_live failed");
				}
				break;
			}

			default:
				break;
			}
		}

		/* Publishing last, and only on sessions still serving after the
		 * handling above — a session dropped in this pass must not be
		 * published to. */
		if (own_telemetry_slot >= 0 &&
		    (fds[own_telemetry_slot].revents & ZSOCK_POLLIN) &&
		    sessions[kOwnSession].state == SESSION_SERVING) {
			if (publish_pending_telemetry(&sessions[kOwnSession]) != 0) {
				session_drop(&sessions[kOwnSession], true, "publish failed");
			}
		}

		/* The relay's three, at the same QoS as this node's own topics and
		 * for the same reasons — docs/mqtt-design.md. */
		node_session *ps = &sessions[kPeerSession];

		if (relay_telemetry_slot >= 0 &&
		    (fds[relay_telemetry_slot].revents & ZSOCK_POLLIN) &&
		    ps->state == SESSION_SERVING) {
			if (publish_relayed(ps, relay_telemetry_evt_fd, &chan_relay_telemetry,
					    kPeerTopicTelemetry, MQTT_QOS_0_AT_MOST_ONCE,
					    "telemetry") != 0) {
				session_drop(ps, true, "relay publish failed");
			}
		}
		if (relay_ack_slot >= 0 && (fds[relay_ack_slot].revents & ZSOCK_POLLIN) &&
		    ps->state == SESSION_SERVING) {
			if (publish_relayed(ps, relay_ack_evt_fd, &chan_relay_ack, kPeerTopicAck,
					    MQTT_QOS_1_AT_LEAST_ONCE, "ack") != 0) {
				session_drop(ps, true, "relay publish failed");
			}
		}
		if (relay_status_slot >= 0 && (fds[relay_status_slot].revents & ZSOCK_POLLIN) &&
		    ps->state == SESSION_SERVING) {
			if (publish_relayed_status(ps) != 0) {
				session_drop(ps, true, "relay publish failed");
			}
		}
	}
	return 0;
}
