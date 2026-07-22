# SPDX-License-Identifier: Apache-2.0
#
# Host-side unit tests for the wire format (firmware/src/protocol.cpp).
#
# The test compiles the application's own source, not a copy -- the same rule the
# rest of the repo follows for generated code: one definition, no drift.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(protocol_test)

set(APP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware/src)

# The same generator line firmware/CMakeLists.txt uses, so the test builds
# against the real schema in proto/ rather than a checked-in snapshot of it.
list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
include(nanopb)
zephyr_nanopb_sources(app ${CMAKE_CURRENT_SOURCE_DIR}/../../proto/node.proto)

target_sources(app PRIVATE src/main.cpp ${APP_SRC}/protocol.cpp)
target_include_directories(app PRIVATE ${APP_SRC})
