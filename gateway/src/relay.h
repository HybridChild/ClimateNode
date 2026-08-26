/* The relay: the gateway's CAN side, and the channels that join it to the MQTT
 * side.
 *
 * Deliberately NOT in app_channels.h, whose stated contract is that nothing in
 * it touches a transport; these channels exist precisely to carry one. What both
 * boards must agree on is in can_link.h. This file is the gateway's half alone,
 * and the peer node includes none of it.
 *
 * The gateway is a **transport hop**, not an aggregator. A peer node encodes its
 * own Telemetry and its own Acks, and the gateway moves those bytes between CAN
 * and MQTT without decoding them. It reads exactly one byte -- can_link.h's
 * message type, which says which topic the payload belongs on -- and strips it.
 *
 * That opacity is why the message types below are byte arrays rather than
 * node_Telemetry: a generated protobuf type in this header would make a schema
 * change on the *peer* rebuild the gateway, which is exactly the coupling the
 * design claims not to have. Compare app_channels.h, which avoids protobuf types
 * for the opposite reason -- there a schema change must stop at protocol.cpp.
 * Same goal from either end: a schema change that does not ripple.
 *
 * This header is the decisions half of notes/zbus-guide.md for the relay's four
 * channels, as app_channels.h is for the sensor's two.
 */
#ifndef RELAY_H_
#define RELAY_H_

#include <zephyr/zbus/zbus.h>

#include <stdint.h>

/* The largest relayed payload, and therefore the size of every upward message.
 *
 * Bounded by node_Ack_size (140 today), which relay.cpp pins with a
 * static_assert — it is the one translation unit allowed to include node.pb.h,
 * so the check lives there and drift becomes a compile error rather than a
 * truncated Ack on the wire. Kept here as a plain number so tests/ and the
 * channels can size themselves without pulling in the generated header. */
constexpr size_t kRelayUpMax = 160;

/* Peer node -> gateway -> broker. Telemetry and Acks both use this shape; which
 * topic it lands on is decided by which channel it arrived on, not by anything
 * inside `bytes`. */
struct relay_up {
	/* Which peer. Today always node 2, but carried explicitly so that adding
	 * a third node is a channel-observer question rather than a rewrite. */
	uint8_t node_id;
	uint16_t len;
	uint8_t bytes[kRelayUpMax];
};

/* Broker -> gateway -> peer node. Bounded by node_Command_size (20). */
struct relay_down {
	uint8_t node_id;
	uint8_t len;
	/* The one field the gateway reads out of a message it forwards. It needs
	 * the sequence so that a command the peer never answers can still be
	 * acknowledged — see the synthesized-Ack note in relay.cpp. Filled in by
	 * main.cpp, which decodes the Command *envelope* and nothing else. */
	uint32_t sequence;
	uint8_t bytes[24];
};

/* Liveness, which on CAN has to be built rather than subscribed to. */
struct relay_status {
	uint8_t node_id;
	bool online;
};

/* relay_up is the largest message on any channel in this application, and so it
 * is what CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_STATIC_DATA_SIZE must be sized to
 * -- despite riding listener channels, because every publish copies its whole
 * message into a pool buffer before any observer is consulted. Getting it wrong
 * is a silent overrun of a fixed-size slot, so it is checked rather than
 * trusted; docs/can-bringup.md has the mechanism.
 *
 * Guarded because the test suites enable CONFIG_ZBUS without the message
 * subscriber, so the symbol does not exist there. */
#ifdef CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_STATIC_DATA_SIZE
static_assert(sizeof(struct relay_up) <= CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_STATIC_DATA_SIZE,
	      "raise CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_STATIC_DATA_SIZE to sizeof(struct "
	      "relay_up)");
static_assert(sizeof(struct relay_down) <= CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_STATIC_DATA_SIZE,
	      "raise CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_STATIC_DATA_SIZE to sizeof(struct "
	      "relay_down)");
static_assert(sizeof(struct relay_status) <= CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_STATIC_DATA_SIZE,
	      "raise CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_STATIC_DATA_SIZE to sizeof(struct "
	      "relay_status)");
#endif

/* ---------------------------------------------------------------------------
 * The channels
 * ---------------------------------------------------------------------------
 *
 * The observer kind follows direction, exactly as in app_channels.h and for the
 * same argument -- state latest-wins, events not collapsed:
 *
 *   chan_relay_telemetry  LISTENER.  State; a superseded reading is one the host
 *                         is better off not receiving.
 *   chan_relay_ack        LISTENER.  An ack is an event, which would normally
 *                         argue for a message subscriber -- but relay.cpp
 *                         serialises command round trips structurally, so at
 *                         most one is ever outstanding and latest-wins cannot
 *                         collapse a set of one. main.cpp's eventfd counter
 *                         checks that premise and logs a breach at ERROR.
 *   chan_relay_status     LISTENER.  Liveness is state, and latest-wins is
 *                         precisely what a retained topic means.
 *   chan_relay_command    MESSAGE SUBSCRIBER.  A command has no next one coming.
 *
 * All four are defined in relay.cpp, per the convention that the owning module
 * defines its channels. The three listeners are defined in main.cpp beside
 * telemetry_listener: the network side owns the observers that feed the network.
 */
ZBUS_CHAN_DECLARE(chan_relay_telemetry);
ZBUS_CHAN_DECLARE(chan_relay_ack);
ZBUS_CHAN_DECLARE(chan_relay_status);
ZBUS_CHAN_DECLARE(chan_relay_command);

/* ---------------------------------------------------------------------------
 * Liveness
 * ---------------------------------------------------------------------------
 *
 * MQTT hands the gateway liveness for free: the broker notices a dead client and
 * publishes its will. CAN hands it nothing -- a silent node and an absent node
 * are the same thing (notes/can-guide.md §1) -- so the gateway builds it from a
 * heartbeat and a clock.
 *
 * The state machine is a pure function so it can be tested without a bus, a
 * clock or a thread, the same move sensor_cmd_in_range() makes in
 * app_channels.h. tests/relay/ is what exercises it.
 */
enum peer_liveness {
	/* Before anything is known. Distinct from OFFLINE on purpose: a gateway
	 * that has just booted has no evidence either way, and announcing a
	 * healthy peer as dead because we were not listening yet would be a
	 * false alarm published to a retained topic. */
	PEER_UNKNOWN,
	PEER_ONLINE,
	PEER_OFFLINE,
};

struct liveness {
	enum peer_liveness state;
	int64_t last_seen_ms;
};

/* Advance the state machine, returning true only when the state *changed* —
 * which is exactly when something needs publishing. Called on every heartbeat
 * and on every receive timeout, so it is also the thing that turns "no frames
 * arrived" into a decision.
 *
 * `last_seen_ms` must be initialised to the time the gateway started listening,
 * not to zero: the timeout is measured from the last evidence, and at boot the
 * last evidence is "we began looking". */
static inline bool liveness_step(struct liveness *l, bool frame_seen, int64_t now_ms,
				 int timeout_ms)
{
	if (frame_seen) {
		l->last_seen_ms = now_ms;
		if (l->state != PEER_ONLINE) {
			l->state = PEER_ONLINE;
			return true;
		}
		return false;
	}

	if ((now_ms - l->last_seen_ms) >= timeout_ms && l->state != PEER_OFFLINE) {
		l->state = PEER_OFFLINE;
		return true;
	}
	return false;
}

#endif /* RELAY_H_ */
