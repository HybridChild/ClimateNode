/* MQTT client node: publishes SCD-40 readings to a Mosquitto broker on the
 * Raspberry Pi, answers commands from it, and relays a peer node's traffic.
 *
 * This file owns the MQTT sessions and nothing else. The application is five
 * translation units, each with one responsibility:
 *
 *   sensor.cpp     acquisition — owns the SCD-40, the sample period, the bounds
 *   protocol.cpp   the wire format — the only place internal types meet protobuf
 *   commands.cpp   command semantics — dispatch, duplicate suppression, identity
 *   relay.cpp      the CAN side — heartbeat liveness, ISO-TP in both directions
 *   main.cpp       this file: connect, poll, publish, reconnect
 *
 * sensor.cpp and main.cpp meet on the zbus channels declared in app_channels.h;
 * relay.cpp and main.cpp meet on the ones in relay.h. See those headers for why
 * each channel uses the observer style it does.
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
 *   node/2/...         the same four, for the F072RB peer node reached over CAN.
 *                      Their payloads are produced and consumed by that node;
 *                      this file only carries them (relay.h).
 *
 * Telemetry, Command and Ack payloads are nanopb-encoded Protobuf, generated from
 * proto/node.proto at build time. `status` stays plain ASCII: the broker itself
 * writes it as the will, so firmware cannot encode it.
 *
 * ---------------------------------------------------------------------------
 * Structure: N sessions, one thread, one wait
 * ---------------------------------------------------------------------------
 *
 * Every piece of connection state lives in a `struct node_session`, and main()
 * steps an array of them. Each session is independently in one of three states —
 * waiting out a backoff, awaiting its CONNACK, or serving — so one can be
 * reconnecting while another publishes.
 *
 * There is still exactly ONE thread and ONE wait. The poll set is assembled from
 * whichever sessions currently have a socket, plus the eventfds of the sessions
 * that are actually serving, and the timeout is the nearest deadline across all
 * of them. The alternative — a thread per session — would keep the old
 * run_session() nearly unchanged at the cost of another stack and the loss of
 * the "one wait" property this program is built around.
 *
 * Reconnect is not error handling bolted on the side; it is the shape of the
 * program, because a node that cannot survive a cable pull is not finished (see
 * the README's learning goals).
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
#include "can_link.h"
#include "commands.h"
#include "protocol.h"
#include "relay.h"

LOG_MODULE_REGISTER(node, LOG_LEVEL_INF);

/* This app's half of the identity seam commands.h declares. At global scope and
 * externally linked on purpose: commands.cpp refers to it by name, and internal
 * linkage would stop the two from meeting (notes/language-cpp.md §6). */
const char *const kNodeClientId = "nucleo-1";

