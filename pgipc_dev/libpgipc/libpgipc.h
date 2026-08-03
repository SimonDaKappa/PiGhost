// libpgipc.h - PiGhost IPC: shared-memory frame ring
//              + writer control protocol
//              + writer convenience API
//              + GPU (dmabuf) payload mode with zero-copy buffer-handle
//                passing over the existing control socket.
//
// STB-style single-file header library.
//
// QUICK START
// -----------
// In exactly ONE .c/.cpp translation unit:
//
//   #define LIBPGIPC_IMPLEMENTATION
//   #include "libpgipc.h"
//
// Every other file that needs the types / declarations:
//
//   #include "libpgipc.h"
//
// OPTIONAL: define LIBPGIPC_STATIC before the implementation #include to make every
// function definition static (private to that translation unit).
//
// OPTIONAL: define LIBPGIPC_ENABLE_GBM before including to compile the GBM
// buffer-allocation convenience helpers (requires <gbm.h> and linking -lgbm). Only GPU
// writers need this; the display service and CPU writers don't.
//
// SYMBOL VISIBILITY: LIBPGIPC_WRITER / LIBPGIPC_READER
// ---------------------------------------------------------
// This header serves three kinds of translation units:
//
//   - writer applications (renderer, slideshow, sine-wave demo, ...)
//   - the single display/reader service
//   - shared test/simulation code that legitimately needs both sides
//
// Wire-format types and the plain (non-fd) control framing functions (pgipc_ctrl_send /
// pgipc_ctrl_recv) are always declared. Both sides need to speak the same protocol.
// Everything else is gated by two feature macros so a writer app cannot accidentally
// call display-only APIs (pgipc_shm_ring_create, checkout/release, dmabuf import
// bookkeeping, ...) and vice versa:
//
//   #define LIBPGIPC_WRITER   before #include to get pgipc_writer_*,
//                              pgipc_shm_ring_attach/pick_write_slot/publish,
//                              pgipc_ctrl_send_fds/recv_fds, and (with
//                              LIBPGIPC_ENABLE_GBM) the GBM helpers.
//
//   #define LIBPGIPC_READER   before #include to get pgipc_shm_ring_create / destroy /
//                              checkout/release/new_generation, pgipc_frame_fd_create,
//                              pgipc_ctrl_send_fds/recv_fds, and pgipc_dmabuf_set_*
//                              bookkeeping.
//
// The SAME two macros gate both the declaration site (above the LIBPGIPC_IMPLEMENTATION
// block) and the implementation site (below it), so defining exactly one of them keeps
// that translation unit free of both the declarations AND the compiled bodies of the
// other side's API.
//
// If a translation unit defines NEITHER macro, both are enabled by default (with a
// one-time #pragma message note) so quick tests/simulators can still just `#include
// "libpgipc.h"` and get the full v1-style surface. Real writer and display code should
// define exactly one macro.
//
// PAYLOAD MODES
// -------------
// A writer session operates in exactly one of two payload modes:
//
//   PGIPC_PAYLOAD_PIXELS (default)
//     Frames are raw pixel bytes written by the CPU directly into
//     ring->frame_bufs[slot].
//
//   PGIPC_PAYLOAD_DMABUF
//     Frames live in PGIPC_NUM_BUFFERS GPU buffers (GBM bos / dmabufs) allocated by the
//     writer. The dmabuf fds are passed ONCE, at session setup, over the control socket
//     via SCM_RIGHTS. After that, the per-frame hot path is *identical* to pixels mode:
//     the shm ring's latest_ready / reader_locked / frame_id / write_ts fields are used
//     exactly the same way. A slot index simply refers to the writer's announced
//     dmabuf[idx] instead of ring->frame_bufs[idx]. No pixel data ever crosses the shm
//     segment.
//
// GPU writer FLOW
// -----------------
// ```c
//   ctx  = pgipc_writer_connect("my-writer", modes, n);
//   mode = pgipc_writer_negotiated_mode(ctx);
//
//   // Allocate 3 scanout-capable linear buffers at the negotiated size.
//   // With LIBPGIPC_ENABLE_GBM this is one call:
//   pgipc_gbm_bufs_t gb;
//   pgipc_gbm_bufs_create(&gb, drm_fd, mode.width, mode.height);
//
//   // Hand the fds to the display (one-time, over the control socket):
//   pgipc_writer_announce_dmabufs(ctx, gb.fds, gb.desc, 5000);
//
//   // Per frame (same as pixels mode, but render with the GPU):
//   int slot = pgipc_writer_write_slot(ctx);
//   ... bind FBO wrapping gb.bo[slot], draw, glFinish() ...   // see note (1)
//   pgipc_writer_publish(ctx, slot, frame_id);
// ```
//
// (1) EXPLICIT SYNC REQUIREMENT: in DMABUF mode the writer MUST ensure the GPU has
//     finished writing the buffer before publish(), e.g. glFinish(), or
//     eglClientWaitSync() on a fence created after the last draw call. publish() only
//     flips an index; it cannot know about in-flight GPU work. (Implicit fencing via
//     the dmabuf usually saves you on Pi/KMS, but do not rely on it across drivers.)
//
// Wrapping a dmabuf in a GLES render target (writer side), sketch:
// ```c
//   EGLint attrs[] = {
//     EGL_WIDTH,  w,  EGL_HEIGHT, h,
//     EGL_LINUX_DRM_FOURCC_EXT,      PGIPC_FORMAT_XRGB8888,
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
// DISPLAY-SERVICE RESPONSIBILITIES (dmabuf mode)
// ----------------------------------------------
//   * Run its control-socket read loop with pgipc_ctrl_recv_fds() (NOT the plain
//     pgipc_ctrl_recv()), otherwise SCM_RIGHTS fds attached to a
//     PGIPC_MSG_DMABUF_ANNOUNCE are silently discarded by the kernel.
//   * On PGIPC_MSG_DMABUF_ANNOUNCE: build a pgipc_dmabuf_set_t with
//     pgipc_dmabuf_set_from_announce(), try to import each fd into KMS
//     (drmPrimeFDToHandle + drmModeAddFB2WithModifiers), and reply with
//     PGIPC_MSG_DMABUF_ACK (accepted=1) or (accepted=0, reason).
//   * The per-frame read path is unchanged: epoll/read-wait on the frame-ready
//     eventfd, checkout() -> idx,
//     but instead of memcpy'ing ring->frame_bufs[idx], page-flip to the KMS
//     framebuffer imported from that writer's dmabuf[idx].
//   * reader_locked semantics: a scanout buffer is "in use" until the flip
//     AWAY from it completes. Keep reader_locked = the currently-scanned-out
//     index for that whole interval, then release/checkout the next.
//   * On writer switch/eviction: call pgipc_evict_writer()
//     (bumps generation AND resets latest_ready to -1) and flip to a
//     display-owned fallback framebuffer BEFORE closing the evicted
//     writer's fds / dropping its KMS fbs - never scan out a buffer you
//     are about to release.
//   * Announced fds belong to the display once received (SCM_RIGHTS dups
//     them); close them with pgipc_dmabuf_set_close() when the writer's
//     control connection goes away.
//
// C++ / FFI
// ---------
// All public symbols are wrapped in extern "C" when compiled as C++.

#ifndef LIBPGIPC_H
#define LIBPGIPC_H

#ifdef LIBPGIPC_STATIC
#define PGIPC_DEF static
#else
#define PGIPC_DEF extern
#endif

/* Default both sides on if the includer didn't pick one, so a bare `#include
 * "libpgipc.h"` still works for quick tests/simulators. Real writer/display code should
 * define exactly one of these before the first #include. */
#if !defined(LIBPGIPC_WRITER) && !defined(LIBPGIPC_READER)
#define LIBPGIPC_WRITER
#define LIBPGIPC_READER
#if !defined(LIBPGIPC_NO_SIDE_WARNING)
#pragma message(                                                                       \
    "[libpgipc] neither LIBPGIPC_WRITER nor LIBPGIPC_READER was defined "              \
    "before #include \"libpgipc.h\". Defaulting to both. Define "                      \
    "LIBPGIPC_NO_SIDE_WARNING to silence this note, or define exactly one of "         \
    "the two macros.")
#endif
#endif

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/eventfd.h>
#include <time.h>

#if defined(LIBPGIPC_ENABLE_GBM) && defined(LIBPGIPC_WRITER)
#include <gbm.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ===========================================================================
// ABI/Cross-language utilities
// ===========================================================================


#ifdef __cplusplus
#define PGIPC_SASSERT(c, m) static_assert(c, m)
#else
#define PGIPC_SASSERT(c, m) _Static_assert(c, m)
#endif

#ifdef __cplusplus
#define PGIPC_ALIGNAS(n) alignas(n)
#define PGIPC_ALIGNOF(t) alignof(t)
#else
#define PGIPC_ALIGNAS(n) _Alignas(n)
#define PGIPC_ALIGNOF(t) _Alignof(t)
#endif

#define PGIPC_CAT_(a, b) a##b
#define PGIPC_CAT(a, b) PGIPC_CAT_(a, b)

#define PGIPC_CACHELINE 64
#define PGIPC_CACHELINE_FIELD(type, name)                                              \
  union {                                                                              \
    PGIPC_ALIGNAS(PGIPC_CACHELINE) type name;                                          \
    unsigned char PGIPC_CAT(pad_, __LINE__)[PGIPC_CACHELINE];                          \
  }

