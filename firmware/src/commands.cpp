/* Command dispatch. See commands.h for why this is its own translation unit. */
#include "commands.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "app_channels.h"

LOG_MODULE_REGISTER(commands, LOG_LEVEL_INF);

namespace {

/* QoS 1 is at-least-once, so a redelivered command must not be executed twice.
 * We report OK for duplicates (the host still wants its Ack) but skip the side
 * effect -- re-running "set interval" would be harmless, "trigger" is not. */
bool have_last_command;
uint32_t last_command_sequence;

/* Hand a command to the sensor thread over the bus and translate the result
 * into an Ack status. Bounds checking is the channel validator's job, so this
 * function only reports what the bus told it -- the wire layer never gets a
 * second, drifting copy of the sensor's rules. */
node_AckStatus forward_to_sensor(const struct sensor_cmd &sc, char *detail, size_t detail_len)
{
	int rc = zbus_chan_pub(&chan_sensor_cmd, &sc, K_MSEC(100));

	switch (rc) {
	case 0:
		return node_AckStatus_ACK_STATUS_OK;

	case -ENOMSG:
		/* The validator rejected it: the message never reached the
		 * channel, so nothing was changed. */
		snprintf(detail, detail_len, "interval %u outside [%u,%u]", sc.interval_ms,
			 SAMPLE_PERIOD_MIN_MS, SAMPLE_PERIOD_MAX_MS);
		return node_AckStatus_ACK_STATUS_INVALID_ARGUMENT;

	default:
		/* Channel busy, or the subscriber's queue is full. */
		snprintf(detail, detail_len, "bus publish failed: %d", rc);
		return node_AckStatus_ACK_STATUS_FAILED;
	}
}

/* Execute a decoded command. Returns the status to report, and may fill
 * `detail` and the DeviceInfo half of the result. */
node_AckStatus apply_command(const node_Command &cmd, struct command_result *out)
{
	switch (cmd.which_payload) {
	case node_Command_set_interval_tag: {
		struct sensor_cmd sc = {};

		sc.kind = SENSOR_CMD_SET_INTERVAL;
		sc.interval_ms = cmd.payload.set_interval.interval_ms;
		return forward_to_sensor(sc, out->detail, sizeof(out->detail));
	}

	case node_Command_trigger_measurement_tag: {
		struct sensor_cmd sc = {};

		sc.kind = SENSOR_CMD_TRIGGER;
		return forward_to_sensor(sc, out->detail, sizeof(out->detail));
	}

	case node_Command_get_device_info_tag:
		out->info = node_DeviceInfo_init_zero;
		strncpy(out->info.firmware_version, kFirmwareVersion,
			sizeof(out->info.firmware_version) - 1);
		strncpy(out->info.board, kBoardName, sizeof(out->info.board) - 1);
		strncpy(out->info.client_id, kNodeClientId, sizeof(out->info.client_id) - 1);
		out->has_info = true;
		return node_AckStatus_ACK_STATUS_OK;

	default:
		/* A oneof member this firmware does not know: the host is newer
		 * than the node. Protobuf decoded it fine -- the gap is in our
		 * handlers, which is exactly what UNSUPPORTED reports. This is
		 * also where a payload that was never a Command at all ends up,
		 * since those decode to which_payload == 0. */
		snprintf(out->detail, sizeof(out->detail), "unknown command tag %u",
			 static_cast<unsigned>(cmd.which_payload));
		return node_AckStatus_ACK_STATUS_UNSUPPORTED;
	}
}

}  // namespace

void handle_command(const node_Command &cmd, struct command_result *out)
{
	*out = {};

	LOG_INF("command seq=%u tag=%u schema=%u", cmd.sequence,
		static_cast<unsigned>(cmd.which_payload), cmd.schema_version);

	if (have_last_command && cmd.sequence == last_command_sequence) {
		LOG_WRN("duplicate command seq=%u ignored", cmd.sequence);
		out->status = node_AckStatus_ACK_STATUS_OK;
		strncpy(out->detail, "duplicate ignored", sizeof(out->detail) - 1);
		return;
	}

	out->status = apply_command(cmd, out);

	have_last_command = true;
	last_command_sequence = cmd.sequence;
}
