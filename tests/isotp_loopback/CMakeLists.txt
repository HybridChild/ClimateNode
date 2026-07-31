# SPDX-License-Identifier: Apache-2.0
#
# The CAN transport, exercised through a real controller with no hardware.
#
# Unlike the other suites here, this one compiles none of the application's .cpp
# files. What is under test is not a function of ours but the *path* our framing
# takes: can_link.h's identifiers and type byte carried over Zephyr's ISO-TP
# through an emulated controller that genuinely segments and reassembles.
#
# It therefore needs the headers both applications share -- can_link.h for the
# address map and the heartbeat frame, relay.h for the gateway's buffer bound --
# and nothing else. No nanopb: see the note on kRelayUpMax in src/main.cpp for
# why the transport is tested against the buffer rather than against the schema.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(isotp_loopback_test)

target_sources(app PRIVATE src/main.cpp)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware/src)
