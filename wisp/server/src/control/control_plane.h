// control_plane.h - the server's producer control-socket thread.
//
// Owns, exclusively:
//  - WISP_CONTROL_SOCK_PATH's listen socket and every accepted producer fd
//  - the session table. The session table single-client invariant is enforced simply
//    by never touching the table from any other thread
//  - the shm ring's generation counter, via wisps_ring_evict_client(), called exactly once
//    per activation switch.
//  - telling the data-plane thread when the negotiated frame size changes
//    (wisps_data_plane_set_mode()), so a switch to a producer negotiated at a different
//    mode doesn't require a data-plane restart
//
// Also answers the admin thread's LIST/SWITCH queries by draining
// wisps_control_query_channel_t.
#ifndef WISPS_CONTROL_PLANE_H
#define WISPS_CONTROL_PLANE_H

#include <wisp/utils.h>

#include "control/control_query.h"
#include "dataplane/data_plane.h"
#include <wisp/wire.h>
#include "session/session_table.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * struct wisps_control_plane_t - control thread state
 * @listen_fd:          bound+listening WISP_CONTROL_SOCK_PATH socket
 * @stop_read_fd:        poll()'d alongside every other fd; readable once
 *                       wisps_control_plane_stop() has been called
 * @stop_write_fd:       wisps_control_plane_stop() writes one byte here
 * @table:               the session table; touched ONLY on this thread
 * @ring:                shm ring, for wisps_ring_evict_client() on every activation switch;
 *                       not owned
 * @frame_fd:            frame-ready eventfd shared with @dp; not owned. Sent to each 
 *                       client via SCM_RIGHTS on WISP_MSG_ACTIVATE_GRANT
 * @dp:                  data-plane handle, for "set mode" when activation switches to 
 *                       a different mode, and "kick" on every eviction; not owned, may 
 *                       be NULL
 * @chan:                control-query channel shared with the admin thread; not owned,
 *                       must already be initialized
 * @supported_modes:     server-supported modes, in the order the server prefers to
 *                       report them
 * @num_supported_modes: number of valid entries in @supported_modes
 *                       (1..WISP_MAX_MODES)
 * @running:             set false by wisps_control_plane_stop()F
 * @evicting_slot:       slot index currently WISPS_SESSION_EVICTING_COOPERATIVE, or
 *                       -1 if no eviction is in flight
 * @pending_grant_target: slot index to grant once @evicting_slot's wind-down resolves
 *                       (recv STOPPED or grace period elapses), or -1 if no eviction
 *                       is in flight, or if it is but no target is queued behind it.
 *                       Distinct from queue order: an admin-forced switch can name
 *                       this directly, jumping ahead of whatever
 *                       wisps_session_table_find_next_queued() would have picked
 * @eviction_deadline_monotonic: CLOCK_MONOTONIC time by which @evicting_slot must have
 *                       sent STOPPED, else the poll loop force-demotes it
 *                       (EVICTING_FORCED). Only meaningful while @evicting_slot >= 0
 *
 * One instance per server process, run on its own thread via
 * wisps_control_plane_run().
 */
typedef struct {
  int listen_fd;
  int stop_read_fd;
  int stop_write_fd;
  wisps_session_table_t table;
  wisp_shm_ring_t *ring;
  int frame_fd;
  wisps_data_plane_t *dp;
  wisps_control_query_channel_t *chan;
  wisp_resolution_t supported_modes[WISP_MAX_MODES];
  uint32_t num_supported_modes;
  WISP_ATOMIC bool running;
  int evicting_slot;
  int pending_grant_target;
  struct timespec eviction_deadline_monotonic;
} wisps_control_plane_t;

/**
 * wisps_control_plane_init() - create and bind the control listen socket
 * @cp:                  control plane state to populate
 * @ring:                shm ring, created by the server via
 *                       wisps_ring_create(); must outlive @cp
 * @frame_fd:            frame-ready eventfd; must outlive @cp. Both @cp and @dp are 
 *                       peer consumers of it
 * @dp:                  data-plane handle to notify on mode switches/evictions, or
 *                       NULL to skip those notifications entirely
 * @chan:                control-query channel shared with the admin thread; must
 *                       already be initialized and must outlive @cp
 * @supported_modes:     server-supported modes
 * @num_supported_modes: number of entries in @supported_modes (1..WISP_MAX_MODES)
 *
 * Unlinks any stale socket file at WISP_CONTROL_SOCK_PATH left over from a previous
 * run before binding, and initializes an empty session table.
 *
 * Return: 0 on success, -1 on error (socket/bind/listen failure, or
 * @num_supported_modes == 0).
 */
int wisps_control_plane_init(wisps_control_plane_t *cp, wisp_shm_ring_t *ring,
                             int frame_fd, wisps_data_plane_t *dp,
                             wisps_control_query_channel_t *chan,
                             const wisp_resolution_t *supported_modes,
                             uint32_t num_supported_modes);

/**
 * wisps_control_plane_run() - the control thread's entry point
 * @arg: wisps_control_plane_t*, already initialized
 *
 * Runs a single poll() loop over the listen socket, the stop pipe, the control-query
 * channel's wake fd, and every currently-connected producer's control fd, until
 * wisps_control_plane_stop() is called. Also periodically calls
 * wisps_session_table_check_heartbeat_timeouts() (every poll() timeout tick) so a dead
 * ACTIVE producer is evicted even if no fd ever becomes readable again. Intended to be
 * passed directly to pthread_create().
 *
 * Return: always NULL.
 */
void *wisps_control_plane_run(void *arg);

/**
 * wisps_control_plane_stop() - request the loop to end
 * @cp: control plane state
 *
 * Sets @running false and wakes a blocked poll() via the internal stop pipe. Call, then
 * pthread_join() the thread running wisps_control_plane_run().
 */
void wisps_control_plane_stop(wisps_control_plane_t *cp);

/**
 * wisps_control_plane_close() - release all control plane resources
 * @cp: control plane state; call after wisps_control_plane_stop() + pthread_join()
 *
 * Closes the listen socket, every still-open producer fd, the stop pipe, and unlinks
 * the socket path. Does not touch @ring/@dp/@chan -- those are borrowed, not owned.
 */
void wisps_control_plane_close(wisps_control_plane_t *cp);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif // WISPS_CONTROL_PLANE_H
