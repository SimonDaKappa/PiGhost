#ifndef WISP_WIRE_H
#define WISP_WIRE_H

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "types.h"
#include "utils.h"
#include "version.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WISP_SHM_NAME "/frame_ring_shm"

/**
 * struct wisp_shm_ring_t - frame buffer shared memory ring for both conn sides
 * @latest_ready:     index of newest complete frame, -1 if none
 * @server_locked:    index server currently holds, -1 if none
 * @client_locked:    index client actively holds/rendering into, -1 if none
 * @frame_counter:    monotonically increasing frame id, global
 * @generation:       bumped by server on every activation switch; a client whose
 *                    granted generation no longer matches this has been evicted and
 *                    must stop writing
 * @frame_id:         frame id stamped into each buffer at write time
 * @write_ts_ns:      when each buffer was published (for latency calc)
 * @frame_bufs:       individual frame buffers (PIXELS payload mode only; in the DMABUF
 *                    mode the indices refer to the client's announced dmabuf set)
 * @bookkeeping_lock: cross-process mutex serializing @latest_ready/@server_locked
 *                    updates. guards bookkeeping only, never the pixel copy
 *
 * Never allocate or copy this struct by value, only ever map it at a fixed address via
 * @wisps_shm_ring_create() and @wispc_shm_attach().
 *
 * @note layout is byte-for-byte identical across payload mode. The dmabuf mode is
 * purely a control-plane extension; slot indices mean the same thing in both modes.
 *
 * @note latest_ready, server_locked, frame_counter, generation are aligned on separate
 * cache lines to help with the cross process/thread hammering on them at high
 * framerates.
 */
typedef struct {
  WISP_ATOMIC WISP_CACHELINE_FIELD(int32_t, latest_ready);
  WISP_ATOMIC WISP_CACHELINE_FIELD(int32_t, server_locked);
  WISP_ATOMIC WISP_CACHELINE_FIELD(int32_t, client_locked);
  WISP_ATOMIC WISP_CACHELINE_FIELD(uint32_t, frame_counter);
  WISP_ATOMIC WISP_CACHELINE_FIELD(uint32_t, generation);
  uint64_t frame_id[WISP_NUM_BUFFERS];
  uint64_t write_ts_ns[WISP_NUM_BUFFERS];
  unsigned char frame_bufs[WISP_NUM_BUFFERS][WISP_FRAME_MAX_SIZE];
  pthread_mutex_t bookkeeping_lock;
} wisp_shm_ring_t;

WISP_SASSERT(offsetof(wisp_shm_ring_t, latest_ready) == 0 * WISP_CACHELINE,
             "field spacing drift");
WISP_SASSERT(offsetof(wisp_shm_ring_t, server_locked) == 1 * WISP_CACHELINE,
             "field spacing drift");
WISP_SASSERT(offsetof(wisp_shm_ring_t, client_locked) == 2 * WISP_CACHELINE,
             "field spacing drift");
WISP_SASSERT(offsetof(wisp_shm_ring_t, frame_counter) == 3 * WISP_CACHELINE,
             "field spacing drift");
WISP_SASSERT(offsetof(wisp_shm_ring_t, generation) == 4 * WISP_CACHELINE,
             "field spacing drift");
WISP_SASSERT(WISP_ALIGNOF(wisp_shm_ring_t) == WISP_CACHELINE, "ring alignment drift");

/**
 * wisp_shm_ring_lock() - acquire @ring's bookkeeping mutex
 * @ring: attached/created ring
 *
 * Recovers from a prior holder crashing (EOWNERDEAD) instead of deadlocking.
 */
static void wisp_shm_ring_lock(wisp_shm_ring_t *ring) {
  int rc = pthread_mutex_lock(&ring->bookkeeping_lock);
  if (rc == EOWNERDEAD) {
    pthread_mutex_consistent(&ring->bookkeeping_lock);
  }
}

/**
 * wisp_shm_ring_unlock() - release @ring's bookkeeping mutex
 * @ring: attached/created ring
 */
