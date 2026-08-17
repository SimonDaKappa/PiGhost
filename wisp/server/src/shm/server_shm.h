#ifndef SERVER_SHM_H
#define SERVER_SHM_H

#include <wisp/wire.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * wisps_shm_ring_create() - create the shared ring buffer
 *
 * Creates a new shared memory segment for the frame ring, unlinking any stale segment
 * left over from a previous run first.
 *
 * Return: pointer to the mapped ring, or NULL on error.
 */
wisp_shm_ring_t *wisps_shm_ring_create(void);

/**
 * wisps_shm_ring_destroy() - unmap and unlink the shared ring
 * @ring: ring returned by wisps_shm_ring_create(), may be NULL
 *
 * Call once, at server shutdown.
 */
void wisps_shm_ring_destroy(wisp_shm_ring_t *ring);

/**
 * wisps_shm_frame_fd_create() - create the frame-ready eventfd
 *
 * Created EFD_NONBLOCK | EFD_CLOEXEC, default (non-EFD_SEMAPHORE) counting mode. Only
 * one client is ever ACTIVE at a time, so a single eventfd is created once here and
 * reused across every activation grant: the server sends a copy of it as SCM_RIGHTS
 * ancillary data on every WISP_MSG_ACTIVATE_GRANT (see wisp_ctrl_send_fds()), and the
 * client writes a 1 to it on every publish (see wispc_publish()).
 *
 * Return: the created eventfd, or -1 on error.
 */
int wisps_shm_frame_fd_create(void);

/**
 * wisps_shm_ring_checkout() - claim the newest ready frame for reading
 * @ring: attached/created ring
 *
 * Marks @ring's newest complete frame as server-locked so the client's slot-picking
 * logic will not reuse it out from under the server. Call wisps_shm_ring_release() when
 * done with the buffer (e.g. after finishing the memcpy/page-flip to HDMI).
 *
 * Return: buffer index to read (0..WISP_NUM_BUFFERS-1), or -1 if no frame has been
 * published yet.
 */
int wisps_shm_ring_checkout(wisp_shm_ring_t *ring);

/**
 * wisps_shm_ring_release() - release the buffer claimed by checkout()
 * @ring: attached/created ring
 *
 * Clears the server-locked index back to -1. Safe to call even if no checkout is
 * currently held.
 */
void wisps_shm_ring_release(wisp_shm_ring_t *ring);

/**
 * wisps_evict_client() - evict the current client
 * @ring: attached/created ring
 *
 * Bumps @generation (so any client still holding the old grant sees it is evicted on
 * its next publish) and resets @latest_ready to -1 so a stale slot index from the
 * previous client is never consumed. Call this on every activation switch, regardless
 * of mode. In the dmabuf mode a stale index would otherwise point into the *previous*
 * client's buffer set.
 */
void wisps_evict_client(wisp_shm_ring_t *ring);

/**
 * struct wisps_dmabuf_set_t - server's record of one client's dmabufs
 * @valid: true once populated from a well-formed announce
 * @fds:   server-owned dup'd fds (close via wisps_dmabuf_set_close())
 * @desc:  geometry, index-aligned with @fds and with ring slot indices
 */
typedef struct {
  bool valid;
  int fds[WISP_NUM_BUFFERS];
  wisp_dmabuf_desc_t desc[WISP_NUM_BUFFERS];
} wisps_dmabuf_set_t;

/**
 * wisps_dmabuf_set_from_announce() - validate and adopt an announce message
 * @set:  output set to populate
 * @msg:  the received WISP_MSG_DMABUF_ANNOUNCE payload
 * @fds:  the fds received alongside @msg via SCM_RIGHTS
 * @nfds: number of entries in @fds
 *
 * Does NOT talk to KMS. the server should attempt the
 * drmPrimeFDToHandle()/drmModeAddFB2WithModifiers() import next and send
 * WISP_MSG_DMABUF_ACK with the outcome.
 *
 * Return: 0 and fills @set (taking ownership of @fds) if the announce is well-formed;
 * -1 and closes all @fds otherwise.
 */
int wisps_dmabuf_set_from_announce(wisps_dmabuf_set_t *set,
                                   const wisp_dmabuf_announce_msg_t *msg,
                                   const int *fds, int nfds);

/**
 * wisps_dmabuf_set_close() - close a client's announced dmabuf fds
 * @set: set previously populated by wisps_dmabuf_set_from_announce(), may
 *       be NULL
 *
 * Call when the client's control connection goes away (disconnect, eviction, crash).
 * After this, the server must not scan out any KMS framebuffer it built from these
 * fds.
 */
void wisps_dmabuf_set_close(wisps_dmabuf_set_t *set);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SERVER_SHM_H */
