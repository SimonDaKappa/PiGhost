#include <client_internal.h>
#include <unistd.h>


int wispc_renderer_init(wispc_ctx_t *ctx) {

}

void wispc_renderer_stop(wispc_renderer_t *renderer, wispc_render_stop_reason_t reason,
                         int error_code) {
  wisp_atomic_store(&renderer->stop_reason, reason);
  wisp_atomic_store(&renderer->error_code, error_code);
  wisp_atomic_store(&renderer->stop, true);

  renderer->clock.ops->force_wake(&renderer->clock);
}

static bool wispc_renderer_running(wispc_ctx_t *ctx) {
  return !wisp_atomic_load(&ctx->renderer.stop) && wisp_atomic_load(&ctx->active);
}

void *wispc_renderer_main(void *arg) {
  wispc_ctx_t *ctx = (wispc_ctx_t *)arg;
  wispc_renderer_t *renderer = &ctx->renderer;
  wispc_clock_ops_t *clock = renderer->clock.ops;
  wispc_event_cfg_t *events = &ctx->events;
  wispc_render_timing_t *timing = &renderer->timing;

  wispc_render_result_t render_res;
  uint32_t missed_frames, buf_idx;
  bool force_wake, gen_valid;
  uint64_t frame_id;
  int render_err;

  while (wispc_renderer_running(ctx)) {
    missed_frames = 0;
    force_wake = false;

    force_wake = clock->wait_next(ctx, &missed_frames);

    if (events->on_render_backpressure && missed_frames > 0)
      events->on_render_backpressure(events->user, missed_frames);

    if (!wispc_renderer_running(ctx))
      break;

    frame_id = wisp_atomic_load(&ctx->ring->frame_counter) + 1;
    buf_idx = wisp_ring_claim_slot(ctx->ring);
    render_err = 0;

    clock_gettime(CLOCK_MONOTONIC, &timing->render_start);
    render_res = renderer->render(ctx, buf_idx, frame_id, &render_err);
    clock_gettime(CLOCK_MONOTONIC, &timing->render_end);

    gen_valid = wisp_atomic_load(&ctx->ring->generation) ==
                wisp_atomic_load(&ctx->granted_generation);
    if (!gen_valid) {
      wisp_atomic_store(&ctx->active, false);
      wispc_renderer_stop(renderer, WISPC_RENDER_STOP_STALE_GENERATION, 0);
      continue;
    }

    switch (render_res) {
    case WISPC_RENDER_PUBLISH:
      clock_gettime(CLOCK_MONOTONIC, &timing->publish_start);
      wisp_shm_ring_publish_slot(ctx->ring, buf_idx, frame_id,
                                 wisp_ts_to_ns(&timing->publish_start));
      wisp_atomic_store(&ctx->ring->frame_counter, frame_id);
      // Potential hang here, but frame fd is just signal not large payload so low risk?
      if (ctx->frame_fd >= 0) {
        uint64_t v = 1;
        ssize_t n;
        do {
          n = write(ctx->frame_fd, &v, sizeof(v));
        } while (n < 0 && errno == EINTR);
      }

      clock_gettime(CLOCK_MONOTONIC, &timing->publish_end);
      if (events->on_render_published)
        events->on_render_published(events->user, frame_id,
                                 wisp_ts_to_ns(&timing->render_end) -
                                     wisp_ts_to_ns(&timing->render_start),
                                 wisp_ts_to_ns(&timing->publish_end) -
                                     wisp_ts_to_ns(&timing->publish_start));
      break;
    case WISPC_RENDER_SKIP:
      wisp_ring_lock(ctx->ring);
      wisp_atomic_store(&ctx->ring->client_locked, -1);
      wisp_ring_unlock(ctx->ring);
      break;
    case WISPC_RENDER_STOP:
      wisp_ring_lock(ctx->ring);
      wisp_atomic_store(&ctx->ring->client_locked, -1);
      wisp_ring_unlock(ctx->ring);
      wispc_renderer_stop(renderer, WISPC_RENDER_STOP_VOLUNTARY, 0);
      break;
    case WISPC_RENDER_ERROR:
      wisp_ring_lock(ctx->ring);
      wisp_atomic_store(&ctx->ring->client_locked, -1);
      wisp_ring_unlock(ctx->ring);
      wispc_renderer_stop(renderer, WISPC_RENDER_STOP_ERROR, render_err);
      if (events->on_render_error)
        events->on_render_error(events->user, frame_id, render_err);
      break;
    }
  }

  wisp_atomic_store(&renderer->exited, true);
  return NULL;
}
