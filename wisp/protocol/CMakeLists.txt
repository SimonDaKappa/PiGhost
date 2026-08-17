# wisp/protocol/CMakeLists.txt
#
# Shared wire contract: wire.h/types.h/utils.h/gbm.h plus the control-socket
# send/recv implementation (src/socket_message.c). Consumed at compile time by
# both wisp_server (wisp/server) and wisp_client (wisp/client_sdk) -- neither
# re-exports it as part of their own public API/headers.
add_library(wisp_protocol STATIC
  src/socket_message.c
)
target_include_directories(wisp_protocol PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Hidden by default: wisp_protocol is a compile-time-only implementation detail for
# both consumers (wisp_server links it into an executable; wisp_client static-links it
# into a shared lib without re-exporting it). Visibility must be set here, at
# wisp_protocol's own compile time -- setting it only on the consuming wisp_client
# target would not hide symbols already baked into wisp_protocol's .o files.
set_target_properties(wisp_protocol PROPERTIES
  C_VISIBILITY_PRESET hidden
)
