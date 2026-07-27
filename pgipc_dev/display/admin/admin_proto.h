// admin_proto.h - wire format for the display's admin/management socket.
//
// Deliberately NOT part of libpgipc.h: producer apps never link against or even see
// this protocol. Reuses onlypgipc_ctrl_send()/pgipc_ctrl_recv() from libpgipc.h for
// wire framing (those two aredeclared unconditionally, regardless of
// LIBPGIPC_READER/LIBPGIPC_WRITER, precisely soheaders like this one can borrow them
// without pulling in producer- or display-onlysymbols).
//
// Every admin connection is short-lived: connect -> send one request -> receive one
// response -> close. There is no persistent admin session state and no heartbeats,
// unlike the producer control protocol.
#ifndef PGIPC_ADMIN_PROTO_H
#define PGIPC_ADMIN_PROTO_H

#include <stdint.h>

#include "libpgipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * PGIPC_ADMIN_SOCK_PATH - path of the display's admin Unix domain socket
 *
 * Separate from the producer control socket so producer apps never need to know this
 * protocol exists at all.
 */
#define PGIPC_ADMIN_SOCK_PATH "/dev/shm/frame_ring_admin.sock"

/**
 * PGIPC_ADMIN_MAX_CLIENTS - upper bound on LIST_RESPONSE entries
 *
 * Matches PGIPC_SESSION_MAX_CLIENTS (session_table.h), duplicated here
 * rather than included so this wire-format header has zero dependency on
 * the display's internal session table representation -- only libpgipc.h
 * itself. Keep these two constants in sync if either changes.
 */
#define PGIPC_ADMIN_MAX_CLIENTS 8

/**
 * PGIPC_ADMIN_REASON_LEN - max bytes (incl NUL) in a switch failure reason
 */
#define PGIPC_ADMIN_REASON_LEN 64

/**
 * enum pgipc_admin_msg_type_t - admin protocol message types
 *
 * A deliberately separate, smaller enum from pgipc_msg_type_t ( admin messages ride the
 * same 1-byte-type + 4-byte-BE-length framing for pgipc_ctrl_send/recv) but must never
 * be confusable with producer control messages, even though the two protocols share
 * framing code and could theoretically be sent down the wrong socket by a bug.
 */
typedef enum {
  // client -> display: "send me a session table snapshot"
  PGIPC_ADMIN_MSG_LIST_REQUEST = 1,
  // display -> client: snapshot reply, payload pgipc_admin_list_response_t
  PGIPC_ADMIN_MSG_LIST_RESPONSE = 2,
  // client -> display: "activate this app_id", payload pgipc_admin_switch_request_t
  PGIPC_ADMIN_MSG_SWITCH_REQUEST = 3,
  // display -> client: outcome, payload pgipc_admin_switch_response_t
  PGIPC_ADMIN_MSG_SWITCH_RESPONSE = 4,
} pgipc_admin_msg_type_t;

/**
 * enum pgipc_admin_client_state_t - wire representation of session state
 *
 * Intentionally a separate enum from the display's internal pgipc_session_state_t:
 * this header must not depend on that internal representation, so the admin plane is
 * responsible for translating one to the other. Keep the two enums' meanings in sync if
 * either changes.
 */
typedef enum {
  PGIPC_ADMIN_STATE_CONNECTED = 0,
  PGIPC_ADMIN_STATE_NEGOTIATED = 1,
  PGIPC_ADMIN_STATE_ACTIVE = 2,
  PGIPC_ADMIN_STATE_REJECTED = 3,
} pgipc_admin_client_state_t;

/**
 * struct pgipc_admin_client_info_t - one LIST_RESPONSE entry
 * @app_id:          NUL-terminated app id
 * @state:           current session state (wire enum, see above)
 * @negotiated_mode: valid once state is NEGOTIATED or ACTIVE
 * @payload_kind:    PIXELS or DMABUF (pgipc_payload_kind_t). That enum IS declared 
 *                   unconditionally, so reusing it here directly is fine.
 */
typedef struct {
  char app_id[PGIPC_APP_ID_LEN];
  pgipc_admin_client_state_t state;
  pgipc_render_mode_t negotiated_mode;
  pgipc_payload_kind_t payload_kind;
} pgipc_admin_client_info_t;

/**
 * struct pgipc_admin_list_response_t - PGIPC_ADMIN_MSG_LIST_RESPONSE payload
 * @count:   number of valid entries in @clients (0..PGIPC_ADMIN_MAX_CLIENTS)
 * @clients: session table snapshot, in slot-index order
 */
typedef struct {
  uint32_t count;
  pgipc_admin_client_info_t clients[PGIPC_ADMIN_MAX_CLIENTS];
} pgipc_admin_list_response_t;

/**
 * struct pgipc_admin_switch_request_t - PGIPC_ADMIN_MSG_SWITCH_REQUEST payload
 * @app_id: NUL-terminated app id to activate
 */
typedef struct {
  char app_id[PGIPC_APP_ID_LEN];
} pgipc_admin_switch_request_t;

/**
 * struct pgipc_admin_switch_response_t - PGIPC_ADMIN_MSG_SWITCH_RESPONSE payload
 * @ok:     1 if the switch succeeded (or was already a no-op)
 * @reason: human-readable failure reason, valid when @ok == 0
 */
typedef struct {
  uint8_t ok;
  char reason[PGIPC_ADMIN_REASON_LEN];
} pgipc_admin_switch_response_t;

#ifdef __cplusplus
}
#endif

#endif // PGIPC_ADMIN_PROTO_H
