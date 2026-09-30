#pragma once

#include <wisp/types.h>
#include <wispc/clock.h>
#include <wispc/events.h>
#include <wispc/render.h>

/**
 * WISPC_API - marks a symbol as part of wispc's public ABI.
 *
 * wispc is built with C_VISIBILITY_PRESET hidden, so every symbol is hidden by default;
 * only functions explicitly marked WISPC_API are exported from libwispc.so. This keeps
 * internal helpers and the statically-linked wisp_protocol symbols out of the shared
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
 * Opaque; treat as a handle. Returned by ::wispc_connect and passed to (almost) every
 * other wispc call.
 */
typedef struct _wispc_ctx_t wispc_ctx_t;

/**
 * typedef wispc_client_cfg_t - client configuration for connection
 * @event_cfg:  configuration for the client's event callbacks
 * @clock_cfg:  configuration for the client's clock
 * @render_cfg: configuration for the renderer
 * @screen_cfg: configuration for the client's screen and supported resolutions
 */
typedef struct {
  wispc_event_cfg_t event_cfg;
  wispc_clock_cfg_t clock_cfg;
  wispc_render_cfg_t render_cfg;
  wispc_screen_cfg_t screen_cfg;
} wispc_client_cfg_t;

/**
 * wispc_connect() - establish a client session with the server
 * @client_id: this client's application id (truncated to WISP_CLIENT_ID_LEN-1 bytes)
 * @cfg:       this client's configuration
 *
 * Connects to the server's control socket, performs the negotiation, and spawns a
 * background control thread that maintains heartbeats and processes all future
 * messages for the lifetime of the session.
 *
 * Return: a new session context, or NULL :
 *  - if the control socket could not be reached
 *  - the server rejected all resolutions or render kind
 *  - user callbacks are malformed
 */
WISPC_API wispc_ctx_t *wispc_connect(const char *client_id, wispc_client_cfg_t cfg);

/**
 * wispc_activate() - attempt to acquire the activation grant for this client once.
 * @ctx: session handle
 *
 * Only __one__ client may be active at a time.
 *
 * Return: 0 on activation, queue position > 0 if queued, or a negative error code on
 * failure.
 */
WISPC_API int wispc_activate(wispc_ctx_t *ctx);

/**
 * wispc_deactivate() - release the activation grant for this client session
 * @ctx: session handle
 *
 * If the client is not currently active but in the queue, it will release its position.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
WISPC_API int wispc_deactivate(wispc_ctx_t *ctx);

/**
 * wispc_disconnect() - tear down a client session
 * @ctx: session handle; freed by this call and must not be used again
 *
 * Stops the control thread, sends WISP_MSG_DISCONNECT, closes the control socket,
 * unmaps the shm ring, and frees @ctx.
 */
WISPC_API void wispc_disconnect(wispc_ctx_t *ctx);

/**
 * wispc_is_active() - check whether this session is the active client
 * @ctx: session handle
 * @queue_position: if not NULL, will be set to the client's current position in the
 * activation queue. 0 indicates that this client is currently active.
 *
 * Return: true if this client currently holds the activation grant and may
 * write/publish frames.
 */
WISPC_API bool wispc_is_active(wispc_ctx_t *ctx, uint32_t *queue_position);

/**
 * wispc_resolution() - the mode chosen during connect
 * @ctx: session handle
 *
 * Return: the render mode accepted by the server at connect time.
 */
WISPC_API wisp_resolution_t wispc_resolution(wispc_ctx_t *ctx);

/**
 * wispc_render_kind() - which mode this session is operating in
 * @ctx: session handle
 *
 * Return: WISP_PAYLOAD_PIXELS or WISP_PAYLOAD_DMABUF.
 */
WISPC_API wisp_render_kind_t wispc_render_kind(wispc_ctx_t *ctx);

/**
 * wispc_notify_tick() - signal the render thread to run one ::render_frame call
 * @ctx: session handle
 *
 * Only meaningful under ::WISPC_CLOCK_KIND_TICKED; a no-op otherwise. Typically
 * called from a separate app-owned simulation/scene thread once per completed
 * simulation step, never from the render thread itself. Safe to call from any thread;
 * does not block. If the render thread is still inside a previous frame render call
 * when this is called again, the tick is recorded as missed and reported via
 * ::wispc_on_render_backpressure once that call returns.
 */
WISPC_API void wispc_notify_tick(wispc_ctx_t *ctx);

#ifdef __cplusplus
} /* extern "C" { */
#endif