# SPDX-License-Identifier: Apache-2.0
#
# Host-side unit tests for the wire format (shared/protocol.cpp).
#
# The test compiles the shared source itself, not a copy -- the same rule the
# rest of the repo follows for generated code: one definition, no drift. Both
# applications link this same file, so a test that passes here is a test of what
# the gateway and the peer node actually run.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(protocol_test)

set(SHARED ${CMAKE_CURRENT_SOURCE_DIR}/../../shared)

# The same generator line both applications use, so the test builds against the
# real schema in proto/ rather than a checked-in snapshot of it.
list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
include(nanopb)
zephyr_nanopb_sources(app ${CMAKE_CURRENT_SOURCE_DIR}/../../proto/node.proto)

target_sources(app PRIVATE src/main.cpp ${SHARED}/protocol.cpp)
target_include_directories(app PRIVATE ${SHARED})
