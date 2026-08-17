#ifndef WISPC_CLIENT_H
#define WISPC_CLIENT_H

#include <wisp/types.h>

/**
 * WISPC_API - marks a symbol as part of wispc's public ABI.
 *
 * wispc is built with C_VISIBILITY_PRESET hidden (see CMakeLists.txt), so every symbol
 * is hidden by default; only functions explicitly marked WISPC_API are exported from
 * libwispc.so. This keeps internal helpers (client_internal.h) and the
 * statically-linked wisp_protocol symbols (wisp_ctrl_send/recv, etc.) out of the shared
 * library's public symbol table.
 *
 * Target is Yocto/OE-core on Raspberry Pi (GCC/Clang only); no MSVC/dllexport needed.
 */
#define WISPC_API __attribute__((visibility("default")))

#ifdef __cplusplus
extern "C" {
#endif

/**
 * typedef wispc_ctx_t - client session handle
 *
 * Opaque; treat as a handle. Returned by wispc_connect() and passed to (almost) every
 * other wispc_*() call.
 */
typedef struct _wispc_ctx_t wispc_ctx_t;

/**
 * wispc_connect() - establish a client session with the server
 * @client_id:     this client's application id (truncated to WISP_CLIENT_ID_LEN-1
 * bytes)
 * @modes:      render modes this client supports, in order of preference
 * @num_modes:  number of entries in @modes (truncated to WISP_MAX_MODES)
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
WISPC_API wispc_ctx_t *wispc_connect(const char *client_id, const wisp_render_mode_t *modes,
                           int num_modes);

/**
 * wispc_disconnect() - tear down a client session
 * @ctx: session handle; freed by this call and must not be used again
 *
 * Stops the control thread, sends WISP_MSG_DISCONNECT, closes the control socket,
 * unmaps the shm ring, and frees @ctx.
 */
WISPC_API void wispc_disconnect(wispc_ctx_t *ctx);

/**
 * wispc_is_active() - check whether this session is the client
 * @ctx: session handle
 *
 * Return: true if this client currently holds the activation grant and may
 * write/publish frames.
 */
WISPC_API bool wispc_is_active(wispc_ctx_t *ctx);

/**
 * wispc_negotiated_mode() - the mode chosen during connect
 * @ctx: session handle
 *
 * Return: the render mode accepted by the server at connect time.
 */
WISPC_API wisp_render_mode_t wispc_negotiated_mode(wispc_ctx_t *ctx);

/**
 * wispc_write_slot() - choose a free buffer index to write
 * @ctx: session handle
 *
 * Picks any index that is neither the currently-published frame nor the server's
 * currently-locked index, guaranteeing the write never tears a frame the server (or the
 * flip-away-from logic in dmabuf mode) is using. A client may only lock at most one
 * frame at any given moment.
 *
 * PIXELS mode: write your CPU-rendered frame into ctx->ring->frame_bufs[idx] (via the
 * pointer returned here), then call wispc_publish().
 * DMABUF mode: the returned index is which of your previously-announced dmabufs
 * (wispc_announce_dmabufs()) to target next.
 *
 * @note: calling this again before the previous index has been wispc_publish()'d
 * aborts the process -- a client may hold at most one claimed slot at a time.
 *
 * Return: a writable buffer index (0..WISP_NUM_BUFFERS-1).
 */
WISPC_API int wispc_write_slot(wispc_ctx_t *ctx);

/**
 * wispc_publish() - publish slot @idx as the newest frame
 * @ctx:      session handle
 * @idx:      buffer index previously returned by wispc_write_slot()
 * @frame_id: client-assigned monotonically increasing frame id
 *
 * PIXELS mode: call after your CPU write into ring->frame_bufs[idx] completes.
 * DMABUF mode: call ONLY after GPU work targeting dmabuf[idx] has fully completed
 * (glFinish() or a client-side fence wait). Publish only flips an index and posts the
 * semaphore; it cannot see in-flight GPU work.
 *
 * Drops the frame if @ctx is not active, or if the ring's generation has advanced past
 * @ctx's granted generation. In the latter case this call also flips @ctx to inactive
 * so subsequent callers see wispc_is_active() return false without waiting for the next
 * control message.
 */
WISPC_API void wispc_publish(wispc_ctx_t *ctx, int idx, uint64_t frame_id);

/**
 * wispc_payload_kind() - which mode this session is operating in
 * @ctx: session handle
 *
 * Return: WISP_PAYLOAD_PIXELS or WISP_PAYLOAD_DMABUF.
 */
WISPC_API wisp_payload_kind_t wispc_payload_kind(wispc_ctx_t *ctx);

#ifdef __cplusplus
} /* extern "C" { */
#endif

#endif /* WISPC_CLIENT_H */