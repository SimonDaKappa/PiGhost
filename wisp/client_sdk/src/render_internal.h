#pragma once

#include <clock_internal.h>
#include <pthread.h>
#include <wisp/utils.h>
#include <wispc/render.h>

/**
 * typedef wispc_render_stop_reason_t - possible reasons why the render thread stopped
 * @WISPC_RENDER_STOP_NONE:      no stop reason
 * @WISPC_RENDER_STOP_VOLUNTARY: the render thread stopped voluntarily
 * @WISPC_RENDER_STOP_ERROR:     the render thread stopped due to an error
 * @WISPC_RENDER_STOP_STALE_GENERATION: the render thread stopped because its generation
 *                                      became stale
 * @WISPC_RENDER_STOP_EVICTED: the render thread stopped because client was evicted.
 */
typedef enum {
  WISPC_RENDER_STOP_NONE = 0,
  WISPC_RENDER_STOP_VOLUNTARY = 1,
  WISPC_RENDER_STOP_ERROR = 2,
  WISPC_RENDER_STOP_STALE_GENERATION = 3,
  WISPC_RENDER_STOP_EVICTED = 4,
} wispc_render_stop_reason_t;

typedef wispc_render_result_t (*universal_render_cb)(wispc_ctx_t *ctx, uint32_t buf_idx,
                                                     uint32_t frame_id, int *err_out);

typedef struct {
  struct timespec render_start;
  struct timespec render_end;
  struct timespec publish_start;
  struct timespec publish_end;
} wispc_render_timing_t;

typedef struct {
  pthread_t thread;
  universal_render_cb render;
  wispc_on_warmup_pixels warmup;
  wispc_on_render_error on_error;
  wispc_on_render_published on_published;
  
  wispc_render_timing_t timing;

  WISP_ATOMIC int32_t render_kind;
  WISP_ATOMIC int32_t stop_reason;
  WISP_ATOMIC int32_t error_code;
  WISP_ATOMIC bool stop;
  WISP_ATOMIC bool exited;

  wispc_clock_t clock;
} wispc_renderer_t;

void wispc_renderer_stop(wispc_renderer_t *renderer, wispc_render_stop_reason_t reason,
                         int error_code);

void *wispc_renderer_main(void *arg);
