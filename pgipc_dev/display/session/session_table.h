// session_table.h - the display's client session table.
//
// Owns the fixed-size array of connected writer sessions. This table is touched ONLY by
// the control-plane thread, no locking is used here on purpose. The admin thread must
// never read/write slots directly; it posts requests to the control-plane thread
// instead and gets a snapshot/response back.
//
// This header is display-only: it uses pgipc_dmabuf_set_t/close(), which are
// LIBPGIPC_READER-gated declarations. Whatever .c file includes this header must
// #define LIBPGIPC_READER before doing so (same convention as data_plane.h/.c).
#ifndef PGIPC_SESSION_TABLE_H
#define PGIPC_SESSION_TABLE_H

#ifndef LIBPGIPC_READER
#error "session_table.h requires #define LIBPGIPC_READER before including it"
#endif

#include <stdbool.h>
#include <time.h>

#include "libpgipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Max concurrently-connected control sessions, not the number of apps installed in the
 * marketplace. */
#define PGIPC_SESSION_MAX_CLIENTS 8

/**
 * enum pgipc_session_state_t - per-client state machine
 */
typedef enum {
  // Socket accepted, no PGIPC_MSG_CONNECT yet
  PGIPC_SESSION_CONNECTED = 0,
  // Mode accepted; not the current ACTIVE writer
  PGIPC_SESSION_NEGOTIATED = 1,
  // Holds the current activation grant
  PGIPC_SESSION_ACTIVE = 2,
  // Mode negotiation failed; connection closing
  PGIPC_SESSION_REJECTED = 3,
} pgipc_session_state_t;

/**
 * struct pgipc_session_t - one connected writer's tracked state
 * @in_use:                   false if this slot is free for reuse
 * @ctrl_fd:                  control socket fd, or -1 if not in_use
 * @app_id:                   from PGIPC_MSG_CONNECT, NUL-terminated. No uniqueness
 *                            constraint.
 * @state:                    current state machine position (see enum above)
 * @offered_modes:            modes offered in CONNECT, in preference order
 * @num_offered_modes:        number of valid entries in @offered_modes
 * @negotiated_mode:          set once MODE{accepted=1} is sent
 * @granted_generation:       ring generation copied at the moment of grant
 * @last_heartbeat_monotonic: last HEARTBEAT recv time (ACTIVE clients only)
 * @payload_kind:             PIXELS until a DMABUF_ANNOUNCE is ACKed
 * @dmabuf_set:               valid only when payload_kind == PGIPC_PAYLOAD_DMABUF
 *
 * One array slot in session table. Never accessed outside the control-plane thread.
 */
typedef struct {
  bool in_use;
  int ctrl_fd;
  char app_id[PGIPC_APP_ID_LEN];
  pgipc_session_state_t state;
  pgipc_render_mode_t offered_modes[PGIPC_MAX_MODES];
  uint32_t num_offered_modes;
  pgipc_render_mode_t negotiated_mode;
  uint32_t granted_generation;
  struct timespec last_heartbeat_monotonic;
  pgipc_payload_kind_t payload_kind;
  pgipc_dmabuf_set_t dmabuf_set;
} pgipc_session_t;

/**
 * struct pgipc_session_table_t - fixed array of client sessions
 * @slots: PGIPC_SESSION_MAX_CLIENTS entries, in_use marks occupancy
 * @active_slot: index of the current ACTIVE session, or -1 if none
 *
 * @active_slot is a cache, not a second source of truth: exactly one slot may have
 * state == PGIPC_SESSION_ACTIVE at a time, and @active_slot always names it (or is -1
 * when no slot is ACTIVE). Kept in sync by pgipc_session_table_activate()/deactivate()
 * so callers don't have to linear-scan for it on every data-plane tick.
 */
typedef struct {
  pgipc_session_t slots[PGIPC_SESSION_MAX_CLIENTS];
  int active_slot;
} pgipc_session_table_t;

/**
 * pgipc_session_table_init() - zero/reset a session table
 * @table: table to initialize
 *
 * Marks every slot free and @active_slot as -1. Call once at startup.
 */
