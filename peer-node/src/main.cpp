/* The CAN session: this node's whole conversation with the gateway.
 *
 * The counterpart of gateway/src/main.cpp, which owns the MQTT session on the
 * gateway. Reading them side by side is the point of the exercise, because the
 * two files do the same job over transports that agree on almost nothing:
 *
 *                       gateway (MQTT)              this node (CAN)
 *   addressing          topic string                11-bit frame id
 *   framing             one payload, delivered      8 bytes; ISO-TP segments
 *                       whole                       and reassembles
 *   which type is this  the topic asserts it        one byte at the front
 *   connection          CONNECT/keepalive/will      none; there is nothing to
 *                                                   connect to
 *   liveness            broker's Last Will          a heartbeat we send, and a
 *                                                   timeout the gateway runs
 *   acknowledgement     PUBACK, per recipient       ISO-TP flow control, and
 *                                                   nothing above it
 *
 * The last two rows are why this file has code the gateway does not: liveness
 * has to be published rather than inferred, because a CAN bus cannot tell a
 * silent node from an absent one. notes/can-guide.md §5 and §9.
 *
 * ---------------------------------------------------------------------------
 * One thread, one clock, one writer
 * ---------------------------------------------------------------------------
 *
 * Everything below runs on main. That is not thrift, it is a correctness
 * property: two isotp_send() calls on the same address from different contexts
 * interleave their frames on one CAN id and corrupt both transfers, so the
 * single-writer rule is load-bearing rather than tidy. It also happens to fit a
 * part with 16 KB of RAM, where a second stack is a real fraction of the
 * budget.
 *
 * The loop's shape follows from that. The heartbeat is the clock: wake at least
 * once per kHeartbeatPeriodMs, and spend the time in between blocked in
 * isotp_recv() waiting for a command. Telemetry arrives asynchronously on a
 * zbus channel and is picked up on whichever wake comes next, so a reading can
 * wait up to one heartbeat period before it is sent -- acceptable against a 5 s
 * sample cadence, and the alternative (a second thread, or a poll over
 * structures isotp.h marks internal) costs more than the latency is worth.
 */
#include <zephyr/kernel.h>
#include <zephyr/canbus/isotp.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/zbus/zbus.h>

#include <errno.h>
#include <string.h>

#include "app_channels.h"
#include "can_link.h"
#include "commands.h"
#include "protocol.h"

LOG_MODULE_REGISTER(node, LOG_LEVEL_INF);

/* This app's half of the identity seam commands.h declares. The gateway defines
 * "nucleo-1"; this is node 2, and the two must differ -- GetDeviceInfo is how a
 * host asks which node it is actually talking to, and a relayed answer with the
 * wrong id would be worse than no answer. */
const char *const kNodeClientId = "nucleo-2";

namespace {

constexpr uint8_t kNodeId = kFirstPeerNodeId; /* 2 */

/* One line, and it is the reason this file compiles unchanged against fdcan1 on
 * the H7, can1 here, and a software loopback node in a test: the devicetree
 * says which controller is *the* CAN bus, so no node label appears in the C. */
const struct device *const can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));

/* ---- ISO-TP addressing ---------------------------------------------------- */

/* Mirrored from the gateway's point of view: what it calls "to peer" is what we
 * receive, and vice versa. Getting this pair backwards produces a link on which
 * every frame is transmitted correctly and nothing is ever delivered, so the
 * constructors in can_link.h are named for direction rather than for role.
 *
 * Four addresses rather than two, because our bind and our sender each need an
 * identifier no other filter on this node is listening to -- can_link.h's header
 * comment has the mechanism and the symptom of getting it wrong. The pairing to hold on to:
 * an ISO-TP context's two addresses are always "the data I move" and "the flow
 * control that answers it", travelling in opposite directions. */
