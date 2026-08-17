# wisp/CMakeLists.txt
#
# Top-level entry point tying the three wisp/ components together on one
# configure/build:
#   protocol/    shared wire contract, statically linked into both of the below
#   server/      wisp_server executable (static links protocol)
#   client_sdk/  wispc shared library for third-party client apps
#                (static links protocol, dynamically linked by clients)
#
# Build from here, e.g.:
#   cmake -S wisp -B build && cmake --build build -j
cmake_minimum_required(VERSION 3.20)
project(wisp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(WISP_BUILD_TESTS
       "Build server unit/integration tests (fetches GoogleTest via CMake FetchContent on first configure -- needs network once, then cached)"
       ON)
set(WISP_SANITIZE
    ""
    CACHE STRING "Build with a sanitizer: empty (none) | thread | address")

add_compile_options(-Wall -Wextra)
add_compile_definitions(_POSIX_C_SOURCE=200809L)

if(WISP_SANITIZE STREQUAL "thread")
  message(STATUS "wisp: building with -fsanitize=thread")
  add_compile_options(-fsanitize=thread -g -O1)
  add_link_options(-fsanitize=thread)
elseif(WISP_SANITIZE STREQUAL "address")
  message(STATUS "wisp: building with -fsanitize=address,undefined")
  add_compile_options(-fsanitize=address,undefined -g -O1)
  add_link_options(-fsanitize=address,undefined)
elseif(NOT WISP_SANITIZE STREQUAL "")
  message(FATAL_ERROR "WISP_SANITIZE must be empty, 'thread', or 'address' (got '${WISP_SANITIZE}')")
endif()

# Order matters: protocol must exist before server/client_sdk link against it.
add_subdirectory(protocol)
add_subdirectory(server)
add_subdirectory(client_sdk)
