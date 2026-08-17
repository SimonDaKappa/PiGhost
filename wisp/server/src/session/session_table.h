// session_table.h - the server's client session table.
//
// Owns the fixed-size array of connected client sessions. This table is touched ONLY by
// the control-plane thread, no locking is used here on purpose. The admin thread must
// never read/write slots directly; it posts requests to the control-plane thread
// instead and gets a snapshot/response back.
//
// This header is server-only: it uses wisps_dmabuf_set_t/close() from shm/server_shm.h.
#ifndef WISPS_SESSION_TABLE_H
#define WISPS_SESSION_TABLE_H

#include <stdbool.h>
#include <time.h>

#include <wisp/wire.h>

#include "shm/server_shm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Max concurrently-connected client-control sessions */
#define WISPS_SESSION_MAX_CLIENTS 8

/**
 * enum wisps_session_state_t - per-client state machine
 * @WISPS_SESSION_CONNECTED:  socket accepted, no WISP_MSG_CONNECT yet
 * @WISPS_SESSION_NEGOTIATED: mode accepted; not the current ACTIVE client
 * @WISPS_SESSION_ACTIVE:     holds the current activation grant
 * @WISPS_SESSION_REJECTED:   mode negotiation failed; connection closing
 */
typedef enum {
  WISPS_SESSION_CONNECTED = 0,
  WISPS_SESSION_NEGOTIATED = 1,
  WISPS_SESSION_ACTIVE = 2,
  WISPS_SESSION_REJECTED = 3,
} wisps_session_state_t;

/**
 * struct wisps_session_t - one connected client's tracked state
 * @in_use:                   false if this slot is free for reuse
 * @ctrl_fd:                  control socket fd, or -1 if not in_use
 * @client_id:                from WISP_MSG_CONNECT, NUL-terminated. No uniqueness
 *                            constraint.
 * @state:                    current state machine position (see enum above)
 * @offered_modes:            modes offered in CONNECT, in preference order
 * @num_offered_modes:        number of valid entries in @offered_modes
 * @negotiated_mode:          set once MODE{accepted=1} is sent
 * @granted_generation:       ring generation copied at the moment of grant
 * @last_heartbeat_monotonic: last HEARTBEAT recv time (ACTIVE clients only)
 * @payload_kind:             PIXELS until a DMABUF_ANNOUNCE is ACKed
 * @dmabuf_set:               valid only when payload_kind == WISP_PAYLOAD_DMABUF
 *
 * One array slot in session table. Never accessed outside the control-plane thread.
 */
typedef struct {
  bool in_use;
  int ctrl_fd;
  char client_id[WISP_CLIENT_ID_LEN];
  wisps_session_state_t state;
  wisp_render_mode_t offered_modes[WISP_MAX_MODES];
  uint32_t num_offered_modes;
  wisp_render_mode_t negotiated_mode;
  uint32_t granted_generation;
  struct timespec last_heartbeat_monotonic;
  wisp_payload_kind_t payload_kind;
  wisps_dmabuf_set_t dmabuf_set;
} wisps_session_t;

/**
 * struct wisps_session_table_t - fixed array of client sessions
 * @slots: WISPS_SESSION_MAX_CLIENTS entries, in_use marks occupancy
 * @active_slot: index of the current ACTIVE session, or -1 if none
 *
 * @active_slot is a cache, not a second source of truth: exactly one slot may have
 * state == WISPS_SESSION_ACTIVE at a time, and @active_slot always names it (or is -1
 * when no slot is ACTIVE). Kept in sync by wisps_session_table_activate()/deactivate()
 * so callers don't have to linear-scan for it on every data-plane tick.
 */
typedef struct {
  wisps_session_t slots[WISPS_SESSION_MAX_CLIENTS];
  int active_slot;
} wisps_session_table_t;

/**
 * wisps_session_table_init() - zero/reset a session table
 * @table: table to initialize
 *
 * Marks every slot free and @active_slot as -1. Call once at startup.
 */