const struct isotp_msg_id rx_addr = {
	.std_id = isotp_id_to_peer(kNodeId), /* 0x7E0: commands, gateway -> us */
};
const struct isotp_msg_id rx_fc_addr = {
	.std_id = isotp_fc_id_to_peer(kNodeId), /* 0x7EC: FC for our sends, gateway -> us */
};
const struct isotp_msg_id tx_addr = {
	.std_id = isotp_id_to_gateway(kNodeId), /* 0x7E8: telemetry and acks, us -> gateway */
};
const struct isotp_msg_id tx_fc_addr = {
	.std_id = isotp_fc_id_to_gateway(kNodeId), /* 0x7E4: FC for our bind, us -> gateway */
};

/* Flow control we advertise to a sender: bs = 0 means "send the whole thing,
 * do not wait for me again", stmin = 0 means "no minimum gap between frames".
 *
 * Both are the permissive end of the range, and they are right here because the
 * only thing that ever sends to this node is a Command -- 20 bytes worst case,
 * three frames. Block size exists so a slow receiver can throttle a fast sender
 * mid-transfer (notes/can-guide.md §8); throttling three frames would buy
 * nothing and cost a round trip per block. A node receiving a firmware image
 * would answer this question differently. */
const struct isotp_fc_opts fc_opts = {
	.bs = 0,
	.stmin = 0,
};

struct isotp_recv_ctx recv_ctx;
struct isotp_send_ctx send_ctx;

/* ---- telemetry from the sensor thread ------------------------------------- */

/* Set by the listener below, cleared once the reading has been sent. A plain
 * bool rather than an atomic: the listener runs in the context of whoever
 * published (the sensor thread), and a single-byte flag written by one thread
 * and read by another is exactly the case k_sem exists for -- which is what the
 * semaphore beside it is doing. The flag only says "there is something to
 * read"; the reading itself lives in the channel, where latest-wins keeps it
 * fresh, and is copied out under zbus's own lock by zbus_chan_read(). */
K_SEM_DEFINE(telemetry_pending, 0, 1);

void on_telemetry(const struct zbus_channel *chan)
{
	ARG_UNUSED(chan);

	/* A listener runs inside zbus_chan_pub(), in the publisher's context and
	 * with the channel locked. So it must not block and must not read the
	 * channel -- it signals, and the loop below does the reading. The
	 * gateway's listener signals an eventfd for the same reason; the only
	 * difference is that its reader is in poll() and ours is not. */
	k_sem_give(&telemetry_pending);
}

/* ---- link state ----------------------------------------------------------- */

/* A peer node whose gateway is absent is in a **normal operating condition**,
 * not an error state. It happens whenever the gateway reboots, whenever the bus
 * is unplugged, and for as long as this node is powered before its gateway is.
 *
 * Treating it as an error costs twice over: a failure line per heartbeat, which
 * buries anything else the console has to say, and a full second of the loop
 * spent inside every isotp_send() waiting for a flow control frame nobody is
 * going to send. The node ends up healthy and unreadable.
 *
 * So the rule below is the same one gateway/src/main.cpp applies to a broker
 * that will not accept a connection: back off, stay quiet, keep sampling. The
 * sensor thread never learns about any of this — it publishes to the channel on
 * its own cadence regardless, which is the whole reason the channel is there.
 */
constexpr uint32_t kTxBackoffMinMs = 2000;
constexpr uint32_t kTxBackoffMaxMs = 30000;

bool link_up = true; /* optimistic: the first failure is the news, not the first success */
uint32_t tx_backoff_ms = 0;
int64_t tx_retry_at = 0;

/* Note an outcome, and log only when it *changes*. Without this the interesting
 * event — "the gateway went away" — is buried in identical lines repeating at
 * 1 Hz forever, which is the opposite of what a log is for. */
void note_link(bool ok, const char *what, int rc)
{
	if (ok) {
		if (!link_up) {
			LOG_INF("gateway reachable again");
		}
		link_up = true;
		tx_backoff_ms = 0;
		return;
	}

	if (link_up) {
		LOG_WRN("gateway unreachable (%s failed: %d) — backing off, still sampling", what,
			rc);
	}
	link_up = false;

	/* Doubling, capped. Same shape as the MQTT reconnect backoff on the
	 * gateway, and for the same reason: retry often enough to notice the
	 * peer return, rarely enough that failing costs nothing. */
	tx_backoff_ms = (tx_backoff_ms == 0) ? kTxBackoffMinMs
					     : MIN(tx_backoff_ms * 2, kTxBackoffMaxMs);
	tx_retry_at = k_uptime_get() + tx_backoff_ms;
}