namespace {

/* ---- configuration -------------------------------------------------------- */

constexpr const char *kBrokerAddr = CONFIG_NET_CONFIG_PEER_IPV4_ADDR;
constexpr uint16_t kBrokerPort = 1883;

constexpr const char *kTopicTelemetry = "node/1/telemetry";
constexpr const char *kTopicCommand = "node/1/command";
constexpr const char *kTopicAck = "node/1/ack";
constexpr const char *kTopicStatus = "node/1/status";

/* The peer node's topics. Its payloads are produced by the F072RB and carried
 * here; nothing in MQTT requires the publisher of a topic to *be* the thing the
 * topic names, and the peer has no IP stack at all. */
/* The peer's MQTT client id. Distinct from kNodeClientId, and asserted so at
 * boot: two sessions presenting the same id make the broker perform a takeover,
 * each CONNECT kicking the other off forever, which reads exactly like a network
 * fault. Note this is the gateway's name FOR the peer — the peer's own
 * commands.h identity is defined in sensor-node/src/main.cpp and reported by
 * GetDeviceInfo; they say the same thing and neither can check the other. */
constexpr const char *kPeerClientId = "nucleo-2";

constexpr const char *kPeerTopicTelemetry = "node/2/telemetry";
constexpr const char *kPeerTopicCommand = "node/2/command";
constexpr const char *kPeerTopicAck = "node/2/ack";
constexpr const char *kPeerTopicStatus = "node/2/status";

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

/* Everything that used to be a file-scope singleton, and one thing that used to
 * be worse than that.
 *
 * The will structures are the reason this is a struct rather than four parallel
 * arrays. They were function-local `static`s inside client_setup(), which is
 * correct for exactly one client and silently wrong for two: both would register
 * the same will topic, so one status topic would get two wills and the other
 * none. Nothing would report it, and it is observable only when a node actually
 * dies. Moving them in here is the fix, not a tidy-up.
 *
 * `next_message_id` moves in for a smaller but similar reason: MQTT packet ids
 * are scoped to a connection, so one shared counter was wrong on principle even
 * while it worked. */
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

/* Two connections to one broker, presenting two client ids.
 *
 * The second one buys exactly one thing, and it is worth being precise because
 * everything else already worked on a single connection: **MQTT 3.1.1 permits
 * one Last Will per connection.** So only a client that *is* node 2 can have the
 * broker publish `node/2/status offline` when the GATEWAY dies. With one
 * connection that failure left node/2/status retained as `online` forever —
 * stale, and stale on the one topic whose whole job is to be true.
 *
 * What it does NOT buy: any knowledge of the peer's own liveness. The broker
 * still cannot observe the F072RB, which has no IP stack; "peer dead, gateway
 * alive" is firmware-published by the relay's heartbeat timeout in either
 * design. The two mechanisms cover different failures, which is why both exist.
 *
 * Note that this session speaks for a node that is not this device, and its
 * `topic_telemetry` is never used — the relay's payloads are published by
 * explicit topic. It is the identity that matters, not the plumbing. */
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

/* How the sensor thread wakes this one.
 *
 * The serve loop must wait on several unrelated things at once: bytes from the
 * broker, and fresh work from two other threads. zsock_poll() only understands
 * file descriptors, and a zbus channel is not one — so each zbus listener writes
 * to an eventfd, which *is* a descriptor and can sit in the same poll set as the
 * sockets. That keeps the loop fully blocking: no timed wakeups just to check
 * whether a bus has something.
 *
 * -1 until main() creates them. The producer threads start at boot and may
 * publish before then; the listeners check, and a reading lost before the
 * network is even up is of no consequence. */
int telemetry_evt_fd = -1;

/* One eventfd per relay channel.
 *
 * A single shared fd would have been cheaper and wrong: it would only say
 * "something happened", and since zbus_chan_read() returns the channel's current
 * value whether or not it is fresh, the reader would have to read all three on
 * every wake and would republish stale telemetry as though it were new. The
 * descriptor is the identity of the event, not merely its occurrence. */
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
 * The `sequence` argument is the entire extent of the gateway's knowledge of
 * this message, and it comes from the Command *envelope* rather than from its
 * payload arm. The relay needs it so that a command the peer never answers can
 * still be acknowledged to the host with a correlatable sequence number — the
 * one deliberate exception to the gateway's opacity, priced and recorded in
 * docs/mqtt-design.md. The bytes themselves are forwarded untouched. */
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

/* Read a PUBLISH payload out of the socket and decode it. The MQTT_EVT_PUBLISH
 * event carries only the *length*; the bytes must be read explicitly.
 *
 * Every byte of the payload MUST leave the socket, even the ones we do not want.
 * The library resumes parsing at whatever follows, so bytes left behind are
 * decoded as the next packet's fixed header and desync the connection — an
 * oversized command would corrupt the stream, not merely be truncated. So we
 * keep the first sizeof(payload)-1 bytes and drain the remainder. */
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

