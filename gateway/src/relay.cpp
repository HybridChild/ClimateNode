/* The CAN half of the gateway. See relay.h for what a relay is and why the
 * channels are shaped the way they are.
 *
 * This file is the mirror of peer-node/src/main.cpp, seen from the other end
 * of the wire: what that file sends, this one receives, and vice versa. Reading
 * the two together is the clearest statement of the link there is.
 *
 * ---------------------------------------------------------------------------
 * Two threads, split by what they block on
 * ---------------------------------------------------------------------------
 *
 * The peer node runs one thread because it has 16 KB of RAM and a heartbeat to
 * keep. The gateway has neither constraint and a harder problem: it must be
 * receiving whenever the peer transmits, *and* able to send a command that
 * arrived from the broker at any moment. One thread doing both would have to
 * choose between blocking in isotp_recv() and blocking in isotp_send().
 *
 * So: an RX thread that only ever reads the link, and a TX thread that only ever
 * writes it. The single-writer rule is not tidiness — two isotp_send() calls on
 * one address from different contexts interleave their frames and corrupt both
 * transfers — and confining every write to one thread is how it is enforced
 * without a mutex.
 *
 * Rejected alternatives, for the record: k_poll on the receive context's fifo
 * (it lives inside struct isotp_recv_ctx, which isotp.h marks internal), and a
 * k_timer for the liveness timeout (expiry runs in ISR context and
 * zbus_chan_pub() takes a mutex, so it would need a work item purely to
 * publish). A timed isotp_recv() gives the same result with neither: the loop
 * that receives also owns the clock, which is the pattern sensor.cpp already
 * uses and docs/sensor-bringup.md already defends.
 */
#include "relay.h"

#include <zephyr/kernel.h>
#include <zephyr/canbus/isotp.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include <errno.h>
#include <string.h>

#include "can_link.h"
#include "protocol.h"

LOG_MODULE_REGISTER(relay, LOG_LEVEL_INF);

/* The one place the relay's byte budget meets the schema. relay.h keeps
 * kRelayUpMax as a plain number so tests and channels can size themselves
 * without the generated header; this is the check that the number is still big
 * enough. A schema change that outgrows it becomes a compile error here rather
 * than a truncated Ack on the wire. */
static_assert(node_Ack_size <= kRelayUpMax, "Ack no longer fits a relay_up");
static_assert(node_Telemetry_size <= kRelayUpMax, "Telemetry no longer fits a relay_up");
static_assert(node_Command_size <= sizeof(((struct relay_down *)nullptr)->bytes),
	      "Command no longer fits a relay_down");

/* Defined at the bottom, beside the channels; declared here so the TX thread
 * can wait on it. */
ZBUS_OBS_DECLARE(relay_cmd_sub);

namespace {

/* Which peer this gateway talks to. One today; the address helpers in
 * can_link.h all take a node id, so a second is a matter of another pair of
 * contexts rather than another protocol. */
constexpr uint8_t kPeerNodeId = kFirstPeerNodeId; /* 2 */

/* Above the sensor thread (7) because this one has a deadline the sensor does
 * not: a heartbeat that arrives while we are not listening is indistinguishable
 * from one that never came. Below main, which owns the keepalive. */
constexpr int kRxPriority = 6;
constexpr int kTxPriority = 7;
constexpr size_t kRxStackSize = 1536;
constexpr size_t kTxStackSize = 1024;

/* How long the RX thread blocks in isotp_recv() before looking at the clock.
 * It bounds how late an offline decision can be, so it wants to be well under
 * the 3.5 s timeout; it also bounds how often this thread wakes for nothing. */
constexpr int kRecvPollMs = 250;

/* How long the TX thread waits for the peer's Ack before answering on its
 * behalf. Generous against a peer that samples every 5 s and answers commands
 * from the same single thread. */
constexpr int kAckWaitMs = 2000;

const struct device *const can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));

/* Mirror of the peer's four: what it calls "to gateway" is what we receive.
 *
 * Data and flow control are on separate identifiers so that our bind and our
 * sender never install two acceptance filters for the same id -- can_link.h's
 * header comment has the mechanism, and the two filters we do install here are
 * 0x7E8 (bind) and 0x7E4 (sender's FC). */
