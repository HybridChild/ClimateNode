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
#include "can_link.h"
#include "commands.h"
#include "protocol.h"
#include "relay.h"

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

/* The peer node's topics. Published by this client, on this connection, for now
 * — nothing in MQTT requires the publisher of the node/2 topics to *be* node 2.
 *
 * That is a deliberate intermediate rather than the end state. A second MQTT
 * connection buys exactly one thing: MQTT 3.1.1 allows one Last Will per
 * connection, so only a client that *is* node 2 can have the broker flip
 * node/2/status to offline when the GATEWAY dies. Everything else here is
 * identical either way, because the broker can never observe the peer's own
 * liveness — "peer dead, gateway alive" is firmware-published in both designs.
 * Deferred so that this step can be verified on its own. */
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

/* One eventfd per relay channel, three more descriptors in the same poll set.
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

/* Defined below, after the client struct it operates on. */
int publish(const char *topic, const uint8_t *payload, size_t payload_len,
	    mqtt_qos qos, bool retain);

/* Does an MQTT topic (length-delimited, not NUL-terminated) equal this literal?
 * A length check first, so a topic that merely starts the same cannot match. */
bool topic_is(const struct mqtt_utf8 *topic, const char *literal)
{
	size_t n = strlen(literal);

	return topic->size == n && memcmp(topic->utf8, literal, n) == 0;
}

/* Encode and publish an Ack on a given topic. Best effort: a failure here is
 * logged, not propagated, because the command itself may already have taken
 * effect. */
void publish_ack(const char *topic, uint32_t seq, node_AckStatus status, const char *detail,
		 const node_DeviceInfo *info)
{
	uint8_t buf[node_Ack_size];
	size_t len = encode_ack(seq, status, detail, info, buf, sizeof(buf));

	if (len == 0) {
		return;
	}
	publish(topic, buf, len, MQTT_QOS_1_AT_LEAST_ONCE, false);
}