	/* Commands are QoS 1, so the broker expects a PUBACK from us. Without it
	 * the broker redelivers (with DUP set) until it gets one. Send it before
	 * doing any work: the PUBACK is a transport-level "I have the bytes", not
	 * an application-level "I executed it" — that is what the Ack is for.
	 *
	 * With a relayed command that distinction stops being academic: this
	 * PUBACK says the *gateway* has the bytes and nothing about whether the
	 * peer ever saw them. QoS is hop-by-hop; the Ack is end to end. */
	if (pub->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
		struct mqtt_puback_param puback = {};

		puback.message_id = pub->message_id;
		mqtt_publish_qos1_ack(c, &puback);
	}

	/* Which node was this addressed to? The topic is the only thing that
	 * distinguishes them — the payloads are the same message type, and
	 * nothing inside a Command says which node it is for. Exactly the point
	 * notes/protobuf-guide.md §4 makes about the topic being part of the
	 * contract. */
	bool for_peer = topic_is(&pub->message.topic.topic, kPeerTopicCommand);

	/* From here the transport is done with the bytes: protocol.cpp decides
	 * whether they are a Command, commands.cpp decides what it means, and
	 * this layer only reports the outcome back to the host. */
	node_Command cmd;

	if (!decode_command(payload, kept, oversized, &cmd)) {
		/* sequence 0: we could not read one, so there is nothing to
		 * correlate against. The host learns the message was garbage.
		 *
		 * Answered by the gateway even when addressed to the peer, and
		 * that is right: bytes that are not a Command cannot be
		 * forwarded as one, and the gateway is the only party in a
		 * position to say so. */
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
 * for. mqtt_client's final member is a `void *user_data` the library never
 * touches (include/zephyr/net/mqtt.h), set to the owning session in
 * client_setup(). One cast, no CONTAINER_OF, and no dependence on the session
 * struct's layout. */
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
	 * our behalf if we vanish without a clean DISCONNECT. This is how the host
	 * learns about a crash or cable pull — no firmware runs in that path.
	 *
	 * Per session, not per file. See the note on struct node_session. */
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
	/* message_id is only meaningful for QoS 1/2 — at QoS 0 there is no PUBACK
	 * to correlate, and the field is not even put on the wire. A counter is
	 * enough here: it only has to be non-zero and distinct among *in-flight*
	 * messages, and we never have more than one outstanding. Per session,
	 * because packet ids are scoped to a connection. */
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
 * silently accept a node/9/command this gateway has no route for, and the set of
 * nodes it can reach is a fact worth stating rather than discovering. */
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
int publish_pending_telemetry(node_session *s)
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

	rc = publish(s, s->topic_telemetry, payload, len, MQTT_QOS_0_AT_MOST_ONCE, false);
	if (rc == 0) {
		LOG_INF("published telemetry seq=%u (%zu bytes)", reading.sequence, len);
	}
	return rc;
}

/* ---- the relay: from CAN to the wire --------------------------------------- */

/* Three listeners, three descriptors. Each does the only thing a listener may
 * do — it runs in the publisher's context (the relay's RX thread) with the
 * channel locked, so it signals and returns. Identical in shape to
 * on_telemetry() above; what differs is only which fd, which is the point. */
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
 * The payload is republished byte for byte. This function does not decode it,
 * does not validate it, and could not tell a Telemetry from an Ack — the channel
 * it arrived on is what decides the topic, which is the whole of the gateway's
 * knowledge about it. That is the dumb-gateway property, and it is one function
 * long on purpose. */
int publish_relayed(node_session *s, int fd, const struct zbus_channel *chan, const char *topic,
		    mqtt_qos qos, const char *what)
{
	zvfs_eventfd_t signalled = 0;

	if (zvfs_eventfd_read(fd, &signalled) != 0) {
		return 0;
	}

	/* Coalescing means different things on the two channels, so it is
	 * reported at different severities. On telemetry it is expected and
	 * documented — latest-wins, and the sequence gap tells the host. On acks
	 * it is an invariant violation: the relay serialises command round trips,
	 * so more than one outstanding ack means that premise broke. */
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
 * gateway's own status uses, because a host should not have to care which node
 * published its liveness or how that node learned it. */
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

/* Give up on a session and schedule the next attempt.
 *
 * `reached_connack` is what decides the backoff: a link that worked and then
 * dropped deserves a fast retry, while one that never connected at all should
 * keep doubling, so a down broker or an unplugged cable is retried patiently
 * instead of in a hot loop. */
void session_drop(node_session *s, bool reached_connack, const char *why)
{
	if (s->state == SESSION_SERVING) {
		/* Best-effort clean teardown. A clean DISCONNECT deliberately
		 * suppresses the will — we only want "offline" published when we
		 * die unexpectedly. */
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
 * harness starting later immediately learns we are up; the will (also retained)
 * overwrites it if we die. */
void session_serving(node_session *s)
{
	s->state = SESSION_SERVING;
	s->backoff_ms = kBackoffMinMs;

	/* The gateway's own session may say "online" unconditionally — it is
	 * speaking for itself, and it is evidently up.
	 *
	 * The peer's session may not. It speaks for a node this device merely
	 * relays, whose liveness the relay tracks independently, and which may
	 * well be dead right now. Announcing "online" here would overwrite a
	 * correct `offline` with a false one, on a retained topic, every time the
	 * gateway reconnected to the broker. So it publishes what the relay
	 * currently believes, and PEER_UNKNOWN publishes nothing at all — see
	 * liveness_step() in relay.h for why that state exists. */
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
 * mqtt_keepalive_time_left() returns -1 when keepalive is 0 (subsys/net/lib/
 * mqtt/mqtt.c), meaning "never". Folding that into a minimum would turn "never"
 * into "immediately" and spin the loop, so negatives are skipped rather than
 * compared. Unreachable while every session sets a keepalive, and one line. */
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
 * and relay.cpp refer to these symbols by name. The relay's three are defined
 * here rather than in relay.cpp for the same reason the sensor's one is: the
 * network side owns the observers that feed the network. */
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
		/* CONFIG_ZVFS_EVENTFD_MAX is the thing to check: it is a hard
		 * count, and the fourth one failing is what you get for adding a
		 * channel without raising it. */
		LOG_ERR("eventfd: %d (raise CONFIG_ZVFS_EVENTFD_MAX?)", errno);
		return 0;
	}

	/* Two sessions presenting the same client id make the broker perform a
	 * takeover: each CONNECT kicks the other off, forever, and it reads
	 * exactly like a network fault. Costs nothing to rule out. */
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

		/* Start anything whose backoff has elapsed. A session that is
		 * down does not stop the others: this is the whole reason the
		 * lifecycle became a state machine rather than a blocking
		 * connect-serve-disconnect function. */
		for (size_t i = 0; i < kSessionCount; i++) {
			if (sessions[i].state == SESSION_IDLE && now >= sessions[i].retry_at) {
				session_connect(&sessions[i]);
			}
		}

		/* Assemble the poll set from what currently exists. A session
		 * without a socket contributes nothing, and an eventfd is only
		 * watched when the session that would publish it is serving —
		 * otherwise a producer signalling into a dead session would spin
		 * this loop at the rate of its own cadence. */
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

		/* One wait, one deadline — still, with every producer keeping its
		 * own clock and saying so through a descriptor. That is the
		 * property worth watching as nodes accumulate: each new source of
		 * work is one more fd and one more `if`, never another timeout to
		 * reconcile against the keepalive. */
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

		/* The relay's three. Same QoS choices as this node's own topics,
		 * for the same reasons: telemetry at 0 because a stale reading is
		 * worse than a missing one, ack at 1 because it is the answer to
		 * a command, status retained because a harness that connects
		 * later still needs to know. docs/mqtt-design.md. */
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
