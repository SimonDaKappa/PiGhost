#pragma once

#include <clock_internal.h>
#include <render_internal.h>
#include <events_internal.h>
#include <wisp/wire.h>
#include <wispc/client.h>

/**
 * enum wispc_state_t - client-side RT lifecycle state machine
 * @WISPC_STATE_DISCONNECTED:       no socket. Initial state, and the state reached
 *                                  after a clean disconnect or an unrecoverable error
 * @WISPC_STATE_CONNECTING:         socket opening + CONNECT sent, awaiting MODE
 * @WISPC_STATE_NEGOTIATED:         mode accepted; idle, not requesting/holding the RT
 *                                  slot
 * @WISPC_STATE_ACTIVATE_REQUESTED: ACTIVATE_REQUEST sent, awaiting GRANT/DENY/QUEUED
 * @WISPC_STATE_QUEUED:             activation deferred; another client holds the RT
 *                                  slot
 * @WISPC_STATE_WARMUP:             granted the RT slot; still non-RT scheduling
 *                                  while the app's warmup callback runs (allocator
 *                                  prewarm, shader JIT, mlockall, dry-run frames)
 * @WISPC_STATE_PROMOTING:          warmup succeeded; wispc is calling
 *                                  sched_setattr()/sched_setscheduler() on its own
 *                                  RT thread(s)
 * @WISPC_STATE_READY_FOR_RT_SENT:  self-promotion done; READY_FOR_RT sent
 * @WISPC_STATE_ACTIVE_RT:          steady state -- render thread spinning,
 *                                  publishing frames, RT scheduling live
 * @WISPC_STATE_EVICT_PENDING:      EVICT_PENDING received (or the app's render
 *                                  callback voluntarily stopped); cooperative
 *                                  wind-down in progress
 * @WISPC_STATE_DEMOTING:           render loop drained; wispc demoting its own RT
 *                                  thread(s) back to non-RT scheduling before
 *                                  reporting STOPPED
 * @WISPC_STATE_STOPPED_ACK_SENT:   demotion done; STOPPED sent
 * @WISPC_STATE_EVICTED_IDLE:       back to non-RT, no longer active; may
 *                                  re-request activation or disconnect
 * @WISPC_STATE_ERROR:              unrecoverable protocol/local error (bad
 *                                  handshake, warmup callback failure, socket
 *                                  reset). Always followed by an automatic
 *                                  transition to DISCONNECTED; the app must call
 *                                  wispc_connect() again to retry
 */
typedef enum {
  WISPC_STATE_DISCONNECTED = 0,
  WISPC_STATE_CONNECTING = 1,
  WISPC_STATE_NEGOTIATED = 2,
  WISPC_STATE_ACTIVATE_REQUESTED = 3,
  WISPC_STATE_QUEUED = 4,
  WISPC_STATE_WARMUP = 5,
  WISPC_STATE_PROMOTING = 6,
  WISPC_STATE_READY_FOR_RT_SENT = 7,
  WISPC_STATE_ACTIVE_RT = 8,
  WISPC_STATE_EVICT_PENDING = 9,
  WISPC_STATE_DEMOTING = 10,
  WISPC_STATE_STOPPED_ACK_SENT = 11,
  WISPC_STATE_EVICTED_IDLE = 12,
  WISPC_STATE_ERROR = 13,
} wispc_state_t;

/**
 * struct _wispc_ctx_t - client session handle implementation
 * @client_id:          client application id
 * @ctrl_fd:            fd to domain socket for control protocol
 * @ring:               frame buffer shared memory
 * @active:             true once granted AND not yet deactivated/evicted
 * @running:            false once we should shut down entirely
 * @granted_generation: current generation number granted by server
 * @mode:               negotiated resolution
 * @frame_fd:           eventfd to signal the server on each publish; received via
 *                      SCM_RIGHTS on WISP_MSG_ACTIVATE_GRANT, -1 until granted
 * @ctrl_thread:        background thread owning the control socket
 * @send_lock:          serializes writes to ctrl_fd from multiple callers
 * @render_kind:       WISP_PAYLOAD_PIXELS until a dmabuf announce is ACKed
 * @dmabuf_ack_state:   0 = pending/none, 1 = accepted, -1 = refused
 * @state:              current position in wispc_state_t; touched ONLY on the
 *                      control thread (both during the synchronous connect()-time
 *                      handshake, before the thread is spawned, and afterward)
 * @callbacks:          app-registered lifecycle hooks from wispc_connect(); every
 *                      field defaults to a no-op except ::warmup (see
 *                      wispc_event_cfg_t), filled in even if the caller passed NULL
 *
 * Opaque to clients/callers. Returned by wispc_connect() and passed to every
 * other wispc_*() call.
 */
struct _wispc_ctx_t {
  char client_id[WISP_CLIENT_ID_LEN];
  int ctrl_fd;
  wisp_shm_ring_t *ring;
  WISP_ATOMIC bool active;
  WISP_ATOMIC bool running;
  WISP_ATOMIC uint32_t granted_generation;
  int frame_fd;
  pthread_t ctrl_thread;
  pthread_mutex_t send_lock;
  wispc_state_t state;
  wispc_renderer_t renderer;
  wispc_event_cfg_t events;
};

/**
 * wispc_shm_attach() - attach to an existing shared ring (client_shm.c)
 * @max_retries:    number of attempts before giving up
 * @retry_delay_ms: delay between attempts, in milliseconds
 *
 * Return: pointer to the mapped ring, or NULL if it never appeared.
 */
wisp_shm_ring_t *wispc_shm_attach(int max_retries, int retry_delay_ms);
