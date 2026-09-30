// admin_proto.h - wire format for the server's admin/management socket.
//
// Deliberately separate from the client-facing wisp protocol (wisp/protocol): producer
// apps never link against or even see this protocol. Reuses wisp_ctrl_send()/
// wisp_ctrl_recv() (wisp/wire.h) for wire framing, since those are part of the shared,
// side-agnostic protocol surface.
//
// Every admin connection is short-lived: connect -> send one request -> receive one
// response -> close. There is no persistent admin session state and no heartbeats,
// unlike the producer control protocol.
#ifndef WISPS_ADMIN_PROTO_H
#define WISPS_ADMIN_PROTO_H

#include <stdint.h>

#include <wisp/wire.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * WISPS_ADMIN_SOCK_PATH - path of the server's admin Unix domain socket
 *
 * Separate from the producer control socket so producer apps never need to know this
 * protocol exists at all.
 */
#define WISPS_ADMIN_SOCK_PATH "/dev/shm/frame_ring_admin.sock"

/**
 * WISPS_ADMIN_MAX_CLIENTS - upper bound on LIST_RESPONSE entries
 *
 * Matches WISPS_SESSION_MAX_CLIENTS (session_table.h), duplicated here
 * rather than included so this wire-format header has zero dependency on
 * the server's internal session table representation -- only libwisp.h
 * itself. Keep these two constants in sync if either changes.
 */
#define WISPS_ADMIN_MAX_CLIENTS 8

/**
 * WISPS_ADMIN_REASON_LEN - max bytes (incl NUL) in a switch failure reason
 */
#define WISPS_ADMIN_REASON_LEN 64

/**
 * enum wisps_admin_msg_type_t - admin protocol message types
 * @WISPS_ADMIN_MSG_LIST_REQUEST:    client -> server: send me a session table snapshot
 * @WISPS_ADMIN_MSG_LIST_RESPONSE:   server -> client: snapshot reply
 * @WISPS_ADMIN_MSG_SWITCH_REQUEST:  client -> server: activate this client_id
 * @WISPS_ADMIN_MSG_SWITCH_RESPONSE: server -> client: outcome
 *
 * A deliberately separate, smaller enum from wisp_msg_kind_t (admin messages ride the
 * same 1-byte-type + 4-byte-BE-length framing for wisps_ctrl_send/recv) but must never
 * be confusable with producer control messages, even though the two protocols share
 * framing code and could theoretically be sent down the wrong socket by a bug.
 */
typedef enum {
  WISPS_ADMIN_MSG_LIST_REQUEST = 1,
  WISPS_ADMIN_MSG_LIST_RESPONSE = 2,
  WISPS_ADMIN_MSG_SWITCH_REQUEST = 3,
  WISPS_ADMIN_MSG_SWITCH_RESPONSE = 4,
} wisps_admin_msg_type_t;

/**
 * enum wisps_admin_client_state_t - wire representation of session state
 * @WISPS_ADMIN_STATE_CONNECTED:            socket accepted, no CONNECT yet
 * @WISPS_ADMIN_STATE_NEGOTIATED:           mode accepted; idle, not requesting/
 *                                          holding the RT slot
 * @WISPS_ADMIN_STATE_QUEUED:               requested activation; waiting for the RT
 *                                          slot to free up
 * @WISPS_ADMIN_STATE_GRANTED_WARMUP:       granted the RT slot; warming up and
 *                                          self-promoting scheduling policy, not
 *                                          yet producing frames
 * @WISPS_ADMIN_STATE_ACTIVE_RT:            holds the RT slot and is producing
 *                                          frames
 * @WISPS_ADMIN_STATE_EVICTING_COOPERATIVE: wind-down in progress after a
 *                                          cooperative eviction request
 * @WISPS_ADMIN_STATE_EVICTING_FORCED:      being forcibly demoted/reclaimed after a
 *                                          grace-period or liveness timeout
 * @WISPS_ADMIN_STATE_REJECTED:             mode negotiation failed; connection
 *                                          closing
 *
 * Intentionally a separate enum from the server's internal wisps_session_state_t:
 * this header must not depend on that internal representation, so the admin plane is
 * responsible for translating one to the other. Keep the two enums' meanings (and
 * value order) in sync if either changes.
 */
typedef enum {
  WISPS_ADMIN_STATE_CONNECTED = 0,
  WISPS_ADMIN_STATE_NEGOTIATED = 1,
  WISPS_ADMIN_STATE_QUEUED = 2,
  WISPS_ADMIN_STATE_GRANTED_WARMUP = 3,
  WISPS_ADMIN_STATE_ACTIVE_RT = 4,
  WISPS_ADMIN_STATE_EVICTING_COOPERATIVE = 5,
  WISPS_ADMIN_STATE_EVICTING_FORCED = 6,
  WISPS_ADMIN_STATE_REJECTED = 7,
} wisps_admin_client_state_t;

/**
 * struct wisps_admin_client_info_t - one LIST_RESPONSE entry
 * @client_id:          NUL-terminated app id
 * @state:           current session state (wire enum, see above)
 * @negotiated_mode: valid once state is NEGOTIATED or later
 * @render_kind:    PIXELS or DMABUF (wisp_render_kind_t). That enum IS declared
 *                   unconditionally, so reusing it here directly is fine.
 */
typedef struct {
  char client_id[WISP_CLIENT_ID_LEN];
  wisps_admin_client_state_t state;
  wisp_resolution_t negotiated_mode;
  wisp_render_kind_t render_kind;
} wisps_admin_client_info_t;

/**
 * struct wisps_admin_list_response_t - WISPS_ADMIN_MSG_LIST_RESPONSE payload
 * @count:   number of valid entries in @clients (0..WISPS_ADMIN_MAX_CLIENTS)
 * @clients: session table snapshot, in slot-index order
 */
typedef struct {
  uint32_t count;
  wisps_admin_client_info_t clients[WISPS_ADMIN_MAX_CLIENTS];
} wisps_admin_list_response_t;

/**
 * struct wisps_admin_switch_request_t - WISPS_ADMIN_MSG_SWITCH_REQUEST payload
 * @client_id: NUL-terminated app id to activate
 */
typedef struct {
  char client_id[WISP_CLIENT_ID_LEN];
} wisps_admin_switch_request_t;

/**
 * struct wisps_admin_switch_response_t - WISPS_ADMIN_MSG_SWITCH_RESPONSE payload
 * @ok:     1 if the switch succeeded (or was already a no-op)
 * @reason: human-readable failure reason, valid when @ok == 0
 */
typedef struct {
  uint8_t ok;
  char reason[WISPS_ADMIN_REASON_LEN];
} wisps_admin_switch_response_t;

#ifdef __cplusplus
}
#endif

#endif // WISPS_ADMIN_PROTO_H
