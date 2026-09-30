#ifndef WISPC_GBM_H
#define WISPC_GBM_H

// $$$SIMON might want to wrap this into render.h::*dmabuf config

#include <wisp/gbm.h>
#if WISP_READY_FOR_GBM

#include <wisp/types.h>
#include "client.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * struct wispc_gbm_bufs_t - a ring's worth of GPU buffers, ready to announce
 * @gbm:  gbm device wrapping the caller's DRM fd (caller keeps the fd open for the
 *        lifetime of this struct)
 * @bo:   the buffer objects; keep alive while rendering into them
 * @fds:  exported dmabuf fds, index-aligned with ring slots
 * @desc: filled-in geometry for wispc_announce_dmabufs()
 */
typedef struct {
  struct gbm_device *gbm;
  struct gbm_bo *bo[WISP_NUM_BUFFERS];
  int fds[WISP_NUM_BUFFERS];
  wisp_dmabuf_desc_t desc[WISP_NUM_BUFFERS];
} wispc_gbm_bufs_t;

/**
 * wispc_announce_dmabufs() - switch this session to the DMABUF mode
 * @ctx:        session handle
 * @fds:        dmabuf fds, index-aligned with ring slots
 * @desc:       geometry for each fd, sized to the negotiated mode
 * @timeout_ms: how long to wait for the server's ack (<0 = wait forever)
 *
 * Call AFTER wispc_connect() (which negotiates the mode you should size the
 * buffers to). Sends the fds + geometry to the server over the control socket
 * (SCM_RIGHTS) and blocks up to @timeout_ms for the server's WISP_MSG_DMABUF_ACK,
 * which is consumed by the background ctrl thread.
 *
 * On success the session's payload kind becomes WISP_PAYLOAD_DMABUF and ring slot
 * indices now refer to your buffers; you may close(fds[i]) afterwards, SCM_RIGHTS gave
 * the server its own references (keep your GBM bos alive to render, of course).
 *
 * Return: 0 on success, -1 on send failure/timeout, -2 if the server refused the
 * import. On refusal/timeout the session stays in PIXELS mode, so a client can fall
 * back to glReadPixels-into-shm if it wants.
 */
WISPC_API int wispc_announce_dmabufs(wispc_ctx_t *ctx, const int fds[WISP_NUM_BUFFERS],
                           const wisp_dmabuf_desc_t desc[WISP_NUM_BUFFERS],
                           int timeout_ms);


/**
 * wispc_gbm_bufs_create() - allocate WISP_NUM_BUFFERS linear scanout buffers
 * @out:    output struct to populate
 * @drm_fd: an open DRM node. Use the card node (/dev/dri/card0|1). The
 *          GBM_BO_USE_SCANOUT flag needs a KMS-capable device; render nodes
 *          (renderD128) may refuse or hand back non-scanout-safe placement
 * @width:  buffer width. Use the negotiated mode's width
 * @height: buffer height. Use the negotiated mode's height
 *
 * Buffers are XRGB8888, LINEAR, RENDERING|SCANOUT usage.
 *
 * Return: 0 on success; -1 on failure with everything already cleaned up.
 */
WISPC_API int wispc_gbm_bufs_create(wispc_gbm_bufs_t *out, int drm_fd, uint32_t width,
                                   uint32_t height);

/**
 * wispc_gbm_bufs_destroy() - free buffers allocated by wispc_gbm_bufs_create()
 * @bufs: struct previously populated by wispc_gbm_bufs_create(), may be NULL
 */
WISPC_API void wispc_gbm_bufs_destroy(wispc_gbm_bufs_t *bufs);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif 

#endif /* WISPC_GBM_H */