static void wisp_shm_ring_unlock(wisp_shm_ring_t *ring) {
  pthread_mutex_unlock(&ring->bookkeeping_lock);
}

/**
 * wisp_shm_ring_write_slot() - claim a free buffer index for the producer side to
 * write into
 * @ring: attached/created ring
 *
 * Picks any index that is neither the currently-published frame (@latest_ready) nor
 * the server's currently-locked index (@server_locked), guaranteeing the write never
 * tears a frame the server (or the flip-away-from logic in dmabuf mode) is using. Only
 * one producer-side writer may hold a claimed slot (@client_locked) at any given
 * moment -- calling this again before wisp_shm_ring_publish_slot() releases the
 * previous claim is a caller bug and aborts the process.
 *
 * This is a pure ring-ownership primitive: it knows nothing about control-plane
 * sessions/activation, so it is equally usable by the real client SDK
 * (wispc_write_slot()) and by test/fake-producer harnesses that bypass the control
 * protocol entirely.
 *
 * Return: a writable buffer index (0..WISP_NUM_BUFFERS-1).
 */
static int wisp_shm_ring_write_slot(wisp_shm_ring_t *ring) {
  wisp_shm_ring_lock(ring);
  int ready = wisp_atomic_load(&ring->latest_ready);
  int server_locked = wisp_atomic_load(&ring->server_locked);
  int client_locked = wisp_atomic_load(&ring->client_locked);

  if (client_locked != -1) {
    /* Caller broke a *very important* invariant: let them crash. The server can
     * recover from this (it never depends on a well-behaved producer). */
    wisp_shm_ring_unlock(ring);
    abort();
  }

  client_locked = -1;
  for (int i = 0; i < WISP_NUM_BUFFERS; i++) {
    if (i != ready && i != server_locked) {
      client_locked = i;
      break;
    }
  }

  if (client_locked == -1) {
    wisp_shm_ring_unlock(ring);
    abort();
  }

  wisp_atomic_store(&ring->client_locked, client_locked);
  wisp_shm_ring_unlock(ring);
  return client_locked;
}

/**
 * wisp_shm_ring_publish_slot() - publish a finished frame as the newest ready
 * @ring:     attached ring
 * @idx:      buffer index previously returned by wisp_shm_ring_write_slot()
 * @frame_id: producer-assigned monotonically increasing frame id
 * @now_ns:   timestamp the write completed (CLOCK_MONOTONIC), used by the server for
 *            latency accounting
 *
 * Low-level ring-ownership primitive (see wisp_shm_ring_write_slot()). Callers that
 * need the frame-ready eventfd post and generation/eviction check should use their
 * side's higher-level wrapper (e.g. wispc_publish()) instead of calling this directly.
 */
static void wisp_shm_ring_publish_slot(wisp_shm_ring_t *ring, int idx,
                                       uint64_t frame_id, uint64_t now_ns) {
  ring->frame_id[idx] = frame_id;
  ring->write_ts_ns[idx] = now_ns;
  wisp_shm_ring_lock(ring);
  wisp_atomic_store(&ring->latest_ready, idx);
  wisp_atomic_store(&ring->client_locked, -1);
  wisp_shm_ring_unlock(ring);
}

// ===========================================================================
// Control protocol
// ===========================================================================

#define WISPS_CONTROL_SOCK_PATH "/dev/shm/frame_ring_control.sock"
#define WISP_MAX_MODES 4
#define WISP_DENY_REASON_LEN 64

/**
 * WISP_CLIENT_HEARTBEAT_MS - client's HEARTBEAT send period, in ms
 *
 * Shared by both sides (not client-only): the client's ctrl thread sends a
 * heartbeat this often while active; the server's session table uses a
 * multiple of this (see WISP_CLIENT_HEARTBEAT_TIMEOUT_MS) to detect a dead ACTIVE
 * client.
 */
#define WISP_CLIENT_HEARTBEAT_MS 500

