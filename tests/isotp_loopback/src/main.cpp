/* The CAN transport, end to end, with no hardware — firmware/src/can_link.h
 * carried over Zephyr's ISO-TP through an emulated controller.
 *
 * ---------------------------------------------------------------------------
 * What this suite is for, and how it differs from the others
 * ---------------------------------------------------------------------------
 *
 * tests/heartbeat/ and tests/relay/ are unit tests in the strict sense: they
 * call a static inline function and look at what it returned. Nothing moves.
 * That is what makes them fast and total, and it is also their limit — they can
 * prove heartbeat_pack() produces eight correct bytes, and cannot prove those
 * eight bytes ever reach a receive filter.
 *
 * This suite runs a driver. `zephyr,can-loopback` is an emulated CAN controller
 * that delivers transmitted frames back to this same node's receive filters,
 * so the whole of ISO-TP genuinely executes: a First Frame is sent, a Flow
 * Control frame comes back, Consecutive Frames follow with their rolling
 * sequence numbers, and the receiver reassembles. None of that is our code, but
 * all of it is code our framing depends on and none of it had been run.
 *
 * What it therefore proves that nothing else does:
 *
 *   - a payload the size of the gateway's receive buffer survives segmentation
 *     and reassembly byte for byte, which is 1 First Frame + 1 Flow Control +
 *     23 Consecutive Frames rather than an assertion about a memcpy;
 *   - the ISO-TP receive pool the two prj.conf files configure is actually big
 *     enough for the direction each node has to reassemble;
 *   - the identifiers can_link.h computes are the identifiers a receive filter
 *     matches, which no amount of arithmetic testing can establish;
 *   - the type byte above ISO-TP arrives at offset 0 of a reassembled payload,
 *     which is the one byte the gateway reads and the whole basis of its
 *     routing.
 *
 * What it still cannot prove is everything electrical: differential levels,
 * termination, a common ground, arbitration between two real transmitters, and
 * the in-frame acknowledgement that makes a node alone on a bus unable to
 * transmit at all. Those need two transceivers and are listed in
 * docs/test-strategy.md as bench work. The value of this file is that the list
 * is now that short.
 *
 * ---------------------------------------------------------------------------
 * Why the sizes below are transport boundaries and not schema sizes
 * ---------------------------------------------------------------------------
 *
 * There is no nanopb here and no node.pb.h. The sizes tested are the ones the
 * *transport* changes behaviour at — 7 bytes is the largest Single Frame under
 * standard addressing, 8 is the first size that must segment, and 1 +
 * kRelayUpMax is the largest payload firmware/src/relay.cpp will ever read into.
 *
 * Testing against kRelayUpMax rather than against node_Ack_size is deliberate
 * and is the stronger of the two: 160 is above 140, and the link between the
 * buffer and the schema is already a static_assert in relay.cpp — the one
 * translation unit allowed to include the generated header. A schema that grew
 * past the buffer would fail to compile there. This file's job is the other
 * half: that the buffer, whatever the schema does, is a size the transport can
 * actually deliver.
 */
#include <zephyr/ztest.h>

#include <zephyr/canbus/isotp.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>

#include <string.h>

#include "can_link.h" /* the contract under test */
#include "relay.h"    /* kRelayUpMax — the gateway's buffer bound */

/* The node whose address triple is exercised. kFirstPeerNodeId rather than a
 * literal 2, so that the identifiers below are the ones the applications
 * compute rather than the ones this file believes they compute. */
constexpr uint8_t kPeerNodeId = kFirstPeerNodeId;

/* The gateway's receive buffer, and therefore the largest payload that can
 * arrive without being cut short. relay.cpp declares uint8_t buf[1 +
 * kRelayUpMax]: one type byte, then the opaque remainder. */
constexpr size_t kMaxPayload = 1 + kRelayUpMax;

const struct device *const can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));