/** Marker for what should be a `atomic_*|_Atomic` type but isn't for cross-language ABI
 * stability and reinterpretation.
 *
 * Plain, fixed-width, non "_Atomic/atomic_*" types. Accessed only through
 * pgipc__a{load,store,fetch_add}_*(), never through <stdatomic.h>'s
 * atomic_load()/atomic_store()/etc, which require an _Atomic-qualified (C) or
 * std::atomic<T> (C++) operand and will not accept these.
 */
#define PGIPC_ATOMIC


/** pgipc__a{load,store,fetch_add}_{i32,u32,bool} - the single, unified atomic
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
 * in C++ mode. Do not mix them together, and just use the PGIPC_ATOMIC flavor.
 */

static inline int32_t pgipc__aload_i32(const int32_t *p) {
  return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}
static inline void pgipc__astore_i32(int32_t *p, int32_t v) {
  __atomic_store_n(p, v, __ATOMIC_SEQ_CST);
}
static inline uint32_t pgipc__aload_u32(const uint32_t *p) {
  return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}
static inline void pgipc__astore_u32(uint32_t *p, uint32_t v) {
  __atomic_store_n(p, v, __ATOMIC_SEQ_CST);
}
static inline uint32_t pgipc__afetch_add_u32(uint32_t *p, uint32_t v) {
  return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST);
}
static inline bool pgipc__aload_bool(const bool *p) {
  return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}
static inline void pgipc__astore_bool(bool *p, bool v) {
  __atomic_store_n(p, v, __ATOMIC_SEQ_CST);
}

static inline uint64_t pgipc__ts_diff_ns(struct timespec a, struct timespec b) {
  int64_t sec_diff = (int64_t)a.tv_sec - (int64_t)b.tv_sec;
  int64_t nsec_diff = (int64_t)a.tv_nsec - (int64_t)b.tv_nsec;
  int64_t total = sec_diff * 1000000000LL + nsec_diff;
  return total > 0 ? (uint64_t)total : 0;
}

// ===========================================================================
// Shared-memory frame ring
// ===========================================================================

#define PGIPC_SHM_NAME "/frame_ring_shm"

#define PGIPC_NUM_BUFFERS 3
#define PGIPC_BYTES_PER_PIXEL 4
#define PGIPC_FRAME_MAX_WIDTH 1280
#define PGIPC_FRAME_MAX_HEIGHT 720
#define PGIPC_FRAME_MAX_SIZE                                                           \
  ((size_t)PGIPC_FRAME_MAX_WIDTH * PGIPC_FRAME_MAX_HEIGHT * PGIPC_BYTES_PER_PIXEL)

/**
 * struct pgipc_shm_ring_t - frame buffer shared memory ring for both conn sides
 * @latest_ready:     index of newest complete frame, -1 if none
 * @reader_locked:    index reader currently holds, -1 if none
 * @frame_counter:    monotonically increasing frame id, global
 * @generation:       bumped by display on every activation switch; a writer whose
 *                    granted generation no longer matches this has been evicted and
 *                    must stop writing
 * @frame_id:         frame id stamped into each buffer at write time
 * @write_ts_ns:      when each buffer was published (for latency calc)
 * @frame_bufs:       individual frame buffers (PIXELS payload mode only; in the DMABUF
 *                    mode the indices refer to the writer's announced dmabuf set)
 * @bookkeeping_lock: cross-process mutex serializing @latest_ready/@reader_locked
 *                    updates. guards bookkeeping only, never the pixel copy
 *
 * Never allocate or copy this struct by value, only ever map it at a fixed address via
 * @pgipc_shm_ring_create() and @pgipc_shm_ring_attach().
 *
 * @note layout is byte-for-byte identical across payload mode. The dmabuf mode is
 * purely a control-plane extension; slot indices mean the same thing in both modes.
 *
 * @note latest_ready, reader_locked, frame_counter, generation are aligned on separate
 * cache lines to help with the cross process/thread hammering on them at high
 * framerates.
 */
typedef struct {
  PGIPC_ATOMIC PGIPC_CACHELINE_FIELD(int32_t, latest_ready);
  PGIPC_ATOMIC PGIPC_CACHELINE_FIELD(int32_t, reader_locked);
  PGIPC_ATOMIC PGIPC_CACHELINE_FIELD(uint32_t, frame_counter);
  PGIPC_ATOMIC PGIPC_CACHELINE_FIELD(uint32_t, generation);
  uint64_t frame_id[PGIPC_NUM_BUFFERS];
  uint64_t write_ts_ns[PGIPC_NUM_BUFFERS];
  unsigned char frame_bufs[PGIPC_NUM_BUFFERS][PGIPC_FRAME_MAX_SIZE];
  pthread_mutex_t bookkeeping_lock;
} pgipc_shm_ring_t;

PGIPC_SASSERT(offsetof(pgipc_shm_ring_t, latest_ready) == 0 * PGIPC_CACHELINE,
              "field spacing drift");
PGIPC_SASSERT(offsetof(pgipc_shm_ring_t, reader_locked) == 1 * PGIPC_CACHELINE,
              "field spacing drift");
PGIPC_SASSERT(offsetof(pgipc_shm_ring_t, frame_counter) == 2 * PGIPC_CACHELINE,
              "field spacing drift");
PGIPC_SASSERT(offsetof(pgipc_shm_ring_t, generation) == 3 * PGIPC_CACHELINE,
              "field spacing drift");
PGIPC_SASSERT(PGIPC_ALIGNOF(pgipc_shm_ring_t) == PGIPC_CACHELINE,
              "ring alignment drift");

#ifdef LIBPGIPC_READER
/**
 * pgipc_shm_ring_create() - create the shared ring buffer
 *
 * Creates a new shared memory segment for the frame ring, unlinking any stale segment
 * left over from a previous run first.
 *
 * Return: pointer to the mapped ring, or NULL on error.
 */
PGIPC_DEF pgipc_shm_ring_t *pgipc_shm_ring_create(void);

/**
 * pgipc_shm_ring_destroy() - unmap and unlink the shared ring
 * @ring: ring returned by pgipc_shm_ring_create(), may be NULL
 *
 * Call once, at display shutdown.
 */
PGIPC_DEF void pgipc_shm_ring_destroy(pgipc_shm_ring_t *ring);

/**
 * pgipc_frame_fd_create() - create the frame-ready eventfd
 *
 * Created EFD_NONBLOCK | EFD_CLOEXEC, default (non-EFD_SEMAPHORE) counting mode. Only
 * one writer is ever ACTIVE at a time, so a single eventfd is created once here and
 * reused across every activation grant: the display sends a copy of it as SCM_RIGHTS
 * ancillary data on every PGIPC_MSG_ACTIVATE_GRANT (see pgipc_ctrl_send_fds()), and the
 * writer writes a 1 to it on every publish (see pgipc_writer_publish()).
 *
 * Return: the created eventfd, or -1 on error.
 */
PGIPC_DEF int pgipc_frame_fd_create(void);

/**
 * pgipc_shm_ring_checkout() - claim the newest ready frame for reading
 * @ring: attached/created ring
 *
 * Marks @ring's newest complete frame as reader-locked so the writer's slot-picking
 * logic will not reuse it out from under the reader. Call pgipc_shm_ring_release() when
 * done with the buffer (e.g. after finishing the memcpy/page-flip to HDMI).
 *
 * Return: buffer index to read (0..PGIPC_NUM_BUFFERS-1), or -1 if no frame has been
 * published yet.
 */
PGIPC_DEF int pgipc_shm_ring_checkout(pgipc_shm_ring_t *ring);

/**
 * pgipc_shm_ring_release() - release the buffer claimed by checkout()
 * @ring: attached/created ring
 *
 * Clears the reader-locked index back to -1. Safe to call even if no checkout is
 * currently held.
 */
PGIPC_DEF void pgipc_shm_ring_release(pgipc_shm_ring_t *ring);

/**
 * pgipc_evict_writer() - evict the current writer
 * @ring: attached/created ring
 *
 * Bumps @generation (so any writer still holding the old grant sees it is evicted on
 * its next publish) and resets @latest_ready to -1 so a stale slot index from the
 * previous writer is never consumed. Call this on every activation switch, regardless
 * of mode. In the dmabuf mode a stale index would otherwise point into the *previous*
 * writer's buffer set.
 */
PGIPC_DEF void pgipc_evict_writer(pgipc_shm_ring_t *ring);
#endif /* LIBPGIPC_READER */

#ifdef LIBPGIPC_WRITER
/**
 * pgipc_shm_ring_attach() - attach to an existing shared ring
 * @max_retries:    number of attempts before giving up
 * @retry_delay_ms: delay between attempts, in milliseconds
 *
 * The display service creates the ring before any writer starts, so a writer racing
 * display startup should retry rather than fail immediately.
 *
 * Return: pointer to the mapped ring, or NULL if it never appeared.
 */
PGIPC_DEF pgipc_shm_ring_t *pgipc_shm_ring_attach(int max_retries, int retry_delay_ms);