const struct isotp_msg_id rx_addr = {
	.std_id = isotp_id_to_gateway(kPeerNodeId), /* 0x7E8: telemetry and acks, peer -> us */
};
const struct isotp_msg_id rx_fc_addr = {
	.std_id = isotp_fc_id_to_gateway(kPeerNodeId), /* 0x7E4: FC for our sends, peer -> us */
};
const struct isotp_msg_id tx_addr = {
	.std_id = isotp_id_to_peer(kPeerNodeId), /* 0x7E0: commands, us -> peer */
};
const struct isotp_msg_id tx_fc_addr = {
	.std_id = isotp_fc_id_to_peer(kPeerNodeId), /* 0x7EC: FC for our bind, us -> peer */
};

/* Flow control we advertise to the peer: take the whole transfer, no gaps. The
 * gateway is the fast end of this link and has no reason to throttle a node
 * sending it 25 bytes. */
const struct isotp_fc_opts fc_opts = {
	.bs = 0,
	.stmin = 0,
};

struct isotp_recv_ctx recv_ctx;
struct isotp_send_ctx send_ctx;

/* Heartbeats arrive as raw frames, so they bypass ISO-TP entirely and land in a
 * message queue filled from the driver's RX callback. A queue rather than a
 * callback that publishes directly, because can_add_rx_filter()'s callback runs
 * in ISR context and zbus_chan_pub() takes a mutex. Depth 4 covers three missed
 * wakeups at 1 Hz, which is already past the point where the peer is declared
 * offline. */
K_MSGQ_DEFINE(heartbeat_q, sizeof(struct can_frame), 4, 4);

/* The rendezvous between the two threads: RX gives it after publishing an Ack
 * upward, TX waits on it after sending a command. One semaphore is the whole
 * mechanism — no mutex, no shared mutable state — and it is what makes "at most
 * one ack outstanding" true by construction rather than by hope. */
K_SEM_DEFINE(ack_received, 0, 1);

/* ---- publishing upward ---------------------------------------------------- */

/* Copy a relayed payload onto a channel. The bytes are moved, never inspected:
 * this function does not know or care what protobuf message it is carrying. */
void publish_up(const struct zbus_channel *chan, const uint8_t *bytes, size_t len)
{
	if (len > kRelayUpMax) {
		/* Refused rather than truncated. Half a protobuf message decodes
		 * into something plausible, which is worse than nothing arriving
		 * — the same argument decode_command() makes about oversized
		 * MQTT payloads. */
		LOG_ERR("relayed payload %zu B exceeds %zu B — dropped", len, kRelayUpMax);
		return;
	}

	struct relay_up up = {};

	up.node_id = kPeerNodeId;
	up.len = static_cast<uint16_t>(len);
	memcpy(up.bytes, bytes, len);

	int rc = zbus_chan_pub(chan, &up, K_MSEC(50));

	if (rc != 0) {
		LOG_WRN("relay publish failed: %d", rc);
	}
}

void publish_status(bool online)
{
	struct relay_status st = {};

	st.node_id = kPeerNodeId;
	st.online = online;

	int rc = zbus_chan_pub(&chan_relay_status, &st, K_MSEC(50));

	if (rc != 0) {
		LOG_WRN("relay status publish failed: %d", rc);
	}
	LOG_INF("peer node %u is %s", kPeerNodeId, online ? "online" : "offline");
}

/* ---- receiving ------------------------------------------------------------ */

/* An ISO-TP payload has arrived from the peer. Read byte 0, route on it, and
 * forward the remainder untouched.
 *
 * Reading that byte is not decoding. It is the topic, moved inside the payload
 * because ISO-TP has no topic to put it on (can_link.h); the gateway needs it
 * for exactly the same reason a subscriber needs to know which topic a message
 * came from, and learns nothing else about the message from it. */
void handle_relayed(const uint8_t *buf, size_t len)
{
	if (len < 1) {
		LOG_WRN("empty ISO-TP payload from peer");
		return;
	}

	switch (buf[0]) {
	case RELAY_MSG_TELEMETRY:
		publish_up(&chan_relay_telemetry, &buf[1], len - 1);
		break;

	case RELAY_MSG_ACK:
		publish_up(&chan_relay_ack, &buf[1], len - 1);
		/* Only after the ack is on its way upward, so the TX thread
		 * cannot be released before the thing it was waiting for
		 * exists. */
		k_sem_give(&ack_received);
		break;

	default:
		/* RELAY_MSG_COMMAND travelling upward would mean the peer is
		 * confused about which end it is; anything else is a newer peer
		 * speaking a protocol this gateway does not have. Either way
		 * there is no topic to put it on. */
		LOG_WRN("unroutable message type %u from peer %u", buf[0], kPeerNodeId);
		break;
	}
}

/* Drain the heartbeat queue. Returns true if at least one valid beat was seen,
 * which is all the liveness machine needs to know. */
