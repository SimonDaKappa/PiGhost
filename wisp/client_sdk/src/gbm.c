#include "wispc/gbm.h"
#if WISP_READY_FOR_GBM

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "client_internal.h"
#include <wisp/gbm.h>

int wispc_gbm_bufs_create(wispc_gbm_bufs_t *out, int drm_fd, uint32_t width,
                                   uint32_t height) {
  memset(out, 0, sizeof(*out));
  for (int i = 0; i < WISP_NUM_BUFFERS; i++)
    out->fds[i] = -1;

  out->gbm = gbm_create_device(drm_fd);
  if (!out->gbm) {
    fprintf(stderr, "[pgipc] gbm_create_device failed\n");
    return -1;
  }

  for (int i = 0; i < WISP_NUM_BUFFERS; i++) {
    /* LINEAR so the server's KMS import never has to guess a modifier;
     * SCANOUT so the allocation is placed somewhere the CRTC can read. */
    out->bo[i] =
        gbm_bo_create(out->gbm, width, height, GBM_FORMAT_XRGB8888,
                      GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR);

    if (!out->bo[i]) {
      fprintf(stderr, "[pgipc] gbm_bo_create failed for buffer %d\n", i);
      goto fail;
    }

    out->fds[i] = gbm_bo_get_fd(out->bo[i]);
    if (out->fds[i] < 0) {
      fprintf(stderr, "[pgipc] gbm_bo_get_fd failed for buffer %d\n", i);
      goto fail;
    }

    out->desc[i].width = width;
    out->desc[i].height = height;
    out->desc[i].fourcc = WISP_FORMAT_XRGB8888;
    out->desc[i].stride = gbm_bo_get_stride(out->bo[i]);
    out->desc[i].offset = gbm_bo_get_offset(out->bo[i], 0);
    out->desc[i].modifier = gbm_bo_get_modifier(out->bo[i]);
    if (out->desc[i].modifier == WISP_MODIFIER_INVALID)
      out->desc[i].modifier = WISP_MODIFIER_LINEAR; /* we asked for LINEAR */
  }
  return 0;

fail:
  wispc_gbm_bufs_destroy(out);
  return -1;
}

void wispc_gbm_bufs_destroy(wispc_gbm_bufs_t *bufs) {
  if (!bufs)
    return;

  for (int i = 0; i < WISP_NUM_BUFFERS; i++) {
    if (bufs->fds[i] >= 0) {
      close(bufs->fds[i]);
      bufs->fds[i] = -1;
    }

    if (bufs->bo[i]) {
      gbm_bo_destroy(bufs->bo[i]);
      bufs->bo[i] = NULL;
    }
  }

  if (bufs->gbm) {
    gbm_device_destroy(bufs->gbm);
    bufs->gbm = NULL;
  }
}

#endif