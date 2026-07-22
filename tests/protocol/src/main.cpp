/* Unit tests for firmware/src/protocol.cpp -- the wire format.
 *
 * Everything here runs with no hardware: the code under test has no socket,
 * device or bus dependency, which is the whole reason it is its own translation
 * unit. See notes/testing-guide.md.
 *
 * These test *behaviour on the wire*, not implementation: what a host receives,
 * and what this node accepts. Several of them are claims from
 * notes/protobuf-guide.md turned into assertions, so the guide and the firmware
 * cannot drift apart silently.
 */
#include <zephyr/ztest.h>

#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h> /* the oversize test builds a valid Command as a fixture */

#include "protocol.h"

/* A reading with nothing at a default value, so every field must appear on the
 * wire. Values match the worked example in notes/protobuf-guide.md §3. */
static struct sensor_reading full_reading(void)
{
	struct sensor_reading r = {};

	r.sequence = 26404;
	r.uptime_ms = 136453275;
	r.co2_ppm = 812;
	r.temperature_c = 22.41f;
	r.humidity_rh = 41.3f;
	r.status = SENSOR_READING_OK;
	return r;
}

static node_Telemetry decode_telemetry(const uint8_t *buf, size_t len)
{
	node_Telemetry msg = node_Telemetry_init_zero;
	pb_istream_t stream = pb_istream_from_buffer(buf, len);

	zassert_true(pb_decode(&stream, node_Telemetry_fields, &msg), "telemetry decode failed");
	return msg;
}

/* ---- telemetry ------------------------------------------------------------ */

ZTEST_SUITE(protocol, NULL, NULL, NULL, NULL, NULL);

ZTEST(protocol, test_telemetry_round_trip)
{
	uint8_t buf[node_Telemetry_size];
	struct sensor_reading r = full_reading();

	size_t len = encode_telemetry(r, buf, sizeof(buf));

	zassert_not_equal(len, 0, "encode reported failure");

	node_Telemetry msg = decode_telemetry(buf, len);

	zassert_equal(msg.schema_version, kSchemaVersion, "schema version not carried");
	zassert_equal(msg.sequence, r.sequence, "sequence not carried");
	zassert_equal(msg.uptime_ms, r.uptime_ms, "uptime not carried");
	zassert_equal(msg.co2_ppm, r.co2_ppm, "co2 not carried");
	/* float32 survives the wire exactly -- protobuf stores the same four
	 * bytes IEEE-754 gave it, so this is an equality, not an approximation. */
	zassert_equal(msg.temperature_c, r.temperature_c, "temperature not carried");
	zassert_equal(msg.humidity_rh, r.humidity_rh, "humidity not carried");
	zassert_equal(msg.sensor_status, node_SensorStatus_SENSOR_STATUS_OK, "status not carried");
}

ZTEST(protocol, test_telemetry_status_mapping)
{
	/* The internal enum and the wire enum are mapped explicitly so they can
	 * be renumbered independently. That mapping is worth pinning: a wrong
	 * one would be invisible on this end and wrong on the host's. */
	const struct {
		enum sensor_reading_status internal;
		node_SensorStatus wire;
	} cases[] = {
		{SENSOR_READING_OK, node_SensorStatus_SENSOR_STATUS_OK},
		{SENSOR_READING_WARMING_UP, node_SensorStatus_SENSOR_STATUS_WARMING_UP},
		{SENSOR_READING_ERROR, node_SensorStatus_SENSOR_STATUS_ERROR},
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		uint8_t buf[node_Telemetry_size];
		struct sensor_reading r = full_reading();

		r.status = cases[i].internal;

		size_t len = encode_telemetry(r, buf, sizeof(buf));
		node_Telemetry msg = decode_telemetry(buf, len);

		zassert_equal(msg.sensor_status, cases[i].wire, "case %zu mapped wrong", i);
	}
}