bool drain_heartbeats(void)
{
	struct can_frame frame;
	bool seen = false;

	while (k_msgq_get(&heartbeat_q, &frame, K_NO_WAIT) == 0) {
		struct heartbeat hb;

		if (!heartbeat_unpack(frame.data, can_dlc_to_bytes(frame.dlc), &hb)) {
			/* A frame at the right id that we cannot parse means a
			 * peer running a layout this gateway does not know. NOT
			 * counted as liveness: pretending to understand it would
			 * report a peer as healthy while relaying nothing. */
			LOG_WRN("undecodable heartbeat from id 0x%03x", frame.id);
			continue;
		}
		seen = true;
		LOG_DBG("heartbeat: state %u, up %us, seq %u", hb.state, hb.uptime_s,
			hb.sequence_low);
	}
	return seen;
}

/* Runs in ISR context, so it does exactly one thing that is safe there. */
void heartbeat_isr(const struct device *dev, struct can_frame *frame, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	/* Dropped rather than blocked if the queue is full: the RX thread is
	 * evidently not keeping up, and the newest beat is not more informative
	 * than the three already waiting. */
	k_msgq_put(&heartbeat_q, frame, K_NO_WAIT);
}

void rx_thread(void *, void *, void *)
{
	if (!device_is_ready(can_dev)) {
		LOG_ERR("CAN device not ready — relay disabled");
		return;
	}

	int rc = can_start(can_dev);

	if (rc != 0 && rc != -EALREADY) {
		LOG_ERR("can_start failed: %d — relay disabled", rc);
		return;
	}

	/* Installed before the ISO-TP bind so it takes the lower filter slot.
	 * That ordering is deliberate and has a visible consequence: `can dump`
	 * from the shell installs a catch-all through the same lowest-free-slot
	 * allocator, and on M_CAN matching stops at the first match — so once
	 * this filter exists, `can dump` will show nothing while heartbeats
	 * arrive perfectly. Use `can filter add <dev> 0x702` instead. The whole
	 * mechanism is in docs/can-bringup.md. */
	const struct can_filter hb_filter = {
		.id = heartbeat_id(kPeerNodeId),
		.mask = CAN_STD_ID_MASK,
	};

	rc = can_add_rx_filter(can_dev, heartbeat_isr, nullptr, &hb_filter);
	if (rc < 0) {
		LOG_ERR("heartbeat filter: %d — relay disabled", rc);
		return;
	}

	rc = isotp_bind(&recv_ctx, can_dev, &rx_addr, &tx_fc_addr, &fc_opts, K_MSEC(200));
	if (rc != ISOTP_N_OK) {
		LOG_ERR("isotp_bind failed: %d — relay disabled", rc);
		return;
	}

	/* The peer logs the same four with in and out swapped. Reading the two
	 * consoles side by side is how you confirm both ends agree on the map. */
	LOG_INF("relay up: peer %u, heartbeat 0x%03x, isotp in 0x%03x (fc out 0x%03x), "
		"out 0x%03x (fc in 0x%03x)",
		kPeerNodeId, heartbeat_id(kPeerNodeId), rx_addr.std_id, tx_fc_addr.std_id,
		tx_addr.std_id, rx_fc_addr.std_id);

	/* Starts UNKNOWN, with the clock running from now. Nothing is published
	 * until either a beat arrives or the timeout elapses, so a gateway
	 * restart never briefly claims a healthy peer is dead. */
	struct liveness peer = {PEER_UNKNOWN, k_uptime_get()};

	while (true) {
		uint8_t buf[1 + kRelayUpMax];
		int len = isotp_recv(&recv_ctx, buf, sizeof(buf), K_MSEC(kRecvPollMs));

		if (len > 0) {
			handle_relayed(buf, static_cast<size_t>(len));
		} else if (len != ISOTP_RECV_TIMEOUT) {
			LOG_WRN("isotp_recv: %d", len);
		}

		/* Both the heartbeat and the clock feed the same step, so a
		 * timeout is not a special case — it is simply a step in which
		 * no frame was seen. */
		if (liveness_step(&peer, drain_heartbeats(), k_uptime_get(), kHeartbeatTimeoutMs)) {
			publish_status(peer.state == PEER_ONLINE);
		}
	}
}

/* ---- sending -------------------------------------------------------------- */

