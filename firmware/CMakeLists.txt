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

# One translation unit per responsibility. protocol.cpp and commands.cpp carry
# no socket or device dependency, which is what lets tests/ compile them on
# their own for the host-side suite -- see notes/testing-guide.md.
target_sources(app PRIVATE
  src/main.cpp      # MQTT session: connect, poll, publish, reconnect
  src/sensor.cpp    # SCD-40 thread: acquisition, sample period, bounds
  src/protocol.cpp  # the wire format: internal types <-> protobuf
  src/commands.cpp  # command semantics: dispatch, dedupe, node identity
  src/relay.cpp     # the CAN side: heartbeat liveness, ISO-TP both ways
                    # sensor.cpp and main.cpp meet on the zbus channels
                    # declared in src/app_channels.h
)
