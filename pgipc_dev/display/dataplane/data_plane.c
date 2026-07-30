// data_plane.c - see data_plane.h.
#define LIBPGIPC_READER
#include "data_plane.h"

#include <errno.h>
#include <stdio.h>
#include <time.h>

int pgipc_data_plane_init(pgipc_data_plane_t *dp, pgipc_shm_ring_t *ring, sem_t *fsem,
                          pgipc_fb_sink_t *sink, uint32_t width, uint32_t height) {
  dp->ring = ring;
  dp->fsem = fsem;
  dp->sink = sink;
  dp->width = width;
  dp->height = height;
  atomic_store(&dp->running, true);
  atomic_store(&dp->stats.frames_rendered, 0);
  atomic_store(&dp->stats.frames_dropped, 0);
  atomic_store(&dp->stats.blit_errors, 0);
  atomic_store(&dp->stats.last_write_to_render_ns, 0);

  if (pgipc_fb_sink_open(sink, width, height) != 0) {
    fprintf(stderr, "[data_plane] sink open failed at %ux%u\n", width, height);
    return -1;
  }
  return 0;
}

void pgipc_data_plane_set_mode(pgipc_data_plane_t *dp, uint32_t width,
                               uint32_t height) {
  /* this is a deliberately lock-free, best-effort update raced against
   * the loop thread. */
  dp->width = width;
  dp->height = height;
  if (pgipc_fb_sink_open(dp->sink, width, height) != 0)
    fprintf(stderr, "[data_plane] sink re-open failed at %ux%u\n", width, height);
}

void *pgipc_data_plane_run(void *arg) {
  pgipc_data_plane_t *dp = (pgipc_data_plane_t *)arg;

  while (atomic_load(&dp->running)) {
    if (sem_wait(dp->fsem) != 0) {
      if (errno == EINTR)
        continue;
      break; /* semaphore destroyed out from under us; nothing to recover */
    }

    if (!atomic_load(&dp->running))
      break;

    int idx = pgipc_shm_ring_checkout(dp->ring);
    if (idx < 0) {
      /* Spurious wake: the writer that posted was evicted (or otherwise
       * stopped publishing) before we got here. Nothing to render. */
      atomic_fetch_add(&dp->stats.frames_dropped, 1);
      continue;
    }

    /* Safe to read only now that checkout() holds reader_locked == idx;
     * a writers publish already does a generation check which guarantees
     * an evicted writer's frame stop landing in the ring, so this frame
     * is legal */
    uint64_t frame_id = dp->ring->frame_id[idx];
    uint64_t write_ts_ns = dp->ring->write_ts_ns[idx];
    const unsigned char *pixels = dp->ring->frame_bufs[idx];

    uint32_t width = dp->width;
    uint32_t height = dp->height;

    int rc = pgipc_fb_sink_blit(dp->sink, pixels, width, height, frame_id);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t now_ns = ((uint64_t)now.tv_sec * 1000000000ULL) + now.tv_nsec;

    pgipc_shm_ring_release(dp->ring);

    if (rc != 0) {
      atomic_fetch_add(&dp->stats.blit_errors, 1);
      continue;
    }

    atomic_fetch_add(&dp->stats.frames_rendered, 1);
    atomic_store(&dp->stats.last_write_to_render_ns, now_ns - write_ts_ns);
  }

  return NULL;
}

void pgipc_data_plane_stop(pgipc_data_plane_t *dp) {
  atomic_store(&dp->running, false);
  sem_post(dp->fsem); /* unblock a sem_wait() the loop may be parked in */
}
