/* The CAN link contract: everything the two ends of the wire must agree on.
 *
 * Included by both applications, because a link is symmetric -- the F072RB peer
 * node sends what the H753ZI gateway expects, and neither is in a position to
 * be right on its own. Anything that only one side needs does NOT belong here.
 *
 * Deliberately free of protobuf, MQTT, zbus and Zephyr driver headers. It is
 * plain constants and two inline functions, which is what lets tests/ compile
 * it with nothing attached. The concepts underneath -- why a CAN identifier
 * names a message rather than a node, why 8 bytes forces this file to exist at
 * all, and what ISO-TP does about it -- are in notes/can-guide.md; the
 * bring-up facts are in docs/can-bringup.md.
 *
 * ---------------------------------------------------------------------------
 * The address map
 * ---------------------------------------------------------------------------
 *
 * Three identifiers per peer node, all 11-bit standard IDs:
 *
 *   0x700 + id   heartbeat, peer -> gateway, one raw frame at 1 Hz
 *   0x7E0 + n    ISO-TP, gateway -> peer  (commands)
 *   0x7E8 + n    ISO-TP, peer -> gateway  (telemetry and acks)
 *
 * where `id` is the node id and n = id - 2, since the gateway is node 1 and the
 * first peer is node 2. For node 2 that is 0x702, 0x7E0 and 0x7E8.
 *
 * The numbers are borrowed rather than invented: 0x700 + node id is CANopen's
 * heartbeat convention, and 0x7E0/0x7E8 is the UDS diagnostic request/response
 * pair. Neither protocol is implemented here. Borrowing the ranges costs
 * nothing and means anyone who has met a CAN bus before can read a trace
 * without this file open, which is the only thing an identifier can do for a
 * human -- see notes/can-guide.md §4 for what it does for the bus.
 *
 * Note what the low identifiers buy on the bus itself: arbitration is won by
 * the numerically lowest id, so the heartbeat at 0x702 beats every ISO-TP frame
 * on the link. That is the right way round -- liveness must not be starved by
 * a long segmented transfer -- and it is a property of the numbering, not of
 * any code below.
 */
#ifndef CAN_LINK_H_
#define CAN_LINK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The bus is 500 kbit/s. That number is NOT here: it lives in each board's
 * devicetree overlay, because it configures a controller rather than describing
 * a message, and Kconfig's default of 125000 would otherwise apply silently.
 * See docs/can-bringup.md. */

/* The gateway is node 1 and speaks MQTT; peers start at 2. */
constexpr uint8_t kGatewayNodeId = 1;
constexpr uint8_t kFirstPeerNodeId = 2;

constexpr uint16_t kHeartbeatIdBase = 0x700;
constexpr uint16_t kIsotpToPeerBase = 0x7E0;
constexpr uint16_t kIsotpToGatewayBase = 0x7E8;

constexpr uint16_t heartbeat_id(uint8_t node_id)
{
	return kHeartbeatIdBase + node_id;
}

constexpr uint16_t isotp_id_to_peer(uint8_t node_id)
{
	return kIsotpToPeerBase + (node_id - kFirstPeerNodeId);
}

constexpr uint16_t isotp_id_to_gateway(uint8_t node_id)
{
	return kIsotpToGatewayBase + (node_id - kFirstPeerNodeId);
}

/* ---------------------------------------------------------------------------
 * The one byte of protocol above ISO-TP
 * ---------------------------------------------------------------------------
 *
 * An ISO-TP payload here is [0] = a message type, [1..] = protobuf bytes
 * verbatim. The gateway reads byte 0, strips it, and forwards the remainder
 * untouched -- it never decodes what a peer sent.
 *
 * That byte exists because ISO-TP has no equivalent of an MQTT topic, and
 * docs/mqtt-design.md's rule still applies: the type is not on the wire
 * (notes/protobuf-guide.md §4), so something outside the payload has to assert
 * it. On MQTT the topic does; here this byte does. It is the same decision,
 * paying the same one-byte price it used to get free.
 *
 * The alternative was a separate address pair per message type, which would
 * mean two binds, two receive contexts and either two threads or a poll over
 * structures isotp.h marks internal. One byte is cheaper, and it keeps the
 * address map short enough to hold in your head.
 */
enum relay_msg_type : uint8_t {
	RELAY_MSG_TELEMETRY = 1,
	RELAY_MSG_ACK = 2,
	RELAY_MSG_COMMAND = 3,
};