ZTEST(protocol, test_telemetry_omits_default_fields)
{
	/* proto3 does not transmit fields equal to their default, so a reading
	 * with nothing measured yet is *shorter* on the wire than a full one --
	 * a telemetry message is smallest exactly when it has least to say.
	 * notes/protobuf-guide.md §5. */
	uint8_t full_buf[node_Telemetry_size];
	uint8_t warm_buf[node_Telemetry_size];

	struct sensor_reading warm = {};

	warm.sequence = 1;
	warm.uptime_ms = 6000;
	warm.status = SENSOR_READING_WARMING_UP;

	size_t full_len = encode_telemetry(full_reading(), full_buf, sizeof(full_buf));
	size_t warm_len = encode_telemetry(warm, warm_buf, sizeof(warm_buf));

	zassert_true(warm_len < full_len, "warming-up reading (%zu B) not shorter than full (%zu B)",
		     warm_len, full_len);

	/* And the three zeroed measurements come back as zero, not as garbage:
	 * absent and zero are indistinguishable for scalars, by design. */
	node_Telemetry msg = decode_telemetry(warm_buf, warm_len);

	zassert_equal(msg.co2_ppm, 0, "absent co2 did not default to 0");
	zassert_equal(msg.temperature_c, 0.0f, "absent temperature did not default to 0");
	zassert_equal(msg.humidity_rh, 0.0f, "absent humidity did not default to 0");
}

ZTEST(protocol, test_telemetry_fits_generated_size)
{
	/* node_Telemetry_size is nanopb's worst case. The firmware sizes its
	 * publish buffer from it, so if a schema change ever made a real message
	 * exceed it, that buffer would be wrong -- this pins the relationship. */
	uint8_t buf[node_Telemetry_size];
	struct sensor_reading r = {};

	r.sequence = UINT32_MAX;
	r.uptime_ms = UINT32_MAX;
	r.co2_ppm = UINT32_MAX;
	r.temperature_c = -273.15f;
	r.humidity_rh = 100.0f;
	r.status = SENSOR_READING_ERROR;

	size_t len = encode_telemetry(r, buf, sizeof(buf));

	zassert_not_equal(len, 0, "worst-case reading failed to encode");
	zassert_true(len <= node_Telemetry_size, "worst case %zu B exceeds node_Telemetry_size %d",
		     len, node_Telemetry_size);
}

ZTEST(protocol, test_telemetry_rejects_undersized_buffer)
{
	/* nanopb never allocates: a message that does not fit fails to encode
	 * rather than growing anything or overrunning the buffer. */
	uint8_t tiny[4];

	zassert_equal(encode_telemetry(full_reading(), tiny, sizeof(tiny)), 0,
		      "encoding into a 4-byte buffer should have failed");
}

/* ---- command decode ------------------------------------------------------- */

ZTEST(protocol, test_decode_rejects_malformed)
{
	/* "garbage": the first byte is 0x67 -> field 12, wire type 7, and 7 is
	 * not a valid wire type. The decoder rejects it before reading further. */
	const uint8_t garbage[] = {'g', 'a', 'r', 'b', 'a', 'g', 'e'};
	node_Command cmd;

	zassert_false(decode_command(garbage, sizeof(garbage), false, &cmd),
		      "structurally invalid payload was accepted");
}

ZTEST(protocol, test_decode_accepts_structurally_legal_nonsense)
{
	/* The other half of the lesson, and the more important one: "hi" is
	 * 0x68 0x69 -> field 13, wire type 0, varint 105. Field 13 is not in the
	 * schema, so it is skipped as an unknown field and the decode SUCCEEDS,
	 * yielding a Command with every field defaulted.
	 *
	 * Nothing about the encoding can catch this -- the type is never on the
	 * wire. What saves the node is which_payload == 0 hitting the default
	 * arm in commands.cpp. notes/protobuf-guide.md §4 and §8. */
	const uint8_t hi[] = {'h', 'i'};
	node_Command cmd;

	zassert_true(decode_command(hi, sizeof(hi), false, &cmd),
		      "structurally legal bytes should decode");
	zassert_equal(cmd.which_payload, 0, "no oneof arm should be set");
	zassert_equal(cmd.sequence, 0, "sequence should default to 0");
	zassert_equal(cmd.schema_version, 0, "schema_version should default to 0");
}

