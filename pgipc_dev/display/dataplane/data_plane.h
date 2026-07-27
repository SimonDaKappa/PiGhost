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

#include <semaphore.h>
#include <stdatomic.h>

#include "fb_sink.h"
#include "libpgipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * struct pgipc_data_plane_stats_t - free-running diagnostic counters
 * @frames_rendered: successful checkout+blit+release cycles
 * @frames_dropped:  sem_wait woke but checkout() found nothing ready (spurious wake,
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
 * @ring:   shm ring, created by the display via pgipc_shm_ring_create()
 * @fsem:   frame-ready semaphore, created via pgipc_shm_sem_create()
 * @sink:   output sink frames are blitted through
 * @width:  current negotiated frame width, in pixels
 * @height: current negotiated frame height, in pixels
 * @running: cleared by pgipc_data_plane_stop() to end the loop
 * @stats:  diagnostic counters, see pgipc_data_plane_stats_t
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
  sem_t *fsem;
  pgipc_fb_sink_t *sink;
  uint32_t width;
  uint32_t height;
  atomic_bool running;
  pgipc_data_plane_stats_t stats;
} pgipc_data_plane_t;

/**
 * pgipc_data_plane_init() - configure a data-plane loop
 * @dp:     handle to initialize
 * @ring:   attached/created shm ring
 * @fsem:   attached/created frame-ready semaphore
 * @sink:   output sink; must outlive @dp
 * @width:  initial negotiated frame width
 * @height: initial negotiated frame height
 *
 * Calls pgipc_fb_sink_open() on @sink with the initial size. Does not start the loop;
 * pass @dp to pgipc_data_plane_run() (typically via pthread_create()) to do that.
 *
 * Return: 0 on success, -1 if the sink failed to open.
 */
int pgipc_data_plane_init(pgipc_data_plane_t *dp, pgipc_shm_ring_t *ring, sem_t *fsem,
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
 * Blocks in sem_wait() until either a frame is published or pgipc_data_plane_stop()
 * wakes it; returns (NULL) once @dp->running is false. Intended to be run on its own
 * thread.
 *
 * Return: always NULL.
 */
void *pgipc_data_plane_run(void *arg);

/**
 * pgipc_data_plane_stop() - signal the loop to exit
 * @dp: running data-plane handle
 *
 * Sets @dp->running = false and posts @dp->fsem once to unblock a sem_wait() the loop
 * may currently be parked in. Caller should pthread_join() the thread
 * pgipc_data_plane_run() was launched on afterwards. Does not call
 * pgipc_fb_sink_close(). That is the caller's responsibility since it also owns the
 * sink's lifetime.
 */
void pgipc_data_plane_stop(pgipc_data_plane_t *dp);

#ifdef __cplusplus
}
#endif

#endif /* PGIPC_DATA_PLANE_H */
