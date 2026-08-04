// pgdp_impl.c - the single translation unit that compiles libpgdp's
// implementation for the server service and its tests.
//
// Deliberately defines NEITHER LIBPGDP_SERVER nor LIBPGDP_CLIENT, so
// BOTH sides' function bodies get compiled in (the header's documented
// "both" default) -- other server-side .c files only need reader-side
// declarations, but test_data_plane.c's fake-client role needs client-side
// bodies to link against too. See test_data_plane.c's file comment.
#define LIBPGDP_IMPLEMENTATION
#define LIBPGDP_NO_SIDE_WARNING
#include "libpgdp.h"