/* This node's own Ack topic — the overwhelming majority of calls. */
void send_ack(uint32_t seq, node_AckStatus status, const char *detail,
	      const node_DeviceInfo *info)
{
	publish_ack(kTopicAck, seq, status, detail, info);
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
		publish_ack(kPeerTopicAck, sequence, node_AckStatus_ACK_STATUS_FAILED,
			    "command too large to relay", nullptr);
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
		publish_ack(kPeerTopicAck, sequence, node_AckStatus_ACK_STATUS_FAILED,
			    "relay busy", nullptr);
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

	/* Which node was this addressed to? Two command topics arrive on one
	 * connection, and the topic is the only thing that distinguishes them —
	 * the payloads are the same message type, and nothing inside a Command
	 * says which node it is for. Exactly the point notes/protobuf-guide.md §4
	 * makes about the topic being part of the contract. */
	bool for_peer = topic_is(&pub->message.topic.topic, kPeerTopicCommand);

	/* From here the transport is done with the bytes: protocol.cpp decides
	 * whether they are a Command, commands.cpp decides what it means, and
	 * this layer only reports the outcome back to the host. */
	node_Command cmd;

	if (!decode_command(payload, kept, oversized, &cmd)) {
		/* sequence 0: we could not read one, so there is nothing to
		 * correlate against. The host learns the message was garbage.
		 *
		 * Answered by the gateway even when it was addressed to the
		 * peer, and that is right: bytes that are not a Command cannot
		 * be forwarded as one, and the gateway is the only party in a
		 * position to say so. */
		publish_ack(for_peer ? kPeerTopicAck : kTopicAck, 0,
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
		LOG_INF("subscribed to %s and %s", kTopicCommand, kPeerTopicCommand);
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

/* Both command topics, in one SUBSCRIBE. A wildcard `node/+/command` would also
 * work and is deliberately not used: it would silently accept a node/9/command
 * this gateway has no route for, and the set of nodes it can reach is a fact
 * worth stating rather than discovering. */
int subscribe_to_commands(void)
{
	struct mqtt_topic topics[2] = {};
	struct mqtt_subscription_list list = {};

	topics[0].topic.utf8 = reinterpret_cast<uint8_t *>(const_cast<char *>(kTopicCommand));
	topics[0].topic.size = strlen(kTopicCommand);
	topics[0].qos = MQTT_QOS_1_AT_LEAST_ONCE;

	topics[1].topic.utf8 = reinterpret_cast<uint8_t *>(const_cast<char *>(kPeerTopicCommand));
	topics[1].topic.size = strlen(kPeerTopicCommand);
	topics[1].qos = MQTT_QOS_1_AT_LEAST_ONCE;

	list.list = topics;
	list.list_count = ARRAY_SIZE(topics);
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
int publish_relayed(int fd, const struct zbus_channel *chan, const char *topic, mqtt_qos qos,
		    const char *what)
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

	rc = publish(topic, up.bytes, up.len, qos, false);
	if (rc == 0) {
		LOG_INF("relayed %s from node %u (%u bytes)", what, up.node_id, up.len);
	}
	return rc;
}

/* The peer's liveness, as plain ASCII on a retained topic — the same shape the
 * gateway's own status uses, because a host should not have to care which node
 * published its liveness or how that node learned it. */
int publish_relayed_status(void)
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

	return publish_text(kPeerTopicStatus, st.online ? "online" : "offline",
			    MQTT_QOS_1_AT_LEAST_ONCE, true);
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
		/* Five descriptors, one wait, one deadline. Before zbus this loop
		 * also owned the sample clock, so the timeout had to be the
		 * minimum of "next sample due" and "keepalive due"; now every
		 * producer keeps its own cadence and says so through a
		 * descriptor, leaving exactly one deadline here — the keepalive.
		 *
		 * That is the property worth noticing as the node count grows.
		 * Four of these five come from threads with entirely unrelated
		 * clocks — a sensor at 5 s, a peer heartbeat at 1 Hz, a command
		 * round trip at 2 s — and not one of them made this loop harder.
		 * Each new source of work is one more fd and one more `if`, never
		 * another timeout to reconcile against the keepalive. */
		struct zsock_pollfd fds[5] = {};

		fds[0].fd = client.transport.tcp.sock;
		fds[0].events = ZSOCK_POLLIN;
		fds[1].fd = telemetry_evt_fd;
		fds[1].events = ZSOCK_POLLIN;
		fds[2].fd = relay_telemetry_evt_fd;
		fds[2].events = ZSOCK_POLLIN;
		fds[3].fd = relay_ack_evt_fd;
		fds[3].events = ZSOCK_POLLIN;
		fds[4].fd = relay_status_evt_fd;
		fds[4].events = ZSOCK_POLLIN;

		if (zsock_poll(fds, ARRAY_SIZE(fds), mqtt_keepalive_time_left(&client)) < 0) {
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

		/* The relay's three. Same QoS choices as this node's own topics,
		 * for the same reasons: telemetry at 0 because a stale reading is
		 * worse than a missing one, ack at 1 because it is the answer to a
		 * command, status retained because a harness that connects later
		 * still needs to know. docs/mqtt-design.md. */
		if (fds[2].revents & ZSOCK_POLLIN) {
			rc = publish_relayed(relay_telemetry_evt_fd, &chan_relay_telemetry,
					     kPeerTopicTelemetry, MQTT_QOS_0_AT_MOST_ONCE,
					     "telemetry");
			if (rc != 0) {
				LOG_ERR("publish: %d", rc);
				break;
			}
		}
		if (fds[3].revents & ZSOCK_POLLIN) {
			rc = publish_relayed(relay_ack_evt_fd, &chan_relay_ack, kPeerTopicAck,
					     MQTT_QOS_1_AT_LEAST_ONCE, "ack");
			if (rc != 0) {
				LOG_ERR("publish: %d", rc);
				break;
			}
		}
		if (fds[4].revents & ZSOCK_POLLIN) {
			rc = publish_relayed_status();
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

/* The relay's three, defined here rather than in relay.cpp for the same reason:
 * the network side owns the observers that feed the network. relay.cpp names
 * them in its ZBUS_CHAN_DEFINEs; this is where they exist. */
ZBUS_LISTENER_DEFINE(relay_telemetry_listener, on_relay_telemetry);
ZBUS_LISTENER_DEFINE(relay_ack_listener, on_relay_ack);
ZBUS_LISTENER_DEFINE(relay_status_listener, on_relay_status);

int main(void)
{
	/* Counter starts at 0 and never blocks a reader, so a poll on it is
	 * simply "has the sensor thread published since I last looked". */
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
