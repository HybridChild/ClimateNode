/* The wire format: where this firmware's internal types become the protobuf
 * contract in proto/node.proto, and back.
 *
 * This is the *only* translation unit that knows both representations. The
 * sensor side (sensor.cpp) never mentions node_Telemetry; the transport side
 * (main.cpp) never mentions struct sensor_reading beyond handing it here. A
 * schema change therefore stops at this file instead of rippling outwards --
 * which is the argument the header comment in app_channels.h makes for the
 * internal types existing at all.
 *
 * Nothing here touches MQTT, sockets, zbus or a device. That is deliberate: it
 * is what lets tests/protocol/ compile this file on its own and exercise every
 * encode and decode path without hardware. See notes/testing-guide.md.
 *
 * Concepts -- varints, wire types, why nanopb needs a bound on every string --
 * are in notes/protobuf-guide.md; the schema decisions are inline in
 * proto/node.proto.
 */
#ifndef PROTOCOL_H_
#define PROTOCOL_H_

#include <stddef.h>
#include <stdint.h>

#include "app_channels.h"

/* Generated from proto/node.proto at build time. C, with extern "C" guards. */
#include <node.pb.h>

/* Bumped only when a change to proto/node.proto breaks old readers. Additive
 * field changes do not touch it -- protobuf handles those on its own. */
constexpr uint32_t kSchemaVersion = 1;

/* Encode a reading as a Telemetry message. Returns the number of bytes written,
 * or 0 if encoding failed. `out_len` must be at least node_Telemetry_size. */
size_t encode_telemetry(const struct sensor_reading &reading, uint8_t *out, size_t out_len);

/* Encode an Ack. `detail` may be nullptr; longer text is truncated to the
 * max_size in proto/node.options, which is fine because hosts branch on
 * `status`, not on the text. `info` may be nullptr; when set it is attached as
 * the optional DeviceInfo submessage. Returns bytes written, or 0 on failure. */
size_t encode_ack(uint32_t seq, node_AckStatus status, const char *detail,
		  const node_DeviceInfo *info, uint8_t *out, size_t out_len);

/* Decode a Command payload. `oversized` reports that the caller could not keep
 * the whole payload; such a message is rejected outright rather than decoded
 * from a truncated buffer, because a partial message can decode into something
 * plausible and acting on half a command is worse than refusing it.
 *
 * Returns false on either failure, with *out left zero-initialised. Note what a
 * `true` return does NOT promise: that the bytes were meant as a Command. The
 * encoding carries no type identity, so unrelated bytes can decode cleanly into
 * a Command with every field defaulted -- see notes/protobuf-guide.md §4. That
 * case is caught downstream by the which_payload dispatch in commands.cpp. */
bool decode_command(const uint8_t *buf, size_t len, bool oversized, node_Command *out);

#endif /* PROTOCOL_H_ */