/* The two ends of the ISO-TP pair, named for direction exactly as both
 * applications name them. `to_gateway` is what the peer transmits on and what
 * the gateway binds to; `to_peer` is the reverse. Under loopback both live on
 * one controller, which is precisely what makes a single node able to run a
 * two-party protocol against itself. */
const struct isotp_msg_id id_to_gateway = {
	.std_id = isotp_id_to_gateway(kPeerNodeId), /* 0x7E8 */
};
const struct isotp_msg_id id_to_peer = {
	.std_id = isotp_id_to_peer(kPeerNodeId), /* 0x7E0 */
};

/* The permissive flow control both applications advertise: take the whole
 * transfer, no minimum gap. Repeated here rather than shared because it is a
 * per-node policy — see the comment on it in sensor-node/src/main.cpp — and a
 * test that silently inherited a change to it would stop testing what it says. */
const struct isotp_fc_opts fc_opts = {
	.bs = 0,
	.stmin = 0,
};

/* ---------------------------------------------------------------------------
 * One transfer, both roles, on one controller
 * ---------------------------------------------------------------------------
 *
 * The argument order is the same for both halves and that is not a coincidence:
 * a receiver binds (rx = where it listens, tx = where it answers with Flow
 * Control), and a sender sends (tx = where the data goes, rx = where Flow
 * Control comes back from). Those are the same two identifiers in the same
 * order, seen from the two ends — so `dst` and `fc` name them once.
 *
 * Bind and unbind live inside the call so that no two tests ever have contexts
 * bound at the same time. Under loopback that matters more than it would on a
 * real bus: every frame reaches every filter on this node, so a receive context
 * left bound in the opposite direction would also be shown the Flow Control
 * frames of the transfer under test.
 */
int transfer(const struct isotp_msg_id *dst, const struct isotp_msg_id *fc, const uint8_t *out,
	     size_t out_len, uint8_t *in, size_t in_cap)
{
	static struct isotp_recv_ctx recv_ctx;
	static struct isotp_send_ctx send_ctx;

	int rc = isotp_bind(&recv_ctx, can_dev, dst, fc, &fc_opts, K_MSEC(200));

	if (rc != ISOTP_N_OK) {
		return rc;
	}

	/* A null completion callback makes this block until the last Consecutive
	 * Frame has gone out — the same property relay.cpp relies on to serialise
	 * command round trips without a mutex. */
	rc = isotp_send(&send_ctx, can_dev, out, out_len, dst, fc, nullptr, nullptr);
	if (rc == ISOTP_N_OK) {
		rc = isotp_recv(&recv_ctx, in, in_cap, K_MSEC(1000));
	}

	isotp_unbind(&recv_ctx);
	return rc;
}

/* A payload that is its own checksum: every byte differs from its neighbours
 * and from its own index mod 256, so a transfer that dropped, duplicated or
 * reordered a Consecutive Frame cannot compare equal by luck. Byte 0 is the
 * message type, as can_link.h specifies. */
void fill(uint8_t *buf, size_t len, enum relay_msg_type type)
{
	buf[0] = type;
	for (size_t i = 1; i < len; i++) {
		buf[i] = static_cast<uint8_t>((i * 7) ^ 0xA5);
	}
}

void *setup(void)
{
	zassert_true(device_is_ready(can_dev), "no CAN device — is zephyr,canbus chosen?");

	/* CAN_MODE_LOOPBACK is not implied by the driver's name, and getting this
	 * wrong is silent. drivers/can/can_loopback.c:79 drops every frame on the
	 * floor when the mode bit is clear: can_send() still returns 0, the
	 * transmit-done callback still fires, and no receive filter is ever
	 * consulted. Written the wrong way round first, this suite reported two
	 * passes — both of them tests that assert something is NOT received.
	 *
	 * It is the same call the H753ZI makes to bisect its own CAN stack
	 * without a transceiver (notes/can-guide.md), so the mode here is not a
	 * testing artefact; it is the bench technique, run in CI.
	 *
	 * Order matters: can_set_mode() returns -EBUSY once the controller is
	 * started, so mode first, then start. */
	int rc = can_set_mode(can_dev, CAN_MODE_LOOPBACK);

	zassert_equal(rc, 0, "can_set_mode(LOOPBACK) failed: %d", rc);

	/* Zephyr CAN drivers come up stopped, and this is the footgun
	 * docs/can-bringup.md warns about: everything initialises cleanly, every
	 * call returns 0, and nothing moves. can_loopback_send() returns
	 * -ENETDOWN until this is called, so the whole suite would fail here
	 * rather than mysteriously. */
	rc = can_start(can_dev);

	zassert_true(rc == 0 || rc == -EALREADY, "can_start failed: %d", rc);
	return NULL;
}

