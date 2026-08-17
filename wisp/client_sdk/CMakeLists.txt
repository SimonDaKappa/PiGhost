# wisp/client_sdk/CMakeLists.txt
#
# NOTE: configured as a subdirectory of the top-level wisp/CMakeLists.txt,
# which provides the wisp_protocol target (wisp/protocol) linked below.
# Configure from wisp/, not from here directly.
#
# Builds libwispc.so -- third-party client apps link with -lwispc and
# #include <wispc/client.h> (and <wispc/gbm.h>).
add_library(wispc SHARED
    src/client_shm.c
    src/client_control.c
    src/client_gbm.c
)
target_include_directories(wispc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# wisp_protocol is an internal implementation detail of the SDK -- PRIVATE so
# it is not re-exported to third-party apps linking wispc; they only ever see
# include/wispc/client.h (and wispc/gbm.h).
target_link_libraries(wispc PRIVATE wisp_protocol)

set_target_properties(wispc PROPERTIES
    C_VISIBILITY_PRESET hidden
    SOVERSION 1
)

# pkg-config file for third-party client apps (`pkg-config --cflags --libs
# wispc`). Not yet wired to `install()` -- packaging/Yocto recipe integration
# is future work; this just keeps the .pc template buildable and in sync with
# the target above.
set(WISPC_VERSION "1.0.0")
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/pkg/wispc.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/wispc.pc
    @ONLY
)