void wisps_session_table_init(wisps_session_table_t *table);

/**
 * wisps_session_table_add() - claim a free slot for a new connection
 * @table: session table
 * @fd:    accepted control socket fd
 *
 * Return: index of the newly claimed slot (state WISPS_SESSION_CONNECTED), or -1 if the
 * table is full (caller should close @fd with no CONNECT response in that case -- a
 * resource limit, not a protocol reply).
 */
int wisps_session_table_add(wisps_session_table_t *table, int fd);

/**
 * wisps_session_table_find_by_fd() - look up a slot by control socket fd
 * @table: session table
 * @fd:    fd to search for
 *
 * Return: slot index, or -1 if no in_use slot has this fd (e.g. after
 * wisps_session_table_remove()).
 */
int wisps_session_table_find_by_fd(wisps_session_table_t *table, int fd);

/**
 * wisps_session_table_find_by_client_id() - look up a slot by client_id
 * @table:  session table
 * @client_id: NUL-terminated app id to search for
 *
 * client_id is not a uniqueness key! If multiple in_use slots share @client_id, the
 * first match wins. Used by the admin plane's switch/list lookups never by the data
 * plane.
 *
 * Return: slot index, or -1 if no in_use slot has this client_id.
 */
int wisps_session_table_find_by_client_id(wisps_session_table_t *table,
                                          const char *client_id);

/**
 * wisps_session_table_activate() - mark a slot ACTIVE
 * @table:      session table
 * @idx:        slot to activate; must currently be WISPS_SESSION_NEGOTIATED
 * @generation: ring generation granted at this activation
 *
 * Deactivates whichever slot was previously ACTIVE (if any) first. Only one slot may
 * be ACTIVE at a time. Stamps @idx's last_heartbeat_monotonic to now, so a
 * freshly-activated client isn't immediately timed out before its first heartbeat
 * arrives.
 */
void wisps_session_table_activate(wisps_session_table_t *table, int idx,
                                  uint32_t generation);

/**
 * wisps_session_table_deactivate() - drop a slot back to NEGOTIATED
 * @table: session table
 * @idx:   slot to deactivate; no-op if it isn't the current ACTIVE slot
 *
 * Does not touch the fd or close anything. Callers decide separately whether to also
 * send WISP_MSG_DEACTIVATE and/or remove the slot entirely.
 */
void wisps_session_table_deactivate(wisps_session_table_t *table, int idx);

/**
 * wisps_session_table_remove() - free a slot (CLOSED transition)
 * @table: session table
 * @idx:   slot to free
 *
 * Closes @idx's dmabuf_set (if valid, via wisps_dmabuf_set_close()), deactivates the
 * slot first if it was ACTIVE, and marks it free for reuse. Does NOT close the control
 * socket fd. The caller owns that lifetime (it's usually already closed/EOF by the time
 * this is called).
 */
void wisps_session_table_remove(wisps_session_table_t *table, int idx);

/**
 * wisps_session_table_check_heartbeat_timeouts() - evict dead ACTIVE clients
 * @table: session table
 * @now:   current CLOCK_MONOTONIC time
 *
 * If the ACTIVE slot's last_heartbeat_monotonic is more than WISP_CLIENT_HEARTBEAT_TIMEOUT_MS
 * old, deactivate it. Only ever examines the ACTIVE slot (NEGOTIATED clients don't
 * heartbeat). Does not send WISP_MSG_DEACTIVATE or bump the ring generation itself.
 * The caller (control-plane loop) is responsible for that, mirroring the split between
 * this pure data-structure module and the I/O-performing control plane.
 *
 * Return: the slot index that was just timed out and deactivated, or -1 if
 * no timeout occurred (including "no slot is ACTIVE").
 */
int wisps_session_table_check_heartbeat_timeouts(wisps_session_table_t *table,
                                                 struct timespec now);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif // WISPS_SESSION_TABLE_H
