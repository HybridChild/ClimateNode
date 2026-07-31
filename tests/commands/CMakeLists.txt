# SPDX-License-Identifier: Apache-2.0
#
# Host-side unit tests for command semantics (shared/commands.cpp).
#
# commands.cpp depends on zbus and protocol, never on MQTT or sockets, so it
# compiles here without the network stack. The chan_sensor_cmd it publishes to
# is DECLARED in app_channels.h (ZBUS_CHAN_DECLARE expands to extern) and
# DEFINED by whoever links it -- each application's sensor.cpp, src/main.cpp here.
# That is the seam the design already had; the tests did not invent one.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(commands_test)

set(SHARED ${CMAKE_CURRENT_SOURCE_DIR}/../../shared)

list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
include(nanopb)
zephyr_nanopb_sources(app ${CMAKE_CURRENT_SOURCE_DIR}/../../proto/node.proto)

target_sources(app PRIVATE src/main.cpp ${SHARED}/commands.cpp)
target_include_directories(app PRIVATE ${SHARED})