/**
 * WISP_CLIENT_HEARTBEAT_TIMEOUT_MS - server's ACTIVE-client dead-heartbeat cutoff
 *
 * If an ACTIVE client's last heartbeat is older than this, the server
 * evicts it. Chosen as 2 * WISP_CLIENT_HEARTBEAT_MS to tolerate exactly one dropped
 * heartbeat before acting.
 */
#define WISP_CLIENT_HEARTBEAT_TIMEOUT_MS (2 * WISP_CLIENT_HEARTBEAT_MS)

/**
 * WISP_CLIENT_RETRY_ACTIVATE_MS - client's timeout between activation requests, in ms
 */
#define WISP_CLIENT_RETRY_ACTIVATE_MS 2000

/**
 * WISP_EVICT_GRACE_MS - cooperative wind-down window offered in WISP_MSG_EVICT_PENDING
 *
 * If the evicted client hasn't sent WISP_MSG_STOPPED within this many ms of the
 * EVICT_PENDING, the server force-demotes and reclaims without further client
 * involvement.
 */
#define WISP_EVICT_GRACE_MS 1000

/**
 * enum wisp_msg_kind_t - control protocol message types
 * @WISP_MSG_CONNECT:          client -> server: "here's what I support"
 * @WISP_MSG_VERSION_MISMATCH: server -> client: "our protocol magic/major versions
 *                             are incompatible, here's what I require". Sent instead
 *                             of WISP_MSG_MODE when CONNECT's magic/major fields fail
 *                             the check; the connection is closed immediately after
 *                             (no mode negotiation is attempted, since the two sides
 *                             cannot be trusted to agree on what any message means).
 * @WISP_MSG_MODE:             server -> client: "render at this mode" / reject
 * @WISP_MSG_ACTIVATE_REQUEST: client -> server: "let me be the client"
 * @WISP_MSG_ACTIVATE_GRANT:   server -> client: "you're it, generation N, here's your
 *                             target core/scheduling hints"
 * @WISP_MSG_ACTIVATE_DENY:    server -> client: "no, and here's why"
 * @WISP_MSG_HEARTBEAT:        client -> server: "still alive, gen N, frame F"
 * @WISP_MSG_DEACTIVATE:       server -> client: "stand down, someone else active"
 * @WISP_MSG_DISCONNECT:       client -> server: "graceful disconnect"
 * @WISP_MSG_DMABUF_ANNOUNCE:  client -> server: "my frames live in these GPU
 *                             buffers". payload is wisp_dmabuf_announce_msg_t, and
 *                             exactly WISP_NUM_BUFFERS dmabuf fds ride alongside in
 *                             SCM_RIGHTS ancillary data. Switches the session to the
 *                             DMABUF payload mode on ACK.
 * @WISP_MSG_DMABUF_ACK:       server -> client: import succeeded / refused
 * @WISP_MSG_ACTIVATE_QUEUED:  server -> client: "another client is active; you're
 *                             number N in line". Sent instead of ACTIVATE_GRANT/DENY
 *                             when ACTIVATE_REQUEST cannot be granted immediately
 *                             because the (single) RT slot is occupied.
 * @WISP_MSG_READY_FOR_RT:     client -> server: "I've self-promoted my scheduling
 *                             policy (see wisp_grant_msg_t hints); start my liveness
 *                             clock". Empty payload; presence is the signal.
 * @WISP_MSG_GRANT_DECLINE:    client -> server: "I'm declining this grant" (warmup
 *                             failed or self-promotion failed). Lets the server offer
 *                             the slot to the next queued client immediately instead
 *                             of waiting out a liveness timeout.
 * @WISP_MSG_EVICT_PENDING:    server -> client: "wind down cooperatively within
 *                             grace_ms". Replaces an immediate hard evict.
 * @WISP_MSG_STOPPED:          client -> server: "I've stopped my render loop, drained,
 *                             and self-demoted to non-RT scheduling". Empty payload;
 *                             presence is the signal. Triggers the server's cpuset
 *                             reclaim.
 * @WISP_MSG_LIVENESS_TIMEOUT: server -> client: best-effort, informational notice
 *                             sent just before a forced (non-cooperative) eviction,
 *                             i.e. no heartbeat/frame observed within the liveness
 *                             window. Empty payload.
 */
