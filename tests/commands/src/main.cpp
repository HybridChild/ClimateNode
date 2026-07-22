/* Unit tests for firmware/src/commands.cpp -- what a Command means.
 *
 * The sensor thread does not exist here. commands.cpp publishes to
 * chan_sensor_cmd, which app_channels.h only DECLARES; this file supplies the
 * definition, so the tests observe exactly what the sensor thread would have
 * received. The validator is the production rule (sensor_cmd_in_range), not a
 * stand-in -- so the -ENOMSG rejection path is the real one.
 *
 * See notes/testing-guide.md.
 */
#include <zephyr/ztest.h>
#include <zephyr/zbus/zbus.h>

#include <string.h>

#include "app_channels.h"
#include "commands.h"

/* The zbus adapter around the shared rule. sensor.cpp has the same two lines;
 * what must not be duplicated is the *rule*, and that lives in one place. */
static bool test_cmd_valid(const void *msg, size_t msg_size)
{
	ARG_UNUSED(msg_size);
	return sensor_cmd_in_range(static_cast<const struct sensor_cmd *>(msg));
}

/* At global scope, outside any anonymous namespace: commands.cpp refers to this
 * symbol by name through ZBUS_CHAN_DECLARE, and internal linkage would stop the
 * two from meeting. notes/language-cpp.md §6. */
ZBUS_CHAN_DEFINE(chan_sensor_cmd, struct sensor_cmd, test_cmd_valid, nullptr,
		 ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(SENSOR_CMD_TRIGGER, 0));

/* commands.cpp remembers the last sequence number it saw, and that state
 * outlives an individual test. Rather than adding a reset function to the
 * production API purely for the tests, each case takes a fresh sequence; the
 * duplicate test reuses one deliberately. */
static uint32_t next_seq(void)
{
	static uint32_t seq = 1000;

	return ++seq;
}

static node_Command make_command(uint32_t seq, pb_size_t tag)
{
	node_Command cmd = node_Command_init_zero;

	cmd.schema_version = 1;
	cmd.sequence = seq;
	cmd.which_payload = tag;
	return cmd;
}

/* Put the channel in a known state so "unchanged" means something. */
static void seed_channel(uint32_t interval_ms)
{
	struct sensor_cmd sc = {};

	sc.kind = SENSOR_CMD_SET_INTERVAL;
	sc.interval_ms = interval_ms;
	zassert_ok(zbus_chan_pub(&chan_sensor_cmd, &sc, K_MSEC(100)), "seeding the channel failed");
}

static struct sensor_cmd read_channel(void)
{
	struct sensor_cmd sc = {};

	zassert_ok(zbus_chan_read(&chan_sensor_cmd, &sc, K_MSEC(100)), "reading the channel failed");
	return sc;
}

ZTEST_SUITE(commands, NULL, NULL, NULL, NULL, NULL);

/* ---- dispatch ------------------------------------------------------------- */

ZTEST(commands, test_set_interval_forwards_to_the_bus)
{
	seed_channel(5000);

	node_Command cmd = make_command(next_seq(), node_Command_set_interval_tag);

	cmd.payload.set_interval.interval_ms = 2000;

	struct command_result result;

	handle_command(cmd, &result);

	zassert_equal(result.status, node_AckStatus_ACK_STATUS_OK, "in-range interval was rejected");
	zassert_false(result.has_info, "set_interval should not report device info");

	struct sensor_cmd sc = read_channel();

	zassert_equal(sc.kind, SENSOR_CMD_SET_INTERVAL, "wrong command kind reached the bus");
	zassert_equal(sc.interval_ms, 2000, "wrong interval reached the bus");
}

ZTEST(commands, test_trigger_forwards_to_the_bus)
{
	seed_channel(5000);

	node_Command cmd = make_command(next_seq(), node_Command_trigger_measurement_tag);
	struct command_result result;

	handle_command(cmd, &result);

	zassert_equal(result.status, node_AckStatus_ACK_STATUS_OK, "trigger was rejected");
	zassert_equal(read_channel().kind, SENSOR_CMD_TRIGGER, "trigger did not reach the bus");
}

ZTEST(commands, test_get_device_info_reports_identity)
{
	node_Command cmd = make_command(next_seq(), node_Command_get_device_info_tag);
	struct command_result result;

	handle_command(cmd, &result);

	zassert_equal(result.status, node_AckStatus_ACK_STATUS_OK, "info request was rejected");
	zassert_true(result.has_info, "info request did not attach device info");
	zassert_str_equal(result.info.firmware_version, kFirmwareVersion, "wrong firmware version");
	zassert_str_equal(result.info.board, kBoardName, "wrong board name");
	zassert_str_equal(result.info.client_id, kNodeClientId, "wrong client id");
}

ZTEST(commands, test_unknown_arm_is_unsupported)
{
	/* A oneof arm this firmware has no handler for: the host is newer than
	 * the node. The protocol decoded it perfectly -- the gap is in our
	 * handlers, and UNSUPPORTED is what says so. */
	node_Command cmd = make_command(next_seq(), 99);
	struct command_result result;

	handle_command(cmd, &result);

	zassert_equal(result.status, node_AckStatus_ACK_STATUS_UNSUPPORTED,
		      "unknown arm should be UNSUPPORTED");
	zassert_not_null(strstr(result.detail, "99"), "detail should name the tag, got '%s'",
			 result.detail);
}

