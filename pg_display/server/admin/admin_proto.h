// admin_proto.h - wire format for the server's admin/management socket.
//
// Deliberately NOT part of libpgdp.h: producer apps never link against or even see
// this protocol. Reuses onlypgdps_ctrl_send()/pgdps_ctrl_recv() from libpgdp.h for
// wire framing (those two aredeclared unconditionally, regardless of
// LIBPGDP_SERVER/LIBPGDP_CLIENT, precisely soheaders like this one can borrow them
// without pulling in producer- or server-onlysymbols).
//
// Every admin connection is short-lived: connect -> send one request -> receive one
// response -> close. There is no persistent admin session state and no heartbeats,
// unlike the producer control protocol.
#ifndef PGDPS_ADMIN_PROTO_H
#define PGDPS_ADMIN_PROTO_H

#include <stdint.h>

#include "libpgdp.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * PGDPS_ADMIN_SOCK_PATH - path of the server's admin Unix domain socket
 *
 * Separate from the producer control socket so producer apps never need to know this
 * protocol exists at all.
 */
#define PGDPS_ADMIN_SOCK_PATH "/dev/shm/frame_ring_admin.sock"

/**
 * PGDPS_ADMIN_MAX_CLIENTS - upper bound on LIST_RESPONSE entries
 *
 * Matches PGDPS_SESSION_MAX_CLIENTS (session_table.h), duplicated here
 * rather than included so this wire-format header has zero dependency on
 * the server's internal session table representation -- only libpgdp.h
 * itself. Keep these two constants in sync if either changes.
 */
#define PGDPS_ADMIN_MAX_CLIENTS 8

/**
 * PGDPS_ADMIN_REASON_LEN - max bytes (incl NUL) in a switch failure reason
 */
#define PGDPS_ADMIN_REASON_LEN 64

/**
 * enum pgdps_admin_msg_type_t - admin protocol message types
 * @PGDPS_ADMIN_MSG_LIST_REQUEST:    client -> server: send me a session table snapshot
 * @PGDPS_ADMIN_MSG_LIST_RESPONSE:   server -> client: snapshot reply
 * @PGDPS_ADMIN_MSG_SWITCH_REQUEST:  client -> server: activate this client_id
 * @PGDPS_ADMIN_MSG_SWITCH_RESPONSE: server -> client: outcome
 *
 * A deliberately separate, smaller enum from pgdp_msg_type_t (admin messages ride the
 * same 1-byte-type + 4-byte-BE-length framing for pgdps_ctrl_send/recv) but must never
 * be confusable with producer control messages, even though the two protocols share
 * framing code and could theoretically be sent down the wrong socket by a bug.
 */
typedef enum {
  PGDPS_ADMIN_MSG_LIST_REQUEST = 1,
  PGDPS_ADMIN_MSG_LIST_RESPONSE = 2,
  PGDPS_ADMIN_MSG_SWITCH_REQUEST = 3,
  PGDPS_ADMIN_MSG_SWITCH_RESPONSE = 4,
} pgdps_admin_msg_type_t;

/**
 * enum pgdps_admin_client_state_t - wire representation of session state
 * @PGDPS_ADMIN_STATE_CONNECTED:  $$$SIMON TODO
 * @PGDPS_ADMIN_STATE_NEGOTIATED: $$$SIMON TODO
 * @PGDPS_ADMIN_STATE_ACTIVE:     $$$SIMON TODO
 * @PGDPS_ADMIN_STATE_REJECTED:   $$$SIMON TODO
 *
 * Intentionally a separate enum from the server's internal pgdps_session_state_t:
 * this header must not depend on that internal representation, so the admin plane is
 * responsible for translating one to the other. Keep the two enums' meanings in sync if
 * either changes.
 */
typedef enum {
  PGDPS_ADMIN_STATE_CONNECTED = 0,
  PGDPS_ADMIN_STATE_NEGOTIATED = 1,
  PGDPS_ADMIN_STATE_ACTIVE = 2,
  PGDPS_ADMIN_STATE_REJECTED = 3,
} pgdps_admin_client_state_t;

/**
 * struct pgdps_admin_client_info_t - one LIST_RESPONSE entry
 * @client_id:          NUL-terminated app id
 * @state:           current session state (wire enum, see above)
 * @negotiated_mode: valid once state is NEGOTIATED or ACTIVE
 * @payload_kind:    PIXELS or DMABUF (pgdp_payload_kind_t). That enum IS declared
 *                   unconditionally, so reusing it here directly is fine.
 */
typedef struct {
  char client_id[PGDP_CLIENT_ID_LEN];
  pgdps_admin_client_state_t state;
  pgdp_render_mode_t negotiated_mode;
  pgdp_payload_kind_t payload_kind;
} pgdps_admin_client_info_t;

/**
 * struct pgdps_admin_list_response_t - PGDPS_ADMIN_MSG_LIST_RESPONSE payload
 * @count:   number of valid entries in @clients (0..PGDPS_ADMIN_MAX_CLIENTS)
 * @clients: session table snapshot, in slot-index order
 */
typedef struct {
  uint32_t count;
  pgdps_admin_client_info_t clients[PGDPS_ADMIN_MAX_CLIENTS];
} pgdps_admin_list_response_t;

/**
 * struct pgdps_admin_switch_request_t - PGDPS_ADMIN_MSG_SWITCH_REQUEST payload
 * @client_id: NUL-terminated app id to activate
 */
typedef struct {
  char client_id[PGDP_CLIENT_ID_LEN];
} pgdps_admin_switch_request_t;

/**
 * struct pgdps_admin_switch_response_t - PGDPS_ADMIN_MSG_SWITCH_RESPONSE payload
 * @ok:     1 if the switch succeeded (or was already a no-op)
 * @reason: human-readable failure reason, valid when @ok == 0
 */
typedef struct {
  uint8_t ok;
  char reason[PGDPS_ADMIN_REASON_LEN];
} pgdps_admin_switch_response_t;

#ifdef __cplusplus
}
#endif

#endif // PGDPS_ADMIN_PROTO_H