/**
 * pgipc_shm_ring_pick_write_slot() - choose a free buffer index to write
 * @ring: attached ring
 *
 * Picks any index that is neither the currently-published frame (@latest_ready) nor the
 * reader's currently-locked index (@reader_locked), guaranteeing the write never tears
 * a frame the reader (or the flip-away-from logic in dmabuf mode) is using.
 *
 * Return: a writable buffer index (0..PGIPC_NUM_BUFFERS-1).
 */
PGIPC_DEF int pgipc_shm_ring_pick_write_slot(pgipc_shm_ring_t *ring);

/**
 * pgipc_shm_ring_publish() - publish a finished frame as the newest ready
 * @ring:     attached ring
 * @idx:      buffer index previously returned by
 *            pgipc_shm_ring_pick_write_slot()
 * @frame_id: writer-assigned monotonically increasing frame id
 * @now_ns:   timestamp the write completed (CLOCK_MONOTONIC), used by the display for
 *            latency accounting
 *
 * Low-level primitive; writer applications normally call pgipc_writer_publish()
 * instead, which also handles the semaphore post and generation/eviction check.
 */
PGIPC_DEF void pgipc_shm_ring_publish(pgipc_shm_ring_t *ring, int idx,
                                      uint64_t frame_id, uint64_t now_ns);
#endif /* LIBPGIPC_WRITER */

// ===========================================================================
// Control protocol
// ===========================================================================

#define PGIPC_CONTROL_SOCK_PATH "/dev/shm/frame_ring_control.sock"
#define PGIPC_APP_ID_LEN 32
#define PGIPC_MAX_MODES 4
#define PGIPC_DENY_REASON_LEN 64

/**
 * enum pgipc_msg_type_t - control protocol message types
 * @PGIPC_MSG_CONNECT:          writer -> display: "here's what I support"
 * @PGIPC_MSG_MODE:             display -> writer: "render at this mode" / reject
 * @PGIPC_MSG_ACTIVATE_REQUEST: writer -> display: "let me be the writer"
 * @PGIPC_MSG_ACTIVATE_GRANT:   display -> writer: "you're it, generation N"
 * @PGIPC_MSG_ACTIVATE_DENY:    display -> writer: "no, and here's why"
 * @PGIPC_MSG_HEARTBEAT:        writer -> display: "still alive, gen N, frame F"
 * @PGIPC_MSG_DEACTIVATE:       display -> writer: "stand down, someone else active"
 * @PGIPC_MSG_DISCONNECT:       writer -> display: "graceful disconnect"
 * @PGIPC_MSG_DMABUF_ANNOUNCE:  writer -> display: "my frames live in these GPU
 *                              buffers". payload is pgipc_dmabuf_announce_msg_t, and
 *                              exactly PGIPC_NUM_BUFFERS dmabuf fds ride alongside in
 *                              SCM_RIGHTS ancillary data. Switches the session to the
 *                              DMABUF payload mode on ACK.
 * @PGIPC_MSG_DMABUF_ACK:       display -> writer: import succeeded / refused
 */
typedef enum {
  PGIPC_MSG_CONNECT = 1,
  PGIPC_MSG_MODE = 2,
  PGIPC_MSG_ACTIVATE_REQUEST = 3,
  PGIPC_MSG_ACTIVATE_GRANT = 4,
  PGIPC_MSG_ACTIVATE_DENY = 5,
  PGIPC_MSG_HEARTBEAT = 6,
  PGIPC_MSG_DEACTIVATE = 7,
  PGIPC_MSG_DISCONNECT = 8,
  PGIPC_MSG_DMABUF_ANNOUNCE = 9,
  PGIPC_MSG_DMABUF_ACK = 10,
} pgipc_msg_type_t;

/**
 * struct pgipc_render_mode_t - arbitrated resolution/frame rate
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
} pgipc_render_mode_t;

/**
 * struct pgipc_hello_msg_t - PGIPC_MSG_CONNECT payload
 * @app_id:    writer application id
 * @num_modes: number of pgipc_render_mode_t entries in @modes
 * @modes:     supported render modes, in order of preference
 */
typedef struct {
  char app_id[PGIPC_APP_ID_LEN];
  uint32_t num_modes;
  pgipc_render_mode_t modes[PGIPC_MAX_MODES];
} pgipc_hello_msg_t;

/**
 * struct pgipc_mode_msg_t - PGIPC_MSG_MODE payload
 * @accepted: 1 if the display accepted one of the offered modes, 0 if rejected
 * @chosen:   the mode chosen by the display (only valid if accepted==1)
 */
typedef struct {
  uint8_t accepted;
  pgipc_render_mode_t chosen;
} pgipc_mode_msg_t;

/**
 * struct pgipc_grant_msg_t - PGIPC_MSG_ACTIVATE_GRANT payload
 * @generation: the generation number granted to the writer; if this no longer matches
 *              the display's generation, the writer has been evicted and must stop
 *              writing
 */
typedef struct {
  uint32_t generation;
} pgipc_grant_msg_t;

/**
 * struct pgipc_deny_msg_t - PGIPC_MSG_ACTIVATE_DENY payload
 * @reason: human-readable explanation of why the writer was denied activation (e.g.
 *          "another app is currently active")
 */
typedef struct {
  char reason[PGIPC_DENY_REASON_LEN];
} pgipc_deny_msg_t;

/**
 * struct pgipc_heartbeat_msg_t - PGIPC_MSG_HEARTBEAT payload
 * @generation:    the generation number the writer believes it holds
 * @frame_counter: the writer's (monotonically increasing) current frame
 */
typedef struct {
  uint32_t generation;
  uint64_t frame_counter;
} pgipc_heartbeat_msg_t;

/**
 * pgipc_ctrl_send() - frame and send one control message
 * @fd:      connected control socket
 * @type:    message type (see pgipc_msg_type_t)
 * @payload: pointer to the message's payload struct, or NULL if @len == 0
 * @len:     size of @payload in bytes
 *
 * Wire format: 1-byte type + 4-byte big-endian length + payload bytes. Blocks until the
 * whole message is written or an error occurs. Used by both sides of the connection for
 * every message type that does not carry fds (for those, see pgipc_ctrl_send_fds()).
 *
 * Return: 0 on success, -1 on error.
 */
PGIPC_DEF int pgipc_ctrl_send(int fd, pgipc_msg_type_t type, const void *payload,
                              uint32_t len);

/**
 * pgipc_ctrl_recv() - receive and unframe one control message
 * @fd:       connected control socket
 * @out_type: set to the received message's type
 * @buf:      caller-provided buffer to receive the payload
 * @bufsize:  capacity of @buf in bytes
 * @out_len:  set to the number of payload bytes actually written to @buf
 *
 * Any fds attached via SCM_RIGHTS to a message received through this function (as
 * opposed to pgipc_ctrl_recv_fds()) are silently discarded by the kernel. Do not use
 * this to receive PGIPC_MSG_DMABUF_ANNOUNCE.
 *
 * Return: 0 on success, -1 on I/O error or peer disconnect, -2 if the message is larger
 * than @bufsize.
 */
PGIPC_DEF int pgipc_ctrl_recv(int fd, pgipc_msg_type_t *out_type, void *buf,
                              uint32_t bufsize, uint32_t *out_len);

// ===========================================================================
// GPU / dmabuf payload mode
// ===========================================================================

/**
 * enum pgipc_payload_kind_t - what a ring slot index refers to
 * @PGIPC_PAYLOAD_PIXELS: raw bytes in ring->frame_bufs[idx] (default)
 * @PGIPC_PAYLOAD_DMABUF: the writer's announced dmabuf[idx]
 */
typedef enum {
  PGIPC_PAYLOAD_PIXELS = 0,
  PGIPC_PAYLOAD_DMABUF = 1,
} pgipc_payload_kind_t;

/* DRM fourcc helpers, so writers don't need <drm_fourcc.h> just for this. Values match
 * the kernel's DRM_FORMAT_* definitions. */
#define PGIPC_FOURCC(a, b, c, d)                                                       \
  ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define PGIPC_FORMAT_XRGB8888 PGIPC_FOURCC('X', 'R', '2', '4')
#define PGIPC_FORMAT_ARGB8888 PGIPC_FOURCC('A', 'R', '2', '4')
#define PGIPC_MODIFIER_LINEAR ((uint64_t)0) /* DRM_FORMAT_MOD_LINEAR */
#define PGIPC_MODIFIER_INVALID ((uint64_t)0x00ffffffffffffffULL)

/**
 * struct pgipc_dmabuf_desc_t - geometry/format of one announced dmabuf
 * @width:    pixel width; must equal the negotiated mode
 * @height:   pixel height; must equal the negotiated mode
 * @fourcc:   DRM fourcc (PGIPC_FORMAT_*)
 * @stride:   bytes per row (from gbm_bo_get_stride(); NOT width*4, the allocator may
 *            pad rows)
 * @offset:   byte offset of plane 0 within the dmabuf (usually 0)
 * @modifier: DRM format modifier; use LINEAR unless you know the display's KMS import
 *            path handles the tiled/compressed modifier
 *
 * Single-plane formats only (XRGB8888/ARGB8888 are single-plane; that is all this
 * display needs). Multi-planar YUV etc. is deliberately out of scope.
 */
