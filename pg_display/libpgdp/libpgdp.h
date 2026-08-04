// libpgdp.h - PiGhost IPC: shared-memory frame ring
//              + client control protocol
//              + client convenience API
//              + GPU (dmabuf) payload mode with zero-copy buffer-handle
//                passing over the existing control socket.
//
// STB-style single-file header library.
//
// QUICK START
// -----------
// In exactly ONE .c/.cpp translation unit:
//
//   #define LIBPGDP_IMPLEMENTATION
//   #include "libpgdp.h"
//
// Every other file that needs the types / declarations:
//
//   #include "libpgdp.h"
//
// OPTIONAL: define LIBPGDP_STATIC before the implementation #include to make every
// function definition static (private to that translation unit).
//
// OPTIONAL: define LIBPGDP_ENABLE_GBM before including to compile the GBM
// buffer-allocation convenience helpers (requires <gbm.h> and linking -lgbm). Only GPU
// clients need this; the server service and CPU clients don't.
//
// SYMBOL VISIBILITY: LIBPGDP_CLIENT / LIBPGDP_SERVER
// ---------------------------------------------------------
// This header serves three kinds of translation units:
//
//   - client applications (renderer, slideshow, sine-wave demo, ...)
//   - the single server/reader service
//   - shared test/simulation code that legitimately needs both sides
//
// Wire-format types and the plain (non-fd) control framing functions (pgdp_ctrl_send /
// pgdp_ctrl_recv) are always declared. Both sides need to speak the same protocol.
// Everything else is gated by two feature macros so a client app cannot accidentally
// call server-only APIs (pgdps_shm_ring_create, checkout/release, dmabuf import
// bookkeeping, ...) and vice versa:
//
//   #define LIBPGDP_CLIENT   before #include to get pgdpc_*,
//                              pgdpc__shm_ring_attach/pick_write_slot/publish,
//                              pgdp_ctrl_send_fds/recv_fds, and (with
//                              LIBPGDP_ENABLE_GBM) the GBM helpers.
//
//   #define LIBPGDP_SERVER   before #include to get pgdps_shm_ring_create / destroy /
//                              checkout/release/new_generation, pgdps_frame_fd_create,
//                              pgdp_ctrl_send_fds/recv_fds, and pgdp_dmabuf_set_*
//                              bookkeeping.
//
// The SAME two macros gate both the declaration site (above the LIBPGDP_IMPLEMENTATION
// block) and the implementation site (below it), so defining exactly one of them keeps
// that translation unit free of both the declarations AND the compiled bodies of the
// other side's API.
//
// If a translation unit defines NEITHER macro, both are enabled by default (with a
// one-time #pragma message note) so quick tests/simulators can still just `#include
// "libpgdp.h"` and get the full v1-style surface. Real client and server code should
// define exactly one macro.
//
// PAYLOAD MODES
// -------------
// A client session operates in exactly one of two payload modes:
//
//   PGDP_PAYLOAD_PIXELS (default)
//     Frames are raw pixel bytes written by the CPU directly into
//     ring->frame_bufs[slot].
//
//   PGDP_PAYLOAD_DMABUF
//     Frames live in PGDP_NUM_BUFFERS GPU buffers (GBM bos / dmabufs) allocated by the
//     client. The dmabuf fds are passed ONCE, at session setup, over the control socket
//     via SCM_RIGHTS. After that, the per-frame hot path is *identical* to pixels mode:
//     the shm ring's latest_ready / reader_locked / frame_id / write_ts fields are used
//     exactly the same way. A slot index simply refers to the client's announced
//     dmabuf[idx] instead of ring->frame_bufs[idx]. No pixel data ever crosses the shm
//     segment.
//
// GPU client FLOW
// -----------------
// ```c
//   ctx  = pgdpc_connect("my-client", modes, n);
//   mode = pgdpc_negotiated_mode(ctx);
//
//   // Allocate 3 scanout-capable linear buffers at the negotiated size.
//   // With LIBPGDP_ENABLE_GBM this is one call:
//   pgdpc_gbm_bufs_t gb;
//   pgdpc_gbm_bufs_create(&gb, drm_fd, mode.width, mode.height);
//
//   // Hand the fds to the server (one-time, over the control socket):
//   pgdpc_announce_dmabufs(ctx, gb.fds, gb.desc, 5000);
//
//   // Per frame (same as pixels mode, but render with the GPU):
//   int slot = pgdpc_write_slot(ctx);
//   ... bind FBO wrapping gb.bo[slot], draw, glFinish() ...   // see note (1)
//   pgdpc_publish(ctx, slot, frame_id);
// ```
//
// (1) EXPLICIT SYNC REQUIREMENT: in DMABUF mode the client MUST ensure the GPU has
//     finished writing the buffer before publish(), e.g. glFinish(), or
//     eglClientWaitSync() on a fence created after the last draw call. publish() only
//     flips an index; it cannot know about in-flight GPU work. (Implicit fencing via
//     the dmabuf usually saves you on Pi/KMS, but do not rely on it across drivers.)
//
// Wrapping a dmabuf in a GLES render target (client side), sketch:
// ```c
//   EGLint attrs[] = {
//     EGL_WIDTH,  w,  EGL_HEIGHT, h,
//     EGL_LINUX_DRM_FOURCC_EXT,      PGDP_FORMAT_XRGB8888,
//     EGL_DMA_BUF_PLANE0_FD_EXT,     gb.fds[i],
//     EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)gb.desc[i].offset,
//     EGL_DMA_BUF_PLANE0_PITCH_EXT,  (EGLint)gb.desc[i].stride,
//     EGL_NONE };
//   EGLImage img = eglCreateImageKHR(dpy, EGL_NO_CONTEXT,
//                                    EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
//   glBindRenderbuffer(GL_RENDERBUFFER, rbo[i]);
//   glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, img);
//   ... attach rbo[i] to fbo[i] as GL_COLOR_ATTACHMENT0 ...
// ```
//
// SERVER-SERVICE RESPONSIBILITIES (dmabuf mode)
// ----------------------------------------------
//   * Run its control-socket read loop with pgdp_ctrl_recv_fds() (NOT the plain
//     pgdp_ctrl_recv()), otherwise SCM_RIGHTS fds attached to a
//     PGDP_MSG_DMABUF_ANNOUNCE are silently discarded by the kernel.
//   * On PGDP_MSG_DMABUF_ANNOUNCE: build a pgdps_dmabuf_set_t with
//     pgdps_dmabuf_set_from_announce(), try to import each fd into KMS
//     (drmPrimeFDToHandle + drmModeAddFB2WithModifiers), and reply with
//     PGDP_MSG_DMABUF_ACK (accepted=1) or (accepted=0, reason).
//   * The per-frame read path is unchanged: epoll/read-wait on the frame-ready
//     eventfd, checkout() -> idx,
//     but instead of memcpy'ing ring->frame_bufs[idx], page-flip to the KMS
//     framebuffer imported from that client's dmabuf[idx].
//   * reader_locked semantics: a scanout buffer is "in use" until the flip
//     AWAY from it completes. Keep reader_locked = the currently-scanned-out
//     index for that whole interval, then release/checkout the next.
//   * On client switch/eviction: call pgdps_evict_client()
//     (bumps generation AND resets latest_ready to -1) and flip to a
//     server-owned fallback framebuffer BEFORE closing the evicted
//     client's fds / dropping its KMS fbs - never scan out a buffer you
//     are about to release.
//   * Announced fds belong to the server once received (SCM_RIGHTS dups
//     them); close them with pgdps_dmabuf_set_close() when the client's
//     control connection goes away.
//
// C++ / FFI
// ---------
// All public symbols are wrapped in extern "C" when compiled as C++.

#ifndef LIBPGDP_H
#define LIBPGDP_H

#ifdef LIBPGDP_STATIC
#define PGDP_DEF static
#else
#define PGDP_DEF extern
#endif

/* Default both sides on if the includer didn't pick one, so a bare `#include
 * "libpgdp.h"` still works for quick tests/simulators. Real client/server code should
 * define exactly one of these before the first #include. */
#if !defined(LIBPGDP_CLIENT) && !defined(LIBPGDP_SERVER)
#define LIBPGDP_CLIENT
#define LIBPGDP_SERVER
#if !defined(LIBPGDP_NO_SIDE_WARNING)
#pragma message(                                                                       \
    "[libpgdp] neither LIBPGDP_CLIENT nor LIBPGDP_SERVER was defined "                 \
    "before #include \"libpgdp.h\". Defaulting to both. Define "                       \
    "LIBPGDP_NO_SIDE_WARNING to silence this note, or define exactly one of "          \
    "the two macros.")
#endif
#endif

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/eventfd.h>
#include <time.h>

#if defined(LIBPGDP_ENABLE_GBM) && defined(LIBPGDP_CLIENT)
#include <gbm.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ===========================================================================
// ABI/Cross-language utilities
// ===========================================================================

#ifdef __cplusplus
#define PGDP_SASSERT(c, m) static_assert(c, m)
#else
#define PGDP_SASSERT(c, m) _Static_assert(c, m)
#endif

#ifdef __cplusplus
#define PGDP_ALIGNAS(n) alignas(n)
#define PGDP_ALIGNOF(t) alignof(t)
#else
#define PGDP_ALIGNAS(n) _Alignas(n)
#define PGDP_ALIGNOF(t) _Alignof(t)
#endif

#define PGDP_CAT_(a, b) a##b
#define PGDP_CAT(a, b) PGDP_CAT_(a, b)

