// admin_plane.h - the display's admin socket thread.
//
// Owns PGIPC_ADMIN_SOCK_PATH. Each connection is handled synchronously: accept -> recv
// one admin request -> translate to a pgipc_control_query_t -> submit to the control
// thread via control_query.h -> block for the result -> send the response -> close.
// This thread never touches the session table directly (single-writer invariant).
//
// Must be compiled with LIBPGIPC_READER defined (uses pgipc_ctrl_send/recv, declared
// unconditionally, but this module is display-only in spirit and follows the same build
// convention as the rest of display/).
#ifndef PGIPC_ADMIN_PLANE_H
#define PGIPC_ADMIN_PLANE_H

#include <stdatomic.h>

#include "admin_proto.h"
#include "control/control_query.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * struct pgipc_admin_plane_t - admin thread state
 * @listen_fd:    bound+listening PGIPC_ADMIN_SOCK_PATH socket
 * @stop_read_fd: poll()'d alongside @listen_fd; readable once pgipc_admin_plane_stop()
 *                has been called
 * @stop_write_fd: pgipc_admin_plane_stop() writes one byte here to wake a blocked
 *                 poll()
 * @chan:      shared rendezvous with the control thread (not owned; caller creates it
 *             and passes the same channel to the control plane)
 * @running:   set false by pgipc_admin_plane_stop() to end the accept loop
 *
 * One instance per display process, run on its own thread via pgipc_admin_plane_run().
 *
 * Uses poll() over [listen_fd, stop_read_fd] rather than a directly-blocking accept(),
 * because accept() blocked in one thread does not reliably wake up when another thread
 * closes the listening fd -- that's a well-known Linux hazard (the fd can even get
 * silently reused by an unrelated allocation in the same process before the blocked
 * accept() notices, so the accept() call ends up permanently stuck on the wrong
 * resource). The self-pipe trick used here (same technique as control_query.h's
 * wake_read_fd/wake_write_fd) avoids that entirely.
 */
typedef struct {
  int listen_fd;
  int stop_read_fd;
  int stop_write_fd;
  pgipc_control_query_channel_t *chan;
  atomic_bool running;
} pgipc_admin_plane_t;

/**
 * pgipc_admin_plane_init() - create and bind the admin listen socket
 * @ap:   admin plane state to populate
 * @chan: control-query channel shared with the control-plane thread; must already be
 *        initialized (pgipc_control_query_channel_init()) and must outlive @ap
 *
 * Unlinks any stale socket file at PGIPC_ADMIN_SOCK_PATH left over from a previous run
 * before binding, matching the shm ring/semaphore's own stale-resource-cleanup
 * convention.
 *
 * Return: 0 on success, -1 on error (socket/bind/listen failure).
 */
int pgipc_admin_plane_init(pgipc_admin_plane_t *ap,
                           pgipc_control_query_channel_t *chan);

/**
 * pgipc_admin_plane_run() - the admin thread's entry point
 * @arg: pgipc_admin_plane_t*, already initialized
 *
 * Accepts connections in a loop until pgipc_admin_plane_stop() is called. Intended to
 * be passed directly to pthread_create().
 *
 * Return: always NULL.
 */
void *pgipc_admin_plane_run(void *arg);

/**
 * pgipc_admin_plane_stop() - request the accept loop to end
 * @ap: admin plane state
 *
 * Sets @running false and writes a byte to the internal stop pipe so a blocked poll()
 * wakes up immediately instead of waiting for the next connection (see this struct's
 * doc-comment for why a self-pipe is used instead of relying on accept() unblocking via
 * a closed fd). Call, then pthread_join() the thread running pgipc_admin_plane_run().
 */
void pgipc_admin_plane_stop(pgipc_admin_plane_t *ap);

/**
 * pgipc_admin_plane_close() - release all admin plane resources
 * @ap: admin plane state; call after pgipc_admin_plane_stop() + pthread_join()
 *
 * Closes the listen socket and the stop pipe, and unlinks the socket path.
 */
void pgipc_admin_plane_close(pgipc_admin_plane_t *ap);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif // PGIPC_ADMIN_PLANE_H
