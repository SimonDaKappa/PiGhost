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
 * @WISPS_SESSION_CONNECTED:            socket accepted, no WISP_MSG_CONNECT yet
 * @WISPS_SESSION_NEGOTIATED:           mode accepted; idle, not requesting/holding
 *                                      the RT slot
 * @WISPS_SESSION_QUEUED:               sent ACTIVATE_REQUEST while the RT slot was
 *                                      held by another session; waiting its turn.
 *                                      Not a separate bounded structure -- queue
 *                                      membership/position is derived by scanning
 *                                      the session table for this state, ordered by
 *                                      request time
 * @WISPS_SESSION_GRANTED_WARMUP:       ACTIVATE_GRANT sent; awaiting READY_FOR_RT
 *                                      (client is warming up and self-promoting its
 *                                      own scheduling policy) or GRANT_DECLINE
 * @WISPS_SESSION_ACTIVE_RT:            READY_FOR_RT received; holds the RT slot and
 *                                      is expected to be producing frames. Only one
 *                                      session may be in this state at a time
 * @WISPS_SESSION_EVICTING_COOPERATIVE: EVICT_PENDING sent; waiting for STOPPED (or
 *                                      the grace period to elapse, which falls
 *                                      through to EVICTING_FORCED)
 * @WISPS_SESSION_EVICTING_FORCED:      grace period elapsed with no STOPPED, or a
 *                                      liveness timeout fired with no cooperative
 *                                      wind-down in progress; server force-demotes
 *                                      and reclaims without further client
 *                                      involvement
 * @WISPS_SESSION_REJECTED:             mode negotiation failed; connection closing
 *
 * Only WISPS_SESSION_CONNECTED, WISPS_SESSION_NEGOTIATED, WISPS_SESSION_ACTIVE_RT,
 * and WISPS_SESSION_REJECTED are currently reachable -- QUEUED/GRANTED_WARMUP/
 * EVICTING_COOPERATIVE/EVICTING_FORCED are formalized here as part of the RT
 * lifecycle's target shape, but control_plane.c does not yet drive sessions through
 * them (no queueing, warmup handshake, or cooperative eviction is implemented yet).
 */
typedef enum {
  WISPS_SESSION_CONNECTED = 0,
  WISPS_SESSION_NEGOTIATED = 1,
  WISPS_SESSION_QUEUED = 2,
  WISPS_SESSION_GRANTED_WARMUP = 3,
  WISPS_SESSION_ACTIVE_RT = 4,
  WISPS_SESSION_EVICTING_COOPERATIVE = 5,
  WISPS_SESSION_EVICTING_FORCED = 6,
  WISPS_SESSION_REJECTED = 7,
} wisps_session_state_t;

/**
 * struct wisps_session_t - one connected client's tracked state
 * @in_use:                     false if this slot is free for reuse
 * @ctrl_fd:                    control socket fd, or -1 if not in_use
 * @client_id:                  from WISP_MSG_CONNECT, NUL-terminated. No uniqueness
 *                              constraint.
 * @state:                      current state machine position (see enum above)
 * @offered_modes:              modes offered in CONNECT, in preference order
 * @num_offered_modes:          number of valid entries in @offered_modes
 * @negotiated_mode:            set once MODE{accepted=1} is sent
 * @granted_generation:         ring generation copied at the moment of grant
 * @last_heartbeat_monotonic:   last HEARTBEAT recv time (ACTIVE_RT clients only)
 * @activate_requested_monotonic: time ACTIVATE_REQUEST was received; valid once
 *                              state is QUEUED or later. Not read today -- reserved
 *                              for FIFO ordering once queue-position reporting is
 *                              implemented (queue membership/order is derived from
 *                              this timestamp, not tracked in a separate structure)
 * @payload_kind:               PIXELS until a DMABUF_ANNOUNCE is ACKed
 * @dmabuf_set:                 valid only when payload_kind == WISP_PAYLOAD_DMABUF
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
  struct timespec activate_requested_monotonic;
  wisp_payload_kind_t payload_kind;
  wisps_dmabuf_set_t dmabuf_set;
} wisps_session_t;


/**
 * struct wisps_session_table_t - fixed array of client sessions
 * @slots: WISPS_SESSION_MAX_CLIENTS entries, in_use marks occupancy
 * @active_slot: index of the current ACTIVE_RT session, or -1 if none
 *
 * @active_slot is a cache, not a second source of truth: exactly one slot may have
 * state == WISPS_SESSION_ACTIVE_RT at a time, and @active_slot always names it (or is
 * -1 when no slot is ACTIVE_RT). Kept in sync by
 * wisps_session_table_activate()/deactivate() so callers don't have to linear-scan
 * for it on every data-plane tick.
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
 * wisps_session_table_activate() - mark a slot ACTIVE_RT
 * @table:      session table
 * @idx:        slot to activate; must currently be WISPS_SESSION_NEGOTIATED
 * @generation: ring generation granted at this activation
 *
 * Deactivates whichever slot was previously ACTIVE_RT (if any) first. Only one slot
 * may be ACTIVE_RT at a time. Stamps @idx's last_heartbeat_monotonic to now, so a
 * freshly-activated client isn't immediately timed out before its first heartbeat
 * arrives.
 *
 * Today this is a direct NEGOTIATED -> ACTIVE_RT transition, bypassing the
 * QUEUED/GRANTED_WARMUP intermediate states formalized in wisps_session_state_t --
 * those aren't wired up yet (see that enum's doc comment).
 */
void wisps_session_table_activate(wisps_session_table_t *table, int idx,
                                  uint32_t generation);

/**
 * wisps_session_table_deactivate() - drop a slot back to NEGOTIATED
 * @table: session table
 * @idx:   slot to deactivate; no-op if it isn't the current ACTIVE_RT slot
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
 * slot first if it was ACTIVE_RT, and marks it free for reuse. Does NOT close the
 * control socket fd. The caller owns that lifetime (it's usually already closed/EOF
 * by the time this is called).
 */
void wisps_session_table_remove(wisps_session_table_t *table, int idx);

/**
 * wisps_session_table_check_heartbeat_timeouts() - evict dead ACTIVE_RT clients
 * @table: session table
 * @now:   current CLOCK_MONOTONIC time
 *
 * If the ACTIVE_RT slot's last_heartbeat_monotonic is more than
 * WISP_CLIENT_HEARTBEAT_TIMEOUT_MS old, deactivate it. Only ever examines the
 * ACTIVE_RT slot (NEGOTIATED clients don't heartbeat). Does not send
 * WISP_MSG_DEACTIVATE or bump the ring generation itself. The caller
 * (control-plane loop) is responsible for that, mirroring the split between this
 * pure data-structure module and the I/O-performing control plane.
 *
 * Return: the slot index that was just timed out and deactivated, or -1 if
 * no timeout occurred (including "no slot is ACTIVE_RT").
 */
int wisps_session_table_check_heartbeat_timeouts(wisps_session_table_t *table,
                                                 struct timespec now);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif // WISPS_SESSION_TABLE_H