typedef struct {
  uint32_t width;
  uint32_t height;
  uint32_t fourcc;
  uint32_t stride;
  uint32_t offset;
  uint64_t modifier;
} pgipc_dmabuf_desc_t;

/**
 * struct pgipc_dmabuf_announce_msg_t - PGIPC_MSG_DMABUF_ANNOUNCE payload
 * @num_buffers: must equal PGIPC_NUM_BUFFERS
 * @desc:        per-buffer geometry, index-aligned with the SCM_RIGHTS fds
 *
 * Exactly @num_buffers dmabuf fds MUST accompany this message as SCM_RIGHTS ancillary
 * data (see pgipc_ctrl_send_fds()). fd[i] corresponds to desc[i], and both correspond
 * to ring slot index i.
 */
typedef struct {
  uint32_t num_buffers;
  pgipc_dmabuf_desc_t desc[PGIPC_NUM_BUFFERS];
} pgipc_dmabuf_announce_msg_t;

/**
 * struct pgipc_dmabuf_ack_msg_t - PGIPC_MSG_DMABUF_ACK payload
 * @accepted: 1 if the display imported all buffers; session is now DMABUF mode
 * @reason:   human-readable refusal reason (valid when accepted==0)
 */
typedef struct {
  uint8_t accepted;
  char reason[PGIPC_DENY_REASON_LEN];
} pgipc_dmabuf_ack_msg_t;

#if defined(LIBPGIPC_WRITER) || defined(LIBPGIPC_READER)
/**
 * pgipc_ctrl_send_fds() - send a control message with attached fds
 * @fd:      connected control socket
 * @type:    message type
 * @payload: message payload, or NULL if @len == 0
 * @len:     size of @payload in bytes
 * @fds:     file descriptors to attach via SCM_RIGHTS
 * @nfds:    number of entries in @fds (must be <= PGIPC_NUM_BUFFERS)
 *
 * Same wire format as pgipc_ctrl_send(); the fds ride as SCM_RIGHTS ancillary data
 * attached to the framing header. Used in both directions: writers send fds on
 * PGIPC_MSG_DMABUF_ANNOUNCE, the display sends the frame-ready eventfd on
 * PGIPC_MSG_ACTIVATE_GRANT.
 *
 * Return: 0 on success, -1 on error (including @nfds out of range).
 */
PGIPC_DEF int pgipc_ctrl_send_fds(int fd, pgipc_msg_type_t type, const void *payload,
                                  uint32_t len, const int *fds, int nfds);
#endif /* LIBPGIPC_WRITER || LIBPGIPC_READER */

#if defined(LIBPGIPC_WRITER) || defined(LIBPGIPC_READER)
/**
 * pgipc_ctrl_recv_fds() - receive a control message, harvesting any fds
 * @fd:       connected control socket
 * @out_type: set to the received message's type
 * @buf:      caller-provided buffer to receive the payload
 * @bufsize:  capacity of @buf in bytes
 * @out_len:  set to the number of payload bytes actually written to @buf
 * @out_fds:  caller-provided array to receive any SCM_RIGHTS fds
 * @max_fds:  capacity of @out_fds; excess fds are closed to avoid leaks
 * @out_nfds: set to the number of fds actually written to @out_fds
 * 
 * Both the display's and the writer's control read loops should use this everywhere
 * instead of pgipc_ctrl_recv(). Receiving an fd-bearing message with the plain function
 * silently discards the fds. Messages without fds set *out_nfds = 0, so this function
 * is always safe to use in place of pgipc_ctrl_recv().
 *
 * Return: 0 on success, -1 on error/disconnect, -2 if the message is larger than
 * @bufsize (any harvested fds are closed first).
 */
PGIPC_DEF int pgipc_ctrl_recv_fds(int fd, pgipc_msg_type_t *out_type, void *buf,
                                  uint32_t bufsize, uint32_t *out_len, int *out_fds,
                                  int max_fds, int *out_nfds);
#endif /* LIBPGIPC_WRITER || LIBPGIPC_READER */

#ifdef LIBPGIPC_READER
/**
 * struct pgipc_dmabuf_set_t - display's record of one writer's dmabufs
 * @valid: true once populated from a well-formed announce
 * @fds:   display-owned dup'd fds (close via pgipc_dmabuf_set_close())
 * @desc:  geometry, index-aligned with @fds and with ring slot indices
 */
typedef struct {
  bool valid;
  int fds[PGIPC_NUM_BUFFERS];
  pgipc_dmabuf_desc_t desc[PGIPC_NUM_BUFFERS];
} pgipc_dmabuf_set_t;

/**
 * pgipc_dmabuf_set_from_announce() - validate and adopt an announce message
 * @set:  output set to populate
 * @msg:  the received PGIPC_MSG_DMABUF_ANNOUNCE payload
 * @fds:  the fds received alongside @msg via SCM_RIGHTS
 * @nfds: number of entries in @fds
 *
 * Does NOT talk to KMS. the display should attempt the
 * drmPrimeFDToHandle()/drmModeAddFB2WithModifiers() import next and send
 * PGIPC_MSG_DMABUF_ACK with the outcome.
 *
 * Return: 0 and fills @set (taking ownership of @fds) if the announce is well-formed;
 * -1 and closes all @fds otherwise.
 */
PGIPC_DEF int pgipc_dmabuf_set_from_announce(pgipc_dmabuf_set_t *set,
                                             const pgipc_dmabuf_announce_msg_t *msg,
                                             const int *fds, int nfds);

/**
 * pgipc_dmabuf_set_close() - close a writer's announced dmabuf fds
 * @set: set previously populated by pgipc_dmabuf_set_from_announce(), may
 *       be NULL
 *
 * Call when the writer's control connection goes away (disconnect, eviction, crash).
 * After this, the display must not scan out any KMS framebuffer it built from these
 * fds.
 */
PGIPC_DEF void pgipc_dmabuf_set_close(pgipc_dmabuf_set_t *set);
#endif /* LIBPGIPC_READER */

// ===========================================================================
// Writer API
// ===========================================================================

/**
 * PGIPC_HEARTBEAT_INTERVAL_MS - writer's HEARTBEAT send period, in ms
 *
 * Shared by both sides (not writer-only): the writer's ctrl thread sends a
 * heartbeat this often while active; the display's session table uses a
 * multiple of this (see PGIPC_HEARTBEAT_TIMEOUT_MS) to detect a dead ACTIVE
 * writer. Declared unconditionally so a LIBPGIPC_READER-only build (e.g.
 * the display) can reference it without pulling in writer-only symbols.
 */
#define PGIPC_HEARTBEAT_INTERVAL_MS 500

/**
 * PGIPC_HEARTBEAT_TIMEOUT_MS - display's ACTIVE-client dead-heartbeat cutoff
 *
 * If an ACTIVE writer's last heartbeat is older than this, the display
 * evicts it. Chosen as 2 * PGIPC_HEARTBEAT_INTERVAL_MS to tolerate exactly one dropped
 * heartbeat before acting.
 */
#define PGIPC_HEARTBEAT_TIMEOUT_MS (2 * PGIPC_HEARTBEAT_INTERVAL_MS)

#ifdef LIBPGIPC_WRITER

#define PGIPC_RETRY_ACTIVATE_MS 2000

/** pgipc_writer_ctx_t - writer session handle
 *
 * Opaque; treat as a handle. Returned by pgipc_writer_connect() and passed to every
 * other pgipc_writer_*() call.
 */
typedef struct _pgipc_writer_ctx_t pgipc_writer_ctx_t;

/**
 * pgipc_writer_connect() - establish a writer session with the display
 * @app_id:     this writer's application id (truncated to PGIPC_APP_ID_LEN-1 bytes)
 * @modes:      render modes this writer supports, in order of preference
 * @num_modes:  number of entries in @modes (truncated to PGIPC_MAX_MODES)
 *
 * Connects to the display's control socket, performs the HELLO/MODE negotiation,
 * requests activation, and (win or lose the first activation race) spawns a background
 * control thread that maintains heartbeats, retries activation while inactive, and
 * processes future grant/deny/deactivate/dmabuf-ack messages for the lifetime of the
 * session.
 *
 * Return: a new session context, or NULL if the control socket could not be reached or
 * the display rejected every offered mode.
 */
PGIPC_DEF pgipc_writer_ctx_t *pgipc_writer_connect(const char *app_id,
                                                   const pgipc_render_mode_t *modes,
                                                   int num_modes);

/**
 * pgipc_writer_is_active() - check whether this session is the writer
 * @ctx: session handle
 *
 * Return: true if this writer currently holds the activation grant and may
 * write/publish frames.
 */
PGIPC_DEF bool pgipc_writer_is_active(pgipc_writer_ctx_t *ctx);

/**
 * pgipc_writer_negotiated_mode() - the mode chosen during connect
 * @ctx: session handle
 *
 * Return: the render mode accepted by the display at connect time.
 */
PGIPC_DEF pgipc_render_mode_t pgipc_writer_negotiated_mode(pgipc_writer_ctx_t *ctx);