typedef enum {
  WISP_MSG_CONNECT = 1,
  WISP_MSG_MODE = 2,
  WISP_MSG_ACTIVATE_REQUEST = 3,
  WISP_MSG_ACTIVATE_GRANT = 4,
  WISP_MSG_ACTIVATE_DENY = 5,
  WISP_MSG_HEARTBEAT = 6,
  WISP_MSG_DEACTIVATE = 7,
  WISP_MSG_DISCONNECT = 8,
  WISP_MSG_DMABUF_ANNOUNCE = 9,
  WISP_MSG_DMABUF_ACK = 10,
  WISP_MSG_ACTIVATE_QUEUED = 11,
  WISP_MSG_READY_FOR_RT = 12,
  WISP_MSG_GRANT_DECLINE = 13,
  WISP_MSG_EVICT_PENDING = 14,
  WISP_MSG_STOPPED = 15,
  WISP_MSG_LIVENESS_TIMEOUT = 16,
  WISP_MSG_VERSION_MISMATCH = 17,
} wisp_msg_kind_t;

/**
 * struct wisp_connect_msg_t - WISP_MSG_CONNECT payload
 * @protocol_magic: this client's WISP_PROTOCOL_MAGIC; must match the server's exactly
 * @protocol_major: this client's WISP_PROTOCOL_VERSION_MAJOR; must match the server's
 *                  exactly
 * @protocol_minor: this client's WISP_PROTOCOL_VERSION_MINOR; backwards-compatbile soft
 *                  gate wire additions only.
 * @client_id:      client application id
 * @num_modes:      number of wisp_render_mode_t entries in @modes
 * @modes:          supported render modes, in order of preference
 *
 * @protocol_magic/@protocol_major are checked first, before anything else in this
 * struct is even looked at -- a mismatch on either gets WISP_MSG_VERSION_MISMATCH
 * back and the connection closed, never a mode negotiation.
 */
typedef struct {
  uint32_t protocol_magic;
  uint32_t protocol_major;
  uint32_t protocol_minor;
  char client_id[WISP_CLIENT_ID_LEN];
  uint32_t num_modes;
  wisp_render_mode_t modes[WISP_MAX_MODES];
} wisp_connect_msg_t;

/**
 * struct wisp_version_mismatch_msg_t - WISP_MSG_VERSION_MISMATCH payload
 * @required_magic: the server's WISP_PROTOCOL_MAGIC
 * @required_major: the server's WISP_PROTOCOL_VERSION_MAJOR
 *
 * Lets a mismatched client log/report specifically what it needs to be rebuilt
 * against, rather than just "rejected".
 */
typedef struct {
  uint32_t required_magic;
  uint32_t required_major;
} wisp_version_mismatch_msg_t;

/**
 * struct wisp_mode_msg_t - WISP_MSG_MODE payload
 * @accepted: 1 if the server accepted one of the offered modes, 0 if rejected
 * @chosen:   the mode chosen by the server (only valid if accepted==1)
 */
typedef struct {
  uint8_t accepted;
  wisp_render_mode_t chosen;
} wisp_mode_msg_t;

/**
 * struct wisp_grant_msg_t - WISP_MSG_ACTIVATE_GRANT payload
 * @generation:        the generation number granted to the client; if this no longer
 *                     matches the server's generation, the client has been evicted and
 *                     must stop writing
 * @target_core:       cpuset core index the server has already placed this client's
 *                     control-thread TID onto; server-computed, not client-asserted
 * @priority_hint:     suggested scheduling priority for the client's self-promotion
 *                     (sched_setattr), server-computed
 * @period_ns:         expected frame period in nanoseconds, informs SCHED_DEADLINE
 *                     parameters the client chooses for itself
 * @runtime_budget_ns: expected per-period runtime budget in nanoseconds, informs
 *                     SCHED_DEADLINE parameters the client chooses for itself
 *
 * target_core/priority_hint/period_ns/runtime_budget_ns are all consumed by the
 * client's warmup callback purely as inputs to its own self-promotion decision; the
 * server never performs the sched_setattr() call on the client's behalf (i.e.,
 * split-responsibility scheduling handshake).
 */
