// session_table.h - the server's client session table.
//
// Owns the fixed-size array of connected client sessions. This table is touched ONLY by
// the control-plane thread, no locking is used here on purpose. The admin thread must
// never read/write slots directly; it posts requests to the control-plane thread
// instead and gets a snapshot/response back.
//
// This header is server-only: it uses pgdps_dmabuf_set_t/close(), which are
// LIBPGDP_SERVER-gated declarations. Whatever .c file includes this header must
// #define LIBPGDP_SERVER before doing so (same convention as data_plane.h/.c).
#ifndef PGDPS_SESSION_TABLE_H
#define PGDPS_SESSION_TABLE_H

#ifndef LIBPGDP_SERVER
#error "session_table.h requires #define LIBPGDP_SERVER before including it"
#endif

#include <stdbool.h>
#include <time.h>

#include "libpgdp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Max concurrently-connected client-control sessions */
#define PGDPS_SESSION_MAX_CLIENTS 8

/**
 * enum pgdps_session_state_t - per-client state machine
 * @PGDPS_SESSION_CONNECTED:  socket accepted, no PGDP_MSG_CONNECT yet
 * @PGDPS_SESSION_NEGOTIATED: mode accepted; not the current ACTIVE client
 * @PGDPS_SESSION_ACTIVE:     holds the current activation grant
 * @PGDPS_SESSION_REJECTED:   mode negotiation failed; connection closing
 */
typedef enum {
  PGDPS_SESSION_CONNECTED = 0,
  PGDPS_SESSION_NEGOTIATED = 1,
  PGDPS_SESSION_ACTIVE = 2,
  PGDPS_SESSION_REJECTED = 3,
} pgdps_session_state_t;

/**
 * struct pgdps_session_t - one connected client's tracked state
 * @in_use:                   false if this slot is free for reuse
 * @ctrl_fd:                  control socket fd, or -1 if not in_use
 * @client_id:                from PGDP_MSG_CONNECT, NUL-terminated. No uniqueness
 *                            constraint.
 * @state:                    current state machine position (see enum above)
 * @offered_modes:            modes offered in CONNECT, in preference order
 * @num_offered_modes:        number of valid entries in @offered_modes
 * @negotiated_mode:          set once MODE{accepted=1} is sent
 * @granted_generation:       ring generation copied at the moment of grant
 * @last_heartbeat_monotonic: last HEARTBEAT recv time (ACTIVE clients only)
 * @payload_kind:             PIXELS until a DMABUF_ANNOUNCE is ACKed
 * @dmabuf_set:               valid only when payload_kind == PGDP_PAYLOAD_DMABUF
 *
 * One array slot in session table. Never accessed outside the control-plane thread.
 */
typedef struct {
  bool in_use;
  int ctrl_fd;
  char client_id[PGDP_CLIENT_ID_LEN];
  pgdps_session_state_t state;
  pgdp_render_mode_t offered_modes[PGDP_MAX_MODES];
  uint32_t num_offered_modes;
  pgdp_render_mode_t negotiated_mode;
  uint32_t granted_generation;
  struct timespec last_heartbeat_monotonic;
  pgdp_payload_kind_t payload_kind;
  pgdps_dmabuf_set_t dmabuf_set;
} pgdps_session_t;

/**
 * struct pgdps_session_table_t - fixed array of client sessions
 * @slots: PGDPS_SESSION_MAX_CLIENTS entries, in_use marks occupancy
 * @active_slot: index of the current ACTIVE session, or -1 if none
 *
 * @active_slot is a cache, not a second source of truth: exactly one slot may have
 * state == PGDPS_SESSION_ACTIVE at a time, and @active_slot always names it (or is -1
 * when no slot is ACTIVE). Kept in sync by pgdps_session_table_activate()/deactivate()
 * so callers don't have to linear-scan for it on every data-plane tick.
 */
typedef struct {
  pgdps_session_t slots[PGDPS_SESSION_MAX_CLIENTS];
  int active_slot;
} pgdps_session_table_t;