/* Answer a command the peer did not. The gateway is allowed to *author* an Ack;
 * it still never decodes the peer's.
 *
 * This is the one place the gateway's opacity is compromised, and the
 * compromise is minimal and named: to correlate this Ack the gateway needs the
 * command's sequence, which main.cpp read out of the Command envelope before
 * forwarding it. One header field, never the payload arm.
 *
 * The alternative was to synthesize nothing and let the host time out. That is
 * genuinely simpler and strictly worse: the host cannot distinguish "the peer is
 * gone" from "the gateway dropped it", and would have to invent its own timeout
 * to say anything at all. Recorded in docs/mqtt-design.md. */
void send_synthetic_ack(uint32_t sequence, const char *detail)
{
	uint8_t buf[node_Ack_size];
	size_t len = encode_ack(sequence, node_AckStatus_ACK_STATUS_FAILED, detail, nullptr, buf,
				sizeof(buf));

	if (len > 0) {
		publish_up(&chan_relay_ack, buf, len);
	}
}

void tx_thread(void *, void *, void *)
{
	while (true) {
		const struct zbus_channel *chan;
		struct relay_down cmd;

		if (zbus_sub_wait_msg(&relay_cmd_sub, &chan, &cmd, K_FOREVER) != 0) {
			continue;
		}
		if (chan != &chan_relay_command) {
			continue;
		}

		/* +1 for the type byte, prepended into one buffer rather than
		 * sent as a separate frame: ISO-TP delivers a payload whole or
		 * not at all, and splitting the two would reintroduce exactly
		 * the reassembly problem it exists to solve. */
		uint8_t frame[1 + sizeof(cmd.bytes)];

		frame[0] = RELAY_MSG_COMMAND;
		memcpy(&frame[1], cmd.bytes, cmd.len);

		/* Any stale give from a previous round trip is cleared here
		 * rather than after the wait, so a late ack cannot satisfy the
		 * *next* command's wait. */
		k_sem_reset(&ack_received);

		/* Blocks until the whole segmented transfer completes. That is
		 * what serialises command round trips structurally, which is the
		 * premise chan_relay_ack's latest-wins observer depends on — see
		 * relay.h. */
		int rc = isotp_send(&send_ctx, can_dev, frame, cmd.len + 1, &tx_addr, &rx_fc_addr,
				    nullptr, nullptr);

		if (rc != ISOTP_N_OK) {
			LOG_WRN("command to peer %u failed to send: %d", cmd.node_id, rc);
			send_synthetic_ack(cmd.sequence, "no route to node over CAN");
			continue;
		}

		if (k_sem_take(&ack_received, K_MSEC(kAckWaitMs)) != 0) {
			LOG_WRN("peer %u did not answer command seq=%u", cmd.node_id,
				cmd.sequence);
			send_synthetic_ack(cmd.sequence, "no response over CAN");
		}
		/* On success the RX thread has already published the peer's own
		 * Ack; there is nothing left for this thread to do. */
	}
}

}  // namespace

/* Definitions at global scope: ZBUS_CHAN_DECLARE in relay.h declares these same
 * symbols, and an anonymous namespace would give them a different linkage than
 * the declaration. notes/language-cpp.md §6. */

ZBUS_MSG_SUBSCRIBER_DEFINE(relay_cmd_sub);

/* The three upward channels. No validator on any of them: the gateway is not in
 * a position to judge a payload it does not decode, and inventing a rule here
 * would be the opposite of the whole design. The observers are defined in
 * main.cpp — the network side owns what feeds the network. */
ZBUS_CHAN_DEFINE(chan_relay_telemetry, struct relay_up,
		 nullptr, nullptr,
		 ZBUS_OBSERVERS(relay_telemetry_listener),
		 ZBUS_MSG_INIT(0));

ZBUS_CHAN_DEFINE(chan_relay_ack, struct relay_up,
		 nullptr, nullptr,
		 ZBUS_OBSERVERS(relay_ack_listener),
		 ZBUS_MSG_INIT(0));

ZBUS_CHAN_DEFINE(chan_relay_status, struct relay_status,
		 nullptr, nullptr,
		 ZBUS_OBSERVERS(relay_status_listener),
		 ZBUS_MSG_INIT(0));

/* The one downward channel, and the only relay message on the message-subscriber
 * pool — which is why that pool stays sized for 32 bytes rather than 164. */
ZBUS_CHAN_DEFINE(chan_relay_command, struct relay_down,
		 nullptr, nullptr,
		 ZBUS_OBSERVERS(relay_cmd_sub),
		 ZBUS_MSG_INIT(0));

K_THREAD_DEFINE(relay_rx_tid, kRxStackSize, rx_thread, NULL, NULL, NULL, kRxPriority, 0, 0);
K_THREAD_DEFINE(relay_tx_tid, kTxStackSize, tx_thread, NULL, NULL, NULL, kTxPriority, 0, 0);
