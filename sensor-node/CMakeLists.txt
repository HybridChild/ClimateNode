# SPDX-License-Identifier: Apache-2.0
#
# The F072RB peer node: a BME280 read over I2C, published as Protobuf over CAN.
# The H753ZI gateway relays those bytes to MQTT without decoding them.
#
# Two of the four translation units are the gateway's own, compiled here by
# relative path rather than copied. That is the same rule tests/ already
# follows: one definition, no drift. It matters most for protocol.cpp, which
# *is* the mapping between the schema and the internal reading struct -- two
# copies of an encoder is two schemas that agree only by luck.

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sensor_node)

set(GW_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src)

# The same generator line firmware/CMakeLists.txt and tests/ use, so all three
# build against proto/ rather than a snapshot of it.
list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
include(nanopb)
zephyr_nanopb_sources(app ${CMAKE_CURRENT_SOURCE_DIR}/../proto/node.proto)

target_sources(app PRIVATE
  src/main.cpp          # the CAN session: heartbeat, ISO-TP send/receive
  src/sensor.cpp        # BME280 thread: acquisition, sample period, bounds
  ${GW_SRC}/protocol.cpp  # shared: the wire format
  ${GW_SRC}/commands.cpp  # shared: command semantics, dispatch, dedupe
)

# app_channels.h, commands.h and protocol.h come from the gateway's source
# directory. Note what this does NOT include: main.cpp and sensor.cpp are this
# node's own, because the session and the sensor are exactly what differs.
target_include_directories(app PRIVATE ${GW_SRC})
