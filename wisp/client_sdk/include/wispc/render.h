#pragma once

#include <wisp/types.h>

/**
 * typedef wispc_render_result_t - per-tick outcome returned by frame render
 * @WISPC_RENDER_PUBLISH: frame is painted and ready; wispc publishes it and posts
 *                        frame_fd.
 * @WISPC_RENDER_SKIP:    nothing new to publish this tick (e.g. no scene change); a
 *                        healthy, ordinary tick, not an error.
 * @WISPC_RENDER_STOP:    app-initiated voluntary wind-down; wispc transitions to
 *                        EVICT_PENDING and stops calling frame render, same as a
 *                        server-initiated eviction.
 * @WISPC_RENDER_ERROR:   unrecoverable app-side render error; wispc winds down the
 *                        same as ::WISPC_RENDER_STOP but fires ::on_render_error
 *                        instead of the ordinary eviction notifier so the app can
 *                        distinguish "I chose to stop" from "rendering broke".
 */
typedef enum {
  WISPC_RENDER_PUBLISH = 0,
  WISPC_RENDER_SKIP = 1,
  WISPC_RENDER_STOP = 2,
  WISPC_RENDER_ERROR = 3,
} wispc_render_result_t;

/**
 * typedef wispc_on_warmup_pixels - warmup the renderer after promotion and before
 * active for PIXELS render kind.
 * @user:      opaque user data
 * @frame_buf: scratch frame buffer. will NOT be published.
 *
 * The buffer will be cleared after warmup completes.
 *
 * Return: >= 0 if success, < 0 if failure.
 */
typedef int (*wispc_on_warmup_pixels)(void *user, void *frame_buf);

/**
 * typedef wispc_on_render_pixels - per-tick render callback. PIXELS payload.
 * @user:      opaque user data
 * @frame_buf: pointer to the frame buffer to render into
 * @frame_id:  unique identifier for the current frame
 * @err_out:   pointer to an integer to store error codes, if any
 *
 * Return: a value of type ::wispc_render_result_t indicating the outcome of the frame
 * render.
 */
typedef wispc_render_result_t (*wispc_on_render_pixels)(void *user, void *frame_buf,
                                                        uint64_t frame_id,
                                                        int *err_out);

/**
 * typedef wispc_on_warmup_dmabuf - warmup the renderer after promotion and before
 * active for DMABUF render kind.
 * @user: opaque user data
 * @buf_idx: fd index for scratch drm buffer index. will NOT be published.
 *
 * The buffer will be cleared after warmup completes.
 *
 * Return: >= 0 if success, < 0 if failure.
 */
typedef int (*wispc_on_warmup_dmabuf)(void *user, void *buf_idx);

/**
 * typedef wispc_on_render_dmabuf - per-tick render callback for DMABUF buffers
 * @user: opaque user data
 * @buf_idx: index of the DMABUF buffer to render into
 * @frame_id: unique identifier for the current frame
 * @err_out: pointer to an integer to store error codes, if any
 *
 * The client __must not__ flip any buffers into hardware.
 * The client __must__ complete its fence wait/sync before indicating
 * ::WISPC_RENDER_PUBLISH.
 *
 * Return: a value of type ::wispc_render_result_t indicating the outcome of the frame
 * render.
 */
typedef wispc_render_result_t (*wispc_on_render_dmabuf)(void *user, uint32_t buf_idx,
                                                        uint64_t frame_id,
                                                        int *err_out);

/**
 * typedef wispc_on_render_error - notify frame render error
 * @user: opaque user data
 * @frame_id: unique identifier for the current frame
 * @err:  frame render error code
 */
typedef void (*wispc_on_render_error)(void *user, uint64_t frame_id, int err);

/**
 * typedef wispc_on_render_published - notify frame rendered and published
 * @user: opaque user data
 * @frame_id: unique identifier for the current frame
 * @render_ns: length of time spent rendering the frame in nanoseconds
 * @publish_ns: length of time spent publishing the frame in nanoseconds
 */
typedef void (*wispc_on_render_published)(void *user, uint64_t frame_id,
                                          uint64_t render_ns, uint64_t publish_ns);

/**
 * typedef wispc_on_render_backpressure - notify when the renderer is under backpressure
 * @user: opaque user data
 * @missed_frames: number of frames missed due to backpressure
 */
typedef void (*wispc_on_render_backpressure)(void *user, uint32_t missed_frames);

typedef struct {
  wispc_on_warmup_pixels on_warmup;
  wispc_on_render_pixels on_render;
} wispc_render_pixels_cfg_t;

typedef struct {
  wispc_on_warmup_dmabuf on_warmup;
  wispc_on_render_dmabuf on_render;
} wispc_render_dmabuf_cfg_t;

typedef struct {
  wisp_render_kind_t render_kind;
  union {
    wispc_render_pixels_cfg_t pixels;
    wispc_render_dmabuf_cfg_t dmabuf;
  } kinds;
} wispc_render_cfg_t;

typedef struct {
  wisp_resolution_t *resolutions;
  uint32_t num_resolutions;
} wispc_screen_cfg_t;