ZTEST_SUITE(isotp_loopback, NULL, setup, NULL, NULL, NULL);

/* ---- the frame-count boundaries ------------------------------------------- */

ZTEST(isotp_loopback, test_single_frame_payload_round_trips)
{
	/* Seven bytes is the largest payload ISO-TP carries in one Single Frame
	 * with standard addressing: the frame is eight bytes and one goes to the
	 * PCI nibble pair. So this transfer involves no Flow Control at all, and
	 * a command (21 bytes with its type byte) is already past it — there is
	 * no message in this project small enough to take this path in practice.
	 * It is here as the floor: if this fails, nothing above it can pass, and
	 * the fault is the controller rather than the segmentation. */
	uint8_t out[7];
	uint8_t in[kMaxPayload];

	fill(out, sizeof(out), RELAY_MSG_COMMAND);

	int len = transfer(&id_to_peer, &id_to_gateway, out, sizeof(out), in, sizeof(in));

	zassert_equal(len, static_cast<int>(sizeof(out)), "single frame returned %d", len);
	zassert_mem_equal(in, out, sizeof(out), "single-frame payload came back altered");
}

ZTEST(isotp_loopback, test_eight_bytes_is_the_first_size_that_segments)
{
	/* One byte more than a Single Frame holds, which is the cheapest possible
	 * exercise of the whole segmented path: First Frame, Flow Control back,
	 * one Consecutive Frame. Everything larger differs only in how many CFs
	 * follow, so if this passes and the 161-byte case fails, the fault is in
	 * buffering rather than in the protocol. That bisect is the reason this
	 * case exists next to the big one. */
	uint8_t out[8];
	uint8_t in[kMaxPayload];

	fill(out, sizeof(out), RELAY_MSG_TELEMETRY);

	int len = transfer(&id_to_gateway, &id_to_peer, out, sizeof(out), in, sizeof(in));

	zassert_equal(len, static_cast<int>(sizeof(out)), "segmented transfer returned %d", len);
	zassert_mem_equal(in, out, sizeof(out), "8-byte payload came back altered");
}

ZTEST(isotp_loopback, test_worst_case_upward_payload_round_trips)
{
	/* The headline. 161 bytes is what firmware/src/relay.cpp reads into, so
	 * it is the largest thing the gateway can be asked to reassemble: one
	 * First Frame carrying 6 bytes, a Flow Control, then 23 Consecutive
	 * Frames of 7. Twenty-five frames, a rolling 4-bit sequence number that
	 * wraps once, and a receive pool of four 56-byte buffers that has to
	 * chain to hold it.
	 *
	 * It also silently checks the pool sizing. CONFIG_ISOTP_RX_BUF_COUNT is 4
	 * by default and sensor-node/prj.conf trims it to 2 to fit 16 KB of RAM;
	 * two buffers hold 112 bytes and would cut this transfer short. The
	 * gateway must not inherit that number, and this is what says so. */
	uint8_t out[kMaxPayload];
	uint8_t in[kMaxPayload];

	fill(out, sizeof(out), RELAY_MSG_ACK);

	int len = transfer(&id_to_gateway, &id_to_peer, out, sizeof(out), in, sizeof(in));

	zassert_equal(len, static_cast<int>(kMaxPayload),
		      "worst-case payload returned %d, expected %zu", len, kMaxPayload);
	zassert_mem_equal(in, out, sizeof(out), "worst-case payload came back altered");
}

