// fb_sink_file.h - dev/test pgdps_fb_sink_t backend that writes each frame out as a
// viewable PPM (P6) image instead of driving real hardware.
//
// Intended for use in test_data_plane.c and any future automated test that wants to
// assert on rendered pixel content without a framebuffer device.
#ifndef PGDPS_FB_SINK_FILE_H
#define PGDPS_FB_SINK_FILE_H

#include "fb_sink.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * struct pgdps_fb_sink_file_opts_t - configuration for the file sink
 * @out_dir:           directory to write into (created if missing); copied into an
 *                     internal fixed-size buffer at pgdps_fb_sink_file_create() time
 *                     (see pgdps_fb_sink_file_create()'s note on the length limit), so
 *                     it does NOT need to outlive the sink
 * @snapshot_interval: every Nth blit is ALSO saved as a distinct numbered file
 *                     (frame_%08llu.ppm); 0 disables numbered snapshots (only
 *                     "latest.ppm" is kept)
 * @max_snapshots:     oldest numbered snapshot files beyond this count are deleted as
 *                     new ones are written, bounding disk usage during long test runs;
 *                     0 = unbounded
 */
typedef struct {
  const char *out_dir;
  uint32_t snapshot_interval;
  uint32_t max_snapshots;
} pgdps_fb_sink_file_opts_t;

/**
 * pgdps_fb_sink_file_create() - construct a file-backed sink
 * @sink: output sink to populate
 * @opts: configuration; @opts->out_dir is copied into a fixed 512-byte internal buffer
 *        (truncated if longer), so @opts itself need not outlive @sink
 *
 * Every blit() overwrites `<out_dir>/latest.ppm` unconditionally, so a developer (or an
 * image-viewer with auto-reload) can just watch that one file. See
 * pgdps_fb_sink_file_opts_t for the numbered-snapshot policy.
 *
 * Return: 0 on success, -1 if @opts->out_dir could not be created/accessed.
 */
int pgdps_fb_sink_file_create(pgdps_fb_sink_t *sink,
                              const pgdps_fb_sink_file_opts_t *opts);

#ifdef __cplusplus
}
#endif

#endif /* PGDPS_FB_SINK_FILE_H */