#define PGDP_CACHELINE 64
#define PGDP_CACHELINE_FIELD(type, name)                                               \
  union {                                                                              \
    PGDP_ALIGNAS(PGDP_CACHELINE) type name;                                            \
    unsigned char PGDP_CAT(pad_, __LINE__)[PGDP_CACHELINE];                            \
  }

/*
 * Marker for what should be a `atomic_*|_Atomic` type but isn't for cross-language ABI
 * stability and reinterpretation.
 *
 * Plain, fixed-width, non "_Atomic/atomic_*" types. Accessed only through
 * pgdp__a{load,store,fetch_add}_*(), never through <stdatomic.h>'s
 * atomic_load()/atomic_store()/etc, which require an _Atomic-qualified (C) or
 * std::atomic<T> (C++) operand and will not accept these.
 */
#define PGDP_ATOMIC

/*
 * pgdp__atomic_{load,store,fetch_add}_{i32,u32,bool} - the single, unified atomic
 * primitive used throughout, for cross-language ABI stability.
 *
 * <stdatomic.h>'s atomic_load()/atomic_store()/atomic_fetch_add() require a pointer to
 * an _Atomic-qualified type in C and a pointer to std::atomic<T> in C++. Since every
 * field these helpers touch is deliberately a plain, fixed-width, non-_Atomic type (a
 * plain int32_t has one obvious binary layout; an _Atomic int / std::atomic<int> does
 * not, and differs across compilers/languages), <stdatomic.h>'s macros cannot be used
 * on them at all.
 *
 * Instead, every atomic access goes through the GNU/Clang-common __atomic_* builtins,
 * which operate on any object of a matching size/alignment and are supported
 * identically by both compilers in both C and C++.
 *
 * Note the converse also holds and this must be applied uniformly rather than
 * mixed with <stdatomic.h>: __atomic_* builtins reject genuinely
 * _Atomic-qualified/std::atomic<T> operands under Clang (both C and C++) and under GCC
 * in C++ mode. Do not mix them together, and just use the PGDP_ATOMIC flavor.
 */

static inline int32_t pgdp__atomic_load_i32(const int32_t *p) {
  return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}
static inline void pgdp__atomic_store_i32(int32_t *p, int32_t v) {
  __atomic_store_n(p, v, __ATOMIC_SEQ_CST);
}
static inline uint32_t pgdp__atomic_load_u32(const uint32_t *p) {
  return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}
static inline void pgdp__atomic_store_u32(uint32_t *p, uint32_t v) {
  __atomic_store_n(p, v, __ATOMIC_SEQ_CST);
}
static inline uint32_t pgdp__atomic_fetch_add_u32(uint32_t *p, uint32_t v) {
  return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST);
}
static inline bool pgdp__atomic_load_bool(const bool *p) {
  return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}
static inline void pgdp__atomic_store_bool(bool *p, bool v) {
  __atomic_store_n(p, v, __ATOMIC_SEQ_CST);
}

#ifndef __cplusplus

#define pgdp_atomic_load(p)                                                            \
  _Generic((p),                                                                        \
      const int32_t *: pgdp__atomic_load_i32,                                          \
      int32_t *: pgdp__atomic_load_i32,                                                \
      const uint32_t *: pgdp__atomic_load_u32,                                         \
      uint32_t *: pgdp__atomic_load_u32,                                               \
      const bool *: pgdp__atomic_load_bool,                                            \
      bool *: pgdp__atomic_load_bool)(p)

#define pgdp_atomic_store(p, v)                                                        \
  _Generic((p),                                                                        \
      int32_t *: pgdp__atomic_store_i32,                                               \
      uint32_t *: pgdp__atomic_store_u32,                                              \
      bool *: pgdp__atomic_store_bool)((p), (v))

#define pgdp_atomic_fetch_add(p, v)                                                    \
  _Generic((p), uint32_t *: pgdp__atomic_fetch_add_u32)((p), (v))

#else

} /* extern "C" */

static inline int32_t pgdp_atomic_load(const int32_t *p) {
  return pgdp__atomic_load_i32(p);
}
static inline uint32_t pgdp_atomic_load(const uint32_t *p) {
  return pgdp__atomic_load_u32(p);
}
static inline bool pgdp_atomic_load(const bool *p) { return pgdp__atomic_load_bool(p); }

static inline void pgdp_atomic_store(int32_t *p, int32_t v) {
  pgdp__atomic_store_i32(p, v);
}
static inline void pgdp_atomic_store(uint32_t *p, uint32_t v) {
  pgdp__atomic_store_u32(p, v);
}
static inline void pgdp_atomic_store(bool *p, bool v) { pgdp__atomic_store_bool(p, v); }

static inline uint32_t pgdp_atomic_fetch_add(uint32_t *p, uint32_t v) {
  return pgdp__atomic_fetch_add_u32(p, v);
}

