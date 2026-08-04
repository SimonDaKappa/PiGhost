// data_plane.h - the display's frame-consuming loop.
//
// Consumes whatever the currently-ACTIVE writer publishes into the shm ring and pushes
// it through a pgipc_fb_sink_t (fb_sink.h). Knows nothing about the control protocol,
// session table, or activation state. The shm ring's
// checkout/release/generation-check-in-publish mechanics are what make this loop safe
// to run completely independent of which writer (if any) currently holds the activation
// grant.
//
// This file must be compiled with LIBPGIPC_READER defined.
#ifndef PGIPC_DATA_PLANE_H
#define PGIPC_DATA_PLANE_H

#include <stdatomic.h>

#include "fb_sink.h"
#include "libpgipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * struct pgipc_data_plane_stats_t - free-running diagnostic counters
 * @frames_rendered: successful checkout+blit+release cycles
 * @frames_dropped:  frame_fd woke but checkout() found nothing ready (spurious wake,
 *                   e.g. the writer that posted was evicted before this loop reached
 *                   checkout())
 * @blit_errors:     blit() returned nonzero; frame is still released, just not counted
 *                   as rendered
 * @last_write_to_render_ns: latency from the frame's write_ts to the moment this loop
 *                           finished blit() for it, nanoseconds
 *
 * Snapshot-read by the admin plane for LIST responses; all fields are atomics so that
 * read is safe without any extra locking.
 */
typedef struct {
  atomic_uint_least64_t frames_rendered;
  atomic_uint_least64_t frames_dropped;
  atomic_uint_least64_t blit_errors;
  atomic_uint_least64_t last_write_to_render_ns;
} pgipc_data_plane_stats_t;

/**
 * struct pgipc_data_plane_t - data-plane loop handle
 * @ring:     shm ring, created by the display via pgipc_shm_ring_create()
 * @frame_fd: frame-ready eventfd, sent to each writer via SCM_RIGHTS on activation
 *            grant
 * @abort_fd: eventfd created internally by dataplane init; written by kick to instantly
 *            wake the loop out of epoll_wait() without touching @frame_fd, e.g. after
 *            an admin-driven eviction/swap
 * @sink:     output sink frames are blitted through
 * @width:    current negotiated frame width, in pixels
 * @height:   current negotiated frame height, in pixels
 * @running:  cleared by pgipc_data_plane_stop() to end the loop
 * @stats:    diagnostic counters, see pgipc_data_plane_stats_t
 *
 * @width/@height are read by the loop on every iteration (not just at open()), so
 * pgipc_data_plane_set_mode() may be called concurrently by the control-plane thread
 * when activation switches to a writer negotiated at a different mode; the loop picks
 * up the new size before its next blit().
 *
 * This is a plain atomic-free field pair by design: writes only ever happen from the
 * control-plane thread while the ACTIVE client is being changed, which is exactly when
 * stale in-flight frames at the old size are also being flushed via
 * pgipc_evict_writer(), so a torn read here is at worst one throwaway blit at a
 * mismatched size, not a correctness issue for anything downstream.
 */
typedef struct {
  pgipc_shm_ring_t *ring;
  int frame_fd;
  int abort_fd;
  int epoll_fd;
  pgipc_fb_sink_t *sink;
  uint32_t width;
  uint32_t height;
  atomic_bool running;
  pgipc_data_plane_stats_t stats;
} pgipc_data_plane_t;

/**
 * pgipc_data_plane_init() - configure a data-plane loop
 * @dp:       handle to initialize
 * @ring:     attached/created shm ring
 * @frame_fd: frame-ready eventfd
 * @sink:     output sink; must outlive @dp
 * @width:    initial negotiated frame width
 * @height:   initial negotiated frame height
 *
 * Opens @sink with the initial size, and creates the internal abort_fd + epoll instance
 * the loop will wait on alongside @frame_fd. Does not start the loop.
 *
 * Return: 0 on success, -1 if the sink failed to open or the abort_fd/epoll instance
 * could not be created.
 */
int pgipc_data_plane_init(pgipc_data_plane_t *dp, pgipc_shm_ring_t *ring, int frame_fd,
                          pgipc_fb_sink_t *sink, uint32_t width, uint32_t height);

/**
 * pgipc_data_plane_set_mode() - change the frame size the loop expects
 * @dp:     running or not-yet-started data-plane handle
 * @width:  new negotiated frame width
 * @height: new negotiated frame height
 *
 * Call this from the control-plane thread whenever activation switches to a writer
 * negotiated at a different mode than the previous one. Also re-opens the sink at the
 * new size (pgipc_fb_sink_open()); see the @width/@height field comment on
 * pgipc_data_plane_t for the concurrency reasoning.
 */
void pgipc_data_plane_set_mode(pgipc_data_plane_t *dp, uint32_t width, uint32_t height);

/**
 * pgipc_data_plane_run() - the loop itself; pthread-compatible entry point
 * @arg: a pgipc_data_plane_t*
 *
 * Blocks in epoll_wait() over {frame_fd, abort_fd} until either a frame is published, a
 * kick wakes it (e.g. after an eviction/swap), or dataplane stop ends the loop; returns
 * (NULL) once @dp->running is false. Intended to be run on its own thread.
 *
 * Return: always NULL.
 */
void *pgipc_data_plane_run(void *arg);

/**
 * pgipc_data_plane_kick() - instantly wake the loop out of epoll_wait()
 * @dp: running data-plane handle
 *
 * Writes to @dp->abort_fd without touching @dp->frame_fd or clearing @dp->running.
 * Intended to be called from the control-plane thread right after
 * pgipc_evict_writer(), so the loop re-checks checkout() promptly instead of
 * potentially blocking until the next frame-ready signal (which may never come if the
 * evicted writer was the last one publishing).
 */
void pgipc_data_plane_kick(pgipc_data_plane_t *dp);

/**
 * pgipc_data_plane_stop() - signal the loop to exit
 * @dp: running data-plane handle
 *
 * Sets @dp->running = false and writes to @dp->abort_fd once to unblock an
 * epoll_wait() the loop may currently be parked in. Caller should pthread_join() the
 * thread pgipc_data_plane_run() was launched on afterwards. Does not call
 * pgipc_fb_sink_close(). That is the caller's responsibility since it also owns the
 * sink's lifetime. Does not close @dp->frame_fd or @dp->abort_fd/epoll_fd; caller
 * should close @dp->abort_fd and @dp->epoll_fd (owned by this handle) after joining.
 */
void pgipc_data_plane_stop(pgipc_data_plane_t *dp);

#ifdef __cplusplus
}
#endif

#endif /* PGIPC_DATA_PLANE_H */
