/* Unit tests for shared/protocol.cpp -- the wire format.
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
 * wire. Values match the worked example in notes/protobuf-guide.md §3. Shaped
 * like this node: three measurements present, no pressure. */
static struct sensor_reading full_reading(void)
{
	struct sensor_reading r = {};

	r.sequence = 26404;
	r.uptime_ms = 136453275;
	r.co2_ppm = 812;
	r.has_co2_ppm = true;
	r.temperature_c = 22.41f;
	r.has_temperature_c = true;
	r.humidity_rh = 41.3f;
	r.has_humidity_rh = true;
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
	zassert_true(msg.has_co2_ppm, "co2 presence not carried");
	zassert_equal(msg.co2_ppm, r.co2_ppm, "co2 not carried");
	/* float32 survives the wire exactly -- protobuf stores the same four
	 * bytes IEEE-754 gave it, so this is an equality, not an approximation. */
	zassert_true(msg.has_temperature_c, "temperature presence not carried");
	zassert_equal(msg.temperature_c, r.temperature_c, "temperature not carried");
	zassert_true(msg.has_humidity_rh, "humidity presence not carried");
	zassert_equal(msg.humidity_rh, r.humidity_rh, "humidity not carried");
	zassert_equal(msg.sensor_status, node_SensorStatus_SENSOR_STATUS_OK, "status not carried");

	/* This node has no barometer, and the absence is on the wire as an
	 * absence -- not as a 0 the host would have to interpret. */
	zassert_false(msg.has_pressure_pa, "a node with no barometer claimed a pressure");
}

