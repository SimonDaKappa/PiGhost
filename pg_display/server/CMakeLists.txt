cmake_minimum_required(VERSION 3.20)
project(pgipc_display C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
# C++23 is required (not just for gtest itself) because libpgipc.h's public
# structs use C11 <stdatomic.h> types (atomic_int, atomic_bool, ...) directly
# as field types. Those typedefs are only made available to C++ translation
# units via libstdc++'s own <stdatomic.h> compatibility shim, which is
# itself gated behind __cpp_lib_stdatomic_h (C++23). Under C++17/20 the shim
# defines nothing and every atomic_* type in the header fails to resolve
# when the header is included from a .cpp file (e.g. our gtest sources).
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PGIPC_BUILD_TESTS
       "Build unit/integration tests (fetches GoogleTest via CMake FetchContent on first configure -- needs network once, then cached)"
       ON)
set(PGIPC_SANITIZE
    ""
    CACHE STRING "Build with a sanitizer: empty (none) | thread | address")

add_compile_options(-Wall -Wextra)
add_compile_definitions(_POSIX_C_SOURCE=200809L)

if(PGIPC_SANITIZE STREQUAL "thread")
  message(STATUS "pgipc: building with -fsanitize=thread")
  add_compile_options(-fsanitize=thread -g -O1)
  add_link_options(-fsanitize=thread)
elseif(PGIPC_SANITIZE STREQUAL "address")
  message(STATUS "pgipc: building with -fsanitize=address,undefined")
  add_compile_options(-fsanitize=address,undefined -g -O1)
  add_link_options(-fsanitize=address,undefined)
elseif(NOT PGIPC_SANITIZE STREQUAL "")
  message(FATAL_ERROR "PGIPC_SANITIZE must be empty, 'thread', or 'address' (got '${PGIPC_SANITIZE}')")
endif()

# ---------------------------------------------------------------------------
# Core display library: every module except the process entrypoint
# (reader.c) and the libpgipc.h implementation TU, which are kept separate
# so tests can link the exact same modules reader.c will eventually use.
# ---------------------------------------------------------------------------
add_library(pgipc_display_core STATIC
  pgipc_impl.c
  session/session_table.c
  dataplane/data_plane.c
  dataplane/fb_sink_file.c
  control/control_query.c
  control/control_plane.c
  admin/admin_plane.c
)
target_include_directories(pgipc_display_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../libpgipc
)
find_package(Threads REQUIRED)
target_link_libraries(pgipc_display_core PUBLIC Threads::Threads m)

# ---------------------------------------------------------------------------
# The real display binary
# ---------------------------------------------------------------------------
add_executable(pgipc_reader reader.c)
target_link_libraries(pgipc_reader PRIVATE pgipc_display_core)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(PGIPC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
