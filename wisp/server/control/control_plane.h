// control_plane.h - the server's producer control-socket thread.
//
// Owns, exclusively:
//  - PGDPS_CONTROL_SOCK_PATH's listen socket and every accepted producer fd
//  - the session table. The session table single-client invariant is enforced simply
//    by never touching the table from any other thread
//  - the shm ring's generation counter, via pgdps_evict_client(), called exactly once
//    per activation switch.
//  - telling the data-plane thread when the negotiated frame size changes
//    (pgdps_data_plane_set_mode()), so a switch to a producer negotiated at a different
//    mode doesn't require a data-plane restart
//
// Also answers the admin thread's LIST/SWITCH queries by draining
// pgdps_control_query_channel_t.
//
// Must be compiled with LIBPGDP_SERVER defined.
#ifndef PGDPS_CONTROL_PLANE_H
#define PGDPS_CONTROL_PLANE_H

#ifndef LIBPGDP_SERVER
#error "control_plane.h requires #define LIBPGDP_SERVER before including it"
#endif

#include <stdatomic.h>

#include "control/control_query.h"
#include "dataplane/data_plane.h"
#include "libpgdp.h"
#include "session/session_table.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * struct pgdps_control_plane_t - control thread state
 * @listen_fd:          bound+listening PGDPS_CONTROL_SOCK_PATH socket
 * @stop_read_fd:        poll()'d alongside every other fd; readable once
 *                       pgdps_control_plane_stop() has been called
 * @stop_write_fd:       pgdps_control_plane_stop() writes one byte here
 * @table:               the session table; touched ONLY on this thread
 * @ring:                shm ring, for pgdps_evict_client() on every activation switch;
 *                       not owned
 * @frame_fd:            frame-ready eventfd shared with @dp; not owned. Sent to each 
 *                       client via SCM_RIGHTS on PGDP_MSG_ACTIVATE_GRANT
 * @dp:                  data-plane handle, for "set mode" when activation switches to 
 *                       a different mode, and "kick" on every eviction; not owned, may 
 *                       be NULL
 * @chan:                control-query channel shared with the admin thread; not owned,
 *                       must already be initialized
 * @supported_modes:     server-supported modes, in the order the server prefers to
 *                       report them
 * @num_supported_modes: number of valid entries in @supported_modes
 *                       (1..PGDP_MAX_MODES)
 * @running:             set false by pgdps_control_plane_stop()F
 *
 * One instance per server process, run on its own thread via
 * pgdps_control_plane_run().
 */
typedef struct {
  int listen_fd;
  int stop_read_fd;
  int stop_write_fd;
  pgdps_session_table_t table;
  pgdp_shm_ring_t *ring;
  int frame_fd;
  pgdps_data_plane_t *dp;
  pgdps_control_query_channel_t *chan;
  pgdp_render_mode_t supported_modes[PGDP_MAX_MODES];
  uint32_t num_supported_modes;
  atomic_bool running;
} pgdps_control_plane_t;

/**
 * pgdps_control_plane_init() - create and bind the control listen socket
 * @cp:                  control plane state to populate
 * @ring:                shm ring, created by the server via
 *                       pgdps_shm_ring_create(); must outlive @cp
 * @frame_fd:            frame-ready eventfd; must outlive @cp. Both @cp and @dp are 
 *                       peer consumers of it
 * @dp:                  data-plane handle to notify on mode switches/evictions, or
 *                       NULL to skip those notifications entirely
 * @chan:                control-query channel shared with the admin thread; must
 *                       already be initialized and must outlive @cp
 * @supported_modes:     server-supported modes
 * @num_supported_modes: number of entries in @supported_modes (1..PGDP_MAX_MODES)
 *
 * Unlinks any stale socket file at PGDPS_CONTROL_SOCK_PATH left over from a previous
 * run before binding, and initializes an empty session table.
 *
 * Return: 0 on success, -1 on error (socket/bind/listen failure, or
 * @num_supported_modes == 0).
 */
int pgdps_control_plane_init(pgdps_control_plane_t *cp, pgdp_shm_ring_t *ring,
                             int frame_fd, pgdps_data_plane_t *dp,
                             pgdps_control_query_channel_t *chan,
                             const pgdp_render_mode_t *supported_modes,
                             uint32_t num_supported_modes);

/**
 * pgdps_control_plane_run() - the control thread's entry point
 * @arg: pgdps_control_plane_t*, already initialized
 *
 * Runs a single poll() loop over the listen socket, the stop pipe, the control-query
 * channel's wake fd, and every currently-connected producer's control fd, until
 * pgdps_control_plane_stop() is called. Also periodically calls
 * pgdps_session_table_check_heartbeat_timeouts() (every poll() timeout tick) so a dead
 * ACTIVE producer is evicted even if no fd ever becomes readable again. Intended to be
 * passed directly to pthread_create().
 *
 * Return: always NULL.
 */
void *pgdps_control_plane_run(void *arg);

/**
 * pgdps_control_plane_stop() - request the loop to end
 * @cp: control plane state
 *
 * Sets @running false and wakes a blocked poll() via the internal stop pipe. Call, then
 * pthread_join() the thread running pgdps_control_plane_run().
 */
void pgdps_control_plane_stop(pgdps_control_plane_t *cp);

/**
 * pgdps_control_plane_close() - release all control plane resources
 * @cp: control plane state; call after pgdps_control_plane_stop() + pthread_join()
 *
 * Closes the listen socket, every still-open producer fd, the stop pipe, and unlinks
 * the socket path. Does not touch @ring/@dp/@chan -- those are borrowed, not owned.
 */
void pgdps_control_plane_close(pgdps_control_plane_t *cp);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif // PGDPS_CONTROL_PLANE_H