/* Is the controller in a state where transmitting can possibly work?
 *
 * Bus-off is the one that matters here, and it is what this node hits with no
 * peer attached: every frame it sends goes unacknowledged, the transmit error
 * counter climbs past 255, and the controller takes itself off the bus
 * (notes/can-guide.md §6). can_send() then returns -ENETUNREACH immediately,
 * which is cheap — but asking first is what lets the heartbeat stay silent
 * about a condition it can neither fix nor influence.
 *
 * Recovery is automatic (CONFIG_CAN_MANUAL_RECOVERY_MODE is off) and does not
 * depend on us trying: the controller returns to error-active after it observes
 * 128 sequences of 11 recessive bits. So skipping a send while bus-off delays
 * nothing. */
bool bus_transmittable(void)
{
	enum can_state state;

	if (can_get_state(can_dev, &state, nullptr) != 0) {
		return true; /* cannot tell — try, and let the send report */
	}
	return state != CAN_STATE_BUS_OFF && state != CAN_STATE_STOPPED;
}

/* ---- sending -------------------------------------------------------------- */

/* Send one already-encoded protobuf message with its type byte in front.
 *
 * The type byte is prepended into a local buffer rather than sent as a separate
 * frame, because ISO-TP delivers a payload whole or not at all and splitting
 * the two would reintroduce exactly the reassembly problem it exists to solve.
 *
 * isotp_send() with a null completion callback BLOCKS until the whole segmented
 * transfer has finished. That is what makes the single-writer rule enforceable
 * without a mutex: this function cannot be re-entered from this thread, and no
 * other thread calls it. It also means the caller's buffer stays valid for the
 * duration, which the API requires. */
int send_message(enum relay_msg_type type, const uint8_t *payload, size_t len)
{
	/* +1 for the type byte. Sized from the largest thing we ever send. */
	uint8_t frame[1 + node_Ack_size];

	if (len + 1 > sizeof(frame)) {
		LOG_ERR("payload %zu B exceeds the ISO-TP buffer", len);
		return -EMSGSIZE;
	}

	frame[0] = static_cast<uint8_t>(type);
	memcpy(&frame[1], payload, len);

	int rc = isotp_send(&send_ctx, can_dev, frame, len + 1, &tx_addr, &rx_fc_addr, nullptr,
			    nullptr);

	/* ISOTP_N_TIMEOUT_BS (-2) is the signature of an absent gateway: the
	 * first frame went out and no flow control came back within a second.
	 * That is not an error to report per attempt — it is a link state, and
	 * note_link() owns saying so once. Anything else is a genuine surprise
	 * and is worth a line of its own. */
	if (rc != ISOTP_N_OK && rc != ISOTP_N_TIMEOUT_BS && rc != ISOTP_N_TIMEOUT_A) {
		LOG_WRN("isotp_send(type=%u, %zu B) failed: %d", type, len + 1, rc);
	}
	return rc;
}

/* Returns the ISO-TP result, not a bool: ISOTP_N_OK, or the code that explains
 * the failure. note_link() prints it, and the distinction it carries is the
 * whole diagnosis -- ISOTP_N_TIMEOUT_BS means the first frame went out and no
 * flow control came back, which is a very different fault from the transmit
 * itself failing, and the two point at different halves of the link. A bool
 * cannot carry that distinction. */
int send_telemetry(const struct sensor_reading &reading)
{
	uint8_t payload[node_Telemetry_size];
	size_t len = encode_telemetry(reading, payload, sizeof(payload));

	if (len == 0) {
		return -EINVAL; /* encode_telemetry already logged why */
	}

	return send_message(RELAY_MSG_TELEMETRY, payload, len);
}