ZTEST(commands, test_no_arm_set_is_unsupported)
{
	/* which_payload == 0 is what a payload that was never a Command decodes
	 * to -- the case protocol.cpp cannot detect, because the type is not on
	 * the wire. This is where the node survives it. */
	node_Command cmd = make_command(next_seq(), 0);
	struct command_result result;

	handle_command(cmd, &result);

	zassert_equal(result.status, node_AckStatus_ACK_STATUS_UNSUPPORTED,
		      "a command with no arm set should be UNSUPPORTED");
}

/* ---- the validator, through the real bus ---------------------------------- */

ZTEST(commands, test_out_of_range_interval_is_rejected_atomically)
{
	/* zbus_chan_pub() returns -ENOMSG when the validator refuses, and the
	 * message is never stored -- so the rejection is atomic, not a partial
	 * application that was undone. Asserting the channel is untouched is
	 * what makes that a fact rather than a claim (notes/zbus-guide.md §6). */
	seed_channel(5000);

	node_Command cmd = make_command(next_seq(), node_Command_set_interval_tag);

	cmd.payload.set_interval.interval_ms = SAMPLE_PERIOD_MIN_MS - 1;

	struct command_result result;

	handle_command(cmd, &result);

	zassert_equal(result.status, node_AckStatus_ACK_STATUS_INVALID_ARGUMENT,
		      "below-minimum interval should be rejected");
	zassert_not_null(strstr(result.detail, "outside"), "detail should state the bounds, got '%s'",
			 result.detail);

	struct sensor_cmd sc = read_channel();

	zassert_equal(sc.interval_ms, 5000, "a rejected publish must leave the channel unchanged");
}

ZTEST(commands, test_interval_bounds_are_inclusive)
{
	const struct {
		uint32_t interval;
		bool accepted;
	} cases[] = {
		{SAMPLE_PERIOD_MIN_MS - 1, false},
		{SAMPLE_PERIOD_MIN_MS, true},
		{SAMPLE_PERIOD_DEFAULT_MS, true},
		{SAMPLE_PERIOD_MAX_MS, true},
		{SAMPLE_PERIOD_MAX_MS + 1, false},
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		node_Command cmd = make_command(next_seq(), node_Command_set_interval_tag);

		cmd.payload.set_interval.interval_ms = cases[i].interval;

		struct command_result result;

		handle_command(cmd, &result);

		node_AckStatus want = cases[i].accepted
					      ? node_AckStatus_ACK_STATUS_OK
					      : node_AckStatus_ACK_STATUS_INVALID_ARGUMENT;

		zassert_equal(result.status, want, "interval %u: expected %d, got %d",
			      cases[i].interval, want, result.status);
	}
}

/* ---- duplicate suppression ------------------------------------------------ */

ZTEST(commands, test_duplicate_sequence_skips_the_side_effect)
{
	/* QoS 1 is at-least-once, so a lost PUBACK makes the broker redeliver.
	 * Re-running "set interval" would be harmless; re-running "trigger" is
	 * not. The host still gets an Ack -- it just does not happen twice. */
	uint32_t seq = next_seq();

	seed_channel(5000);

	node_Command first = make_command(seq, node_Command_set_interval_tag);

	first.payload.set_interval.interval_ms = 2000;

	struct command_result result;

	handle_command(first, &result);
	zassert_equal(result.status, node_AckStatus_ACK_STATUS_OK, "first delivery failed");
	zassert_equal(read_channel().interval_ms, 2000, "first delivery did not take effect");

	/* Same sequence, different payload: if the side effect ran, the channel
	 * would now say 3000. */
	node_Command repeat = make_command(seq, node_Command_set_interval_tag);

	repeat.payload.set_interval.interval_ms = 3000;

	handle_command(repeat, &result);

	zassert_equal(result.status, node_AckStatus_ACK_STATUS_OK, "duplicate should still ack OK");
	zassert_str_equal(result.detail, "duplicate ignored", "duplicate should say so");
	zassert_equal(read_channel().interval_ms, 2000,
		      "duplicate command was executed a second time");
}

ZTEST(commands, test_new_sequence_after_a_duplicate_still_acts)
{
	uint32_t seq = next_seq();

	seed_channel(5000);

	node_Command cmd = make_command(seq, node_Command_set_interval_tag);

	cmd.payload.set_interval.interval_ms = 2000;

	struct command_result result;

	handle_command(cmd, &result);
	handle_command(cmd, &result); /* suppressed */

	node_Command fresh = make_command(next_seq(), node_Command_set_interval_tag);

	fresh.payload.set_interval.interval_ms = 4000;

	handle_command(fresh, &result);

	zassert_equal(result.status, node_AckStatus_ACK_STATUS_OK, "fresh command was rejected");
	zassert_equal(read_channel().interval_ms, 4000,
		      "suppression must not latch: a new sequence has to act");
}
