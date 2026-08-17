# wisp/server/CMakeLists.txt
#
# NOTE: configured as a subdirectory of the top-level wisp/CMakeLists.txt,
# which sets the C/CXX standards, -Wall -Wextra, WISP_BUILD_TESTS,
# WISP_SANITIZE, and add_subdirectory(protocol) (providing the wisp_protocol
# target linked below). Configure from wisp/, not from here directly:
#   cmake -S wisp -B build && cmake --build build -j

# ---------------------------------------------------------------------------
# Core server library: every module except the process entrypoint (server.c),
# kept separate so tests can link the exact same modules server.c uses.
# ---------------------------------------------------------------------------
add_library(wisp_server_core STATIC
  src/session/session_table.c
  src/dataplane/data_plane.c
  src/dataplane/fb_sink_file.c
  src/control/control_query.c
  src/control/control_plane.c
  src/admin/admin_plane.c
  src/shm/server_shm.c
)
target_include_directories(wisp_server_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
find_package(Threads REQUIRED)

# wisp_protocol (wisp/protocol) is statically linked in, PUBLIC so any server
# module can #include <wisp/some_proto_header.h> transitively.
target_link_libraries(wisp_server_core PUBLIC Threads::Threads m wisp_protocol)

# ---------------------------------------------------------------------------
# The real server binary
# ---------------------------------------------------------------------------
add_executable(server src/server.c)
target_link_libraries(server PRIVATE wisp_server_core)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(WISP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
