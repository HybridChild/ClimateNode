/* Unit tests for shared/can_link.h -- the CAN link contract.
 *
 * Two things are pinned here, and both are pinned because nothing else can pin
 * them: the byte layout of the heartbeat frame, and the address map. Everywhere
 * else in this project the wire format is generated from proto/node.proto and
 * the generator is responsible for agreement between the two ends. These eight
 * bytes are hand-packed, so agreement is a property of this header and of
 * whoever read it correctly -- which is exactly the situation a test is for.
 *
 * The failure these guard against is not a crash. A shift in the wrong
 * direction, or a byte transposed, produces a frame that transmits perfectly,
 * passes every CAN-level check, and means something else at the far end. See
 * notes/can-guide.md §8 for why a bus with 8 payload bytes ends up here at all.
 */
#include <zephyr/ztest.h>

#include <string.h>

#include "can_link.h"

ZTEST_SUITE(heartbeat, NULL, NULL, NULL, NULL, NULL);

/* ---- the frame ------------------------------------------------------------ */

ZTEST(heartbeat, test_pack_layout_is_exact)
{
	/* The layout asserted byte by byte rather than through a round trip.
	 * A round trip alone would pass just as happily if pack and unpack were
	 * wrong in the same direction, and "wrong in the same direction" is
	 * precisely what one author writing both functions produces. This test
	 * is the one that would catch it, because these bytes are what the
	 * *other* board sees. */
	struct heartbeat hb = {};

	hb.state = HEARTBEAT_STATE_OK;
	hb.uptime_s = 0x04030201;
	hb.sequence_low = 0x7B;

	uint8_t buf[kHeartbeatLen];

	memset(buf, 0xAA, sizeof(buf));
	heartbeat_pack(&hb, buf);

	zassert_equal(buf[0], kHeartbeatLayoutVersion, "byte 0 is the layout version");
	zassert_equal(buf[1], HEARTBEAT_STATE_OK, "byte 1 is the state");
	/* Little-endian: the LOW byte of the uptime comes first. Written out as
	 * four separate assertions so a failure names the byte. */
	zassert_equal(buf[2], 0x01, "uptime byte 0 (LSB)");
	zassert_equal(buf[3], 0x02, "uptime byte 1");
	zassert_equal(buf[4], 0x03, "uptime byte 2");
	zassert_equal(buf[5], 0x04, "uptime byte 3 (MSB)");
	zassert_equal(buf[6], 0x7B, "byte 6 is the truncated sequence");
	zassert_equal(buf[7], 0xFF, "byte 7 is reserved and must be 0xFF");
}

ZTEST(heartbeat, test_round_trip)
{
	const struct heartbeat cases[] = {
		{HEARTBEAT_STATE_BOOT, 0, 0},
		{HEARTBEAT_STATE_WARMING_UP, 1, 1},
		{HEARTBEAT_STATE_OK, 86400, 200},
		{HEARTBEAT_STATE_SENSOR_ERROR, UINT32_MAX, 255},
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		uint8_t buf[kHeartbeatLen];
		struct heartbeat out = {};

		heartbeat_pack(&cases[i], buf);

		zassert_true(heartbeat_unpack(buf, sizeof(buf), &out), "case %zu did not unpack",
			     i);
		zassert_equal(out.state, cases[i].state, "case %zu: state", i);
		zassert_equal(out.uptime_s, cases[i].uptime_s, "case %zu: uptime", i);
		zassert_equal(out.sequence_low, cases[i].sequence_low, "case %zu: sequence", i);
	}
}

ZTEST(heartbeat, test_uptime_wraps_at_uint32)
{
	/* ~136 years, so this will not happen -- but the field is the full width
	 * of the type, and a pack that dropped the top byte would look correct
	 * for the first 194 days. Asserting the boundary is how you find out
	 * now instead of then. */
	struct heartbeat hb = {};

	hb.state = HEARTBEAT_STATE_OK;
	hb.uptime_s = UINT32_MAX;

	uint8_t buf[kHeartbeatLen];
	struct heartbeat out = {};

	heartbeat_pack(&hb, buf);
	zassert_true(heartbeat_unpack(buf, sizeof(buf), &out), "unpack failed");
	zassert_equal(out.uptime_s, UINT32_MAX, "the top byte of the uptime was lost");
}

/* ---- rejecting what we do not understand ---------------------------------- */

