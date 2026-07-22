/* Command semantics: what this node *does* when a Command arrives.
 *
 * Deliberately separate from both neighbours. protocol.cpp turns bytes into a
 * node_Command and knows nothing about what the fields mean; main.cpp owns the
 * MQTT session and knows nothing about command semantics beyond "decode it,
 * handle it, ack the result". This file is the middle: dispatch, duplicate
 * suppression, this node's identity, and forwarding to the sensor thread.
 *
 * It depends on zbus and protocol, never on MQTT or sockets -- which is what
 * lets tests/commands/ link it against a test-owned chan_sensor_cmd and
 * exercise the real rejection path. See notes/testing-guide.md.
 */
#ifndef COMMANDS_H_
#define COMMANDS_H_

#include <node.pb.h>

/* Who this node is. Reported by GetDeviceInfo, and kNodeClientId doubles as the
 * MQTT client id main.cpp connects with -- one identity, named once. The bounds
 * are the max_size values in proto/node.options; strings longer than those are
 * truncated when encoded. */
constexpr const char *kFirmwareVersion = "0.5.0";
constexpr const char *kBoardName = CONFIG_BOARD;
constexpr const char *kNodeClientId = "nucleo-1";

/* What happened, not what to do: handle_command() performs its own side effects
 * and reports the outcome. `detail` is sized to the max_size of Ack.detail. */
struct command_result {
	node_AckStatus status;
	char detail[48];
	bool has_info;
	node_DeviceInfo info;
};

/* Execute a decoded command and fill *out with the Ack-worthy outcome.
 *
 * Duplicate suppression lives here rather than in the transport: QoS 1 is
 * at-least-once, so a redelivered command must not be executed twice. A repeat
 * of the previous sequence number is reported OK (the host still gets its Ack)
 * with the side effect skipped. */
void handle_command(const node_Command &cmd, struct command_result *out);

#endif /* COMMANDS_H_ */