void pgipc_session_table_init(pgipc_session_table_t *table);

/**
 * pgipc_session_table_add() - claim a free slot for a new connection
 * @table: session table
 * @fd:    accepted control socket fd
 *
 * Return: index of the newly claimed slot (state PGIPC_SESSION_CONNECTED), or -1 if the
 * table is full (caller should close @fd with no CONNECT response in that case -- a
 * resource limit, not a protocol reply).
 */
int pgipc_session_table_add(pgipc_session_table_t *table, int fd);

/**
 * pgipc_session_table_find_by_fd() - look up a slot by control socket fd
 * @table: session table
 * @fd:    fd to search for
 *
 * Return: slot index, or -1 if no in_use slot has this fd (e.g. after
 * pgipc_session_table_remove()).
 */
int pgipc_session_table_find_by_fd(pgipc_session_table_t *table, int fd);

/**
 * pgipc_session_table_find_by_app_id() - look up a slot by app_id
 * @table:  session table
 * @app_id: NUL-terminated app id to search for
 *
 * app_id is not a uniqueness key! If multiple in_use slots share @app_id, the first
 * match wins. Used by the admin plane's switch/list lookups never by the data plane.
 *
 * Return: slot index, or -1 if no in_use slot has this app_id.
 */
int pgipc_session_table_find_by_app_id(pgipc_session_table_t *table,
                                       const char *app_id);

/**
 * pgipc_session_table_activate() - mark a slot ACTIVE
 * @table:      session table
 * @idx:        slot to activate; must currently be PGIPC_SESSION_NEGOTIATED
 * @generation: ring generation granted at this activation
 *
 * Deactivates whichever slot was previously ACTIVE (if any) first. Only one slot may
 * be ACTIVE at a time. Stamps @idx's last_heartbeat_monotonic to now, so a
 * freshly-activated client isn't immediately timed out before its first heartbeat
 * arrives.
 */
void pgipc_session_table_activate(pgipc_session_table_t *table, int idx,
                                  uint32_t generation);

/**
 * pgipc_session_table_deactivate() - drop a slot back to NEGOTIATED
 * @table: session table
 * @idx:   slot to deactivate; no-op if it isn't the current ACTIVE slot
 *
 * Does not touch the fd or close anything. Callers decide separately whether to also
 * send PGIPC_MSG_DEACTIVATE and/or remove the slot entirely.
 */
void pgipc_session_table_deactivate(pgipc_session_table_t *table, int idx);

/**
 * pgipc_session_table_remove() - free a slot (CLOSED transition)
 * @table: session table
 * @idx:   slot to free
 *
 * Closes @idx's dmabuf_set (if valid, via pgipc_dmabuf_set_close()), deactivates the
 * slot first if it was ACTIVE, and marks it free for reuse. Does NOT close the control
 * socket fd. The caller owns that lifetime (it's usually already closed/EOF by the time
 * this is called).
 */
void pgipc_session_table_remove(pgipc_session_table_t *table, int idx);

/**
 * pgipc_session_table_check_heartbeat_timeouts() - evict dead ACTIVE clients
 * @table: session table
 * @now:   current CLOCK_MONOTONIC time
 *
 * If the ACTIVE slot's last_heartbeat_monotonic is more than PGIPC_HEARTBEAT_TIMEOUT_MS
 * old, deactivate it. Only ever examines the ACTIVE slot (NEGOTIATED clients don't
 * heartbeat). Does not send PGIPC_MSG_DEACTIVATE or bump the ring generation itself.
 * The caller (control-plane loop) is responsible for that, mirroring the split between
 * this pure data-structure module and the I/O-performing control plane.
 *
 * Return: the slot index that was just timed out and deactivated, or -1 if
 * no timeout occurred (including "no slot is ACTIVE").
 */
int pgipc_session_table_check_heartbeat_timeouts(pgipc_session_table_t *table,
                                                 struct timespec now);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif // PGIPC_SESSION_TABLE_H
