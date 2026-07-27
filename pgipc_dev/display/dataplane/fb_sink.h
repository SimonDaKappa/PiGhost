// fb_sink.h - abstract output sink for the display data-plane loop.
//
// The data-plane loop (data_plane.h) never talks to /dev/fb0 or any other hardware
// directly; it blits through this small vtable interface instead. This is what lets the
// ring/checkout/blit/release mechanics be exercised in test_data_plane.c on a dev
// machine with no real HDMI output at all, while the exact same data_plane.c code
// drives real hardware later via fb_sink_linuxfb.c.
//
// A sink only ever sees PGIPC_PAYLOAD_PIXELS-mode data (e.g., XRGB8888), one full frame
// at a time, sized to the currently negotiated render mode. dmabuf/KMS scanout is a
// different data path entirely and is not modeled by this interface.
#ifndef PGIPC_FB_SINK_H
#define PGIPC_FB_SINK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pgipc_fb_sink_t pgipc_fb_sink_t;

/**
 * struct pgipc_fb_sink_ops_t - backend vtable for one output sink
 * @open:  (re)configure the sink for a given frame size, called once before the first
 *         blit() and again whenever the negotiated mode changes. Return 0 on success,
 *         -1 on failure.
 * @blit:  push exactly one full XRGB8888 frame to the sink. @pixels is @height rows of
 *         @width pixels, tightly packed (PGIPC_BYTES_PER_PIXEL bytes per pixel, no
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
} pgipc_fb_sink_ops_t;

/**
 * struct pgipc_fb_sink_t - a bound sink instance
 * @ops: backend vtable
 * @ctx: backend-private state, passed to every ops call
 *
 * Backends provide a constructor (e.g. pgipc_fb_sink_file_create()) that fills this in;
 * callers only ever use the ops through the pgipc_fb_sink_* wrapper functions below,
 * never @ops/@ctx directly.
 */
struct pgipc_fb_sink_t {
  const pgipc_fb_sink_ops_t *ops;
  void *ctx;
};

/** pgipc_fb_sink_open() - see pgipc_fb_sink_ops_t::open */
static inline int pgipc_fb_sink_open(pgipc_fb_sink_t *sink, uint32_t width,
                                     uint32_t height) {
  return sink->ops->open(sink->ctx, width, height);
}

/** pgipc_fb_sink_blit() - see pgipc_fb_sink_ops_t::blit */
static inline int pgipc_fb_sink_blit(pgipc_fb_sink_t *sink, const unsigned char *pixels,
                                     uint32_t width, uint32_t height,
                                     uint64_t frame_id) {
  return sink->ops->blit(sink->ctx, pixels, width, height, frame_id);
}

/** pgipc_fb_sink_close() - see pgipc_fb_sink_ops_t::close */
static inline void pgipc_fb_sink_close(pgipc_fb_sink_t *sink) {
  if (sink->ops->close)
    sink->ops->close(sink->ctx);
}

#ifdef __cplusplus
}
#endif

#endif /* PGIPC_FB_SINK_H */