typedef struct {
  uint32_t generation;
  uint32_t target_core;
  uint32_t priority_hint;
  uint64_t period_ns;
  uint64_t runtime_budget_ns;
} wisp_grant_msg_t;

/**
 * struct wisp_queued_msg_t - WISP_MSG_ACTIVATE_QUEUED payload
 * @queue_position: 1-based position in line for the RT slot (1 = next to be granted)
 *
 * Informational only; queue membership itself is derived server-side from the session
 * table, not tracked in a separate bounded structure. This message may be re-sent with
 * an updated @queue_position as the queue shifts.
 */
typedef struct {
  uint32_t queue_position;
} wisp_queued_msg_t;

/**
 * enum wisp_grant_decline_reason_t - WISP_MSG_GRANT_DECLINE reason codes
 * @WISP_GRANT_DECLINE_WARMUP_FAILED:    the client's warmup callback returned failure
 *                                       or exceeded its timeout
 * @WISP_GRANT_DECLINE_PROMOTION_FAILED: the client's own sched_setattr()/
 *                                       sched_setscheduler() self-promotion call failed
 *                                       (e.g. missing CAP_SYS_NICE)
 */
typedef enum {
  WISP_GRANT_DECLINE_WARMUP_FAILED = 1,
  WISP_GRANT_DECLINE_PROMOTION_FAILED = 2,
} wisp_grant_decline_reason_t;

/**
 * struct wisp_grant_decline_msg_t - WISP_MSG_GRANT_DECLINE payload
 * @reason: why the client is declining this activation grant
 *
 */
typedef struct {
  uint32_t reason;
} wisp_grant_decline_msg_t;

/**
 * struct wisp_evict_pending_msg_t - WISP_MSG_EVICT_PENDING payload
 * @grace_ms: cooperative wind-down window, in milliseconds, before the server treats
 *            this as a forced eviction
 */
typedef struct {
  uint32_t grace_ms;
} wisp_evict_pending_msg_t;

/**
 * struct wisp_deny_msg_t - WISP_MSG_ACTIVATE_DENY payload
 * @reason: human-readable explanation of why the client was denied activation (e.g.
 *          "another app is currently active")
 */
typedef struct {
  char reason[WISP_DENY_REASON_LEN];
} wisp_deny_msg_t;

/**
 * struct wisp_heartbeat_msg_t - WISP_MSG_HEARTBEAT payload
 * @generation:    the generation number the client believes it holds
 * @frame_counter: the client's (monotonically increasing) current frame
 */
typedef struct {
  uint32_t generation;
  uint64_t frame_counter;
} wisp_heartbeat_msg_t;

/**
 * struct wisp_dmabuf_announce_msg_t - WISP_MSG_DMABUF_ANNOUNCE payload
 * @nbufs: must equal WISP_NUM_BUFFERS
 * @desc:  per-buffer geometry, index-aligned with the SCM_RIGHTS fds
 *
 * Exactly @nbufs dmabuf fds MUST accompany this message as SCM_RIGHTS ancillary
 * data (see wisp_ctrl_send_fds()). fd[i] corresponds to desc[i], and both correspond
 * to ring slot index i.
 */
typedef struct {
  uint32_t nbufs;
  wisp_dmabuf_desc_t desc[WISP_NUM_BUFFERS];
} wisp_dmabuf_announce_msg_t;

/**
 * struct wisp_dmabuf_ack_msg_t - WISP_MSG_DMABUF_ACK payload
 * @accepted: 1 if the server imported all buffers; session is now DMABUF mode
 * @reason:   human-readable refusal reason (valid when accepted==0)
 */
typedef struct {
  uint8_t accepted;
  char reason[WISP_DENY_REASON_LEN];
} wisp_dmabuf_ack_msg_t;