extern "C" {

#endif /* __cplusplus */

static inline uint64_t pgdp__ts_diff_ns(struct timespec a, struct timespec b) {
  int64_t sec_diff = (int64_t)a.tv_sec - (int64_t)b.tv_sec;
  int64_t nsec_diff = (int64_t)a.tv_nsec - (int64_t)b.tv_nsec;
  int64_t total = sec_diff * 1000000000LL + nsec_diff;
  return total > 0 ? (uint64_t)total : 0;
}

// ===========================================================================
// Shared-memory frame ring
// ===========================================================================

#define PGDP_SHM_NAME "/frame_ring_shm"

#define PGDP_NUM_BUFFERS 3
#define PGDP_BYTES_PER_PIXEL 4
#define PGDP_FRAME_MAX_WIDTH 1280
#define PGDP_FRAME_MAX_HEIGHT 720
#define PGDP_FRAME_MAX_SIZE                                                            \
  ((size_t)PGDP_FRAME_MAX_WIDTH * PGDP_FRAME_MAX_HEIGHT * PGDP_BYTES_PER_PIXEL)

/**
 * struct pgdp_shm_ring_t - frame buffer shared memory ring for both conn sides
 * @latest_ready:     index of newest complete frame, -1 if none
 * @reader_locked:    index reader currently holds, -1 if none
 * @frame_counter:    monotonically increasing frame id, global
 * @generation:       bumped by server on every activation switch; a client whose
 *                    granted generation no longer matches this has been evicted and
 *                    must stop writing
 * @frame_id:         frame id stamped into each buffer at write time
 * @write_ts_ns:      when each buffer was published (for latency calc)
 * @frame_bufs:       individual frame buffers (PIXELS payload mode only; in the DMABUF
 *                    mode the indices refer to the client's announced dmabuf set)
 * @bookkeeping_lock: cross-process mutex serializing @latest_ready/@reader_locked
 *                    updates. guards bookkeeping only, never the pixel copy
 *
 * Never allocate or copy this struct by value, only ever map it at a fixed address via
 * @pgdps_shm_ring_create() and @pgdpc__shm_ring_attach().
 *
 * @note layout is byte-for-byte identical across payload mode. The dmabuf mode is
 * purely a control-plane extension; slot indices mean the same thing in both modes.
 *
 * @note latest_ready, reader_locked, frame_counter, generation are aligned on separate
 * cache lines to help with the cross process/thread hammering on them at high
 * framerates.
 */
typedef struct {
  PGDP_ATOMIC PGDP_CACHELINE_FIELD(int32_t, latest_ready);
  PGDP_ATOMIC PGDP_CACHELINE_FIELD(int32_t, reader_locked);
  PGDP_ATOMIC PGDP_CACHELINE_FIELD(uint32_t, frame_counter);
  PGDP_ATOMIC PGDP_CACHELINE_FIELD(uint32_t, generation);
  uint64_t frame_id[PGDP_NUM_BUFFERS];
  uint64_t write_ts_ns[PGDP_NUM_BUFFERS];
  unsigned char frame_bufs[PGDP_NUM_BUFFERS][PGDP_FRAME_MAX_SIZE];
  pthread_mutex_t bookkeeping_lock;
} pgdp_shm_ring_t;

PGDP_SASSERT(offsetof(pgdp_shm_ring_t, latest_ready) == 0 * PGDP_CACHELINE,
             "field spacing drift");
PGDP_SASSERT(offsetof(pgdp_shm_ring_t, reader_locked) == 1 * PGDP_CACHELINE,
             "field spacing drift");
PGDP_SASSERT(offsetof(pgdp_shm_ring_t, frame_counter) == 2 * PGDP_CACHELINE,
             "field spacing drift");
PGDP_SASSERT(offsetof(pgdp_shm_ring_t, generation) == 3 * PGDP_CACHELINE,
             "field spacing drift");
PGDP_SASSERT(PGDP_ALIGNOF(pgdp_shm_ring_t) == PGDP_CACHELINE, "ring alignment drift");

/**
 * struct pgdp_render_mode_t - arbitrated resolution/frame rate
 * @width:  pixels per row
 * @height: rows per frame
 * @fps:    frames per second
 *
 * Always 1:1 with the physical panel; no scaling or interpolation.
 */
typedef struct {
  uint32_t width;
  uint32_t height;
  uint32_t fps;
} pgdp_render_mode_t;

/**
 * enum pgdp_payload_kind_t - what a ring slot index refers to
 * @PGDP_PAYLOAD_PIXELS: raw bytes in ring->frame_bufs[idx] (default)
 * @PGDP_PAYLOAD_DMABUF: the client's announced dmabuf[idx]
 */
typedef enum {
  PGDP_PAYLOAD_PIXELS = 0,
  PGDP_PAYLOAD_DMABUF = 1,
} pgdp_payload_kind_t;

/**
 * struct pgdp_dmabuf_desc_t - geometry/format of one announced dmabuf
 * @width:    pixel width; must equal the negotiated mode
 * @height:   pixel height; must equal the negotiated mode
 * @fourcc:   DRM fourcc (PGDP_FORMAT_*)
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
} pgdp_dmabuf_desc_t;

#ifdef LIBPGDP_CLIENT

/**
 * typedef pgdpc_ctx_t - client session handle
 *
 * Opaque; treat as a handle. Returned by pgdpc_connect() and passed to (almost) every
 * other pgdpc_*() call.
 */
typedef struct _pgdpc_ctx_t pgdpc_ctx_t;

#if defined(LIBPGDP_ENABLE_GBM)

/**
 * struct pgdpc_gbm_bufs_t - a ring's worth of GPU buffers, ready to announce
 * @gbm:  gbm device wrapping the caller's DRM fd (caller keeps the fd open for the
 *        lifetime of this struct)
 * @bo:   the buffer objects; keep alive while rendering into them
 * @fds:  exported dmabuf fds, index-aligned with ring slots
 * @desc: filled-in geometry for pgdpc_announce_dmabufs()
 */
typedef struct {
  struct gbm_device *gbm;
  struct gbm_bo *bo[PGDP_NUM_BUFFERS];
  int fds[PGDP_NUM_BUFFERS];
  pgdp_dmabuf_desc_t desc[PGDP_NUM_BUFFERS];
} pgdpc_gbm_bufs_t;

#endif /* LIBPGDP_ENABLE_GBM */

/**
 * pgdpc_connect() - establish a client session with the server
 * @client_id:     this client's application id (truncated to PGDP_CLIENT_ID_LEN-1
 * bytes)
 * @modes:      render modes this client supports, in order of preference
 * @num_modes:  number of entries in @modes (truncated to PGDP_MAX_MODES)
 *
 * Connects to the server's control socket, performs the HELLO/MODE negotiation,
 * requests activation, and (win or lose the first activation race) spawns a background
 * control thread that maintains heartbeats, retries activation while inactive, and
 * processes future grant/deny/deactivate/dmabuf-ack messages for the lifetime of the
 * session.
 *
 * Return: a new session context, or NULL if the control socket could not be reached or
 * the server rejected every offered mode.
 */
PGDP_DEF pgdpc_ctx_t *pgdpc_connect(const char *client_id,
                                    const pgdp_render_mode_t *modes, int num_modes);

/**
 * pgdpc_disconnect() - tear down a client session
 * @ctx: session handle; freed by this call and must not be used again
 *
 * Stops the control thread, sends PGDP_MSG_DISCONNECT, closes the control socket,
 * unmaps the shm ring, and frees @ctx.
 */
PGDP_DEF void pgdpc_disconnect(pgdpc_ctx_t *ctx);

/**
 * pgdpc_is_active() - check whether this session is the client
 * @ctx: session handle
 *
 * Return: true if this client currently holds the activation grant and may
 * write/publish frames.
 */
PGDP_DEF bool pgdpc_is_active(pgdpc_ctx_t *ctx);

/**
 * pgdpc_negotiated_mode() - the mode chosen during connect
 * @ctx: session handle
 *
 * Return: the render mode accepted by the server at connect time.
 */
PGDP_DEF pgdp_render_mode_t pgdpc_negotiated_mode(pgdpc_ctx_t *ctx);

/**
 * pgdpc_announce_dmabufs() - switch this session to the DMABUF mode
 * @ctx:        session handle
 * @fds:        dmabuf fds, index-aligned with ring slots
 * @desc:       geometry for each fd, sized to the negotiated mode
 * @timeout_ms: how long to wait for the server's ack (<0 = wait forever)
 *
 * Call AFTER pgdpc_connect() (which negotiates the mode you should size the
 * buffers to). Sends the fds + geometry to the server over the control socket
 * (SCM_RIGHTS) and blocks up to @timeout_ms for the server's PGDP_MSG_DMABUF_ACK,
 * which is consumed by the background ctrl thread.
 *
 * On success the session's payload kind becomes PGDP_PAYLOAD_DMABUF and ring slot
 * indices now refer to your buffers; you may close(fds[i]) afterwards, SCM_RIGHTS gave
 * the server its own references (keep your GBM bos alive to render, of course).
 *
 * Return: 0 on success, -1 on send failure/timeout, -2 if the server refused the
 * import. On refusal/timeout the session stays in PIXELS mode, so a client can fall
 * back to glReadPixels-into-shm if it wants.
 */
PGDP_DEF int pgdpc_announce_dmabufs(pgdpc_ctx_t *ctx, const int fds[PGDP_NUM_BUFFERS],
                                    const pgdp_dmabuf_desc_t desc[PGDP_NUM_BUFFERS],
                                    int timeout_ms);

/**
 * pgdpc_payload_kind() - which mode this session is operating in
 * @ctx: session handle
 *
 * Return: PGDP_PAYLOAD_PIXELS or PGDP_PAYLOAD_DMABUF.
 */
PGDP_DEF pgdp_payload_kind_t pgdpc_payload_kind(pgdpc_ctx_t *ctx);

/**
 * pgdpc_publish() - publish slot @idx as the newest frame
 * @ctx:      session handle
 * @idx:      buffer index previously returned by pgdpc_write_slot()
 * @frame_id: client-assigned monotonically increasing frame id
 *
 * PIXELS mode: call after your CPU write into ring->frame_bufs[idx] completes.
 * DMABUF mode: call ONLY after GPU work targeting dmabuf[idx] has fully completed
 * (glFinish() or a client-side fence wait). Publish only flips an index and posts the
 * semaphore; it cannot see in-flight GPU work.
 *
 * Drops the frame if @ctx is not active, or if the ring's generation has advanced past
 * @ctx's granted generation. In the latter case this call also flips @ctx to inactive
 * so subsequent callers see pgdpc_is_active() return false without waiting for
 * the next control message.
 */
PGDP_DEF void pgdpc_publish(pgdpc_ctx_t *ctx, int idx, uint64_t frame_id);

/**
 * pgdpc_write_slot() - choose a free buffer index to write
 * @ctx: client session context
 *
 * Picks any index that is neither the currently-published frame (@latest_ready) nor the
 * reader's currently-locked index (@reader_locked), guaranteeing the write never tears
 * a frame the reader (or the flip-away-from logic in dmabuf mode) is using.
 *
 * Return: a writable buffer index (0..PGDP_NUM_BUFFERS-1).
 */
PGDP_DEF int pgdpc_write_slot(pgdpc_ctx_t *ctx);

#if defined(LIBPGDP_ENABLE_GBM)

/**
 * pgdpc_gbm_bufs_create() - allocate PGDP_NUM_BUFFERS linear scanout buffers
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
PGDP_DEF int pgdpc_gbm_bufs_create(pgdpc_gbm_bufs_t *out, int drm_fd, uint32_t width,
                                   uint32_t height);

/**
 * pgdpc_gbm_bufs_destroy() - free buffers allocated by pgdpc_gbm_bufs_create()
 * @bufs: struct previously populated by pgdpc_gbm_bufs_create(), may be NULL
 */
PGDP_DEF void pgdpc_gbm_bufs_destroy(pgdpc_gbm_bufs_t *bufs);

#endif /* LIBPGDP_ENABLE_GBM */

#endif /* LIBPGDP_CLIENT */

#ifdef LIBPGDP_SERVER
/**
 * pgdps_shm_ring_create() - create the shared ring buffer
 *
 * Creates a new shared memory segment for the frame ring, unlinking any stale segment
 * left over from a previous run first.
 *
 * Return: pointer to the mapped ring, or NULL on error.
 */
PGDP_DEF pgdp_shm_ring_t *pgdps_shm_ring_create(void);

/**
 * pgdps_shm_ring_destroy() - unmap and unlink the shared ring
 * @ring: ring returned by pgdps_shm_ring_create(), may be NULL
 *
 * Call once, at server shutdown.
 */
PGDP_DEF void pgdps_shm_ring_destroy(pgdp_shm_ring_t *ring);

/**
 * pgdps_frame_fd_create() - create the frame-ready eventfd
 *
 * Created EFD_NONBLOCK | EFD_CLOEXEC, default (non-EFD_SEMAPHORE) counting mode. Only
 * one client is ever ACTIVE at a time, so a single eventfd is created once here and
 * reused across every activation grant: the server sends a copy of it as SCM_RIGHTS
 * ancillary data on every PGDP_MSG_ACTIVATE_GRANT (see pgdp_ctrl_send_fds()), and the
 * client writes a 1 to it on every publish (see pgdpc_publish()).
 *
 * Return: the created eventfd, or -1 on error.
 */
PGDP_DEF int pgdps_frame_fd_create(void);

/**
 * pgdps_shm_ring_checkout() - claim the newest ready frame for reading
 * @ring: attached/created ring
 *
 * Marks @ring's newest complete frame as reader-locked so the client's slot-picking
 * logic will not reuse it out from under the reader. Call pgdps_shm_ring_release() when
 * done with the buffer (e.g. after finishing the memcpy/page-flip to HDMI).
 *
 * Return: buffer index to read (0..PGDP_NUM_BUFFERS-1), or -1 if no frame has been
 * published yet.
 */
PGDP_DEF int pgdps_shm_ring_checkout(pgdp_shm_ring_t *ring);

/**
 * pgdps_shm_ring_release() - release the buffer claimed by checkout()
 * @ring: attached/created ring
 *
 * Clears the reader-locked index back to -1. Safe to call even if no checkout is
 * currently held.
 */
PGDP_DEF void pgdps_shm_ring_release(pgdp_shm_ring_t *ring);

/**
 * pgdps_evict_client() - evict the current client
 * @ring: attached/created ring
 *
 * Bumps @generation (so any client still holding the old grant sees it is evicted on
 * its next publish) and resets @latest_ready to -1 so a stale slot index from the
 * previous client is never consumed. Call this on every activation switch, regardless
 * of mode. In the dmabuf mode a stale index would otherwise point into the *previous*
 * client's buffer set.
 */
PGDP_DEF void pgdps_evict_client(pgdp_shm_ring_t *ring);
#endif /* LIBPGDP_SERVER */

// ===========================================================================
// Control protocol
// ===========================================================================

#define PGDPS_CONTROL_SOCK_PATH "/dev/shm/frame_ring_control.sock"
#define PGDP_CLIENT_ID_LEN 32
#define PGDP_MAX_MODES 4
#define PGDP_DENY_REASON_LEN 64

/**
 * enum pgdp_msg_type_t - control protocol message types
 * @PGDP_MSG_CONNECT:          client -> server: "here's what I support"
 * @PGDP_MSG_MODE:             server -> client: "render at this mode" / reject
 * @PGDP_MSG_ACTIVATE_REQUEST: client -> server: "let me be the client"
 * @PGDP_MSG_ACTIVATE_GRANT:   server -> client: "you're it, generation N"
 * @PGDP_MSG_ACTIVATE_DENY:    server -> client: "no, and here's why"
 * @PGDP_MSG_HEARTBEAT:        client -> server: "still alive, gen N, frame F"
 * @PGDP_MSG_DEACTIVATE:       server -> client: "stand down, someone else active"
 * @PGDP_MSG_DISCONNECT:       client -> server: "graceful disconnect"
 * @PGDP_MSG_DMABUF_ANNOUNCE:  client -> server: "my frames live in these GPU
 *                              buffers". payload is pgdp_dmabuf_announce_msg_t, and
 *                              exactly PGDP_NUM_BUFFERS dmabuf fds ride alongside in
 *                              SCM_RIGHTS ancillary data. Switches the session to the
 *                              DMABUF payload mode on ACK.
 * @PGDP_MSG_DMABUF_ACK:       server -> client: import succeeded / refused
 */
typedef enum {
  PGDP_MSG_CONNECT = 1,
  PGDP_MSG_MODE = 2,
  PGDP_MSG_ACTIVATE_REQUEST = 3,
  PGDP_MSG_ACTIVATE_GRANT = 4,
  PGDP_MSG_ACTIVATE_DENY = 5,
  PGDP_MSG_HEARTBEAT = 6,
  PGDP_MSG_DEACTIVATE = 7,
  PGDP_MSG_DISCONNECT = 8,
  PGDP_MSG_DMABUF_ANNOUNCE = 9,
  PGDP_MSG_DMABUF_ACK = 10,
} pgdp_msg_type_t;

/**
 * pgdp_ctrl_send() - frame and send one control message
 * @fd:      connected control socket
 * @type:    message type (see pgdp_msg_type_t)
 * @payload: pointer to the message's payload struct, or NULL if @len == 0
 * @len:     size of @payload in bytes
 *
 * Wire format: 1-byte type + 4-byte big-endian length + payload bytes. Blocks until the
 * whole message is written or an error occurs. Used by both sides of the connection for
 * every message type that does not carry fds (for those, see pgdp_ctrl_send_fds()).
 *
 * Return: 0 on success, -1 on error.
 */
PGDP_DEF int pgdp_ctrl_send(int fd, pgdp_msg_type_t type, const void *payload,
                            uint32_t len);

/**
 * pgdp_ctrl_recv() - receive and unframe one control message
 * @fd:       connected control socket
 * @out_type: set to the received message's type
 * @buf:      caller-provided buffer to receive the payload
 * @bufsize:  capacity of @buf in bytes
 * @out_len:  set to the number of payload bytes actually written to @buf
 *
 * Any fds attached via SCM_RIGHTS to a message received through this function (as
 * opposed to pgdp_ctrl_recv_fds()) are silently discarded by the kernel. Do not use
 * this to receive PGDP_MSG_DMABUF_ANNOUNCE.
 *
 * Return: 0 on success, -1 on I/O error or peer disconnect, -2 if the message is larger
 * than @bufsize.
 */
PGDP_DEF int pgdp_ctrl_recv(int fd, pgdp_msg_type_t *out_type, void *buf,
                            uint32_t bufsize, uint32_t *out_len);

/**
 * pgdp_ctrl_send_fds() - send a control message with attached fds
 * @fd:      connected control socket
 * @type:    message type
 * @payload: message payload, or NULL if @len == 0
 * @len:     size of @payload in bytes
 * @fds:     file descriptors to attach via SCM_RIGHTS
 * @nfds:    number of entries in @fds (must be <= PGDP_NUM_BUFFERS)
 *
 * Same wire format as pgdp_ctrl_send(); the fds ride as SCM_RIGHTS ancillary data
 * attached to the framing header. Used in both directions: clients send fds on
 * PGDP_MSG_DMABUF_ANNOUNCE, the server sends the frame-ready eventfd on
 * PGDP_MSG_ACTIVATE_GRANT.
 *
 * Return: 0 on success, -1 on error (including @nfds out of range).
 */
PGDP_DEF int pgdp_ctrl_send_fds(int fd, pgdp_msg_type_t type, const void *payload,
                                uint32_t len, const int *fds, int nfds);

/**
 * pgdp_ctrl_recv_fds() - receive a control message, harvesting any fds
 * @fd:       connected control socket
 * @out_type: set to the received message's type
 * @buf:      caller-provided buffer to receive the payload
 * @bufsize:  capacity of @buf in bytes
 * @out_len:  set to the number of payload bytes actually written to @buf
 * @out_fds:  caller-provided array to receive any SCM_RIGHTS fds. Nullable.
 * @max_fds:  capacity of @out_fds; excess fds are closed to avoid leaks. Zeroable.
 * @out_nfds: set to the number of fds actually written to @out_fds. Zeroable
 *
 * Receiving an fd-bearing message with out_fds = NULL will silently discard. There
 * should be good reason to ignore any possible fds.
 *
 * Return: 0 on success, -1 on error/disconnect, -2 if the message is larger than
 * @bufsize (any harvested fds are closed first).
 */
PGDP_DEF int pgdp_ctrl_recv_fds(int fd, pgdp_msg_type_t *out_type, void *buf,
                                uint32_t bufsize, uint32_t *out_len, int *out_fds,
                                int max_fds, int *out_nfds);

/**
 * struct pgdp_connect_msg_t - PGDP_MSG_CONNECT payload
 * @client_id:    client application id
 * @num_modes: number of pgdp_render_mode_t entries in @modes
 * @modes:     supported render modes, in order of preference
 */
typedef struct {
  char client_id[PGDP_CLIENT_ID_LEN];
  uint32_t num_modes;
  pgdp_render_mode_t modes[PGDP_MAX_MODES];
} pgdp_connect_msg_t;

/**
 * struct pgdp_mode_msg_t - PGDP_MSG_MODE payload
 * @accepted: 1 if the server accepted one of the offered modes, 0 if rejected
 * @chosen:   the mode chosen by the server (only valid if accepted==1)
 */
typedef struct {
  uint8_t accepted;
  pgdp_render_mode_t chosen;
} pgdp_mode_msg_t;

/**
 * struct pgdp_grant_msg_t - PGDP_MSG_ACTIVATE_GRANT payload
 * @generation: the generation number granted to the client; if this no longer matches
 *              the server's generation, the client has been evicted and must stop
 *              writing
 */
typedef struct {
  uint32_t generation;
} pgdp_grant_msg_t;

/**
 * struct pgdp_deny_msg_t - PGDP_MSG_ACTIVATE_DENY payload
 * @reason: human-readable explanation of why the client was denied activation (e.g.
 *          "another app is currently active")
 */
typedef struct {
  char reason[PGDP_DENY_REASON_LEN];
} pgdp_deny_msg_t;

/**
 * struct pgdp_heartbeat_msg_t - PGDP_MSG_HEARTBEAT payload
 * @generation:    the generation number the client believes it holds
 * @frame_counter: the client's (monotonically increasing) current frame
 */
typedef struct {
  uint32_t generation;
  uint64_t frame_counter;
} pgdp_heartbeat_msg_t;

// ===========================================================================
// GPU / dmabuf payload mode
// ===========================================================================

/* DRM fourcc helpers, so clients don't need <drm_fourcc.h> just for this. Values match
 * the kernel's DRM_FORMAT_* definitions. */
#define PGDP_FOURCC(a, b, c, d)                                                        \
  ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define PGDP_FORMAT_XRGB8888 PGDP_FOURCC('X', 'R', '2', '4')
#define PGDP_FORMAT_ARGB8888 PGDP_FOURCC('A', 'R', '2', '4')
#define PGDP_MODIFIER_LINEAR ((uint64_t)0) /* DRM_FORMAT_MOD_LINEAR */
#define PGDP_MODIFIER_INVALID ((uint64_t)0x00ffffffffffffffULL)

/**
 * struct pgdp_dmabuf_announce_msg_t - PGDP_MSG_DMABUF_ANNOUNCE payload
 * @num_buffers: must equal PGDP_NUM_BUFFERS
 * @desc:        per-buffer geometry, index-aligned with the SCM_RIGHTS fds
 *
 * Exactly @num_buffers dmabuf fds MUST accompany this message as SCM_RIGHTS ancillary
 * data (see pgdp_ctrl_send_fds()). fd[i] corresponds to desc[i], and both correspond
 * to ring slot index i.
 */
typedef struct {
  uint32_t num_buffers;
  pgdp_dmabuf_desc_t desc[PGDP_NUM_BUFFERS];
} pgdp_dmabuf_announce_msg_t;

/**
 * struct pgdp_dmabuf_ack_msg_t - PGDP_MSG_DMABUF_ACK payload
 * @accepted: 1 if the server imported all buffers; session is now DMABUF mode
 * @reason:   human-readable refusal reason (valid when accepted==0)
 */
typedef struct {
  uint8_t accepted;
  char reason[PGDP_DENY_REASON_LEN];
} pgdp_dmabuf_ack_msg_t;

#ifdef LIBPGDP_SERVER
/**
 * struct pgdps_dmabuf_set_t - server's record of one client's dmabufs
 * @valid: true once populated from a well-formed announce
 * @fds:   server-owned dup'd fds (close via pgdps_dmabuf_set_close())
 * @desc:  geometry, index-aligned with @fds and with ring slot indices
 */
typedef struct {
  bool valid;
  int fds[PGDP_NUM_BUFFERS];
  pgdp_dmabuf_desc_t desc[PGDP_NUM_BUFFERS];
} pgdps_dmabuf_set_t;

/**
 * pgdps_dmabuf_set_from_announce() - validate and adopt an announce message
 * @set:  output set to populate
 * @msg:  the received PGDP_MSG_DMABUF_ANNOUNCE payload
 * @fds:  the fds received alongside @msg via SCM_RIGHTS
 * @nfds: number of entries in @fds
 *
 * Does NOT talk to KMS. the server should attempt the
 * drmPrimeFDToHandle()/drmModeAddFB2WithModifiers() import next and send
 * PGDP_MSG_DMABUF_ACK with the outcome.
 *
 * Return: 0 and fills @set (taking ownership of @fds) if the announce is well-formed;
 * -1 and closes all @fds otherwise.
 */
PGDP_DEF int pgdps_dmabuf_set_from_announce(pgdps_dmabuf_set_t *set,
                                            const pgdp_dmabuf_announce_msg_t *msg,
                                            const int *fds, int nfds);

/**
 * pgdps_dmabuf_set_close() - close a client's announced dmabuf fds
 * @set: set previously populated by pgdps_dmabuf_set_from_announce(), may
 *       be NULL
 *
 * Call when the client's control connection goes away (disconnect, eviction, crash).
 * After this, the server must not scan out any KMS framebuffer it built from these
 * fds.
 */
PGDP_DEF void pgdps_dmabuf_set_close(pgdps_dmabuf_set_t *set);
#endif /* LIBPGDP_SERVER */

/**
 * PGDPC_HEARTBEAT_INTERVAL_MS - client's HEARTBEAT send period, in ms
 *
 * Shared by both sides (not client-only): the client's ctrl thread sends a
 * heartbeat this often while active; the server's session table uses a
 * multiple of this (see PGDPS_HEARTBEAT_TIMEOUT_MS) to detect a dead ACTIVE
 * client. Declared unconditionally so a LIBPGDP_SERVER-only build (e.g.
 * the server) can reference it without pulling in client-only symbols.
 */
#define PGDPC_HEARTBEAT_INTERVAL_MS 500

/**
 * PGDPS_HEARTBEAT_TIMEOUT_MS - server's ACTIVE-client dead-heartbeat cutoff
 *
 * If an ACTIVE client's last heartbeat is older than this, the server
 * evicts it. Chosen as 2 * PGDPC_HEARTBEAT_INTERVAL_MS to tolerate exactly one dropped
 * heartbeat before acting.
 */
#define PGDPS_HEARTBEAT_TIMEOUT_MS (2 * PGDPC_HEARTBEAT_INTERVAL_MS)

/**
 * PGDPC_RETRY_ACTIVATE_MS - client's timeout between activation requests, in ms
 */
#define PGDPC_RETRY_ACTIVATE_MS 2000

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBPGDP_H */

// ===========================================================================
// IMPLEMENTATION
// ===========================================================================
#if defined(LIBPGDP_IMPLEMENTATION) && !defined(LIBPGDP_IMPLEMENTATION_DONE)
#define LIBPGDP_IMPLEMENTATION_DONE

#ifdef __cplusplus
extern "C" {
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

/**
 * pgdp__send_all() - write() until @len bytes are sent or an error occurs
 * @fd:  where to write
 * @buf: payload to write
 * @len: amount to write from payload
 *
 * Returns 0 on success, -1 on error (EINTR is retried transparently).
 */
static int pgdp__send_all(int fd, const void *buf, size_t len) {
  const unsigned char *p = (const unsigned char *)buf;
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = write(fd, p + sent, len - sent);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    sent += (size_t)n;
  }
  return 0;
}

/**
 * pgdp__recv_all() - read() until @len bytes are received or an error/EOF
 * @fd:  where to read from
 * @buf: where to read into
 * @len: amount to read
 *
 * Returns 0 on success, -1 on error or peer-closed (EINTR retried).
 */
static int pgdp__recv_all(int fd, void *buf, size_t len) {
  unsigned char *p = (unsigned char *)buf;
  size_t got = 0;
  while (got < len) {
    ssize_t n = read(fd, p + got, len - got);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (n == 0)
      return -1; /* peer closed */
    got += (size_t)n;
  }
  return 0;
}

#ifdef LIBPGDP_CLIENT

/**
 * struct _pgdpc_ctx_t - client session handle implementation
 * @client_id:             client application id
 * @ctrl_fd:            fd to domain socket for control protocol
 * @ring:               frame buffer shared memory
 * @active:             true once granted AND not yet deactivated/evicted
 * @running:            false once we should shut down entirely
 * @granted_generation: current generation number granted by server
 * @mode:               negotiated resolution/fps once accepted
 * @frame_fd:            eventfd to signal the server on each publish; received via
 *                       SCM_RIGHTS on PGDP_MSG_ACTIVATE_GRANT, -1 until granted
 * @ctrl_thread:        background thread owning the control socket
 * @send_lock:          serializes writes to ctrl_fd from multiple callers
 * @payload_kind:       PGDP_PAYLOAD_PIXELS until a dmabuf announce is ACKed
 * @dmabuf_ack_state:   0 = pending/none, 1 = accepted, -1 = refused
 *
 * Opaque to clients/callers. Returned by pgdpc_connect() and passed to every
 * other pgdpc_*() call.
 */
struct _pgdpc_ctx_t {
  char client_id[PGDP_CLIENT_ID_LEN];
  int ctrl_fd;
  pgdp_shm_ring_t *ring;
  PGDP_ATOMIC bool active;
  PGDP_ATOMIC bool running;
  PGDP_ATOMIC uint32_t granted_generation;
  pgdp_render_mode_t mode;
  int frame_fd;
  pthread_t ctrl_thread;
  pthread_mutex_t send_lock;
  PGDP_ATOMIC int32_t payload_kind;
  PGDP_ATOMIC int32_t dmabuf_ack_state;
};

/**
 * pgdpc__shm_ring_attach() - attach to an existing shared ring
 * @max_retries:    number of attempts before giving up
 * @retry_delay_ms: delay between attempts, in milliseconds
 *
 * The server service creates the ring before any client starts, so a client racing
 * server startup should retry rather than fail immediately.
 *
 * Return: pointer to the mapped ring, or NULL if it never appeared.
 */
PGDP_DEF pgdp_shm_ring_t *pgdpc__shm_ring_attach(int max_retries, int retry_delay_ms) {
  int fd = -1;

  for (int i = 0; i < max_retries; i++) {
    fd = shm_open(PGDP_SHM_NAME, O_RDWR, 0666);
    if (fd >= 0)
      break;
    struct timespec ts = {.tv_sec = retry_delay_ms / 1000,
                          .tv_nsec = (retry_delay_ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
  }

  if (fd < 0) {
    fprintf(stderr, "[pgipc] gave up waiting for shm segment %s\n", PGDP_SHM_NAME);
    return NULL;
  }

  pgdp_shm_ring_t *ring = (pgdp_shm_ring_t *)mmap(
      NULL, sizeof(pgdp_shm_ring_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  close(fd);

  if (ring == MAP_FAILED) {
    perror("mmap");
    return NULL;
  }
  return ring;
}

/**
 * pgdpc__handle_grant() - shared grant-processing path used by both the synchronous
 * connect()-time handshake and the async ctrl thread.
 * @ctx:       session handle
 * @grant:     grant for session
 * @frame_fd:  frame-ready eventfd received via SCM_RIGHTS alongside @grant, or -1 if
 *             none arrived (treated as a malformed grant)
 *
 * Attaches the shm ring and adopts @frame_fd BEFORE flipping @ctx->active so no caller
 * can observe active==true with a NULL ring / invalid frame_fd (see note on
 * pgdpc_is_active()). Closes any previously-held frame_fd first, since a
 * re-grant (e.g. after being deactivated and reactivated) carries a fresh fd.
 *
 * Returns 0 on success, -1 if shm attachment failed or @frame_fd < 0 (caller should
 * treat the session as unusable).
 */
static int pgdpc__handle_grant(pgdpc_ctx_t *ctx, const pgdp_grant_msg_t *grant,
                               int frame_fd) {
  if (!ctx->ring)
    ctx->ring = pgdpc__shm_ring_attach(50, 100);

  if (!ctx->ring || frame_fd < 0) {
    fprintf(stderr, "[pgipc:%s] could not attach to shm ring or frame-ready eventfd\n",
            ctx->client_id);
    if (frame_fd >= 0)
      close(frame_fd);
    return -1;
  }

  if (ctx->frame_fd >= 0)
    close(ctx->frame_fd);
  ctx->frame_fd = frame_fd;

  pgdp_atomic_store(&ctx->granted_generation, grant->generation);
  pgdp_atomic_store(&ctx->active, true);

  printf("[pgipc:%s] ACTIVATED, generation=%u\n", ctx->client_id, grant->generation);
  fflush(stdout);
  return 0;
}
#endif /* LIBPGDP_CLIENT */

// ---------------------------------------------------------------------------
// shm ring implementation
// ---------------------------------------------------------------------------

/**
 * pgdp__shm_ring_lock() - acquire @ring's bookkeeping mutex
 * @ring: attached/created ring
 *
 * Recovers from a prior holder crashing (EOWNERDEAD) instead of deadlocking.
 */
static void pgdp__shm_ring_lock(pgdp_shm_ring_t *ring) {
  int rc = pthread_mutex_lock(&ring->bookkeeping_lock);
  if (rc == EOWNERDEAD) {
    pthread_mutex_consistent(&ring->bookkeeping_lock);
  }
}

/**
 * pgdp__shm_ring_unlock() - release @ring's bookkeeping mutex
 * @ring: attached/created ring
 */
static void pgdp__shm_ring_unlock(pgdp_shm_ring_t *ring) {
  pthread_mutex_unlock(&ring->bookkeeping_lock);
}

#ifdef LIBPGDP_SERVER

PGDP_DEF pgdp_shm_ring_t *pgdps_shm_ring_create(void) {
  if (!__atomic_always_lock_free(sizeof(int32_t), 0) ||
      !__atomic_always_lock_free(sizeof(uint32_t), 0)) {
    fprintf(stderr, "[pgipc] WARNING: 32-bit atomics are not always lock-free on this "
                    "platform; cross-process atomics may not work as intended.\n");
  }

  shm_unlink(PGDP_SHM_NAME);

  int fd = shm_open(PGDP_SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0666);
  if (fd < 0) {
    perror("shm_open (create)");
    return NULL;
  }

  if (ftruncate(fd, (off_t)sizeof(pgdp_shm_ring_t)) != 0) {
    perror("ftruncate");
    close(fd);
    return NULL;
  }

  pgdp_shm_ring_t *ring = (pgdp_shm_ring_t *)mmap(
      NULL, sizeof(pgdp_shm_ring_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  close(fd);
  if (ring == MAP_FAILED) {
    perror("mmap");
    return NULL;
  }

  memset(ring, 0, sizeof(pgdp_shm_ring_t));
  pgdp_atomic_store(&ring->latest_ready, -1);
  pgdp_atomic_store(&ring->reader_locked, -1);
  pgdp_atomic_store(&ring->frame_counter, 0);
  pgdp_atomic_store(&ring->generation, 0);

  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
  pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
  pthread_mutex_init(&ring->bookkeeping_lock, &attr);
  pthread_mutexattr_destroy(&attr);

  return ring;
}

PGDP_DEF void pgdps_shm_ring_destroy(pgdp_shm_ring_t *ring) {
  if (ring) {
    pthread_mutex_destroy(&ring->bookkeeping_lock);
    munmap(ring, sizeof(pgdp_shm_ring_t));
  }
  shm_unlink(PGDP_SHM_NAME);
}

PGDP_DEF int pgdps_frame_fd_create(void) {
  int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (fd < 0)
    perror("eventfd (frame_fd create)");
  return fd;
}

PGDP_DEF int pgdps_shm_ring_checkout(pgdp_shm_ring_t *ring) {
  pgdp__shm_ring_lock(ring);
  int idx = pgdp_atomic_load(&ring->latest_ready);
  if (idx >= 0)
    pgdp_atomic_store(&ring->reader_locked, idx);
  pgdp__shm_ring_unlock(ring);
  return idx;
}

PGDP_DEF void pgdps_shm_ring_release(pgdp_shm_ring_t *ring) {
  pgdp__shm_ring_lock(ring);
  pgdp_atomic_store(&ring->reader_locked, -1);
  pgdp__shm_ring_unlock(ring);
}

PGDP_DEF void pgdps_evict_client(pgdp_shm_ring_t *ring) {
  pgdp__shm_ring_lock(ring);
  pgdp_atomic_fetch_add(&ring->generation, 1);
  pgdp_atomic_store(&ring->latest_ready, -1);
  pgdp__shm_ring_unlock(ring);
}
#endif /* LIBPGDP_SERVER */

#ifdef LIBPGDP_CLIENT

PGDP_DEF int pgdpc_write_slot(pgdpc_ctx_t *ctx) {

  pgdp__shm_ring_lock(ctx->ring);
  int ready = pgdp_atomic_load(&ctx->ring->latest_ready);
  int locked = pgdp_atomic_load(&ctx->ring->reader_locked);

  int slot = 0;
  for (int i = 0; i < PGDP_NUM_BUFFERS; i++) {
    if (i != ready && i != locked) {
      slot = i;
      break;
    }
  }

  pgdp__shm_ring_unlock(ctx->ring);
  return slot;
}

/**
 * pgdpc__shm_ring_publish() - publish a finished frame as the newest ready
 * @ring:     attached ring
 * @idx:      buffer index previously returned by pgdpc_write_slot()
 * @frame_id: client-assigned monotonically increasing frame id
 * @now_ns:   timestamp the write completed (CLOCK_MONOTONIC), used by the server for
 *            latency accounting
 *
 * Low-level primitive; client applications normally call pgdpc_publish()
 * instead, which also handles the semaphore post and generation/eviction check.
 */
PGDP_DEF void pgdpc__shm_ring_publish(pgdp_shm_ring_t *ring, int idx, uint64_t frame_id,
                                      uint64_t now_ns) {
  ring->frame_id[idx] = frame_id;
  ring->write_ts_ns[idx] = now_ns;
  pgdp__shm_ring_lock(ring);
  pgdp_atomic_store(&ring->latest_ready, idx);
  pgdp__shm_ring_unlock(ring);
}
#endif /* LIBPGDP_CLIENT */

// ---------------------------------------------------------------------------
// Control protocol framing (shared by both sides)
// ---------------------------------------------------------------------------

PGDP_DEF int pgdp_ctrl_send(int fd, pgdp_msg_type_t type, const void *payload,
                            uint32_t len) {
  unsigned char hdr[5];
  uint32_t nlen = htonl(len);

  hdr[0] = (unsigned char)type;
  memcpy(hdr + 1, &nlen, sizeof(hdr) - 1);

  if (pgdp__send_all(fd, hdr, sizeof(hdr)) != 0)
    return -1;

  if (len > 0 && pgdp__send_all(fd, payload, len) != 0)
    return -1;

  return 0;
}

PGDP_DEF int pgdp_ctrl_recv(int fd, pgdp_msg_type_t *out_type, void *buf,
                            uint32_t bufsize, uint32_t *out_len) {
  unsigned char hdr[5];
  uint32_t nlen;

  if (pgdp__recv_all(fd, hdr, sizeof(hdr)) != 0)
    return -1;

  *out_type = (pgdp_msg_type_t)hdr[0];

  memcpy(&nlen, hdr + 1, 4);
  uint32_t len = ntohl(nlen);

  if (len > bufsize)
    return -2;
  if (len > 0 && pgdp__recv_all(fd, buf, len) != 0)
    return -1;

  *out_len = len;
  return 0;
}

// ---------------------------------------------------------------------------
// fd-carrying framing (SCM_RIGHTS)
// ---------------------------------------------------------------------------

PGDP_DEF int pgdp_ctrl_send_fds(int fd, pgdp_msg_type_t type, const void *payload,
                                uint32_t len, const int *fds, int nfds) {
  unsigned char hdr[5];
  uint32_t nlen = htonl(len);
  hdr[0] = (unsigned char)type;
  memcpy(hdr + 1, &nlen, sizeof(hdr) - 1);

  if (nfds <= 0)
    return pgdp_ctrl_send(fd, type, payload, len);
  if (nfds > PGDP_NUM_BUFFERS)
    return -1;

  /* Ancillary data must ride with actual bytes; attach it to the header. */
  struct iovec iov = {.iov_base = hdr, .iov_len = sizeof(hdr)};
  union { /* aligned cmsg buffer */
    char buf[CMSG_SPACE(sizeof(int) * PGDP_NUM_BUFFERS)];
    struct cmsghdr align;
  } u;
  memset(&u, 0, sizeof(u));

  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = u.buf;
  msg.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)nfds);

  struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
  c->cmsg_level = SOL_SOCKET;
  c->cmsg_type = SCM_RIGHTS;
  c->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nfds);
  memcpy(CMSG_DATA(c), fds, sizeof(int) * (size_t)nfds);

  ssize_t n;
  do {
    n = sendmsg(fd, &msg, 0);
  } while (n < 0 && errno == EINTR);
  if (n < 0)
    return -1;

  /* Extremely unlikely for a 5-byte header on SOCK_STREAM, but finish it (the cmsg was
   * already consumed with the first byte). */
  if ((size_t)n < sizeof(hdr) &&
      pgdp__send_all(fd, hdr + n, sizeof(hdr) - (size_t)n) != 0)
    return -1;

  if (len > 0 && pgdp__send_all(fd, payload, len) != 0)
    return -1;

  return 0;
}

/**
 * pgdp__close_fds() - close every valid (>=0) fd in @fds [0..nfds).
 * @fds:  file descriptors to close
 * @nfds: number of fds
 *
 * Used by fd-receiving/bookkeeping paths to avoid leaking fds on error or after a
 * dmabuf set is retired.
 */
static void pgdp__close_fds(int *fds, int nfds) {
  for (int i = 0; i < nfds; i++)
    if (fds[i] >= 0)
      close(fds[i]);
}

PGDP_DEF int pgdp_ctrl_recv_fds(int fd, pgdp_msg_type_t *out_type, void *buf,
                                uint32_t bufsize, uint32_t *out_len, int *out_fds,
                                int max_fds, int *out_nfds) {
  unsigned char hdr[5];
  uint32_t nlen, len;

  /* Handle nullable fds to ignore incoming on recv (basically subset functionality so
   * it functions like pgdp_ctrl_recv() ) $$$TODO SIMON */
  // int *fds, *nfds;
  // if (out_fds)
  //   fds = out_fds;
  // if (out_nfds)
  //   nfds = out_nfds;

  *out_nfds = 0;

  struct iovec iov = {.iov_base = hdr, .iov_len = sizeof(hdr)};
  union {
    char buf[CMSG_SPACE(sizeof(int) * PGDP_NUM_BUFFERS)];
    struct cmsghdr align;
  } u;
  memset(&u, 0, sizeof(u));

  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = u.buf;
  msg.msg_controllen = sizeof(u.buf);

  ssize_t n;
  do {
    n = recvmsg(fd, &msg, 0);
  } while (n < 0 && errno == EINTR);
  if (n <= 0)
    return -1; /* error, timeout (EAGAIN), or peer closed */

  /* Harvest any passed fds (they arrive with the first chunk). */
  for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
      int cnt = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
      const unsigned char *src = CMSG_DATA(c);

      for (int i = 0; i < cnt; i++) {
        int newfd;
        memcpy(&newfd, src + (size_t)i * sizeof(int), sizeof(int));
        if (*out_nfds < max_fds)
          out_fds[(*out_nfds)++] = newfd;
        else
          close(newfd); /* more fds than caller can take: don't leak */
      }
    }
  }

  if ((size_t)n < sizeof(hdr) &&
      pgdp__recv_all(fd, hdr + n, sizeof(hdr) - (size_t)n) != 0)
    goto fail;

  *out_type = (pgdp_msg_type_t)hdr[0];

  memcpy(&nlen, hdr + 1, 4);
  len = ntohl(nlen);

  if (len > bufsize) {
    pgdp__close_fds(out_fds, *out_nfds);
    *out_nfds = 0;
    return -2;
  }
  if (len > 0 && pgdp__recv_all(fd, buf, len) != 0)
    goto fail;

  *out_len = len;
  return 0;

fail:
  pgdp__close_fds(out_fds, *out_nfds);
  *out_nfds = 0;
  return -1;
}

// ---------------------------------------------------------------------------
// server-side dmabuf bookkeeping
// ---------------------------------------------------------------------------

#ifdef LIBPGDP_SERVER
PGDP_DEF int pgdps_dmabuf_set_from_announce(pgdps_dmabuf_set_t *set,
                                            const pgdp_dmabuf_announce_msg_t *msg,
                                            const int *fds, int nfds) {
  memset(set, 0, sizeof(*set));
  for (int i = 0; i < PGDP_NUM_BUFFERS; i++)
    set->fds[i] = -1;

  if (msg->num_buffers != PGDP_NUM_BUFFERS || nfds != PGDP_NUM_BUFFERS) {
    int tmp[PGDP_NUM_BUFFERS];

    for (int i = 0; i < nfds && i < PGDP_NUM_BUFFERS; i++)
      tmp[i] = fds[i];

    pgdp__close_fds(tmp, nfds < PGDP_NUM_BUFFERS ? nfds : PGDP_NUM_BUFFERS);
    return -1;
  }

  for (int i = 0; i < PGDP_NUM_BUFFERS; i++) {
    const pgdp_dmabuf_desc_t *d = &msg->desc[i];

    /* stride is in bytes; must cover the row */
    if (fds[i] < 0 || d->width == 0 || d->height == 0 || d->stride == 0 ||
        d->stride < d->width) {
      pgdp__close_fds((int *)fds, PGDP_NUM_BUFFERS);
      return -1;
    }
  }

  for (int i = 0; i < PGDP_NUM_BUFFERS; i++) {
    set->fds[i] = fds[i];
    set->desc[i] = msg->desc[i];
  }

  set->valid = true;
  return 0;
}

PGDP_DEF void pgdps_dmabuf_set_close(pgdps_dmabuf_set_t *set) {
  if (!set)
    return;

  pgdp__close_fds(set->fds, PGDP_NUM_BUFFERS);

  for (int i = 0; i < PGDP_NUM_BUFFERS; i++)
    set->fds[i] = -1;
  set->valid = false;
}
#endif /* LIBPGDP_SERVER */

// ---------------------------------------------------------------------------
// client API
// ---------------------------------------------------------------------------

#ifdef LIBPGDP_CLIENT

/**
 * pgdp__client_ctrl - background ctrl thread for clients.
 * @arg: client session handle
 *
 * While client is running, send heartbeat periodically.
 * While client is active,
 */
static void *pgdp__client_ctrl(void *arg) {
  pgdpc_ctx_t *ctx = (pgdpc_ctx_t *)arg;
  struct timespec last_heartbeat = {0};
  struct timespec last_activate_attempt = {0};
  struct timeval tv = {.tv_sec = 0, .tv_usec = PGDPC_HEARTBEAT_INTERVAL_MS * 1000};

  setsockopt(ctx->ctrl_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  while (pgdp_atomic_load(&ctx->running)) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    long since_hb_ms = (now.tv_sec - last_heartbeat.tv_sec) * 1000 +
                       (now.tv_nsec - last_heartbeat.tv_nsec) / 1000000;

    if (pgdp_atomic_load(&ctx->active) && since_hb_ms >= PGDPC_HEARTBEAT_INTERVAL_MS) {
      pgdp_heartbeat_msg_t hb = {
          .generation = pgdp_atomic_load(&ctx->granted_generation),
          .frame_counter = ctx->ring ? pgdp_atomic_load(&ctx->ring->frame_counter) : 0,
      };

      pthread_mutex_lock(&ctx->send_lock);
      pgdp_ctrl_send(ctx->ctrl_fd, PGDP_MSG_HEARTBEAT, &hb, sizeof(hb));
      pthread_mutex_unlock(&ctx->send_lock);
      last_heartbeat = now;
    }

    long since_req_ms = (now.tv_sec - last_activate_attempt.tv_sec) * 1000 +
                        (now.tv_nsec - last_activate_attempt.tv_nsec) / 1000000;

    if (!pgdp_atomic_load(&ctx->active) && since_req_ms >= PGDPC_RETRY_ACTIVATE_MS) {
      pthread_mutex_lock(&ctx->send_lock);
      pgdp_ctrl_send(ctx->ctrl_fd, PGDP_MSG_ACTIVATE_REQUEST, NULL, 0);
      pthread_mutex_unlock(&ctx->send_lock);
      last_activate_attempt = now;
    }

    pgdp_msg_type_t type;
    unsigned char buf[256];
    uint32_t len;
    int fds[1];
    int nfds = 0;

    int rc =
        pgdp_ctrl_recv_fds(ctx->ctrl_fd, &type, buf, sizeof(buf), &len, fds, 1, &nfds);
    if (rc == -1)
      continue; /* timeout or disconnect */
    if (rc == -2)
      continue; /* oversized message */

    switch (type) {
    case PGDP_MSG_ACTIVATE_GRANT: {
      pgdp_grant_msg_t grant;

      memcpy(&grant, buf, sizeof(grant));
      /* Shared helper attaches the shm ring and adopts the received frame-ready
       * eventfd BEFORE setting active=true, so a concurrent pgdpc_is_active()
       * can never observe active with a NULL ring / invalid frame_fd. */
      pgdpc__handle_grant(ctx, &grant, nfds > 0 ? fds[0] : -1);
      break;
    }
    case PGDP_MSG_ACTIVATE_DENY: {
      pgdp_deny_msg_t deny;

      memcpy(&deny, buf, sizeof(deny));
      printf("[pgipc:%s] activation denied: %s (will retry)\n", ctx->client_id,
             deny.reason);
      fflush(stdout);
      break;
    }
    case PGDP_MSG_DEACTIVATE:
      pgdp_atomic_store(&ctx->active, false);
      printf("[pgipc:%s] DEACTIVATED (another app took over)\n", ctx->client_id);
      fflush(stdout);
      break;
    case PGDP_MSG_DMABUF_ACK: {
      pgdp_dmabuf_ack_msg_t ack;

      memcpy(&ack, buf, sizeof(ack) < len ? sizeof(ack) : len);

      if (ack.accepted) {
        pgdp_atomic_store(&ctx->payload_kind, PGDP_PAYLOAD_DMABUF);
        pgdp_atomic_store(&ctx->dmabuf_ack_state, 1);
        printf("[pgipc:%s] dmabuf set accepted; session is now zero-copy\n",
               ctx->client_id);
      } else {
        ack.reason[PGDP_DENY_REASON_LEN - 1] = '\0';
        pgdp_atomic_store(&ctx->dmabuf_ack_state, -1);
        printf("[pgipc:%s] dmabuf set refused: %s (staying in PIXELS mode)\n",
               ctx->client_id, ack.reason);
      }
      fflush(stdout);
      break;
    }
    default:
      break;
    }
  }
  return NULL;
}
#endif /* LIBPGDP_CLIENT */

#ifdef LIBPGDP_CLIENT
PGDP_DEF pgdpc_ctx_t *pgdpc_connect(const char *client_id,
                                    const pgdp_render_mode_t *modes, int num_modes) {
  pgdpc_ctx_t *ctx = (pgdpc_ctx_t *)calloc(1, sizeof(pgdpc_ctx_t));
  if (!ctx)
    return NULL;

  strncpy(ctx->client_id, client_id, PGDP_CLIENT_ID_LEN - 1);
  pthread_mutex_init(&ctx->send_lock, NULL);
  pgdp_atomic_store(&ctx->running, true);
  pgdp_atomic_store(&ctx->active, false);
  pgdp_atomic_store(&ctx->payload_kind, PGDP_PAYLOAD_PIXELS);
  pgdp_atomic_store(&ctx->dmabuf_ack_state, 0);
  ctx->frame_fd = -1;

  int fd = -1;
  for (int i = 0; i < 100; i++) {
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
      break;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, PGDPS_CONTROL_SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
      break;

    close(fd);
    fd = -1;

    struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000L};
    nanosleep(&ts, NULL);
  }

  if (fd < 0) {
    fprintf(stderr, "[pgipc:%s] could not connect to server control socket\n",
            client_id);
    free(ctx);
    return NULL;
  }
  ctx->ctrl_fd = fd;

  pgdp_connect_msg_t connect;
  memset(&connect, 0, sizeof(connect));
  strncpy(connect.client_id, ctx->client_id, PGDP_CLIENT_ID_LEN - 1);
  connect.num_modes =
      (uint32_t)(num_modes > PGDP_MAX_MODES ? PGDP_MAX_MODES : num_modes);
  for (uint32_t i = 0; i < connect.num_modes; i++)
    connect.modes[i] = modes[i];
  pgdp_ctrl_send(ctx->ctrl_fd, PGDP_MSG_CONNECT, &connect, sizeof(connect));

  pgdp_msg_type_t type;
  unsigned char buf[256];
  uint32_t len;

  if (pgdp_ctrl_recv(ctx->ctrl_fd, &type, buf, sizeof(buf), &len) == 0 &&
      type == PGDP_MSG_MODE) {
    pgdp_mode_msg_t mode;

    memcpy(&mode, buf, sizeof(mode));
    if (!mode.accepted) {
      fprintf(stderr, "[pgipc:%s] server rejected all offered modes\n", client_id);
      close(fd);
      pthread_mutex_destroy(&ctx->send_lock);
      free(ctx);
      return NULL;
    }

    ctx->mode = mode.chosen;
    printf("[pgipc:%s] negotiated mode: %ux%u@%ufps\n", client_id, mode.chosen.width,
           mode.chosen.height, mode.chosen.fps);
  }

  type = (pgdp_msg_type_t)-1;
  len = (uint32_t)-1;
  memset(buf, 0, sizeof(buf));

  /* --- Initial activation handshake (synchronous) ----------------------- */
  /* Send the first request here so the ctrl thread starts in a known state. */
  pgdp_ctrl_send(ctx->ctrl_fd, PGDP_MSG_ACTIVATE_REQUEST, NULL, 0);

  int grant_fds[1];
  int grant_nfds = 0;

  if (pgdp_ctrl_recv_fds(ctx->ctrl_fd, &type, buf, sizeof(buf), &len, grant_fds, 1,
                         &grant_nfds) == 0) {
    if (type == PGDP_MSG_ACTIVATE_GRANT) {
      pgdp_grant_msg_t grant;

      memcpy(&grant, buf, sizeof(grant));

      /* Same helper the ctrl thread uses for later grants -- keeps the
       * attach-before-active ordering in exactly one place. */
      if (pgdpc__handle_grant(ctx, &grant, grant_nfds > 0 ? grant_fds[0] : -1) != 0) {
        close(fd);
        pthread_mutex_destroy(&ctx->send_lock);
        free(ctx);
        return NULL;
      }
    } else if (type == PGDP_MSG_ACTIVATE_DENY) {
      pgdp_deny_msg_t deny;

      memcpy(&deny, buf, sizeof(deny));

      /* Resources stay NULL; ctrl thread will retry and attach on grant. */
      printf("[pgipc:%s] initial activation denied: %s (ctrl thread will "
             "retry)\n",
             ctx->client_id, deny.reason);
      fflush(stdout);
    }
    /* Any other message (e.g. unexpected MSG_MODE) is ignored; the ctrl
     * thread will recover via the periodic retry logic. */
  }

  pthread_create(&ctx->ctrl_thread, NULL, pgdp__client_ctrl, ctx);
  return ctx;
}

PGDP_DEF bool pgdpc_is_active(pgdpc_ctx_t *ctx) {
  return pgdp_atomic_load(&ctx->active);
}

PGDP_DEF pgdp_render_mode_t pgdpc_negotiated_mode(pgdpc_ctx_t *ctx) {
  return ctx->mode;
}

PGDP_DEF pgdp_payload_kind_t pgdpc_payload_kind(pgdpc_ctx_t *ctx) {
  return (pgdp_payload_kind_t)pgdp_atomic_load(&ctx->payload_kind);
}

PGDP_DEF int pgdpc_announce_dmabufs(pgdpc_ctx_t *ctx, const int fds[PGDP_NUM_BUFFERS],
                                    const pgdp_dmabuf_desc_t desc[PGDP_NUM_BUFFERS],
                                    int timeout_ms) {
  pgdp_dmabuf_announce_msg_t msg;
  memset(&msg, 0, sizeof(msg));

  msg.num_buffers = PGDP_NUM_BUFFERS;
  for (int i = 0; i < PGDP_NUM_BUFFERS; i++)
    msg.desc[i] = desc[i];

  pgdp_atomic_store(&ctx->dmabuf_ack_state, 0);

  pthread_mutex_lock(&ctx->send_lock);
  int rc = pgdp_ctrl_send_fds(ctx->ctrl_fd, PGDP_MSG_DMABUF_ANNOUNCE, &msg, sizeof(msg),
                              fds, PGDP_NUM_BUFFERS);
  pthread_mutex_unlock(&ctx->send_lock);
  if (rc != 0)
    return -1;

  /* The ctrl thread owns the socket's read side, so wait for it to flip the
   * ack flag rather than reading here ourselves. Polling at 5ms granularity
   * is plenty for a one-time setup handshake. */
  struct timespec poll_ts = {.tv_sec = 0, .tv_nsec = 5 * 1000000L};
  long waited_ms = 0;
  for (;;) {
    int st = pgdp_atomic_load(&ctx->dmabuf_ack_state);

    if (st == 1)
      return 0;
    if (st == -1)
      return -2;
    if (timeout_ms >= 0 && waited_ms >= timeout_ms)
      return -1;
    nanosleep(&poll_ts, NULL);
    waited_ms += 5;
  }
}

PGDP_DEF void pgdpc_publish(pgdpc_ctx_t *ctx, int idx, uint64_t frame_id) {
  if (!pgdp_atomic_load(&ctx->active))
    return;
  if (pgdp_atomic_load(&ctx->ring->generation) !=
      pgdp_atomic_load(&ctx->granted_generation)) {
    pgdp_atomic_store(&ctx->active, false);
    return;
  }

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  uint64_t now_ns = ((uint64_t)now.tv_sec * 1000000000ULL) + now.tv_nsec;

  pgdpc__shm_ring_publish(ctx->ring, idx, frame_id, now_ns);
  if (ctx->frame_fd >= 0) {
    uint64_t v = 1;
    ssize_t n;
    do {
      n = write(ctx->frame_fd, &v, sizeof(v));
    } while (n < 0 && errno == EINTR);
  }
}

PGDP_DEF void pgdpc_disconnect(pgdpc_ctx_t *ctx) {
  pgdp_atomic_store(&ctx->running, false);

  pthread_join(ctx->ctrl_thread, NULL);
  pgdp_ctrl_send(ctx->ctrl_fd, PGDP_MSG_DISCONNECT, NULL, 0);
  close(ctx->ctrl_fd);

  if (ctx->ring)
    munmap(ctx->ring, sizeof(pgdp_shm_ring_t));
  if (ctx->frame_fd >= 0)
    close(ctx->frame_fd);

  pthread_mutex_destroy(&ctx->send_lock);
  free(ctx);
}

// ---------------------------------------------------------------------------
// Optional GBM allocation helpers
// ---------------------------------------------------------------------------
#if defined(LIBPGDP_ENABLE_GBM)

PGDP_DEF int pgdpc_gbm_bufs_create(pgdpc_gbm_bufs_t *out, int drm_fd, uint32_t width,
                                   uint32_t height) {
  memset(out, 0, sizeof(*out));
  for (int i = 0; i < PGDP_NUM_BUFFERS; i++)
    out->fds[i] = -1;

  out->gbm = gbm_create_device(drm_fd);
  if (!out->gbm) {
    fprintf(stderr, "[pgipc] gbm_create_device failed\n");
    return -1;
  }

  for (int i = 0; i < PGDP_NUM_BUFFERS; i++) {
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
    out->desc[i].fourcc = PGDP_FORMAT_XRGB8888;
    out->desc[i].stride = gbm_bo_get_stride(out->bo[i]);
    out->desc[i].offset = gbm_bo_get_offset(out->bo[i], 0);
    out->desc[i].modifier = gbm_bo_get_modifier(out->bo[i]);
    if (out->desc[i].modifier == PGDP_MODIFIER_INVALID)
      out->desc[i].modifier = PGDP_MODIFIER_LINEAR; /* we asked for LINEAR */
  }
  return 0;

fail:
  pgdpc_gbm_bufs_destroy(out);
  return -1;
}

PGDP_DEF void pgdpc_gbm_bufs_destroy(pgdpc_gbm_bufs_t *bufs) {
  if (!bufs)
    return;

  for (int i = 0; i < PGDP_NUM_BUFFERS; i++) {
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

#endif /* LIBPGDP_ENABLE_GBM */

#endif /* LIBPGDP_CLIENT */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBPGDP_IMPLEMENTATION && !LIBPGDP_IMPLEMENTATION_DONE */
