/* Wire-format encode/decode. See protocol.h for what this file is for and why
 * it has no MQTT, socket, zbus or device dependency. */
#include "protocol.h"

#include <zephyr/logging/log.h>

#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>

LOG_MODULE_REGISTER(protocol, LOG_LEVEL_INF);

size_t encode_telemetry(const struct sensor_reading &reading, uint8_t *out, size_t out_len)
{
	node_Telemetry msg = node_Telemetry_init_zero;

	msg.schema_version = kSchemaVersion;
	msg.sequence = reading.sequence;
	msg.uptime_ms = reading.uptime_ms;
	msg.co2_ppm = reading.co2_ppm;
	msg.temperature_c = reading.temperature_c;
	msg.humidity_rh = reading.humidity_rh;

	/* The internal enum and the wire enum are mapped explicitly rather than
	 * cast. The cost is this switch; the benefit is that the two can be
	 * renumbered independently, and the compiler flags the mapping when
	 * either gains a value. */
	switch (reading.status) {
	case SENSOR_READING_OK:
		msg.sensor_status = node_SensorStatus_SENSOR_STATUS_OK;
		break;
	case SENSOR_READING_WARMING_UP:
		msg.sensor_status = node_SensorStatus_SENSOR_STATUS_WARMING_UP;
		break;
	case SENSOR_READING_ERROR:
	default:
		msg.sensor_status = node_SensorStatus_SENSOR_STATUS_ERROR;
		break;
	}

	/* An output stream writing into a caller-supplied buffer. nanopb never
	 * allocates: if the message does not fit, encoding fails rather than
	 * growing anything. node_Telemetry_size (36) is the generated upper
	 * bound, so a correctly sized buffer cannot overflow here. */
	pb_ostream_t stream = pb_ostream_from_buffer(out, out_len);

	if (!pb_encode(&stream, node_Telemetry_fields, &msg)) {
		LOG_ERR("telemetry encode failed: %s", PB_GET_ERROR(&stream));
		return 0;
	}
	return stream.bytes_written;
}

size_t encode_ack(uint32_t seq, node_AckStatus status, const char *detail,
		  const node_DeviceInfo *info, uint8_t *out, size_t out_len)
{
	node_Ack ack = node_Ack_init_zero;

	ack.schema_version = kSchemaVersion;
	ack.sequence = seq;
	ack.status = status;

	if (detail != nullptr) {
		/* Bounded by max_size:48 in node.options. Truncating the
		 * diagnostic is fine; hosts branch on `status`, not this text. */
		strncpy(ack.detail, detail, sizeof(ack.detail) - 1);
	}
	if (info != nullptr) {
		ack.has_device_info = true;
		ack.device_info = *info;
	}

	pb_ostream_t stream = pb_ostream_from_buffer(out, out_len);

	if (!pb_encode(&stream, node_Ack_fields, &ack)) {
		LOG_ERR("ack encode failed: %s", PB_GET_ERROR(&stream));
		return 0;
	}
	return stream.bytes_written;
}

bool decode_command(const uint8_t *buf, size_t len, bool oversized, node_Command *out)
{
	*out = node_Command_init_zero;

	if (oversized) {
		LOG_ERR("command decode failed: payload too large");
		return false;
	}

	pb_istream_t stream = pb_istream_from_buffer(buf, len);

	if (!pb_decode(&stream, node_Command_fields, out)) {
		LOG_ERR("command decode failed: %s", PB_GET_ERROR(&stream));
		*out = node_Command_init_zero;
		return false;
	}
	return true;
}
