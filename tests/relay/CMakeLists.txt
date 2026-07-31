# SPDX-License-Identifier: Apache-2.0
#
# Unit tests for the relay's liveness state machine (gateway/src/relay.h).
#
# relay.h is the gateway's own header, not a shared one -- only the gateway
# relays. can_link.h, which it is tested against, is shared, so this suite needs
# both include directories.
#
# No relay.cpp here, and that is the point: liveness_step() is a static inline
# taking a clock reading as an argument, so it can be exercised without a CAN
# controller, a thread, or the passage of time. The parts of relay.cpp that
# cannot be reached this way are listed in docs/test-strategy.md.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(relay_test)

target_sources(app PRIVATE src/main.cpp)
target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../gateway/src  # relay.h -- gateway-only
  ${CMAKE_CURRENT_SOURCE_DIR}/../../shared        # can_link.h -- both ends
)