/**
 * pgipc_writer_announce_dmabufs() - switch this session to the DMABUF mode
 * @ctx:        session handle
 * @fds:        dmabuf fds, index-aligned with ring slots
 * @desc:       geometry for each fd, sized to the negotiated mode
 * @timeout_ms: how long to wait for the display's ack (<0 = wait forever)
 *
 * Call AFTER pgipc_writer_connect() (which negotiates the mode you should size the
 * buffers to). Sends the fds + geometry to the display over the control socket
 * (SCM_RIGHTS) and blocks up to @timeout_ms for the display's PGIPC_MSG_DMABUF_ACK,
 * which is consumed by the background ctrl thread.
 *
 * On success the session's payload kind becomes PGIPC_PAYLOAD_DMABUF and ring slot
 * indices now refer to your buffers; you may close(fds[i]) afterwards, SCM_RIGHTS gave
 * the display its own references (keep your GBM bos alive to render, of course).
 *
 * Return: 0 on success, -1 on send failure/timeout, -2 if the display refused the
 * import. On refusal/timeout the session stays in PIXELS mode, so a writer can fall
 * back to glReadPixels-into-shm if it wants.
 */
PGIPC_DEF int
pgipc_writer_announce_dmabufs(pgipc_writer_ctx_t *ctx, const int fds[PGIPC_NUM_BUFFERS],
                              const pgipc_dmabuf_desc_t desc[PGIPC_NUM_BUFFERS],
                              int timeout_ms);

/**
 * pgipc_writer_payload_kind() - which mode this session is operating in
 * @ctx: session handle
 *
 * Return: PGIPC_PAYLOAD_PIXELS or PGIPC_PAYLOAD_DMABUF.
 */
PGIPC_DEF pgipc_payload_kind_t pgipc_writer_payload_kind(pgipc_writer_ctx_t *ctx);

/**
 * pgipc_writer_write_slot() - choose a free buffer index to write
 * @ctx: session handle
 *
 * Thin wrapper over pgipc_shm_ring_pick_write_slot() for @ctx->ring.
 *
 * Return: a writable buffer index (0..PGIPC_NUM_BUFFERS-1).
 */
PGIPC_DEF int pgipc_writer_write_slot(pgipc_writer_ctx_t *ctx);

/**
 * pgipc_writer_publish() - publish slot @idx as the newest frame
 * @ctx:      session handle
 * @idx:      buffer index previously returned by
 *            pgipc_writer_write_slot()
 * @frame_id: writer-assigned monotonically increasing frame id
 *
 * PIXELS mode: call after your CPU write into ring->frame_bufs[idx] completes.
 * DMABUF mode: call ONLY after GPU work targeting dmabuf[idx] has fully completed
 * (glFinish() or a client-side fence wait). Publish only flips an index and posts the
 * semaphore; it cannot see in-flight GPU work.
 *
 * Drops the frame if @ctx is not active, or if the ring's generation has advanced past
 * @ctx's granted generation. In the latter case this call also flips @ctx to inactive
 * so subsequent callers see pgipc_writer_is_active() return false without waiting for
 * the next control message.
 */
PGIPC_DEF void pgipc_writer_publish(pgipc_writer_ctx_t *ctx, int idx,
                                    uint64_t frame_id);

/**
 * pgipc_writer_disconnect() - tear down a writer session
 * @ctx: session handle; freed by this call and must not be used again
 *
 * Stops the control thread, sends PGIPC_MSG_DISCONNECT, closes the control socket,
 * unmaps the shm ring, and frees @ctx.
 */
PGIPC_DEF void pgipc_writer_disconnect(pgipc_writer_ctx_t *ctx);

#endif /* LIBPGIPC_WRITER */

// ===========================================================================
// GBM allocation helpers
// ===========================================================================
#if defined(LIBPGIPC_ENABLE_GBM) && defined(LIBPGIPC_WRITER)

/**
 * struct pgipc_gbm_bufs_t - a ring's worth of GPU buffers, ready to announce
 * @gbm:  gbm device wrapping the caller's DRM fd (caller keeps the fd open for the
 *        lifetime of this struct)
 * @bo:   the buffer objects; keep alive while rendering into them
 * @fds:  exported dmabuf fds, index-aligned with ring slots
 * @desc: filled-in geometry for pgipc_writer_announce_dmabufs()
 */
typedef struct {
  struct gbm_device *gbm;
  struct gbm_bo *bo[PGIPC_NUM_BUFFERS];
  int fds[PGIPC_NUM_BUFFERS];
  pgipc_dmabuf_desc_t desc[PGIPC_NUM_BUFFERS];
} pgipc_gbm_bufs_t;

/**
 * pgipc_gbm_bufs_create() - allocate PGIPC_NUM_BUFFERS linear scanout buffers
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
PGIPC_DEF int pgipc_gbm_bufs_create(pgipc_gbm_bufs_t *out, int drm_fd, uint32_t width,
                                    uint32_t height);

/**
 * pgipc_gbm_bufs_destroy() - free buffers allocated by pgipc_gbm_bufs_create()
 * @bufs: struct previously populated by pgipc_gbm_bufs_create(), may be NULL
 */
PGIPC_DEF void pgipc_gbm_bufs_destroy(pgipc_gbm_bufs_t *bufs);

#endif /* LIBPGIPC_ENABLE_GBM && LIBPGIPC_WRITER */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBPGIPC_H */

// ===========================================================================
// IMPLEMENTATION
// ===========================================================================
#if defined(LIBPGIPC_IMPLEMENTATION) && !defined(LIBPGIPC_IMPLEMENTATION_DONE)
#define LIBPGIPC_IMPLEMENTATION_DONE

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

/** pgipc__send_all() - write() until @len bytes are sent or an error occurs
 * @fd:  where to write
 * @buf: payload to write
 * @len: amount to write from payload
 *
 * Returns 0 on success, -1 on error (EINTR is retried transparently).
 */
