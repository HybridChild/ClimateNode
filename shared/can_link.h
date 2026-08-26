/* The CAN link contract: everything the two ends of the wire must agree on.
 *
 * Included by both applications, because a link is symmetric -- the F072RB peer
 * node sends what the H753ZI gateway expects, and neither is in a position to be
 * right on its own. Anything only one side needs does NOT belong here.
 *
 * Deliberately free of protobuf, MQTT, zbus and Zephyr driver headers: plain
 * constants and two inline functions, which is what lets tests/ compile it with
 * nothing attached.
 *
 * Five 11-bit identifiers per peer, `id` the node id and n = id - 2:
 *
 *   0x700 + id   heartbeat,      peer -> gateway, one raw frame at 1 Hz
 *   0x7E0 + n    ISO-TP data,    gateway -> peer  (commands)
 *   0x7E4 + n    ISO-TP flow control for the above, peer -> gateway
 *   0x7E8 + n    ISO-TP data,    peer -> gateway  (telemetry and acks)
 *   0x7EC + n    ISO-TP flow control for the above, gateway -> peer
 *
 * Three properties of that map are load-bearing, and each is explained in
 * docs/can-bringup.md *The address map* / *Flow control needs its own
 * identifiers*: no two contexts on a node share an identifier (the loser is
 * starved with no diagnostic); the heartbeat sorts below the ISO-TP ranges, so
 * liveness cannot be starved by a segmented transfer; and the four-apart spacing
 * caps the peer count at kMaxPeerNodeId. tests/heartbeat/ asserts all three on
 * the constants alone, where no driver's semantics can intervene.
 *
 * Concepts -- why an identifier names a message rather than a node, and what
 * ISO-TP does about 8-byte frames -- are in notes/can-guide.md.
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

/* The last peer this address map can express. Four identifiers spaced four
 * apart, so n = 0..3 before 0x7E0 + n would collide with the FC range at
 * 0x7E4. A fifth peer is not a tight fit, it is a wrong one: it would be given
 * an identifier another node is already filtering on, and the symptom would be
 * the same silently-swallowed flow control the map exists to avoid. Asserted in
 * tests/heartbeat/ rather than left to be discovered. */
constexpr uint8_t kMaxPeerNodeId = kFirstPeerNodeId + 3;

/* Data and flow control are separated on purpose -- see the header comment. The
 * suffix names the direction the frame TRAVELS, never the node that cares about
 * it, because "to gateway" is unambiguous read from either end while "rx" is
 * only ever true for one of them. */
constexpr uint16_t kHeartbeatIdBase = 0x700;
constexpr uint16_t kIsotpToPeerBase = 0x7E0;
constexpr uint16_t kIsotpFcToGatewayBase = 0x7E4;
constexpr uint16_t kIsotpToGatewayBase = 0x7E8;
constexpr uint16_t kIsotpFcToPeerBase = 0x7EC;

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

/* Flow control answering a gateway -> peer transfer, so it travels peer ->
 * gateway: the peer's bind sends it, the gateway's sender listens for it. */
constexpr uint16_t isotp_fc_id_to_gateway(uint8_t node_id)
{
	return kIsotpFcToGatewayBase + (node_id - kFirstPeerNodeId);
}

/* Flow control answering a peer -> gateway transfer, so it travels gateway ->
 * peer: the gateway's bind sends it, the peer's sender listens for it. */
constexpr uint16_t isotp_fc_id_to_peer(uint8_t node_id)
{
	return kIsotpFcToPeerBase + (node_id - kFirstPeerNodeId);
}

/* ---------------------------------------------------------------------------
 * The one byte of protocol above ISO-TP
 * ---------------------------------------------------------------------------
 *
 * An ISO-TP payload here is [0] = a message type, [1..] = protobuf bytes
 * verbatim. The gateway reads byte 0, strips it, and forwards the remainder
 * untouched -- it never decodes what a peer sent.
 *
 * ISO-TP has no equivalent of an MQTT topic, and the type is not on the wire
 * (notes/protobuf-guide.md §4), so something outside the payload has to assert
 * it. On MQTT the topic does that for free; here it costs a byte.
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
 * Exactly 8 bytes, hand-packed, sent as one raw CAN frame -- no ISO-TP, because
 * a heartbeat that needed segmenting would depend on the very link it reports
 * on. The layout is fixed by agreement, in the style of a DBC file
 * (notes/can-guide.md §8):
 *
 *   byte 0   layout version, currently 1
 *   byte 1   node state (enum heartbeat_state)
 *   byte 2-5 uptime in seconds, uint32 little-endian
 *   byte 6   low 8 bits of the telemetry sequence
 *   byte 7   reserved, 0xFF
 *
 * Byte 0 is a version, and first, because this layout has none of the evolution
 * machinery protobuf gives the payloads -- nothing else in the frame could tell
 * a reader that the sender's idea of byte 3 has changed. Byte 6 is truncated to
 * 8 bits deliberately: the gateway only ever compares adjacent beats, so
 * wrapping every 256 samples is harmless, and a full uint32 would not have fitted
 * beside the uptime anyway.
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