/**
 * wisp_ctrl_send() - frame and send one control message
 * @fd:      connected control socket
 * @kind:    message kind
 * @payload: pointer to the message's payload struct, or NULL if @len == 0
 * @len:     size of @payload in bytes
 *
 * Wire format: 1-byte kind + 4-byte big-endian length + payload bytes. Blocks until the
 * whole message is written or an error occurs. Used by both sides of the connection for
 * every message kind that does not carry fds (for those, see wisp_ctrl_send_fds()).
 *
 * Return: 0 on success, -1 on error.
 */
int wisp_ctrl_send(int fd, wisp_msg_kind_t kind, const void *payload, uint32_t len);

/**
 * wisp_ctrl_recv() - receive and unframe one control message
 * @fd:       connected control socket
 * @out_type: set to the received message's kind
 * @buf:      caller-provided buffer to receive the payload
 * @bufsize:  capacity of @buf in bytes
 * @out_len:  set to the number of payload bytes actually written to @buf
 *
 * Any fds attached via SCM_RIGHTS to a message received through this function (as
 * opposed to wisp_ctrl_recv_fds()) are silently discarded by the kernel. Do not use
 * this to receive WISP_MSG_DMABUF_ANNOUNCE.
 *
 * Return: 0 on success, -1 on I/O error or peer disconnect, -2 if the message is larger
 * than @bufsize.
 */
int wisp_ctrl_recv(int fd, wisp_msg_kind_t *out_type, void *buf, uint32_t bufsize,
                   uint32_t *out_len);

/**
 * wisp_ctrl_send_fds() - send a control message with attached fds
 * @fd:      connected control socket
 * @kind:    message kind
 * @payload: message payload, or NULL if @len == 0
 * @len:     size of @payload in bytes
 * @fds:     file descriptors to attach via SCM_RIGHTS
 * @nfds:    number of entries in @fds (must be <= WISP_NUM_BUFFERS)
 *
 * Same wire format as wisp_ctrl_send(); the fds ride as SCM_RIGHTS ancillary data
 * attached to the framing header. Used in both directions: clients send fds on
 * WISP_MSG_DMABUF_ANNOUNCE, the server sends the frame-ready eventfd on
 * WISP_MSG_ACTIVATE_GRANT.
 *
 * Return: 0 on success, -1 on error (including @nfds out of range).
 */
int wisp_ctrl_send_fds(int fd, wisp_msg_kind_t kind, const void *payload, uint32_t len,
                       const int *fds, int nfds);

/**
 * wisp_ctrl_recv_fds() - receive a control message, harvesting any fds
 * @fd:       connected control socket
 * @out_type: set to the received message's kind
 * @buf:      caller-provided buffer to receive the payload
 * @bufsize:  capacity of @buf in bytes
 * @out_len:  set to the number of payload bytes actually written to @buf
 * @out_fds:  caller-provided array to receive any SCM_RIGHTS fds. Nullable.
 * @max_fds:  capacity of @out_fds; excess fds are closed to avoid leaks. Zeroable.
 * @out_nfds: set to the number of fds actually written to @out_fds. Zeroable
 *
 * Receiving an fd-bearing message with out_fds = NULL will silently discard. There
 * should be good reason to ignore any possible fds.
 *
 * Return: 0 on success, -1 on error/disconnect, -2 if the message is larger than
 * @bufsize (any harvested fds are closed first).
 */
int wisp_ctrl_recv_fds(int fd, wisp_msg_kind_t *out_type, void *buf, uint32_t bufsize,
                       uint32_t *out_len, int *out_fds, int max_fds, int *out_nfds);

/**
 * wisp_close_fds() - close every valid (>=0) fd in @fds [0..nfds)
 * @fds:  file descriptors to close
 * @nfds: number of fds
 *
 * Shared fd-ownership helper: both sides use this to release SCM_RIGHTS-received
 * fds (e.g. dmabuf sets) on error/eviction/teardown paths, so ring/session cleanup
 * on either side of the connection never leaks fds.
 */
void wisp_close_fds(int *fds, int nfds);

#ifdef __cplusplus
}
#endif

#endif /* WISP_WIRE_H */