/* ---- the framing above ISO-TP --------------------------------------------- */

ZTEST(isotp_loopback, test_the_type_byte_leads_every_message)
{
	/* can_link.h's one byte of protocol, checked across the frame-count
	 * boundary rather than only in memory. The gateway switches on offset 0
	 * of a *reassembled* payload, so what matters is that reassembly puts the
	 * first byte the peer wrote at offset 0 — which is a claim about where a
	 * First Frame's six data bytes land, not about the sender's memcpy.
	 *
	 * All three types are sent at a size that segments, because that is the
	 * case where being wrong is possible. */
	const enum relay_msg_type types[] = {RELAY_MSG_TELEMETRY, RELAY_MSG_ACK,
					     RELAY_MSG_COMMAND};

	for (size_t i = 0; i < ARRAY_SIZE(types); i++) {
		uint8_t out[64];
		uint8_t in[kMaxPayload];

		fill(out, sizeof(out), types[i]);

		int len = transfer(&id_to_gateway, &id_to_peer, out, sizeof(out), in, sizeof(in));

		zassert_equal(len, static_cast<int>(sizeof(out)), "type %u returned %d", types[i],
			      len);
		zassert_equal(in[0], types[i], "type byte arrived as %u, sent %u", in[0], types[i]);
		zassert_mem_equal(&in[1], &out[1], sizeof(out) - 1,
				  "payload after the type byte was altered");
	}
}

ZTEST(isotp_loopback, test_a_receiver_bound_elsewhere_hears_nothing)
{
	/* The addresses are not decoration. A transfer aimed at the gateway must
	 * not be delivered to a context bound to the peer's identifier, and under
	 * loopback — where every frame physically reaches every filter on this
	 * node — the only thing preventing it is the identifier itself. On a real
	 * bus the same check would be indistinguishable from the wiring being
	 * wrong, which is why it is worth making here.
	 *
	 * Expressed as a timeout: bind to 0x7E0, send to 0x7E8, and receive
	 * nothing. The transfer cannot complete either, since no Flow Control
	 * will come back, so isotp_send() is expected to fail — this asserts what
	 * the *receiver* saw, which is the part under test. */
	static struct isotp_recv_ctx wrong_ctx;
	static struct isotp_send_ctx send_ctx;
	uint8_t out[64];
	uint8_t in[kMaxPayload];

	fill(out, sizeof(out), RELAY_MSG_TELEMETRY);

	int rc = isotp_bind(&wrong_ctx, can_dev, &id_to_peer, &id_to_gateway, &fc_opts,
			    K_MSEC(200));

	zassert_equal(rc, ISOTP_N_OK, "bind failed: %d", rc);

	(void)isotp_send(&send_ctx, can_dev, out, sizeof(out), &id_to_gateway, &id_to_peer,
			 nullptr, nullptr);

	rc = isotp_recv(&wrong_ctx, in, sizeof(in), K_MSEC(200));
	isotp_unbind(&wrong_ctx);

	zassert_true(rc < 0, "a context bound to 0x%03x received %d bytes addressed to 0x%03x",
		     id_to_peer.std_id, rc, id_to_gateway.std_id);
}

/* ---- the raw frame path --------------------------------------------------- */

CAN_MSGQ_DEFINE(heartbeat_q, 4);

