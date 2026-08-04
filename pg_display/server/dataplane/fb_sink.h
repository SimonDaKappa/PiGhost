// fb_sink.h - abstract output sink for the server data-plane loop.
//
// The data-plane loop (data_plane.h) never talks to /dev/fb0 or any other hardware
// directly; it blits through this small vtable interface instead. This is what lets the
// ring/checkout/blit/release mechanics be exercised in test_data_plane.c on a dev
// machine with no real HDMI output at all, while the exact same data_plane.c code
// drives real hardware later via fb_sink_linuxfb.c.
//
// A sink only ever sees PGDP_PAYLOAD_PIXELS-mode data (e.g., XRGB8888), one full frame
// at a time, sized to the currently negotiated render mode. dmabuf/KMS scanout is a
// different data path entirely and is not modeled by this interface.
#ifndef PGDPS_FB_SINK_H
#define PGDPS_FB_SINK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pgdps_fb_sink_t pgdps_fb_sink_t;

/**
 * struct pgdps_fb_sink_ops_t - backend vtable for one output sink
 * @open:  (re)configure the sink for a given frame size, called once before the first
 *         blit() and again whenever the negotiated mode changes. Return 0 on success,
 *         -1 on failure.
 * @blit:  push exactly one full XRGB8888 frame to the sink. @pixels is @height rows of
 *         @width pixels, tightly packed (PGDP_BYTES_PER_PIXEL bytes per pixel, no
 *         source-side stride padding - the shm ring never pads rows). @frame_id is the
 *         producer-assigned id, forwarded for logging/diagnostics only. Return 0 on
 *         success, -1 on failure (logged by the caller; a failed blit does not stop the
 *         data-plane loop).
 * @close: release any resources opened by open()/the sink's constructor. May be called
 *         with the sink never having been opened.
 *
 */
typedef struct {
  int (*open)(void *ctx, uint32_t width, uint32_t height);
  int (*blit)(void *ctx, const unsigned char *pixels, uint32_t width, uint32_t height,
              uint64_t frame_id);
  void (*close)(void *ctx);
} pgdps_fb_sink_ops_t;

/**
 * struct pgdps_fb_sink_t - a bound sink instance
 * @ops: backend vtable
 * @ctx: backend-private state, passed to every ops call
 *
 * Backends provide a constructor (e.g. pgdps_fb_sink_file_create()) that fills this in;
 * callers only ever use the ops through the pgdps_fb_sink_* wrapper functions below,
 * never @ops/@ctx directly.
 */
struct pgdps_fb_sink_t {
  const pgdps_fb_sink_ops_t *ops;
  void *ctx;
};

/**
 * pgdps_fb_sink_open() - see pgdps_fb_sink_ops_t::open */
static inline int pgdps_fb_sink_open(pgdps_fb_sink_t *sink, uint32_t width,
                                     uint32_t height) {
  return sink->ops->open(sink->ctx, width, height);
}

/**
 * pgdps_fb_sink_blit() - see pgdps_fb_sink_ops_t::blit */
static inline int pgdps_fb_sink_blit(pgdps_fb_sink_t *sink, const unsigned char *pixels,
                                     uint32_t width, uint32_t height,
                                     uint64_t frame_id) {
  return sink->ops->blit(sink->ctx, pixels, width, height, frame_id);
}

/**
 * pgdps_fb_sink_close() - see pgdps_fb_sink_ops_t::close */
static inline void pgdps_fb_sink_close(pgdps_fb_sink_t *sink) {
  if (sink->ops->close)
    sink->ops->close(sink->ctx);
}

#ifdef __cplusplus
}
#endif

#endif /* PGDPS_FB_SINK_H */