ZTEST(protocol, test_telemetry_pressure_only_reading)
{
	/* The mirror image: a node that measures pressure and nothing else. The
	 * same Telemetry message describes both node populations without either
	 * having to send a field it cannot measure, which is what the `optional`
	 * fields in proto/node.proto buy. */
	uint8_t buf[node_Telemetry_size];
	struct sensor_reading r = {};

	r.sequence = 3;
	r.uptime_ms = 91000;
	r.pressure_pa = 101325;
	r.has_pressure_pa = true;
	r.status = SENSOR_READING_OK;

	size_t len = encode_telemetry(r, buf, sizeof(buf));

	zassert_not_equal(len, 0, "encode reported failure");

	node_Telemetry msg = decode_telemetry(buf, len);

	zassert_true(msg.has_pressure_pa, "pressure presence not carried");
	zassert_equal(msg.pressure_pa, 101325, "pressure not carried");
	zassert_false(msg.has_co2_ppm, "co2 should be absent");
	zassert_false(msg.has_temperature_c, "temperature should be absent");
	zassert_false(msg.has_humidity_rh, "humidity should be absent");
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

ZTEST(protocol, test_telemetry_omits_unset_fields)
{
	/* An unset field is not transmitted, so a reading with nothing measured
	 * yet is *shorter* on the wire than a full one -- a telemetry message is
	 * smallest exactly when it has least to say. notes/protobuf-guide.md §5.
	 *
	 * Note which property this rests on, because two different ones would
	 * both produce a shorter message. Without explicit presence it would hold
	 * because every measurement is *zero* and proto3 omits defaults; here it
	 * holds because every measurement is *absent*, which the sensor thread
	 * states deliberately. The next test is the one that separates them. */
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

	/* And what the host sees is *absence*, not three zeros it would have to
	 * interpret. Before explicit presence this decoded to co2 == 0, which is
	 * a plausible-looking CO2 concentration and a false one. */
	node_Telemetry msg = decode_telemetry(warm_buf, warm_len);

	zassert_false(msg.has_co2_ppm, "warming-up reading claimed a co2 measurement");
	zassert_false(msg.has_temperature_c, "warming-up reading claimed a temperature");
	zassert_false(msg.has_humidity_rh, "warming-up reading claimed a humidity");
}

ZTEST(protocol, test_telemetry_transmits_an_explicit_zero)
{
	/* What explicit presence costs, stated as an assertion rather than left
	 * as a footnote. proto3's "a default value is free on the wire" rule
	 * applies to a field that is *unset*; a field explicitly set to its
	 * default is now written out in full, tag and all.
	 *
	 * So the two readings below carry the same numeric information -- zero
	 * degrees -- and the one that means it is the longer of the two. That is
	 * the trade: a few bytes for the ability to distinguish "0 C" from "no
	 * thermometer". notes/protobuf-guide.md §5. */
	uint8_t said_buf[node_Telemetry_size];
	uint8_t unsaid_buf[node_Telemetry_size];

	struct sensor_reading said = {};

	said.sequence = 9;
	said.temperature_c = 0.0f;
	said.has_temperature_c = true; /* measured, and it really is 0 C */
	said.status = SENSOR_READING_OK;

	struct sensor_reading unsaid = said;

	unsaid.has_temperature_c = false; /* no thermometer at all */

	size_t said_len = encode_telemetry(said, said_buf, sizeof(said_buf));
	size_t unsaid_len = encode_telemetry(unsaid, unsaid_buf, sizeof(unsaid_buf));

	zassert_equal(said_len, unsaid_len + 5,
		      "an explicit 0.0f should cost a 1-byte tag plus 4 bytes of float "
		      "(%zu vs %zu)",
		      said_len, unsaid_len);

	/* And the receiver can tell them apart, which is the entire point. */
	zassert_true(decode_telemetry(said_buf, said_len).has_temperature_c,
		     "an explicitly measured 0 C decoded as absent");
	zassert_false(decode_telemetry(unsaid_buf, unsaid_len).has_temperature_c,
		      "an unmeasured temperature decoded as present");
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
	r.has_co2_ppm = true;
	r.temperature_c = -273.15f;
	r.has_temperature_c = true;
	r.humidity_rh = 100.0f;
	r.has_humidity_rh = true;
	/* No node measures all four today, but the bound has to cover the message
	 * the schema permits, not the one this firmware happens to send. */
	r.pressure_pa = UINT32_MAX;
	r.has_pressure_pa = true;
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

/* ---- schema versioning: old readers and old writers ----------------------- */

/* A decoder for the *previous* Telemetry: no `optional` on the measurements, no
 * pressure_pa. Written out by hand with nanopb's X-macro so the old schema is a
 * real, executable participant rather than an assumption -- these two tests are
 * the "add a field, decode old<->new" exercise the README asks for, and neither
 * of them can pass by accident.
 *
 * Note what is NOT redefined here: the field numbers and wire types are
 * identical to the current schema, because that is the whole claim being
 * tested. Adding `optional` and adding field 8 changed what gets written; it
 * did not renumber anything. */
struct OldTelemetry {
	uint32_t schema_version;
	uint32_t sequence;
	uint32_t uptime_ms;
	uint32_t co2_ppm;
	float temperature_c;
	float humidity_rh;
	node_SensorStatus sensor_status;
};

#define OldTelemetry_FIELDLIST(X, a)                                                               \
	X(a, STATIC, SINGULAR, UINT32, schema_version, 1)                                          \
	X(a, STATIC, SINGULAR, UINT32, sequence, 2)                                                \
	X(a, STATIC, SINGULAR, UINT32, uptime_ms, 3)                                               \
	X(a, STATIC, SINGULAR, UINT32, co2_ppm, 4)                                                 \
	X(a, STATIC, SINGULAR, FLOAT, temperature_c, 5)                                            \
	X(a, STATIC, SINGULAR, FLOAT, humidity_rh, 6)                                              \
	X(a, STATIC, SINGULAR, UENUM, sensor_status, 7)
#define OldTelemetry_CALLBACK NULL
#define OldTelemetry_DEFAULT  NULL

PB_BIND(OldTelemetry, OldTelemetry, AUTO)

/* PB_BIND defines the descriptor object; the generator normally emits this
 * alias beside it, and pb_encode/pb_decode want the pointer. */
#define OldTelemetry_fields &OldTelemetry_msg

ZTEST(protocol, test_old_reader_decodes_new_telemetry)
{
	/* Forwards compatibility: a host still running the previous schema is
	 * handed a message from the current firmware, including a pressure_pa it
	 * has never heard of.
	 *
	 * It must not fail. Field 8 arrives as tag 8, wire type 0 -- and an
	 * unknown varint is skippable without knowing anything about it, which is
	 * precisely why protobuf can be extended at all. */
	uint8_t buf[node_Telemetry_size];
	struct sensor_reading r = full_reading();

	r.pressure_pa = 99400;
	r.has_pressure_pa = true;

	size_t len = encode_telemetry(r, buf, sizeof(buf));
	struct OldTelemetry old = {};
	pb_istream_t stream = pb_istream_from_buffer(buf, len);

	zassert_true(pb_decode(&stream, OldTelemetry_fields, &old),
		     "the previous schema failed to decode a current message");

	zassert_equal(old.sequence, r.sequence, "sequence not readable by the old schema");
	zassert_equal(old.co2_ppm, r.co2_ppm, "co2 not readable by the old schema");
	zassert_equal(old.temperature_c, r.temperature_c, "temperature not readable");
	zassert_equal(old.sensor_status, node_SensorStatus_SENSOR_STATUS_OK, "status not readable");

	/* pressure_pa is simply gone: it was skipped, not stored anywhere. The
	 * old host is not wrong about the pressure, it is unaware of it -- which
	 * is the only outcome an old reader can honestly produce. */
}

ZTEST(protocol, test_new_reader_decodes_old_telemetry)
{
	/* Backwards compatibility, and the asymmetry that makes this interesting.
	 * A message written by the previous schema decodes cleanly, and the
	 * has_ bits come out true for what it actually sent.
	 *
	 * But an old writer's ZERO is unrecoverable. Under the old schema a
	 * 0 ppm reading was omitted from the wire, so a new reader sees it as
	 * absent -- and it is right to, because those bytes carry no evidence
	 * either way. Explicit presence can only describe what a sender chose to
	 * state; it cannot retroactively add information to a message that was
	 * written before anyone was asked to state it. */
	uint8_t buf[node_Telemetry_size];
	struct OldTelemetry old = {};

	old.schema_version = kSchemaVersion;
	old.sequence = 12;
	old.uptime_ms = 44000;
	old.co2_ppm = 0; /* the old schema cannot distinguish this from "unmeasured" */
	old.temperature_c = 19.5f;
	old.humidity_rh = 44.0f;
	old.sensor_status = node_SensorStatus_SENSOR_STATUS_OK;

	pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));

	zassert_true(pb_encode(&stream, OldTelemetry_fields, &old), "old-schema encode failed");

	node_Telemetry msg = decode_telemetry(buf, stream.bytes_written);

	zassert_equal(msg.sequence, 12, "sequence not carried across schemas");
	zassert_true(msg.has_temperature_c, "a value the old writer sent decoded as absent");
	zassert_equal(msg.temperature_c, 19.5f, "temperature not carried across schemas");
	zassert_false(msg.has_co2_ppm, "an omitted zero cannot decode as a present zero");
	zassert_false(msg.has_pressure_pa, "the old writer cannot have sent a pressure");
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