ZTEST(protocol, test_decode_rejects_oversized)
{
	/* A truncated payload can decode into something plausible, so oversize
	 * is refused outright rather than acted on in part -- even when the
	 * bytes we did keep are a perfectly valid command. */
	uint8_t buf[node_Command_size];
	node_Command cmd = node_Command_init_zero;

	cmd.schema_version = kSchemaVersion;
	cmd.sequence = 7;
	cmd.which_payload = node_Command_trigger_measurement_tag;

	pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));

	zassert_true(pb_encode(&stream, node_Command_fields, &cmd), "fixture encode failed");

	node_Command out;

	zassert_false(decode_command(buf, stream.bytes_written, true, &out),
		      "oversized payload was accepted");
	zassert_equal(out.which_payload, 0, "rejected decode must leave the output zeroed");
}

ZTEST(protocol, test_decode_empty_payload)
{
	/* Zero bytes is a valid Protobuf message: every field defaulted. It has
	 * to be handled as "no arm set", not as an error. */
	node_Command cmd;

	zassert_true(decode_command(nullptr, 0, false, &cmd), "empty payload should decode");
	zassert_equal(cmd.which_payload, 0, "empty payload should set no arm");
}

/* ---- ack ------------------------------------------------------------------ */

static node_Ack decode_ack(const uint8_t *buf, size_t len)
{
	node_Ack ack = node_Ack_init_zero;
	pb_istream_t stream = pb_istream_from_buffer(buf, len);

	zassert_true(pb_decode(&stream, node_Ack_fields, &ack), "ack decode failed");
	return ack;
}

ZTEST(protocol, test_ack_round_trip)
{
	uint8_t buf[node_Ack_size];
	size_t len = encode_ack(42, node_AckStatus_ACK_STATUS_INVALID_ARGUMENT, "out of range",
				nullptr, buf, sizeof(buf));

	zassert_not_equal(len, 0, "ack encode reported failure");

	node_Ack ack = decode_ack(buf, len);

	zassert_equal(ack.schema_version, kSchemaVersion, "schema version not carried");
	zassert_equal(ack.sequence, 42, "sequence not carried");
	zassert_equal(ack.status, node_AckStatus_ACK_STATUS_INVALID_ARGUMENT, "status not carried");
	zassert_str_equal(ack.detail, "out of range", "detail not carried");
	zassert_false(ack.has_device_info, "device_info should be absent");
}

ZTEST(protocol, test_ack_truncates_long_detail)
{
	/* Ack.detail is bounded by max_size:48 in proto/node.options, so nanopb
	 * gives it a fixed 48-byte array -- 47 characters plus the NUL. Longer
	 * diagnostics are truncated rather than rejected, because the host
	 * branches on `status`, not on this text. */
	char detail[80];

	memset(detail, 'x', sizeof(detail));
	detail[sizeof(detail) - 1] = '\0';

	uint8_t buf[node_Ack_size];
	size_t len = encode_ack(1, node_AckStatus_ACK_STATUS_FAILED, detail, nullptr, buf,
				sizeof(buf));

	zassert_not_equal(len, 0, "over-long detail should truncate, not fail");

	node_Ack ack = decode_ack(buf, len);

	zassert_equal(strlen(ack.detail), 47, "detail should be truncated to 47 characters");
}

ZTEST(protocol, test_ack_with_device_info_fits)
{
	/* The largest Ack this node can send: a status, a full-length detail and
	 * the optional DeviceInfo submessage. It must still fit node_Ack_size,
	 * which is what main.cpp sizes its buffer from. */
	node_DeviceInfo info = node_DeviceInfo_init_zero;

	memset(info.firmware_version, 'v', sizeof(info.firmware_version) - 1);
	memset(info.board, 'b', sizeof(info.board) - 1);
	memset(info.client_id, 'c', sizeof(info.client_id) - 1);

	char detail[48];

	memset(detail, 'd', sizeof(detail) - 1);
	detail[sizeof(detail) - 1] = '\0';

	uint8_t buf[node_Ack_size];
	size_t len = encode_ack(UINT32_MAX, node_AckStatus_ACK_STATUS_OK, detail, &info, buf,
				sizeof(buf));

	zassert_not_equal(len, 0, "worst-case ack failed to encode");
	zassert_true(len <= node_Ack_size, "worst case %zu B exceeds node_Ack_size %d", len,
		     node_Ack_size);

	node_Ack ack = decode_ack(buf, len);

	zassert_true(ack.has_device_info, "device_info should be present");
	zassert_equal(strlen(ack.device_info.board), sizeof(info.board) - 1,
		      "board string should survive at its bound");
}
