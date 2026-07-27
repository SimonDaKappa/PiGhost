// control_plane.h - the display's producer control-socket thread.
//
// Owns, exclusively:
//  - PGIPC_CONTROL_SOCK_PATH's listen socket and every accepted producer fd
//  - the session table. The session table single-writer invariant is enforced simply
//    by never touching the table from any other thread
//  - the shm ring's generation counter, via pgipc_evict_writer(), called exactly once
//    per activation switch.
//  - telling the data-plane thread when the negotiated frame size changes
//    (pgipc_data_plane_set_mode()), so a switch to a producer negotiated at a different
//    mode doesn't require a data-plane restart
//
// Also answers the admin thread's LIST/SWITCH queries by draining
// pgipc_control_query_channel_t.
//
// Must be compiled with LIBPGIPC_READER defined.
#ifndef PGIPC_CONTROL_PLANE_H
#define PGIPC_CONTROL_PLANE_H

#ifndef LIBPGIPC_READER
#error "control_plane.h requires #define LIBPGIPC_READER before including it"
#endif

#include <stdatomic.h>

#include "control/control_query.h"
#include "dataplane/data_plane.h"
#include "libpgipc.h"
#include "session/session_table.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * struct pgipc_control_plane_t - control thread state
 * @listen_fd:          bound+listening PGIPC_CONTROL_SOCK_PATH socket
 * @stop_read_fd:        poll()'d alongside every other fd; readable once
 *                       pgipc_control_plane_stop() has been called
 * @stop_write_fd:       pgipc_control_plane_stop() writes one byte here
 * @table:               the session table; touched ONLY on this thread
 * @ring:                shm ring, for pgipc_evict_writer() on every activation switch;
 *                       not owned
 * @dp:                  data-plane handle, for pgipc_data_plane_set_mode() when
 *                       activation switches to a different mode; not owned, may be
 *                       NULL (e.g. in tests that don't stand up a real data plane)
 * @chan:                control-query channel shared with the admin thread; not owned,
 *                       must already be initialized
 * @supported_modes:     display-supported modes, in the order the display prefers to
 *                       report them
 * @num_supported_modes: number of valid entries in @supported_modes
 *                       (1..PGIPC_MAX_MODES)
 * @running:             set false by pgipc_control_plane_stop()F
 *
 * One instance per display process, run on its own thread via
 * pgipc_control_plane_run().
 */
typedef struct {
  int listen_fd;
  int stop_read_fd;
  int stop_write_fd;
  pgipc_session_table_t table;
  pgipc_shm_ring_t *ring;
  pgipc_data_plane_t *dp;
  pgipc_control_query_channel_t *chan;
  pgipc_render_mode_t supported_modes[PGIPC_MAX_MODES];
  uint32_t num_supported_modes;
  atomic_bool running;
} pgipc_control_plane_t;

/**
 * pgipc_control_plane_init() - create and bind the control listen socket
 * @cp:                  control plane state to populate
 * @ring:                shm ring, created by the display via
 *                       pgipc_shm_ring_create(); must outlive @cp
 * @dp:                  data-plane handle to notify on mode switches, or NULL to skip
 *                       that notification entirely
 * @chan:                control-query channel shared with the admin thread; must
 *                       already be initialized and must outlive @cp
 * @supported_modes:     display-supported modes
 * @num_supported_modes: number of entries in @supported_modes (1..PGIPC_MAX_MODES)
 *
 * Unlinks any stale socket file at PGIPC_CONTROL_SOCK_PATH left over from a previous
 * run before binding, and initializes an empty session table.
 *
 * Return: 0 on success, -1 on error (socket/bind/listen failure, or
 * @num_supported_modes == 0).
 */
int pgipc_control_plane_init(pgipc_control_plane_t *cp, pgipc_shm_ring_t *ring,
                             pgipc_data_plane_t *dp,
                             pgipc_control_query_channel_t *chan,
                             const pgipc_render_mode_t *supported_modes,
                             uint32_t num_supported_modes);

/**
 * pgipc_control_plane_run() - the control thread's entry point
 * @arg: pgipc_control_plane_t*, already initialized
 *
 * Runs a single poll() loop over the listen socket, the stop pipe, the control-query
 * channel's wake fd, and every currently-connected producer's control fd, until
 * pgipc_control_plane_stop() is called. Also periodically calls
 * pgipc_session_table_check_heartbeat_timeouts() (every poll() timeout tick) so a dead
 * ACTIVE producer is evicted even if no fd ever becomes readable again. Intended to be
 * passed directly to pthread_create().
 *
 * Return: always NULL.
 */
void *pgipc_control_plane_run(void *arg);

/**
 * pgipc_control_plane_stop() - request the loop to end
 * @cp: control plane state
 *
 * Sets @running false and wakes a blocked poll() via the internal stop pipe. Call, then
 * pthread_join() the thread running pgipc_control_plane_run().
 */
void pgipc_control_plane_stop(pgipc_control_plane_t *cp);

/**
 * pgipc_control_plane_close() - release all control plane resources
 * @cp: control plane state; call after pgipc_control_plane_stop() + pthread_join()
 *
 * Closes the listen socket, every still-open producer fd, the stop pipe, and unlinks
 * the socket path. Does not touch @ring/@dp/@chan -- those are borrowed, not owned.
 */
void pgipc_control_plane_close(pgipc_control_plane_t *cp);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif // PGIPC_CONTROL_PLANE_H
