/* The relay: the gateway's CAN side, and the channels that join it to the MQTT
 * side.
 *
 * Deliberately NOT in app_channels.h. That header's stated contract is that
 * nothing in it touches a transport, and it is the documented reference half of
 * notes/zbus-guide.md; the channels below exist precisely to carry another
 * transport's bytes. What both boards must agree on lives in can_link.h — the
 * address map, the heartbeat frame, the message-type byte. This file is the
 * gateway's half alone, and the peer node includes none of it.
 *
 * ---------------------------------------------------------------------------
 * What the relay is, in one paragraph
 * ---------------------------------------------------------------------------
 *
 * The gateway is a **transport hop**, not an aggregator. A peer node encodes its
 * own Telemetry and its own Acks, and the gateway moves those bytes between CAN
 * and MQTT without decoding them. It reads exactly one byte of what it carries —
 * can_link.h's message type, which says which topic the payload belongs on — and
 * strips it. Everything after that byte is opaque.
 *
 * That opacity is a property worth being able to demonstrate: add a field to the
 * peer's schema, reflash only the peer, and the gateway relays the new bytes
 * untouched while an old host still decodes them. It is also why the message
 * types below are byte arrays rather than node_Telemetry — a generated protobuf
 * type in this header would make a schema change rebuild the gateway, which is
 * exactly the coupling the design is claiming not to have.
 *
 * Compare app_channels.h, which avoids protobuf types for the *opposite* reason:
 * there, a schema change must stop at protocol.cpp instead of reaching the
 * sensor thread. Same goal — a schema change that does not ripple — approached
 * from either end.
 *
 * ---------------------------------------------------------------------------
 * Why the message types are asymmetric
 * ---------------------------------------------------------------------------
 *
 * relay_up is ~164 bytes and relay_down is 32, and that gap is deliberate rather
 * than incidental. zbus's message-subscriber net_buf pool is a *single* pool
 * sized by the largest message on *any* message-subscriber channel; listener
 * channels store their message in the channel itself and never touch the pool.
 *
 * So the big upward messages ride listeners, and only the small downward one is
 * a message subscriber. That is what lets
 * CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_STATIC_DATA_SIZE stay at 32 rather than
 * 164 — worth about 130 bytes times the pool size. The observer kinds were
 * chosen on their merits first (see the table below); the RAM is the reward for
 * having chosen them correctly.
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

/* ---------------------------------------------------------------------------
 * The channels
 * ---------------------------------------------------------------------------
 *
 * The observer kind follows direction, exactly as it does in app_channels.h and
 * for the same argument:
 *
 *   chan_relay_telemetry  LISTENER.  State, latest-wins. A reading superseded
 *                         while the network thread was busy is a reading the
 *                         host is better off not receiving. Same reasoning as
 *                         chan_telemetry, and as telemetry being QoS 0.
 *
 *   chan_relay_ack        LISTENER.  An ack is an event, which would normally
 *                         argue for a message subscriber — but at most one ack
 *                         is ever outstanding, because relay.cpp serialises
 *                         command round-trips structurally (isotp_send() blocks
 *                         until the transfer completes, and the ack wait blocks
 *                         after it). Latest-wins cannot collapse a set of one.
 *                         The eventfd counter checks that premise for free:
 *                         more than one signal means the invariant broke, which
 *                         main.cpp logs at ERROR rather than WARN.
 *
 *   chan_relay_status     LISTENER.  Liveness is state, and latest-wins is
 *                         precisely what a retained topic means.
 *
 *   chan_relay_command    MESSAGE SUBSCRIBER.  A command has no next one
 *                         coming, so none may be collapsed. Same argument as
 *                         chan_sensor_cmd, and as command/ack being QoS 1.
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
 * publishes its will. CAN hands it nothing — a silent node and an absent node
 * are the same thing (notes/can-guide.md §1) — so the gateway builds it from a
 * heartbeat and a clock.
 *
 * The state machine is a pure function so it can be tested without a bus, a
 * clock or a thread. That is the same move sensor_cmd_in_range() makes in
 * app_channels.h: the *rule* is an ordinary function, and the code that has to
 * touch hardware only supplies its arguments.
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