ZTEST(heartbeat, test_unpack_rejects_wrong_layout_version)
{
	/* The entire justification for spending one byte in eight on a version.
	 * A future node that repurposes byte 7 must be ignored rather than
	 * half-read: the gateway will then time it out and report it offline,
	 * which is the correct answer for a peer it cannot understand. */
	uint8_t buf[kHeartbeatLen];
	struct heartbeat hb = {HEARTBEAT_STATE_OK, 1234, 5};
	struct heartbeat out = {};

	heartbeat_pack(&hb, buf);
	buf[0] = kHeartbeatLayoutVersion + 1;

	zassert_false(heartbeat_unpack(buf, sizeof(buf), &out),
		      "a frame from a newer layout was accepted");
}

ZTEST(heartbeat, test_unpack_rejects_wrong_length)
{
	/* A shorter frame is perfectly legal CAN -- the DLC says how many bytes
	 * are there, and nothing obliges a sender to fill all eight. Without
	 * this check, unpacking a 4-byte frame would read past the data and
	 * report an uptime assembled from whatever followed it. */
	uint8_t buf[kHeartbeatLen];
	struct heartbeat hb = {HEARTBEAT_STATE_OK, 1234, 5};
	struct heartbeat out = {};

	heartbeat_pack(&hb, buf);

	zassert_false(heartbeat_unpack(buf, 4, &out), "a short frame was accepted");
	zassert_false(heartbeat_unpack(buf, 0, &out), "an empty frame was accepted");
}

ZTEST(heartbeat, test_unpack_rejects_unknown_state)
{
	/* Deliberately the opposite of the protobuf rule. An unknown *enum
	 * value* in a protobuf message is preserved and handled by a default
	 * arm (notes/protobuf-guide.md §7), because the schema can grow one
	 * safely. Here it cannot: this is a fixed byte layout with no way to
	 * describe what else may have changed alongside it, so an unknown state
	 * means the sender is speaking a layout we do not have, and the version
	 * byte should have said so. Rejecting is the conservative reading. */
	uint8_t buf[kHeartbeatLen];
	struct heartbeat hb = {HEARTBEAT_STATE_OK, 1, 1};
	struct heartbeat out = {};

	heartbeat_pack(&hb, buf);
	buf[1] = HEARTBEAT_STATE_SENSOR_ERROR + 1;

	zassert_false(heartbeat_unpack(buf, sizeof(buf), &out), "an unknown state was accepted");
}

/* ---- the address map ------------------------------------------------------ */

ZTEST(heartbeat, test_address_map)
{
	/* The map for node 2, spelled out. These three numbers appear in the
	 * bring-up procedure in docs/can-bringup.md and in every `can filter
	 * add` a human will type at the shell, so they are worth being an
	 * assertion rather than a comment. */
	zassert_equal(heartbeat_id(2), 0x702, "heartbeat id for node 2");
	zassert_equal(isotp_id_to_peer(2), 0x7E0, "gateway -> node 2");
	zassert_equal(isotp_id_to_gateway(2), 0x7E8, "node 2 -> gateway");

	/* And a third node would slot in beside it without colliding. */
	zassert_equal(heartbeat_id(3), 0x703, "heartbeat id for node 3");
	zassert_equal(isotp_id_to_peer(3), 0x7E1, "gateway -> node 3");
	zassert_equal(isotp_id_to_gateway(3), 0x7E9, "node 3 -> gateway");
}

ZTEST(heartbeat, test_every_id_is_a_valid_standard_identifier)
{
	/* 11-bit ids, so the ceiling is 0x7FF. The ISO-TP ranges start at 0x7E0
	 * and 0x7E8, which leaves room for 8 peers on the response range before
	 * the identifier space runs out -- and the failure at peer 10 would be
	 * a silently truncated id, not an error. Worth knowing where the wall
	 * is before someone adds a ninth node. */
	constexpr uint16_t kMaxStdId = 0x7FF;

	for (uint8_t id = kFirstPeerNodeId; id <= 9; id++) {
		zassert_true(heartbeat_id(id) <= kMaxStdId, "heartbeat id for node %u", id);
		zassert_true(isotp_id_to_peer(id) <= kMaxStdId, "request id for node %u", id);
		zassert_true(isotp_id_to_gateway(id) <= kMaxStdId, "response id for node %u", id);
	}

	/* The heartbeat range must stay below the ISO-TP ranges, because a lower
	 * identifier wins arbitration (notes/can-guide.md §4). That ordering is
	 * what keeps liveness from being starved by a long segmented transfer,
	 * and it is a property of these constants rather than of any code. */
	zassert_true(heartbeat_id(9) < isotp_id_to_peer(kFirstPeerNodeId),
		     "heartbeats must outrank ISO-TP traffic on the bus");
}