static int pgipc__send_all(int fd, const void *buf, size_t len) {
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

/** pgipc__recv_all() - read() until @len bytes are received or an error/EOF
 * @fd:  where to read from
 * @buf: where to read into
 * @len: amount to read
 *
 * Returns 0 on success, -1 on error or peer-closed (EINTR retried).
 */
static int pgipc__recv_all(int fd, void *buf, size_t len) {
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

#ifdef LIBPGIPC_WRITER

/**
 * struct _pgipc_writer_ctx_t - writer session handle implementation
 * @app_id:             writer application id
 * @ctrl_fd:            fd to domain socket for control protocol
 * @ring:               frame buffer shared memory
 * @active:             true once granted AND not yet deactivated/evicted
 * @running:            false once we should shut down entirely
 * @granted_generation: current generation number granted by display
 * @mode:               negotiated resolution/fps once accepted
 * @frame_fd:            eventfd to signal the display on each publish; received via
 *                       SCM_RIGHTS on PGIPC_MSG_ACTIVATE_GRANT, -1 until granted
 * @ctrl_thread:        background thread owning the control socket
 * @send_lock:          serializes writes to ctrl_fd from multiple callers
 * @payload_kind:       PGIPC_PAYLOAD_PIXELS until a dmabuf announce is ACKed
 * @dmabuf_ack_state:   0 = pending/none, 1 = accepted, -1 = refused
 *
 * Opaque to writers/callers. Returned by pgipc_writer_connect() and passed to every
 * other pgipc_writer_*() call.
 */
struct _pgipc_writer_ctx_t {
  char app_id[PGIPC_APP_ID_LEN];
  int ctrl_fd;
  pgipc_shm_ring_t *ring;
  PGIPC_ATOMIC bool active;
  PGIPC_ATOMIC bool running;
  PGIPC_ATOMIC uint32_t granted_generation;
  pgipc_render_mode_t mode;
  int frame_fd;
  pthread_t ctrl_thread;
  pthread_mutex_t send_lock;
  PGIPC_ATOMIC int32_t payload_kind;
  PGIPC_ATOMIC int32_t dmabuf_ack_state;
};

/** pgipc__writer_handle_grant() - shared grant-processing path used by both
 * the synchronous connect()-time handshake and the async ctrl thread.
 * @ctx:       session handle
 * @grant:     grant for session
 * @frame_fd:  frame-ready eventfd received via SCM_RIGHTS alongside @grant, or -1 if
 *             none arrived (treated as a malformed grant)
 *
 * Attaches the shm ring and adopts @frame_fd BEFORE flipping @ctx->active so no caller
 * can observe active==true with a NULL ring / invalid frame_fd (see note on
 * pgipc_writer_is_active()). Closes any previously-held frame_fd first, since a
 * re-grant (e.g. after being deactivated and reactivated) carries a fresh fd.
 *
 * Returns 0 on success, -1 if shm attachment failed or @frame_fd < 0 (caller should
 * treat the session as unusable).
 */
static int pgipc__writer_handle_grant(pgipc_writer_ctx_t *ctx,
                                      const pgipc_grant_msg_t *grant, int frame_fd) {
  if (!ctx->ring)
    ctx->ring = pgipc_shm_ring_attach(50, 100);

  if (!ctx->ring || frame_fd < 0) {
    fprintf(stderr, "[pgipc:%s] could not attach to shm ring or frame-ready eventfd\n",
            ctx->app_id);
    if (frame_fd >= 0)
      close(frame_fd);
    return -1;
  }

  if (ctx->frame_fd >= 0)
    close(ctx->frame_fd);
  ctx->frame_fd = frame_fd;

  pgipc__astore_u32(&ctx->granted_generation, grant->generation);
  pgipc__astore_bool(&ctx->active, true);

  printf("[pgipc:%s] ACTIVATED, generation=%u\n", ctx->app_id, grant->generation);
  fflush(stdout);
  return 0;
}
#endif /* LIBPGIPC_WRITER */

// ---------------------------------------------------------------------------
// shm ring implementation
// ---------------------------------------------------------------------------

/** pgipc__ring_lock() - acquire @ring's bookkeeping mutex
 * @ring: attached/created ring
 *
 * Recovers from a prior holder crashing (EOWNERDEAD) instead of deadlocking.
 */
static void pgipc__ring_lock(pgipc_shm_ring_t *ring) {
  int rc = pthread_mutex_lock(&ring->bookkeeping_lock);
  if (rc == EOWNERDEAD) {
    pthread_mutex_consistent(&ring->bookkeeping_lock);
  }
}

/** pgipc__ring_unlock() - release @ring's bookkeeping mutex */
static void pgipc__ring_unlock(pgipc_shm_ring_t *ring) {
  pthread_mutex_unlock(&ring->bookkeeping_lock);
}

#ifdef LIBPGIPC_READER
PGIPC_DEF pgipc_shm_ring_t *pgipc_shm_ring_create(void) {
  if (!__atomic_always_lock_free(sizeof(int32_t), 0) ||
      !__atomic_always_lock_free(sizeof(uint32_t), 0)) {
    fprintf(stderr, "[pgipc] WARNING: 32-bit atomics are not always lock-free on this "
                    "platform; cross-process atomics may not work as intended.\n");
  }

  shm_unlink(PGIPC_SHM_NAME);

  int fd = shm_open(PGIPC_SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0666);
  if (fd < 0) {
    perror("shm_open (create)");
    return NULL;
  }

  if (ftruncate(fd, (off_t)sizeof(pgipc_shm_ring_t)) != 0) {
    perror("ftruncate");
    close(fd);
    return NULL;
  }

  pgipc_shm_ring_t *ring = (pgipc_shm_ring_t *)mmap(
      NULL, sizeof(pgipc_shm_ring_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  close(fd);
  if (ring == MAP_FAILED) {
    perror("mmap");
    return NULL;
  }

  memset(ring, 0, sizeof(pgipc_shm_ring_t));
  pgipc__astore_i32(&ring->latest_ready, -1);
  pgipc__astore_i32(&ring->reader_locked, -1);
  pgipc__astore_u32(&ring->frame_counter, 0);
  pgipc__astore_u32(&ring->generation, 0);

  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
  pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
  pthread_mutex_init(&ring->bookkeeping_lock, &attr);
  pthread_mutexattr_destroy(&attr);

  return ring;
}

PGIPC_DEF void pgipc_shm_ring_destroy(pgipc_shm_ring_t *ring) {
  if (ring) {
    pthread_mutex_destroy(&ring->bookkeeping_lock);
    munmap(ring, sizeof(pgipc_shm_ring_t));
  }
  shm_unlink(PGIPC_SHM_NAME);
}

PGIPC_DEF int pgipc_frame_fd_create(void) {
  int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (fd < 0)
    perror("eventfd (frame_fd create)");
  return fd;
}

PGIPC_DEF int pgipc_shm_ring_checkout(pgipc_shm_ring_t *ring) {
  pgipc__ring_lock(ring);
  int idx = pgipc__aload_i32(&ring->latest_ready);
  if (idx >= 0)
    pgipc__astore_i32(&ring->reader_locked, idx);
  pgipc__ring_unlock(ring);
  return idx;
}

PGIPC_DEF void pgipc_shm_ring_release(pgipc_shm_ring_t *ring) {
  pgipc__ring_lock(ring);
  pgipc__astore_i32(&ring->reader_locked, -1);
  pgipc__ring_unlock(ring);
}

PGIPC_DEF void pgipc_evict_writer(pgipc_shm_ring_t *ring) {
  pgipc__ring_lock(ring);
  pgipc__afetch_add_u32(&ring->generation, 1);
  pgipc__astore_i32(&ring->latest_ready, -1);
  pgipc__ring_unlock(ring);
}
#endif /* LIBPGIPC_READER */

#ifdef LIBPGIPC_WRITER
PGIPC_DEF pgipc_shm_ring_t *pgipc_shm_ring_attach(int max_retries, int retry_delay_ms) {
  int fd = -1;

  for (int i = 0; i < max_retries; i++) {
    fd = shm_open(PGIPC_SHM_NAME, O_RDWR, 0666);
    if (fd >= 0)
      break;
    struct timespec ts = {.tv_sec = retry_delay_ms / 1000,
                          .tv_nsec = (retry_delay_ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
  }

  if (fd < 0) {
    fprintf(stderr, "[pgipc] gave up waiting for shm segment %s\n", PGIPC_SHM_NAME);
    return NULL;
  }

  pgipc_shm_ring_t *ring = (pgipc_shm_ring_t *)mmap(
      NULL, sizeof(pgipc_shm_ring_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  close(fd);

  if (ring == MAP_FAILED) {
    perror("mmap");
    return NULL;
  }
  return ring;
}

PGIPC_DEF int pgipc_shm_ring_pick_write_slot(pgipc_shm_ring_t *ring) {
  pgipc__ring_lock(ring);
  int ready = pgipc__aload_i32(&ring->latest_ready);
  int locked = pgipc__aload_i32(&ring->reader_locked);

  int slot = 0;
  for (int i = 0; i < PGIPC_NUM_BUFFERS; i++) {
    if (i != ready && i != locked) {
      slot = i;
      break;
    }
  }

  pgipc__ring_unlock(ring);
  return slot;
}

PGIPC_DEF void pgipc_shm_ring_publish(pgipc_shm_ring_t *ring, int idx,
                                      uint64_t frame_id, uint64_t now_ns) {
  ring->frame_id[idx] = frame_id;
  ring->write_ts_ns[idx] = now_ns;
  pgipc__ring_lock(ring);
  pgipc__astore_i32(&ring->latest_ready, idx);
  pgipc__ring_unlock(ring);
}
#endif /* LIBPGIPC_WRITER */

// ---------------------------------------------------------------------------
// Control protocol framing (shared by both sides)
// ---------------------------------------------------------------------------

PGIPC_DEF int pgipc_ctrl_send(int fd, pgipc_msg_type_t type, const void *payload,
                              uint32_t len) {
  unsigned char hdr[5];
  uint32_t nlen = htonl(len);

  hdr[0] = (unsigned char)type;
  memcpy(hdr + 1, &nlen, sizeof(hdr) - 1);

  if (pgipc__send_all(fd, hdr, sizeof(hdr)) != 0)
    return -1;

  if (len > 0 && pgipc__send_all(fd, payload, len) != 0)
    return -1;

  return 0;
}

PGIPC_DEF int pgipc_ctrl_recv(int fd, pgipc_msg_type_t *out_type, void *buf,
                              uint32_t bufsize, uint32_t *out_len) {
  unsigned char hdr[5];
  uint32_t nlen;

  if (pgipc__recv_all(fd, hdr, sizeof(hdr)) != 0)
    return -1;

  *out_type = (pgipc_msg_type_t)hdr[0];

  memcpy(&nlen, hdr + 1, 4);
  uint32_t len = ntohl(nlen);

  if (len > bufsize)
    return -2;
  if (len > 0 && pgipc__recv_all(fd, buf, len) != 0)
    return -1;

  *out_len = len;
  return 0;
}

// ---------------------------------------------------------------------------
// fd-carrying framing (SCM_RIGHTS)
// ---------------------------------------------------------------------------

#if defined(LIBPGIPC_WRITER) || defined(LIBPGIPC_READER)

PGIPC_DEF int pgipc_ctrl_send_fds(int fd, pgipc_msg_type_t type, const void *payload,
                                  uint32_t len, const int *fds, int nfds) {
  unsigned char hdr[5];
  uint32_t nlen = htonl(len);
  hdr[0] = (unsigned char)type;
  memcpy(hdr + 1, &nlen, sizeof(hdr) - 1);

  if (nfds <= 0)
    return pgipc_ctrl_send(fd, type, payload, len);
  if (nfds > PGIPC_NUM_BUFFERS)
    return -1;

  /* Ancillary data must ride with actual bytes; attach it to the header. */
  struct iovec iov = {.iov_base = hdr, .iov_len = sizeof(hdr)};
  union { /* aligned cmsg buffer */
    char buf[CMSG_SPACE(sizeof(int) * PGIPC_NUM_BUFFERS)];
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
      pgipc__send_all(fd, hdr + n, sizeof(hdr) - (size_t)n) != 0)
    return -1;

  if (len > 0 && pgipc__send_all(fd, payload, len) != 0)
    return -1;

  return 0;
}
#endif /* LIBPGIPC_WRITER || LIBPGIPC_READER */

#if defined(LIBPGIPC_WRITER) || defined(LIBPGIPC_READER)
/** pgipc__close_fds() - close every valid (>=0) fd in @fds[0..nfds).
 *
 * Used by fd-receiving/bookkeeping paths to avoid leaking fds on error or after a
 * dmabuf set is retired.
 */
static void pgipc__close_fds(int *fds, int nfds) {
  for (int i = 0; i < nfds; i++)
    if (fds[i] >= 0)
      close(fds[i]);
}

PGIPC_DEF int pgipc_ctrl_recv_fds(int fd, pgipc_msg_type_t *out_type, void *buf,
                                  uint32_t bufsize, uint32_t *out_len, int *out_fds,
                                  int max_fds, int *out_nfds) {
  unsigned char hdr[5];
  uint32_t nlen, len;
  *out_nfds = 0;

  struct iovec iov = {.iov_base = hdr, .iov_len = sizeof(hdr)};
  union {
    char buf[CMSG_SPACE(sizeof(int) * PGIPC_NUM_BUFFERS)];
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
      pgipc__recv_all(fd, hdr + n, sizeof(hdr) - (size_t)n) != 0)
    goto fail;

  *out_type = (pgipc_msg_type_t)hdr[0];

  memcpy(&nlen, hdr + 1, 4);
  len = ntohl(nlen);

  if (len > bufsize) {
    pgipc__close_fds(out_fds, *out_nfds);
    *out_nfds = 0;
    return -2;
  }
  if (len > 0 && pgipc__recv_all(fd, buf, len) != 0)
    goto fail;

  *out_len = len;
  return 0;

fail:
  pgipc__close_fds(out_fds, *out_nfds);
  *out_nfds = 0;
  return -1;
}
#endif /* LIBPGIPC_WRITER || LIBPGIPC_READER */

// ---------------------------------------------------------------------------
// display-side dmabuf bookkeeping
// ---------------------------------------------------------------------------

#ifdef LIBPGIPC_READER
PGIPC_DEF int pgipc_dmabuf_set_from_announce(pgipc_dmabuf_set_t *set,
                                             const pgipc_dmabuf_announce_msg_t *msg,
                                             const int *fds, int nfds) {
  memset(set, 0, sizeof(*set));
  for (int i = 0; i < PGIPC_NUM_BUFFERS; i++)
    set->fds[i] = -1;

  if (msg->num_buffers != PGIPC_NUM_BUFFERS || nfds != PGIPC_NUM_BUFFERS) {
    int tmp[PGIPC_NUM_BUFFERS];

    for (int i = 0; i < nfds && i < PGIPC_NUM_BUFFERS; i++)
      tmp[i] = fds[i];

    pgipc__close_fds(tmp, nfds < PGIPC_NUM_BUFFERS ? nfds : PGIPC_NUM_BUFFERS);
    return -1;
  }

  for (int i = 0; i < PGIPC_NUM_BUFFERS; i++) {
    const pgipc_dmabuf_desc_t *d = &msg->desc[i];

    /* stride is in bytes; must cover the row */
    if (fds[i] < 0 || d->width == 0 || d->height == 0 || d->stride == 0 ||
        d->stride < d->width) {
      pgipc__close_fds((int *)fds, PGIPC_NUM_BUFFERS);
      return -1;
    }
  }

  for (int i = 0; i < PGIPC_NUM_BUFFERS; i++) {
    set->fds[i] = fds[i];
    set->desc[i] = msg->desc[i];
  }

  set->valid = true;
  return 0;
}

PGIPC_DEF void pgipc_dmabuf_set_close(pgipc_dmabuf_set_t *set) {
  if (!set)
    return;

  pgipc__close_fds(set->fds, PGIPC_NUM_BUFFERS);

  for (int i = 0; i < PGIPC_NUM_BUFFERS; i++)
    set->fds[i] = -1;
  set->valid = false;
}
#endif /* LIBPGIPC_READER */

// ---------------------------------------------------------------------------
// writer API
// ---------------------------------------------------------------------------

#ifdef LIBPGIPC_WRITER

/** pgipc__writer_ctrl - background ctrl thread for writers.
 * @arg: writer session handle
 *
 * While writer is running, send heartbeat periodically.
 * While writer is active,
 */
static void *pgipc__writer_ctrl(void *arg) {
  pgipc_writer_ctx_t *ctx = (pgipc_writer_ctx_t *)arg;
  struct timespec last_heartbeat = {0};
  struct timespec last_activate_attempt = {0};
  struct timeval tv = {.tv_sec = 0, .tv_usec = PGIPC_HEARTBEAT_INTERVAL_MS * 1000};

  setsockopt(ctx->ctrl_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  while (pgipc__aload_bool(&ctx->running)) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    long since_hb_ms = (now.tv_sec - last_heartbeat.tv_sec) * 1000 +
                       (now.tv_nsec - last_heartbeat.tv_nsec) / 1000000;

    if (pgipc__aload_bool(&ctx->active) && since_hb_ms >= PGIPC_HEARTBEAT_INTERVAL_MS) {
      pgipc_heartbeat_msg_t hb = {
          .generation = pgipc__aload_u32(&ctx->granted_generation),
          .frame_counter = ctx->ring ? pgipc__aload_u32(&ctx->ring->frame_counter) : 0,
      };

      pthread_mutex_lock(&ctx->send_lock);
      pgipc_ctrl_send(ctx->ctrl_fd, PGIPC_MSG_HEARTBEAT, &hb, sizeof(hb));
      pthread_mutex_unlock(&ctx->send_lock);
      last_heartbeat = now;
    }

    long since_req_ms = (now.tv_sec - last_activate_attempt.tv_sec) * 1000 +
                        (now.tv_nsec - last_activate_attempt.tv_nsec) / 1000000;

    if (!pgipc__aload_bool(&ctx->active) && since_req_ms >= PGIPC_RETRY_ACTIVATE_MS) {
      pthread_mutex_lock(&ctx->send_lock);
      pgipc_ctrl_send(ctx->ctrl_fd, PGIPC_MSG_ACTIVATE_REQUEST, NULL, 0);
      pthread_mutex_unlock(&ctx->send_lock);
      last_activate_attempt = now;
    }

    pgipc_msg_type_t type;
    unsigned char buf[256];
    uint32_t len;
    int fds[1];
    int nfds = 0;

    int rc = pgipc_ctrl_recv_fds(ctx->ctrl_fd, &type, buf, sizeof(buf), &len, fds,
                                 1, &nfds);
    if (rc == -1)
      continue; /* timeout or disconnect */
    if (rc == -2)
      continue; /* oversized message */

    switch (type) {
    case PGIPC_MSG_ACTIVATE_GRANT: {
      pgipc_grant_msg_t grant;

      memcpy(&grant, buf, sizeof(grant));
      /* Shared helper attaches the shm ring and adopts the received frame-ready
       * eventfd BEFORE setting active=true, so a concurrent pgipc_writer_is_active()
       * can never observe active with a NULL ring / invalid frame_fd. */
      pgipc__writer_handle_grant(ctx, &grant, nfds > 0 ? fds[0] : -1);
      break;
    }
    case PGIPC_MSG_ACTIVATE_DENY: {
      pgipc_deny_msg_t deny;

      memcpy(&deny, buf, sizeof(deny));
      printf("[pgipc:%s] activation denied: %s (will retry)\n", ctx->app_id,
             deny.reason);
      fflush(stdout);
      break;
    }
    case PGIPC_MSG_DEACTIVATE:
      pgipc__astore_bool(&ctx->active, false);
      printf("[pgipc:%s] DEACTIVATED (another app took over)\n", ctx->app_id);
      fflush(stdout);
      break;
    case PGIPC_MSG_DMABUF_ACK: {
      pgipc_dmabuf_ack_msg_t ack;

      memcpy(&ack, buf, sizeof(ack) < len ? sizeof(ack) : len);

      if (ack.accepted) {
        pgipc__astore_i32(&ctx->payload_kind, PGIPC_PAYLOAD_DMABUF);
        pgipc__astore_i32(&ctx->dmabuf_ack_state, 1);
        printf("[pgipc:%s] dmabuf set accepted; session is now zero-copy\n",
               ctx->app_id);
      } else {
        ack.reason[PGIPC_DENY_REASON_LEN - 1] = '\0';
        pgipc__astore_i32(&ctx->dmabuf_ack_state, -1);
        printf("[pgipc:%s] dmabuf set refused: %s (staying in PIXELS mode)\n",
               ctx->app_id, ack.reason);
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
#endif /* LIBPGIPC_WRITER */

#ifdef LIBPGIPC_WRITER
PGIPC_DEF pgipc_writer_ctx_t *pgipc_writer_connect(const char *app_id,
                                                   const pgipc_render_mode_t *modes,
                                                   int num_modes) {
  pgipc_writer_ctx_t *ctx = (pgipc_writer_ctx_t *)calloc(1, sizeof(pgipc_writer_ctx_t));
  if (!ctx)
    return NULL;

  strncpy(ctx->app_id, app_id, PGIPC_APP_ID_LEN - 1);
  pthread_mutex_init(&ctx->send_lock, NULL);
  pgipc__astore_bool(&ctx->running, true);
  pgipc__astore_bool(&ctx->active, false);
  pgipc__astore_i32(&ctx->payload_kind, PGIPC_PAYLOAD_PIXELS);
  pgipc__astore_i32(&ctx->dmabuf_ack_state, 0);
  ctx->frame_fd = -1;

  int fd = -1;
  for (int i = 0; i < 100; i++) {
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
      break;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, PGIPC_CONTROL_SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
      break;

    close(fd);
    fd = -1;

    struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000L};
    nanosleep(&ts, NULL);
  }

  if (fd < 0) {
    fprintf(stderr, "[pgipc:%s] could not connect to display control socket\n", app_id);
    free(ctx);
    return NULL;
  }
  ctx->ctrl_fd = fd;

  pgipc_hello_msg_t hello;
  memset(&hello, 0, sizeof(hello));
  strncpy(hello.app_id, ctx->app_id, PGIPC_APP_ID_LEN - 1);
  hello.num_modes =
      (uint32_t)(num_modes > PGIPC_MAX_MODES ? PGIPC_MAX_MODES : num_modes);
  for (uint32_t i = 0; i < hello.num_modes; i++)
    hello.modes[i] = modes[i];
  pgipc_ctrl_send(ctx->ctrl_fd, PGIPC_MSG_CONNECT, &hello, sizeof(hello));

  pgipc_msg_type_t type;
  unsigned char buf[256];
  uint32_t len;

  if (pgipc_ctrl_recv(ctx->ctrl_fd, &type, buf, sizeof(buf), &len) == 0 &&
      type == PGIPC_MSG_MODE) {
    pgipc_mode_msg_t mode;

    memcpy(&mode, buf, sizeof(mode));
    if (!mode.accepted) {
      fprintf(stderr, "[pgipc:%s] display rejected all offered modes\n", app_id);
      close(fd);
      pthread_mutex_destroy(&ctx->send_lock);
      free(ctx);
      return NULL;
    }

    ctx->mode = mode.chosen;
    printf("[pgipc:%s] negotiated mode: %ux%u@%ufps\n", app_id, mode.chosen.width,
           mode.chosen.height, mode.chosen.fps);
  }

  type = (pgipc_msg_type_t)-1;
  len = (uint32_t)-1;
  memset(buf, 0, sizeof(buf));

  /* --- Initial activation handshake (synchronous) ----------------------- */
  /* Send the first request here so the ctrl thread starts in a known state. */
  pgipc_ctrl_send(ctx->ctrl_fd, PGIPC_MSG_ACTIVATE_REQUEST, NULL, 0);

  int grant_fds[1];
  int grant_nfds = 0;

  if (pgipc_ctrl_recv_fds(ctx->ctrl_fd, &type, buf, sizeof(buf), &len, grant_fds, 1,
                          &grant_nfds) == 0) {
    if (type == PGIPC_MSG_ACTIVATE_GRANT) {
      pgipc_grant_msg_t grant;

      memcpy(&grant, buf, sizeof(grant));

      /* Same helper the ctrl thread uses for later grants -- keeps the
       * attach-before-active ordering in exactly one place. */
      if (pgipc__writer_handle_grant(ctx, &grant,
                                     grant_nfds > 0 ? grant_fds[0] : -1) != 0) {
        close(fd);
        pthread_mutex_destroy(&ctx->send_lock);
        free(ctx);
        return NULL;
      }
    } else if (type == PGIPC_MSG_ACTIVATE_DENY) {
      pgipc_deny_msg_t deny;

      memcpy(&deny, buf, sizeof(deny));

      /* Resources stay NULL; ctrl thread will retry and attach on grant. */
      printf("[pgipc:%s] initial activation denied: %s (ctrl thread will "
             "retry)\n",
             ctx->app_id, deny.reason);
      fflush(stdout);
    }
    /* Any other message (e.g. unexpected MSG_MODE) is ignored; the ctrl
     * thread will recover via the periodic retry logic. */
  }

  pthread_create(&ctx->ctrl_thread, NULL, pgipc__writer_ctrl, ctx);
  return ctx;
}

PGIPC_DEF bool pgipc_writer_is_active(pgipc_writer_ctx_t *ctx) {
  return pgipc__aload_bool(&ctx->active);
}

PGIPC_DEF pgipc_render_mode_t pgipc_writer_negotiated_mode(pgipc_writer_ctx_t *ctx) {
  return ctx->mode;
}

PGIPC_DEF pgipc_payload_kind_t pgipc_writer_payload_kind(pgipc_writer_ctx_t *ctx) {
  return (pgipc_payload_kind_t)pgipc__aload_i32(&ctx->payload_kind);
}

PGIPC_DEF int
pgipc_writer_announce_dmabufs(pgipc_writer_ctx_t *ctx, const int fds[PGIPC_NUM_BUFFERS],
                              const pgipc_dmabuf_desc_t desc[PGIPC_NUM_BUFFERS],
                              int timeout_ms) {
  pgipc_dmabuf_announce_msg_t msg;
  memset(&msg, 0, sizeof(msg));

  msg.num_buffers = PGIPC_NUM_BUFFERS;
  for (int i = 0; i < PGIPC_NUM_BUFFERS; i++)
    msg.desc[i] = desc[i];

  pgipc__astore_i32(&ctx->dmabuf_ack_state, 0);

  pthread_mutex_lock(&ctx->send_lock);
  int rc = pgipc_ctrl_send_fds(ctx->ctrl_fd, PGIPC_MSG_DMABUF_ANNOUNCE, &msg,
                               sizeof(msg), fds, PGIPC_NUM_BUFFERS);
  pthread_mutex_unlock(&ctx->send_lock);
  if (rc != 0)
    return -1;

  /* The ctrl thread owns the socket's read side, so wait for it to flip the
   * ack flag rather than reading here ourselves. Polling at 5ms granularity
   * is plenty for a one-time setup handshake. */
  struct timespec poll_ts = {.tv_sec = 0, .tv_nsec = 5 * 1000000L};
  long waited_ms = 0;
  for (;;) {
    int st = pgipc__aload_i32(&ctx->dmabuf_ack_state);

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

PGIPC_DEF int pgipc_writer_write_slot(pgipc_writer_ctx_t *ctx) {
  return pgipc_shm_ring_pick_write_slot(ctx->ring);
}

PGIPC_DEF void pgipc_writer_publish(pgipc_writer_ctx_t *ctx, int idx,
                                    uint64_t frame_id) {
  if (!pgipc__aload_bool(&ctx->active))
    return;
  if (pgipc__aload_u32(&ctx->ring->generation) !=
      pgipc__aload_u32(&ctx->granted_generation)) {
    pgipc__astore_bool(&ctx->active, false);
    return;
  }

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  uint64_t now_ns = ((uint64_t)now.tv_sec * 1000000000ULL) + now.tv_nsec;

  pgipc_shm_ring_publish(ctx->ring, idx, frame_id, now_ns);
  if (ctx->frame_fd >= 0) {
    uint64_t v = 1;
    ssize_t n;
    do {
      n = write(ctx->frame_fd, &v, sizeof(v));
    } while (n < 0 && errno == EINTR);
  }
}

PGIPC_DEF void pgipc_writer_disconnect(pgipc_writer_ctx_t *ctx) {
  pgipc__astore_bool(&ctx->running, false);

  pthread_join(ctx->ctrl_thread, NULL);
  pgipc_ctrl_send(ctx->ctrl_fd, PGIPC_MSG_DISCONNECT, NULL, 0);
  close(ctx->ctrl_fd);

  if (ctx->ring)
    munmap(ctx->ring, sizeof(pgipc_shm_ring_t));
  if (ctx->frame_fd >= 0)
    close(ctx->frame_fd);

  pthread_mutex_destroy(&ctx->send_lock);
  free(ctx);
}
#endif /* LIBPGIPC_WRITER */

// ---------------------------------------------------------------------------
// Optional GBM allocation helpers
// ---------------------------------------------------------------------------
#if defined(LIBPGIPC_ENABLE_GBM) && defined(LIBPGIPC_WRITER)

PGIPC_DEF int pgipc_gbm_bufs_create(pgipc_gbm_bufs_t *out, int drm_fd, uint32_t width,
                                    uint32_t height) {
  memset(out, 0, sizeof(*out));
  for (int i = 0; i < PGIPC_NUM_BUFFERS; i++)
    out->fds[i] = -1;

  out->gbm = gbm_create_device(drm_fd);
  if (!out->gbm) {
    fprintf(stderr, "[pgipc] gbm_create_device failed\n");
    return -1;
  }

  for (int i = 0; i < PGIPC_NUM_BUFFERS; i++) {
    /* LINEAR so the display's KMS import never has to guess a modifier;
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
    out->desc[i].fourcc = PGIPC_FORMAT_XRGB8888;
    out->desc[i].stride = gbm_bo_get_stride(out->bo[i]);
    out->desc[i].offset = gbm_bo_get_offset(out->bo[i], 0);
    out->desc[i].modifier = gbm_bo_get_modifier(out->bo[i]);
    if (out->desc[i].modifier == PGIPC_MODIFIER_INVALID)
      out->desc[i].modifier = PGIPC_MODIFIER_LINEAR; /* we asked for LINEAR */
  }
  return 0;

fail:
  pgipc_gbm_bufs_destroy(out);
  return -1;
}

PGIPC_DEF void pgipc_gbm_bufs_destroy(pgipc_gbm_bufs_t *bufs) {
  if (!bufs)
    return;

  for (int i = 0; i < PGIPC_NUM_BUFFERS; i++) {
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

#endif /* LIBPGIPC_ENABLE_GBM && LIBPGIPC_WRITER */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBPGIPC_IMPLEMENTATION && !LIBPGIPC_IMPLEMENTATION_DONE */
