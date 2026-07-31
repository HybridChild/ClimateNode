# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(scd40_read)

# Generate node.pb.c/.h from the shared contract in proto/ at build time, so the
# firmware can never drift from the schema. The generated code is C (nanopb
# emits extern "C" guards, so including it from C++ is fine) and lives in the
# build directory -- it is never checked in and never hand-edited.
#
# The .options file sitting next to node.proto is picked up automatically by the
# nanopb generator; it is what keeps the generated structs fixed-size.
list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
include(nanopb)

zephyr_nanopb_sources(app ${CMAKE_CURRENT_SOURCE_DIR}/../proto/node.proto)

set(SHARED ${CMAKE_CURRENT_SOURCE_DIR}/../shared)

# One translation unit per responsibility. Four are this application's own; two
# come from shared/, compiled by path rather than copied, because the peer node
# links the very same files -- one definition, no drift. protocol.cpp and
# commands.cpp carry no socket or device dependency, which is what also lets
# tests/ compile them on their own -- see notes/testing-guide.md.
target_sources(app PRIVATE
  src/main.cpp         # MQTT sessions: connect, poll, publish, reconnect
  src/sensor.cpp       # SCD-40 thread: acquisition, sample period, bounds
  src/relay.cpp        # the CAN side: heartbeat liveness, ISO-TP both ways
  ${SHARED}/protocol.cpp  # shared: the wire format, internal types <-> protobuf
  ${SHARED}/commands.cpp  # shared: dispatch, dedupe, node identity
)

# app_channels.h, can_link.h, commands.h and protocol.h all live in shared/. The
# threads above meet on the zbus channels declared in shared/app_channels.h;
# relay.cpp and main.cpp also meet on the ones in src/relay.h, which stays here
# because only the gateway relays.
target_include_directories(app PRIVATE ${SHARED})