/**
 * pgdps_session_table_init() - zero/reset a session table
 * @table: table to initialize
 *
 * Marks every slot free and @active_slot as -1. Call once at startup.
 */
void pgdps_session_table_init(pgdps_session_table_t *table);

/**
 * pgdps_session_table_add() - claim a free slot for a new connection
 * @table: session table
 * @fd:    accepted control socket fd
 *
 * Return: index of the newly claimed slot (state PGDPS_SESSION_CONNECTED), or -1 if the
 * table is full (caller should close @fd with no CONNECT response in that case -- a
 * resource limit, not a protocol reply).
 */
int pgdps_session_table_add(pgdps_session_table_t *table, int fd);

/**
 * pgdps_session_table_find_by_fd() - look up a slot by control socket fd
 * @table: session table
 * @fd:    fd to search for
 *
 * Return: slot index, or -1 if no in_use slot has this fd (e.g. after
 * pgdps_session_table_remove()).
 */
int pgdps_session_table_find_by_fd(pgdps_session_table_t *table, int fd);

/**
 * pgdps_session_table_find_by_client_id() - look up a slot by client_id
 * @table:  session table
 * @client_id: NUL-terminated app id to search for
 *
 * client_id is not a uniqueness key! If multiple in_use slots share @client_id, the
 * first match wins. Used by the admin plane's switch/list lookups never by the data
 * plane.
 *
 * Return: slot index, or -1 if no in_use slot has this client_id.
 */
int pgdps_session_table_find_by_client_id(pgdps_session_table_t *table,
                                          const char *client_id);

/**
 * pgdps_session_table_activate() - mark a slot ACTIVE
 * @table:      session table
 * @idx:        slot to activate; must currently be PGDPS_SESSION_NEGOTIATED
 * @generation: ring generation granted at this activation
 *
 * Deactivates whichever slot was previously ACTIVE (if any) first. Only one slot may
 * be ACTIVE at a time. Stamps @idx's last_heartbeat_monotonic to now, so a
 * freshly-activated client isn't immediately timed out before its first heartbeat
 * arrives.
 */
void pgdps_session_table_activate(pgdps_session_table_t *table, int idx,
                                  uint32_t generation);

/**
 * pgdps_session_table_deactivate() - drop a slot back to NEGOTIATED
 * @table: session table
 * @idx:   slot to deactivate; no-op if it isn't the current ACTIVE slot
 *
 * Does not touch the fd or close anything. Callers decide separately whether to also
 * send PGDP_MSG_DEACTIVATE and/or remove the slot entirely.
 */
void pgdps_session_table_deactivate(pgdps_session_table_t *table, int idx);

/**
 * pgdps_session_table_remove() - free a slot (CLOSED transition)
 * @table: session table
 * @idx:   slot to free
 *
 * Closes @idx's dmabuf_set (if valid, via pgdps_dmabuf_set_close()), deactivates the
 * slot first if it was ACTIVE, and marks it free for reuse. Does NOT close the control
 * socket fd. The caller owns that lifetime (it's usually already closed/EOF by the time
 * this is called).
 */
void pgdps_session_table_remove(pgdps_session_table_t *table, int idx);

/**
 * pgdps_session_table_check_heartbeat_timeouts() - evict dead ACTIVE clients
 * @table: session table
 * @now:   current CLOCK_MONOTONIC time
 *
 * If the ACTIVE slot's last_heartbeat_monotonic is more than PGDPS_HEARTBEAT_TIMEOUT_MS
 * old, deactivate it. Only ever examines the ACTIVE slot (NEGOTIATED clients don't
 * heartbeat). Does not send PGDP_MSG_DEACTIVATE or bump the ring generation itself.
 * The caller (control-plane loop) is responsible for that, mirroring the split between
 * this pure data-structure module and the I/O-performing control plane.
 *
 * Return: the slot index that was just timed out and deactivated, or -1 if
 * no timeout occurred (including "no slot is ACTIVE").
 */
int pgdps_session_table_check_heartbeat_timeouts(pgdps_session_table_t *table,
                                                 struct timespec now);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif // PGDPS_SESSION_TABLE_H
