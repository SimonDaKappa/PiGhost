// data_plane.c - see data_plane.h.
#include "data_plane.h"

#include <errno.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

int wisps_data_plane_init(wisps_data_plane_t *dp, wisp_shm_ring_t *ring, int frame_fd,
                          wisps_fb_sink_t *sink, uint32_t width, uint32_t height) {
  dp->ring = ring;
  dp->frame_fd = frame_fd;
  dp->sink = sink;
  dp->width = width;
  dp->height = height;
  wisp_atomic_store(&dp->running, true);
  wisp_atomic_store(&dp->stats.frames_rendered, 0);
  wisp_atomic_store(&dp->stats.frames_dropped, 0);
  wisp_atomic_store(&dp->stats.blit_errors, 0);
  wisp_atomic_store(&dp->stats.last_write_to_render_ns, 0);

  if (wisps_fb_sink_open(sink, width, height) != 0) {
    fprintf(stderr, "[data_plane] sink open failed at %ux%u\n", width, height);
    return -1;
  }

  dp->abort_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (dp->abort_fd < 0) {
    perror("[data_plane] eventfd (abort_fd)");
    return -1;
  }

  dp->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (dp->epoll_fd < 0) {
    perror("[data_plane] epoll_create1");
    close(dp->abort_fd);
    return -1;
  }

  struct epoll_event ev;
  ev.events = EPOLLIN;
  ev.data.fd = dp->frame_fd;
  epoll_ctl(dp->epoll_fd, EPOLL_CTL_ADD, dp->frame_fd, &ev);

  ev.events = EPOLLIN;
  ev.data.fd = dp->abort_fd;
  epoll_ctl(dp->epoll_fd, EPOLL_CTL_ADD, dp->abort_fd, &ev);

  return 0;
}


void *wisps_data_plane_run(void *arg) {
  wisps_data_plane_t *dp = (wisps_data_plane_t *)arg;

  while (wisp_atomic_load(&dp->running)) {
    struct epoll_event events[2];
    int nfds = epoll_wait(dp->epoll_fd, events, 2, -1);
    if (nfds < 0) {
      if (errno == EINTR)
        continue;
      break; /* epoll instance destroyed out from under us; nothing to recover */
    }

    if (!wisp_atomic_load(&dp->running))
      break;

    bool have_frame_signal = false;
    for (int i = 0; i < nfds; i++) {
      uint64_t counter;
      if (events[i].data.fd == dp->frame_fd) {
        /* read() clears (coalesces) the counter; N pending publishes since our
         * last wake collapse into a single "check the ring" pass, which is
         * correct since the ring only tracks the single latest-ready frame. */
        while (read(dp->frame_fd, &counter, sizeof(counter)) > 0) {}
        have_frame_signal = true;
      } 
      else if (events[i].data.fd == dp->abort_fd) {
        /* Kick: drain and fall through to re-check checkout() / running. */
        while (read(dp->abort_fd, &counter, sizeof(counter)) > 0) {}
      }
    }

    if (!have_frame_signal)
      continue; /* woken only by a kick; nothing new to render yet */

    int idx = wisps_shm_ring_checkout(dp->ring);
    if (idx < 0) {
      /* Spurious wake: the client that posted was evicted (or otherwise
       * stopped publishing) before we got here. Nothing to render. */
      wisp_atomic_fetch_add(&dp->stats.frames_dropped, 1);
      continue;
    }

    /* Safe to read only now that checkout() holds server_locked == idx;
     * a clients publish already does a generation check which guarantees
     * an evicted client's frame stop landing in the ring, so this frame
     * is legal */
    uint64_t frame_id = dp->ring->frame_id[idx];
    uint64_t write_ts_ns = dp->ring->write_ts_ns[idx];
    const unsigned char *pixels = dp->ring->frame_bufs[idx];

    uint32_t width = dp->width;
    uint32_t height = dp->height;

    int rc = wisps_fb_sink_blit(dp->sink, pixels, width, height, frame_id);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t now_ns = ((uint64_t)now.tv_sec * 1000000000ULL) + now.tv_nsec;

    wisps_shm_ring_release(dp->ring);

    if (rc != 0) {
      wisp_atomic_fetch_add(&dp->stats.blit_errors, 1);
      continue;
    }

    wisp_atomic_fetch_add(&dp->stats.frames_rendered, 1);
    wisp_atomic_store(&dp->stats.last_write_to_render_ns, now_ns - write_ts_ns);
  }

  return NULL;
}

void wisps_data_plane_stop(wisps_data_plane_t *dp) {
  wisp_atomic_store(&dp->running, false);
  wisps_data_plane_kick(dp); /* unblock an epoll_wait() the loop may be parked in */
}


void wisps_data_plane_set_mode(wisps_data_plane_t *dp, uint32_t width,
                               uint32_t height) {
  /* this is a deliberately lock-free, best-effort update raced against
   * the loop thread. */
  dp->width = width;
  dp->height = height;
  if (wisps_fb_sink_open(dp->sink, width, height) != 0)
    fprintf(stderr, "[data_plane] sink re-open failed at %ux%u\n", width, height);
}


void wisps_data_plane_kick(wisps_data_plane_t *dp) {
  uint64_t v = 1;
  ssize_t n;
  do {
    n = write(dp->abort_fd, &v, sizeof(v));
  } while (n < 0 && errno == EINTR);
}