/* ---- the heartbeat -------------------------------------------------------- */

/* What the last reading said, so the heartbeat can report the node's state
 * without re-reading the sensor. Written and read only by main. */
struct heartbeat hb_state = {
	.state = HEARTBEAT_STATE_BOOT,
	.uptime_s = 0,
	.sequence_low = 0,
};

/* Transmission bookkeeping for the heartbeat, written from the CAN TX ISR.
 *
 * The heartbeat must be sent with a completion callback, because can_send()
 * without one blocks for an unbounded time on this controller -- and main is
 * the only thread this node has.
 *
 * Two things combine to make the wait unbounded. First, a NULL callback means
 * z_impl_can_send() installs its own and does k_sem_take(K_FOREVER) on
 * completion (can_common.c:69); the k_timeout_t argument bounds only the wait
 * for a free transmit mailbox (can_stm32_bxcan.c:800-808), so K_NO_WAIT does
 * not make the call non-blocking. Second, completion may never come: the driver
 * clears NART, so the hardware retries an unacknowledged frame indefinitely,
 * and sets ABOM, so the bus-off that would abort the retrying is recovered from
 * automatically. There is no state a lone transmitter reaches where the
 * hardware gives up.
 *
 * A blocking heartbeat therefore parks main for as long as the link is down,
 * which stops telemetry and -- since main also owns isotp_recv() -- stops
 * command handling too. Worse, it does so invisibly: the send eventually
 * succeeds when the bus returns, so note_link() is never told anything changed.
 *
 * With a callback, can_send() returns as soon as the frame is queued and the
 * link verdict is formed from what has actually COMPLETED rather than from what
 * was accepted for transmission. */
atomic_t beats_in_flight = ATOMIC_INIT(0);
atomic_t last_beat_error = ATOMIC_INIT(0);

void heartbeat_sent(const struct device *dev, int error, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	atomic_set(&last_beat_error, error);
	atomic_dec(&beats_in_flight);
}

void send_heartbeat(void)
{
	struct can_frame frame = {};

	frame.id = heartbeat_id(kNodeId);
	frame.dlc = kHeartbeatLen;

	hb_state.uptime_s = static_cast<uint32_t>(k_uptime_get() / 1000);
	heartbeat_pack(&hb_state, frame.data);

	/* K_NO_WAIT plus a real callback, which together are what make this
	 * genuinely non-blocking -- see the note on beats_in_flight above for
	 * why the callback is the load-bearing half. A heartbeat that had to
	 * queue behind something is already late, and the next one is a second
	 * away; blocking main here would delay the command handling this loop
	 * also owes, for as long as the link stayed down.
	 *
	 * The heartbeat is NOT backed off when the link is down, unlike
	 * telemetry: it is one non-blocking frame, it is what a returning
	 * gateway is waiting to hear, and going quiet would make recovery
	 * depend on the telemetry cadence. Only its logging is suppressed. */
	int rc = can_send(can_dev, &frame, K_NO_WAIT, heartbeat_sent, nullptr);

	if (rc == 0) {
		atomic_inc(&beats_in_flight);
	}

	/* Three things have to hold for the link to count as up, and each of
	 * them fails in a different way:
	 *
	 *   rc == 0            the frame was accepted. -EAGAIN here means all
	 *                      three transmit mailboxes are still occupied by
	 *                      earlier beats, which is the steady state of an
	 *                      unacknowledged bus.
	 *   in flight <= 1     the PREVIOUS beat completed. At 1 Hz, a beat
	 *                      still outstanding when the next one is queued
	 *                      means nothing is acknowledging us. This is what
	 *                      notices the fault at t+2s instead of t+4s, when
	 *                      the mailboxes finally fill.
	 *   no error           the last completed beat reported success.
	 *
	 * Reading a result that belongs to the previous beat is deliberate: a
	 * transmission that has not finished has no verdict yet, and inventing
	 * one is precisely the mistake this replaces. */
	int err = (rc != 0) ? rc : static_cast<int>(atomic_get(&last_beat_error));

	if (err == 0 && atomic_get(&beats_in_flight) > 1) {
		/* A distinct code rather than a bare false, so the console names
		 * which of the three conditions failed. note_link() prints the
		 * number it is given, so handing it a 0 here would report
		 * "failed: 0" and say nothing at all. */
		err = -EBUSY;
	}

	note_link(err == 0, "heartbeat", err);
}