ZTEST(isotp_loopback, test_heartbeat_frame_reaches_a_filter_and_unpacks)
{
	/* The heartbeat bypasses ISO-TP entirely — eight hand-packed bytes in one
	 * raw frame — and this is the first time those bytes have been through a
	 * controller. tests/heartbeat/ proves heartbeat_pack() and
	 * heartbeat_unpack() are inverses; it cannot prove that a frame at
	 * heartbeat_id(2) is matched by a filter built from heartbeat_id(2), or
	 * that eight bytes survive a DLC.
	 *
	 * can_add_rx_filter_msgq() rather than a callback for the same reason
	 * relay.cpp uses one: the callback runs in ISR context, and everything
	 * the gateway wants to do on receipt takes a mutex. */
	const struct can_filter filter = {
		.id = heartbeat_id(kPeerNodeId),
		.mask = CAN_STD_ID_MASK,
	};

	int filter_id = can_add_rx_filter_msgq(can_dev, &heartbeat_q, &filter);

	zassert_true(filter_id >= 0, "could not add heartbeat filter: %d", filter_id);

	const struct heartbeat sent = {
		.state = HEARTBEAT_STATE_OK,
		.uptime_s = 0xDEADBEEF, /* every byte distinct, so a swapped pair shows */
		.sequence_low = 0x5A,
	};

	struct can_frame frame = {};

	frame.id = heartbeat_id(kPeerNodeId);
	frame.dlc = kHeartbeatLen;
	heartbeat_pack(&sent, frame.data);

	int rc = can_send(can_dev, &frame, K_MSEC(200), nullptr, nullptr);

	zassert_equal(rc, 0, "can_send failed: %d", rc);

	struct can_frame received = {};

	rc = k_msgq_get(&heartbeat_q, &received, K_MSEC(500));
	can_remove_rx_filter(can_dev, filter_id);

	zassert_equal(rc, 0, "no heartbeat frame arrived at the filter");
	zassert_equal(received.id, heartbeat_id(kPeerNodeId), "wrong id: 0x%03x", received.id);
	zassert_equal(received.dlc, kHeartbeatLen, "wrong dlc: %u", received.dlc);

	struct heartbeat got = {};

	zassert_true(heartbeat_unpack(received.data, received.dlc, &got),
		     "a frame this project packed was rejected by its own unpacker");
	zassert_equal(got.state, sent.state, "state changed in flight");
	zassert_equal(got.uptime_s, sent.uptime_s, "uptime changed in flight: 0x%08x", got.uptime_s);
	zassert_equal(got.sequence_low, sent.sequence_low, "sequence changed in flight");
}

ZTEST(isotp_loopback, test_the_heartbeat_filter_ignores_the_isotp_identifiers)
{
	/* The address map's three identifiers per node are close together on
	 * purpose — 0x702, 0x7E0, 0x7E8 — and a mask that was too loose would
	 * feed ISO-TP frames into the heartbeat queue, where heartbeat_unpack()
	 * would reject them one at a time and the peer would be declared offline
	 * while transmitting perfectly. That failure would look like a dead node
	 * rather than like a wrong mask, so it is worth one test.
	 *
	 * CAN_STD_ID_MASK is what relay.cpp uses; this is what it buys. */
	const struct can_filter filter = {
		.id = heartbeat_id(kPeerNodeId),
		.mask = CAN_STD_ID_MASK,
	};

	int filter_id = can_add_rx_filter_msgq(can_dev, &heartbeat_q, &filter);

	zassert_true(filter_id >= 0, "could not add heartbeat filter: %d", filter_id);

	struct can_frame frame = {};

	frame.dlc = 8;
	memset(frame.data, 0xC3, sizeof(frame.data));

	/* uint32_t, not uint16_t: isotp_msg_id::std_id is an 11-bit bitfield of a
	 * uint32_t, and so is can_frame::id. Narrowing it here would be a
	 * conversion the compiler is right to refuse. */
	const uint32_t others[] = {id_to_gateway.std_id, id_to_peer.std_id,
				   static_cast<uint32_t>(heartbeat_id(kPeerNodeId)) + 1};

	for (size_t i = 0; i < ARRAY_SIZE(others); i++) {
		frame.id = others[i];
		zassert_equal(can_send(can_dev, &frame, K_MSEC(200), nullptr, nullptr), 0,
			      "can_send failed for 0x%03x", others[i]);
	}

	struct can_frame received = {};
	int rc = k_msgq_get(&heartbeat_q, &received, K_MSEC(200));

	can_remove_rx_filter(can_dev, filter_id);

	zassert_true(rc != 0, "the heartbeat filter matched 0x%03x", received.id);
}
