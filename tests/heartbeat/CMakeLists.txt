# SPDX-License-Identifier: Apache-2.0
#
# Unit tests for the hand-packed heartbeat frame in shared/can_link.h.
#
# There is no .cpp to compile: can_link.h is constants and two static inline
# functions, on purpose, so both applications can include it and a test can
# exercise it with nothing else linked in. No CAN driver, no nanopb, no zbus.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(heartbeat_test)

target_sources(app PRIVATE src/main.cpp)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../shared)