/* ---- receiving ------------------------------------------------------------ */

void handle_incoming(const uint8_t *buf, size_t len)
{
	if (len < 1) {
		LOG_WRN("empty ISO-TP payload");
		return;
	}

	if (buf[0] != RELAY_MSG_COMMAND) {
		/* The gateway only ever sends commands downward. Anything else
		 * is a bug at the far end or a newer peer's traffic on our
		 * address, and either way this node has no handler. */
		LOG_WRN("unexpected message type %u from the gateway", buf[0]);
		return;
	}

	node_Command cmd;

	/* `oversized` is false: ISO-TP already refused anything longer than the
	 * buffer, so a payload that arrives here arrived whole. That is one of
	 * the two things the gateway's MQTT path has to check for itself. */
	if (!decode_command(&buf[1], len - 1, false, &cmd)) {
		uint8_t payload[node_Ack_size];
		/* Sequence 0: the bytes did not decode, so there is no sequence
		 * to echo. The host correlates on it and will see a 0 it never
		 * sent, which is the honest report of "something arrived and it
		 * was not a Command". */
		size_t n = encode_ack(0, node_AckStatus_ACK_STATUS_MALFORMED, "decode failed",
				      nullptr, payload, sizeof(payload));

		if (n > 0) {
			send_message(RELAY_MSG_ACK, payload, n);
		}
		return;
	}

	struct command_result result;

	/* The shared commands.cpp -- the same object code the gateway runs,
	 * including its duplicate suppression. Worth noticing that the dedupe is
	 * still useful here even though CAN has no QoS 1 redelivery: the gateway
	 * can retry a command whose ack it never saw, and this node must not act
	 * on it twice. The reason changed; the requirement did not. */
	handle_command(cmd, &result);

	uint8_t payload[node_Ack_size];
	size_t n = encode_ack(cmd.sequence, result.status, result.detail,
			      result.has_info ? &result.info : nullptr, payload, sizeof(payload));

	if (n > 0) {
		send_message(RELAY_MSG_ACK, payload, n);
	}
}

}  // namespace

/* At global scope: chan_telemetry in sensor.cpp names this observer, and
 * internal linkage would stop the two from meeting. */
ZBUS_LISTENER_DEFINE(telemetry_listener, on_telemetry);

