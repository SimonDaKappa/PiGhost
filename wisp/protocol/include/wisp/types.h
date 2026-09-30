#ifndef WISP_TYPES_H
#define WISP_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WISP_CLIENT_ID_LEN 32
#define WISP_NUM_BUFFERS 3
#define WISP_BYTES_PER_PIXEL 4
#define WISP_FRAME_MAX_WIDTH 1280
#define WISP_FRAME_MAX_HEIGHT 720
#define WISP_FRAME_MAX_SIZE                                                            \
  ((size_t)WISP_FRAME_MAX_WIDTH * WISP_FRAME_MAX_HEIGHT * WISP_BYTES_PER_PIXEL)

/**
 * struct wisp_resolution_t - arbitrated resolution
 * @width:  pixels per row
 * @height: rows per frame
 *
 * Always 1:1 with the physical panel; no scaling or interpolation.
 */
typedef struct {
  uint32_t width;
  uint32_t height;
} wisp_resolution_t;

/**
 * typedef wisp_render_kind_t - what a ring slot index refers to
 * @WISP_PAYLOAD_PIXELS: raw bytes in ring->frame_bufs[idx] (default)
 * @WISP_PAYLOAD_DMABUF: the client's announced dmabuf[idx]
 */
typedef uint32_t wisp_render_kind_t;
#define WISP_PAYLOAD_PIXELS ((wisp_render_kind_t)0)
#define WISP_PAYLOAD_DMABUF ((wisp_render_kind_t)1)

/* DRM fourcc helpers, so clients don't need <drm_fourcc.h> just for this. Values match
 * the kernel's DRM_FORMAT_* definitions. */
#define WISP_FOURCC(a, b, c, d)                                                        \
  ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define WISP_FORMAT_XRGB8888 WISP_FOURCC('X', 'R', '2', '4')
#define WISP_FORMAT_ARGB8888 WISP_FOURCC('A', 'R', '2', '4')
#define WISP_MODIFIER_LINEAR ((uint64_t)0) /* DRM_FORMAT_MOD_LINEAR */
#define WISP_MODIFIER_INVALID ((uint64_t)0x00ffffffffffffffULL)

/**
 * struct wisp_dmabuf_desc_t - geometry/format of one announced dmabuf
 * @width:    pixel width; must equal the negotiated mode
 * @height:   pixel height; must equal the negotiated mode
 * @fourcc:   DRM fourcc (WISP_FORMAT_*)
 * @stride:   bytes per row (from gbm_bo_get_stride(); NOT width*4, the allocator may
 *            pad rows)
 * @offset:   byte offset of plane 0 within the dmabuf (usually 0)
 * @modifier: DRM format modifier; use LINEAR unless you know the server's KMS import
 *            path handles the tiled/compressed modifier
 *
 * Single-plane formats only (XRGB8888/ARGB8888 are single-plane; that is all this
 * server needs). Multi-planar YUV etc. is deliberately out of scope.
 */
typedef struct {
  uint32_t width;
  uint32_t height;
  uint32_t fourcc;
  uint32_t stride;
  uint32_t offset;
  uint64_t modifier;
} wisp_dmabuf_desc_t;

#ifdef __cplusplus
} /* extern "C" { */
#endif

#endif /* WISP_TYPES_H */