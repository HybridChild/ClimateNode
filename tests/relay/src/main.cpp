/* Unit tests for the relay's liveness state machine — gateway/src/relay.h.
 *
 * On MQTT, liveness is something you subscribe to: the broker notices a dead
 * client and publishes its Last Will. On CAN there is no broker and no
 * connection, so a silent node and an absent node are the same thing
 * (notes/can-guide.md §1) and the gateway has to *decide*. This file tests the
 * deciding.
 *
 * Everything here runs with no CAN controller, no thread, and no passage of
 * time, because liveness_step() takes the clock as an argument rather than
 * reading it. That is not a testing seam bolted on afterwards — it is the same
 * shape as sensor_cmd_in_range() in app_channels.h, where the *rule* is an
 * ordinary function and only its caller touches hardware. Being able to
 * fast-forward three hours in a nanosecond is the reward for that shape.
 */
#include <zephyr/ztest.h>

#include "can_link.h" /* kHeartbeatTimeoutMs — the real number, not a copy */
#include "relay.h"

/* The real timeout, so these tests fail if someone changes it without thinking
 * about what depends on it. */
constexpr int kTimeout = kHeartbeatTimeoutMs; /* 3500 */

ZTEST_SUITE(relay, NULL, NULL, NULL, NULL, NULL);

/* A gateway that has just started listening at t = `now`. */
static struct liveness fresh(int64_t now)
{
	struct liveness l = {PEER_UNKNOWN, now};

	return l;
}

/* ---- the boot window ------------------------------------------------------ */

ZTEST(relay, test_boot_state_publishes_nothing)
{
	/* The single most important case, and the reason PEER_UNKNOWN exists as
	 * a state distinct from PEER_OFFLINE.
	 *
	 * `status` is a RETAINED topic. If a gateway restart published `offline`
	 * for a peer that was in fact fine, that lie would sit on the broker
	 * until the next heartbeat corrected it — and every host connecting in
	 * between would be told the peer is dead. So until there is evidence
	 * either way, the gateway says nothing at all. */
	struct liveness l = fresh(1000);

	for (int64_t t = 1000; t < 1000 + kTimeout; t += 100) {
		zassert_false(liveness_step(&l, false, t, kTimeout),
			      "published a state change at t=%lld with no evidence", t);
	}
	zassert_equal(l.state, PEER_UNKNOWN, "state should still be UNKNOWN");
}

ZTEST(relay, test_silence_from_boot_eventually_reports_offline)
{
	/* The other half: silence is not evidence *forever*. Once a full timeout
	 * has passed since we started listening, "no heartbeat has ever arrived"
	 * is a conclusion rather than an absence of one. */
	struct liveness l = fresh(1000);

	zassert_false(liveness_step(&l, false, 1000 + kTimeout - 1, kTimeout),
		      "declared offline one millisecond early");
	zassert_true(liveness_step(&l, false, 1000 + kTimeout, kTimeout),
		     "never declared offline");
	zassert_equal(l.state, PEER_OFFLINE, "state should be OFFLINE");
}

ZTEST(relay, test_first_heartbeat_reports_online)
{
	struct liveness l = fresh(1000);

	zassert_true(liveness_step(&l, true, 1100, kTimeout), "first heartbeat did not publish");
	zassert_equal(l.state, PEER_ONLINE, "state should be ONLINE");
	zassert_equal(l.last_seen_ms, 1100, "last_seen not updated");
}

/* ---- transitions only ----------------------------------------------------- */

ZTEST(relay, test_steady_heartbeats_publish_once)
{
	/* A peer beats at 1 Hz forever. The gateway must publish `online` once,
	 * not 86 400 times a day. Retained-topic churn is not free: every
	 * publish is a QoS 1 round trip and a broker write, and a host watching
	 * node/2/status wants to see events, not a metronome. */
	struct liveness l = fresh(0);
	int publishes = 0;

	for (int64_t t = 1000; t <= 60000; t += 1000) {
		if (liveness_step(&l, true, t, kTimeout)) {
			publishes++;
		}
	}
	zassert_equal(publishes, 1, "sixty heartbeats produced %d publishes, expected 1",
		      publishes);
}

