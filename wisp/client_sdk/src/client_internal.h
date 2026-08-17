#ifndef CLIENT_INTERNAL_H
#define CLIENT_INTERNAL_H

#include <wisp/wire.h>
#include "wispc/client.h"

/**
 * struct _wispc_ctx_t - client session handle implementation
 * @client_id:             client application id
 * @ctrl_fd:            fd to domain socket for control protocol
 * @ring:               frame buffer shared memory
 * @active:             true once granted AND not yet deactivated/evicted
 * @running:            false once we should shut down entirely
 * @granted_generation: current generation number granted by server
 * @mode:               negotiated resolution/fps once accepted
 * @frame_fd:            eventfd to signal the server on each publish; received via
 *                       SCM_RIGHTS on WISP_MSG_ACTIVATE_GRANT, -1 until granted
 * @ctrl_thread:        background thread owning the control socket
 * @send_lock:          serializes writes to ctrl_fd from multiple callers
 * @payload_kind:       WISP_PAYLOAD_PIXELS until a dmabuf announce is ACKed
 * @dmabuf_ack_state:   0 = pending/none, 1 = accepted, -1 = refused
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
  wisp_render_mode_t mode;
  int frame_fd;
  pthread_t ctrl_thread;
  pthread_mutex_t send_lock;
  WISP_ATOMIC int32_t payload_kind;
  WISP_ATOMIC int32_t dmabuf_ack_state;
};

/**
 * wispc_shm_attach() - attach to an existing shared ring (client_shm.c)
 * @max_retries:    number of attempts before giving up
 * @retry_delay_ms: delay between attempts, in milliseconds
 *
 * Return: pointer to the mapped ring, or NULL if it never appeared.
 */
wisp_shm_ring_t *wispc_shm_attach(int max_retries, int retry_delay_ms);

/**
 * wispc_shm_publish() - publish a finished frame as the newest ready (client_shm.c)
 * @ring:     attached ring
 * @idx:      buffer index previously returned by wispc_write_slot()
 * @frame_id: client-assigned monotonically increasing frame id
 * @now_ns:   timestamp the write completed (CLOCK_MONOTONIC)
 *
 * Low-level primitive; client applications normally call wispc_publish() instead.
 */
void wispc_shm_publish(wisp_shm_ring_t *ring, int idx, uint64_t frame_id,
                       uint64_t now_ns);

#endif /* CLIENT_INTERNAL_H */