cmake_minimum_required(VERSION 3.20)
project(pgdp_server C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
# C++23 is required (not just for gtest itself) because libpgdp.h's public
# structs use C11 <stdatomic.h> types (atomic_int, atomic_bool, ...) directly
# as field types. Those typedefs are only made available to C++ translation
# units via libstdc++'s own <stdatomic.h> compatibility shim, which is
# itself gated behind __cpp_lib_stdatomic_h (C++23). Under C++17/20 the shim
# defines nothing and every atomic_* type in the header fails to resolve
# when the header is included from a .cpp file (e.g. our gtest sources).
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PGDP_BUILD_TESTS
       "Build unit/integration tests (fetches GoogleTest via CMake FetchContent on first configure -- needs network once, then cached)"
       ON)
set(PGDP_SANITIZE
    ""
    CACHE STRING "Build with a sanitizer: empty (none) | thread | address")

add_compile_options(-Wall -Wextra)
add_compile_definitions(_POSIX_C_SOURCE=200809L)

if(PGDP_SANITIZE STREQUAL "thread")
  message(STATUS "pgipc: building with -fsanitize=thread")
  add_compile_options(-fsanitize=thread -g -O1)
  add_link_options(-fsanitize=thread)
elseif(PGDP_SANITIZE STREQUAL "address")
  message(STATUS "pgipc: building with -fsanitize=address,undefined")
  add_compile_options(-fsanitize=address,undefined -g -O1)
  add_link_options(-fsanitize=address,undefined)
elseif(NOT PGDP_SANITIZE STREQUAL "")
  message(FATAL_ERROR "PGDP_SANITIZE must be empty, 'thread', or 'address' (got '${PGDP_SANITIZE}')")
endif()

# ---------------------------------------------------------------------------
# Core server library: every module except the process entrypoint
# (reader.c) and the libpgdp.h implementation TU, which are kept separate
# so tests can link the exact same modules reader.c will eventually use.
# ---------------------------------------------------------------------------
add_library(pgdp_server_core STATIC
  pgdp_impl.c
  session/session_table.c
  dataplane/data_plane.c
  dataplane/fb_sink_file.c
  control/control_query.c
  control/control_plane.c
  admin/admin_plane.c
)
target_include_directories(pgdp_server_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../libpgdp
)
find_package(Threads REQUIRED)
target_link_libraries(pgdp_server_core PUBLIC Threads::Threads m)

# ---------------------------------------------------------------------------
# The real server binary
# ---------------------------------------------------------------------------
add_executable(server server.c)
target_link_libraries(server PRIVATE pgdp_server_core)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(PGDP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