/* ---------------------------------------------------------------------------
 * The heartbeat frame
 * ---------------------------------------------------------------------------
 *
 * Exactly 8 bytes, hand-packed, sent as one raw CAN frame -- no ISO-TP. This is
 * where the eight-byte limit stops being an abstraction: a protobuf Telemetry
 * cannot fit, and a heartbeat that needed segmenting would defeat its own
 * purpose, since the thing it reports on is the link that would have to carry
 * the segments.
 *
 * So the layout is fixed by agreement, in the style a DBC file records for a
 * real vehicle bus (notes/can-guide.md §8):
 *
 *   byte 0   layout version, currently 1
 *   byte 1   node state (enum heartbeat_state)
 *   byte 2-5 uptime in seconds, uint32 little-endian
 *   byte 6   low 8 bits of the telemetry sequence
 *   byte 7   reserved, 0xFF
 *
 * Two decisions worth keeping:
 *
 * Byte 0 is a version, and it is first, because this layout has none of the
 * evolution machinery protobuf gives the payloads. Nothing else in the frame
 * can tell a reader that the sender's idea of byte 3 has changed. Its cost is
 * one byte of eight -- 12.5% of the frame spent on being able to change the
 * other seven -- and that is the honest price of hand-packing.
 *
 * Byte 6 is the sequence truncated to 8 bits, which is deliberate and lossy.
 * The gateway compares it against the last telemetry it relayed and can see a
 * gap without decoding anything; wrapping every 256 samples is harmless because
 * the comparison is only ever between adjacent beats. A full uint32 would not
 * have fitted beside the uptime anyway, which is the sort of thing eight bytes
 * decides for you.
 */
constexpr uint8_t kHeartbeatLayoutVersion = 1;
constexpr size_t kHeartbeatLen = 8;

enum heartbeat_state : uint8_t {
	HEARTBEAT_STATE_BOOT = 0,
	HEARTBEAT_STATE_WARMING_UP = 1,
	HEARTBEAT_STATE_OK = 2,
	HEARTBEAT_STATE_SENSOR_ERROR = 3,
};

struct heartbeat {
	enum heartbeat_state state;
	uint32_t uptime_s;
	uint8_t sequence_low;
};

/* Pack into exactly kHeartbeatLen bytes. Little-endian by hand rather than by
 * memcpy of a uint32: both parts are ARM and little-endian today, and writing
 * the shifts out is what stops that from being load-bearing. */
static inline void heartbeat_pack(const struct heartbeat *hb, uint8_t *out)
{
	out[0] = kHeartbeatLayoutVersion;
	out[1] = static_cast<uint8_t>(hb->state);
	out[2] = static_cast<uint8_t>(hb->uptime_s & 0xFF);
	out[3] = static_cast<uint8_t>((hb->uptime_s >> 8) & 0xFF);
	out[4] = static_cast<uint8_t>((hb->uptime_s >> 16) & 0xFF);
	out[5] = static_cast<uint8_t>((hb->uptime_s >> 24) & 0xFF);
	out[6] = hb->sequence_low;
	out[7] = 0xFF;
}

/* Unpack, returning false if this is not a frame we understand. Rejecting on
 * the version is the whole reason byte 0 exists: a future node that repurposes
 * byte 7 must be ignored rather than half-read, and "ignored" here means the
 * gateway will time the peer out and report it offline -- which is the correct
 * outcome for a peer it cannot understand. Length is checked too, since a
 * shorter frame is legal CAN and would otherwise read past the data. */
static inline bool heartbeat_unpack(const uint8_t *in, size_t len, struct heartbeat *out)
{
	if (len != kHeartbeatLen || in[0] != kHeartbeatLayoutVersion) {
		return false;
	}
	if (in[1] > HEARTBEAT_STATE_SENSOR_ERROR) {
		return false;
	}

	out->state = static_cast<enum heartbeat_state>(in[1]);
	out->uptime_s = static_cast<uint32_t>(in[2]) | (static_cast<uint32_t>(in[3]) << 8) |
			(static_cast<uint32_t>(in[4]) << 16) |
			(static_cast<uint32_t>(in[5]) << 24);
	out->sequence_low = in[6];
	return true;
}

/* One beat per second; the gateway calls a peer offline after 3.5 s of silence.
 * The ratio is what matters: 3.5 tolerates three lost beats without a false
 * alarm, and is short enough that a dead peer is noticed within one telemetry
 * period. Both ends need these, which is why they are here and not in either
 * app -- a peer beating slower than the gateway's timeout would look
 * permanently dead, and nothing would report the disagreement. */
constexpr uint32_t kHeartbeatPeriodMs = 1000;
constexpr uint32_t kHeartbeatTimeoutMs = 3500;

#endif /* CAN_LINK_H_ */
