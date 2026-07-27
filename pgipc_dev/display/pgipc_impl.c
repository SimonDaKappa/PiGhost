// pgipc_impl.c - the single translation unit that compiles libpgipc's
// implementation for the display service and its tests.
//
// Deliberately defines NEITHER LIBPGIPC_READER nor LIBPGIPC_WRITER, so
// BOTH sides' function bodies get compiled in (the header's documented
// "both" default) -- other display-side .c files only need reader-side
// declarations, but test_data_plane.c's fake-writer role needs writer-side
// bodies to link against too. See test_data_plane.c's file comment.
#define LIBPGIPC_IMPLEMENTATION
#define LIBPGIPC_NO_SIDE_WARNING
#include "libpgipc.h"