int main(void)
{
	if (!device_is_ready(can_dev)) {
		LOG_ERR("CAN device not ready");
		return 0;
	}

	/* Controllers come up stopped. Nothing transmits or receives until this
	 * call, and the failure mode is the nasty kind: every log line looks
	 * healthy and no frame moves. See docs/can-bringup.md. */
	int rc = can_start(can_dev);

	if (rc != 0 && rc != -EALREADY) {
		LOG_ERR("can_start failed: %d", rc);
		return 0;
	}

	/* Binding installs a hardware acceptance filter for rx_addr and gives us
	 * a receive context to read reassembled payloads from. Until this
	 * returns, commands from the gateway are dropped by the controller
	 * itself rather than by us. */
	rc = isotp_bind(&recv_ctx, can_dev, &rx_addr, &tx_fc_addr, &fc_opts, K_MSEC(200));

	if (rc != ISOTP_N_OK) {
		LOG_ERR("isotp_bind failed: %d", rc);
		return 0;
	}

	/* All four identifiers, read back out of can_link.h rather than written
	 * as literals: seeing them here and mirrored on the gateway's console is
	 * the cheapest confirmation that both ends computed the same map. */
	LOG_INF("node %u on CAN: heartbeat 0x%03x, isotp in 0x%03x (fc out 0x%03x), "
		"out 0x%03x (fc in 0x%03x)",
		kNodeId, heartbeat_id(kNodeId), rx_addr.std_id, tx_fc_addr.std_id,
		tx_addr.std_id, rx_fc_addr.std_id);

	int64_t next_beat = k_uptime_get();

	while (true) {
		int64_t until_beat = next_beat - k_uptime_get();

		if (until_beat <= 0) {
			/* Skipped while bus-off, because it cannot succeed and
			 * trying does nothing to hasten recovery. */
			if (bus_transmittable()) {
				send_heartbeat();
			} else {
				note_link(false, "bus", -ENETUNREACH);
			}
			/* Re-based on the schedule, not on now: a slow iteration
			 * shifts one beat rather than permanently drifting the
			 * cadence the gateway is timing us against. */
			next_beat += kHeartbeatPeriodMs;
			continue;
		}

		/* One blocking call, ended by either of the two things that can
		 * happen: a command arrives, or the beat comes due. Same shape
		 * as the sensor thread's wait, and as the gateway's poll(). */
		uint8_t buf[1 + node_Command_size];
		int len = isotp_recv(&recv_ctx, buf, sizeof(buf), K_MSEC(until_beat));

		if (len > 0) {
			handle_incoming(buf, static_cast<size_t>(len));
		} else if (len != ISOTP_RECV_TIMEOUT) {
			LOG_WRN("isotp_recv failed: %d", len);
		}

		/* Whatever ended the wait, send a reading if one is waiting.
		 * k_sem_take with K_NO_WAIT is a non-blocking "was anything
		 * published?", and the count saturates at 1, so several readings
		 * taken during a long transfer collapse to one -- which is what
		 * the channel did to them anyway. Latest wins on both. */
		if (k_sem_take(&telemetry_pending, K_NO_WAIT) == 0) {
			struct sensor_reading reading;

			if (zbus_chan_read(&chan_telemetry, &reading, K_MSEC(50)) == 0) {
				/* The heartbeat reports what the last reading
				 * said, so the gateway can distinguish "peer
				 * alive, sensor broken" from "peer gone" without
				 * decoding a single protobuf byte. This happens
				 * whether or not the reading can be transmitted:
				 * the state is true regardless of who is
				 * listening. */
				enum heartbeat_state was = hb_state.state;

				hb_state.state = (reading.status == SENSOR_READING_ERROR)
							 ? HEARTBEAT_STATE_SENSOR_ERROR
							 : HEARTBEAT_STATE_OK;
				hb_state.sequence_low =
					static_cast<uint8_t>(reading.sequence & 0xFF);

				/* On change only, same rule as the link state. It
				 * matters because sensor.cpp announces the sensor
				 * once at boot and never again, so a console
				 * attached later — the normal case, since screen
				 * cannot be open before the board resets — has no
				 * way to know whether readings are real. An
				 * ERROR reading is also nearly invisible on the
				 * wire: with every measurement absent it encodes
				 * to about ten bytes rather than twenty-five, so
				 * "the telemetry looks short" is the only other
				 * clue you would get. */
				if (hb_state.state != was) {
					if (hb_state.state == HEARTBEAT_STATE_SENSOR_ERROR) {
						LOG_ERR("sensor reads are failing — telemetry "
							"carries no measurements");
					} else {
						LOG_INF("sensor readings OK (seq %u)",
							reading.sequence);
					}
				}

				/* The expensive one, and so the one that gets
				 * backed off: a failed isotp_send() costs a full
				 * ISOTP_BS_TIMEOUT of this thread, which is a
				 * second of not answering commands and not
				 * beating on time. Attempting that every sample
				 * against an absent gateway is how the loop
				 * loses its clock. */
				if (bus_transmittable() &&
				    (link_up || k_uptime_get() >= tx_retry_at)) {
					int rc = send_telemetry(reading);

					note_link(rc == ISOTP_N_OK, "telemetry", rc);
				}
			}
		}
	}

	return 0;
}
