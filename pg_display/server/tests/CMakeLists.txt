include(FetchContent)

# Result is cached under the build directory after the first configure, so
# this only needs network access once per fresh build dir.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# ---------------------------------------------------------------------------
# Unit tests: fast, no threads/sockets/shm. Pure data-structure coverage.
# Safe to run on every commit.
# ---------------------------------------------------------------------------
add_executable(pgipc_unit_tests
  unit/session_table_test.cpp
)
target_link_libraries(pgipc_unit_tests PRIVATE pgipc_display_core gtest_main)
gtest_discover_tests(pgipc_unit_tests)

# ---------------------------------------------------------------------------
# Integration tests: real threads, real POSIX shm/semaphores/sockets. Each
# test binds fixed, well-known resource names (PGIPC_SEM_NAME, the shm ring
# segment, PGIPC_ADMIN_SOCK_PATH), so tests within and especially across
# these binaries must never run concurrently with each other. RUN_SERIAL 
# enforces that under ctest.
#
# This is also the primary target for sanitizer builds
# (-DPGIPC_SANITIZE=thread) and repeat-to-catch-flakes runs:
#   cmake -B build -DPGIPC_SANITIZE=thread && cmake --build build
#   ctest --test-dir build -R integration --repeat-until-fail 20
# ---------------------------------------------------------------------------
add_executable(pgipc_integration_tests
  integration/data_plane_test.cpp
  integration/admin_plane_test.cpp
  integration/control_plane_test.cpp
)
target_link_libraries(pgipc_integration_tests PRIVATE pgipc_display_core gtest_main)
gtest_discover_tests(pgipc_integration_tests
  TEST_PREFIX "integration."
  PROPERTIES RUN_SERIAL TRUE
             TIMEOUT 60
)