ZTEST(relay, test_continued_silence_publishes_once)
{
	/* Symmetrically: a peer that is gone should produce one `offline`, not
	 * one every 250 ms for as long as it stays gone — and the RX thread
	 * calls this every 250 ms by design. */
	struct liveness l = fresh(0);

	liveness_step(&l, true, 1000, kTimeout); /* online */

	int publishes = 0;

	for (int64_t t = 1250; t <= 60000; t += 250) {
		if (liveness_step(&l, false, t, kTimeout)) {
			publishes++;
		}
	}
	zassert_equal(publishes, 1, "sustained silence produced %d publishes, expected 1",
		      publishes);
	zassert_equal(l.state, PEER_OFFLINE, "state should be OFFLINE");
}

/* ---- the timeout itself --------------------------------------------------- */

ZTEST(relay, test_timeout_is_measured_from_the_last_beat)
{
	/* Not from boot, and not from the last *step*. A peer that beat at
	 * t=10000 is not late until t=13500, however many times the RX thread
	 * woke up in between. */
	struct liveness l = fresh(0);

	liveness_step(&l, true, 10000, kTimeout);

	zassert_false(liveness_step(&l, false, 10000 + kTimeout - 1, kTimeout),
		      "declared offline before the timeout elapsed");
	zassert_true(liveness_step(&l, false, 10000 + kTimeout, kTimeout),
		     "did not declare offline at the timeout");
}

ZTEST(relay, test_three_lost_beats_are_tolerated)
{
	/* The ratio in can_link.h is 3500 ms against a 1000 ms beat, and this is
	 * what that buys: three consecutive lost frames without a false alarm.
	 * CAN retransmits automatically on error, so isolated losses are normal
	 * on a noisy bus and must not read as a dead node. */
	struct liveness l = fresh(0);

	liveness_step(&l, true, 1000, kTimeout);

	/* Beats at 2000, 3000, 4000 all go missing; the fourth arrives. */
	for (int64_t t = 2000; t <= 4000; t += 1000) {
		zassert_false(liveness_step(&l, false, t, kTimeout),
			      "a lost beat at t=%lld was treated as death", t);
	}
	zassert_false(liveness_step(&l, true, 4400, kTimeout),
		      "recovery within the window should not republish online");
	zassert_equal(l.state, PEER_ONLINE, "peer should still be considered online");
}

ZTEST(relay, test_recovery_after_offline_publishes_online)
{
	/* A peer that was declared dead and comes back must be announced again —
	 * the retained topic still says `offline`, and nothing else will correct
	 * it. This is the round trip the bench lab exercises by power-cycling
	 * the peer. */
	struct liveness l = fresh(0);

	liveness_step(&l, true, 1000, kTimeout);
	zassert_true(liveness_step(&l, false, 1000 + kTimeout, kTimeout), "should go offline");

	zassert_true(liveness_step(&l, true, 20000, kTimeout), "recovery was not announced");
	zassert_equal(l.state, PEER_ONLINE, "state should be ONLINE again");
}

ZTEST(relay, test_heartbeat_wins_over_an_expired_clock)
{
	/* Both inputs point opposite ways in the same call: the beat is older
	 * than the timeout, but a beat *did* arrive. Evidence beats inference,
	 * because the frame is a fact and the timeout is only a guess about what
	 * its absence meant. */
	struct liveness l = fresh(0);

	liveness_step(&l, true, 1000, kTimeout);

	zassert_false(liveness_step(&l, true, 1000 + kTimeout * 3, kTimeout),
		      "a heartbeat should keep the peer online, not flip it offline");
	zassert_equal(l.state, PEER_ONLINE, "state should be ONLINE");
}

/* ---- the message budget --------------------------------------------------- */

ZTEST(relay, test_relay_message_sizes)
{
	/* The asymmetry relay.h argues for, as an assertion. If relay_down ever
	 * grew past relay_up's neighbourhood the RAM argument would silently
	 * stop holding, because the message-subscriber pool is sized by the
	 * largest message on any such channel.
	 *
	 * The upper bound on relay_up against node_Ack_size is NOT checked here:
	 * that needs the generated header, and relay.cpp static_asserts it in
	 * the one translation unit allowed to include it. */
	zassert_true(sizeof(struct relay_down) <= 64,
		     "relay_down grew to %zu B — the msg-subscriber pool is sized by it",
		     sizeof(struct relay_down));
	zassert_true(sizeof(struct relay_up) > sizeof(struct relay_down),
		     "the asymmetry relay.h relies on has been lost");
	zassert_equal(sizeof(struct relay_status), 2,
		      "relay_status should be two bytes; it carries no payload");
}
