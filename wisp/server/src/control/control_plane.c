// control_plane.c - see control_plane.h.
#include "control_plane.h"

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/**
 * WISPS_CONTROL_RECV_BUF_SIZE - max buffer size for control message
 *
 * Generous fixed buffer for one control-protocol message. The largest non-fd-bearing
 * payload today is wisp_connect_msg_t (well under 1KiB); the only fd-bearing message
 * (DMABUF_ANNOUNCE) is handled via wisp_ctrl_recv_fds() into this same buffer.
 */
#define WISPS_CONTROL_RECV_BUF_SIZE 4096

/**
 * WISPS_CONTROL_MAX_POLLFDS - max fds to concurrently poll
 *
 * WISPS_CONTROL_MAX_CLIENTS worth of producer fds, plus listen_fd, stop_read_fd, and
 * schan's wake_fd.
 */
#define WISPS_CONTROL_MAX_POLLFDS (WISPS_SESSION_MAX_CLIENTS + 3)

/**
 * WISPS_CONTROL_POLL_TIMEOUT_MS - how often the poll() loop wakes
 *
 * Wakes up even with nothing to do, so wisps_session_table_check_heartbeat_timeouts()
 * runs regularly instead of only when socket activity happens to occur.
 */
#define WISPS_CONTROL_POLL_TIMEOUT_MS 250

/**
 * negotiate_mode() - first-exact-match rule.
 * @cp:          control plane state
 * @offered:     producer's offered modes, in the producer's preference order
 * @num_offered: number of valid entries in @offered
 * @out_chosen:  filled in with the matched mode iff this returns true
 *
 * Return: true iff some offered mode exactly (width, height, fps all equal) matches
 * some entry in @cp->supported_modes.
 */
static bool negotiate_mode(wisps_control_plane_t *cp, const wisp_render_mode_t *offered,
                           uint32_t num_offered, wisp_render_mode_t *out_chosen);

/**
 * do_grant() - unconditionally perform a grant to @idx, no eviction involved
 * @cp:  control plane state
 * @idx: slot to grant; caller guarantees the RT slot is currently free (no slot is
 *       ACTIVE_RT or GRANTED_WARMUP)
 *
 * Bumps the ring generation, sends WISP_MSG_ACTIVATE_GRANT, and notifies the data
 * plane if the negotiated frame size changed. Shared tail end of grant_slot() (RT
 * slot already free) and finish_pending_grant() (RT slot just freed by an eviction).
 */
static void do_grant(wisps_control_plane_t *cp, int idx);

/**
 * grant_slot() - grant @idx the RT slot, evicting cooperatively first if occupied
 * @cp:  control plane state
 * @idx: slot to grant; caller guarantees this slot is currently
 *       WISPS_SESSION_NEGOTIATED or WISPS_SESSION_QUEUED
 *
 * If the RT slot is free, grants @idx immediately (see do_grant()). If it's held by
 * an ACTIVE_RT session, begins cooperative eviction instead (see begin_eviction())
 * and defers the actual grant until that resolves. If it's held by a
 * GRANTED_WARMUP session (mid-handshake for a different, not-yet-active grant), the
 * in-flight warmup is left alone and @idx is simply remembered as the next
 * pending-grant target -- applied once that warmup resolves one way or the other.
 */
static void grant_slot(wisps_control_plane_t *cp, int idx);

/**
 * handle_readable() - one recv+dispatch cycle for slot @idx.
 * @cp:  control plane handling the message
 * @idx: session table slot index of the readable fd
 */
static void handle_readable(wisps_control_plane_t *cp, int idx);

/**
 * handle_list_query() - fill a LIST query from the real table.
 * @cp:    control plane handling the query
 * @query: the LIST query to fill
 */
static void handle_list_query(wisps_control_plane_t *cp, wisps_control_query_t *query);

/**
 * handle_switch_query() - switch semantics.
 * @cp:    control plane handling the query
 * @query: the SWITCH query to handle
 */
static void handle_switch_query(wisps_control_plane_t *cp,
                                wisps_control_query_t *query);

/**
 * drain_admin_query() - answer whatever the admin thread posted.
 * @cp: control plane handling the admin query
 */
static void drain_admin_query(wisps_control_plane_t *cp);

/**
 * handle_disconnect() - close+free a slot.
 * @cp:  control plane handling the message
 * @idx: session table slot index of the disconnecting fd
 *
 * Shared by: an explicit WISP_MSG_DISCONNECT, a socket EOF/error, and a rejected
 * CONNECT (REJECTED -> CLOSED is immediate).
 */
static void handle_disconnect(wisps_control_plane_t *cp, int idx);

/**
 * handle_connect() - CONNECT -> mode negotiation.
 * @cp:      control plane handling the message
 * @idx:     session table slot index of the connecting fd
 * @connect: the CONNECT message payload
 */
static void handle_connect(wisps_control_plane_t *cp, int idx,
                           const wisp_connect_msg_t *connect);

/**
 * handle_activate_request() - granting rules.
 * @cp:  control plane handling the message
 * @idx: session table slot index of the requesting fd
 */
static void handle_activate_request(wisps_control_plane_t *cp, int idx);

/**
 * handle_heartbeat() - refresh the ACTIVE client's liveness stamp.
 * @cp:  control plane handling the message
 * @idx: session table slot index of the heartbeat fd
 */
static void handle_heartbeat(wisps_control_plane_t *cp, int idx);

/**
 * handle_dmabuf_announce() - always refuse in v1.
 * @cp:   control plane handling the message
 * @idx:  session table slot index of the announcing fd
 * @msg:  the DMABUF_ANNOUNCE message payload
 * @fds:  array of file descriptors accompanying the message
 * @nfds: number of valid entries in @fds
 *
 * Real KMS/DRM import is out of scope for this version; always responding (never
 * leaving the announcing producer waiting) lets it fall back to its own
 * glReadPixels-into-shm path per libwisp.h's client-side fallback note.
 */
static void handle_dmabuf_announce(wisps_control_plane_t *cp, int idx,
                                   const wisp_dmabuf_announce_msg_t *msg,
                                   const int *fds, int nfds);

/**
 * handle_ready_for_rt() - GRANTED_WARMUP -> ACTIVE_RT, start the liveness clock.
 * @cp:  control plane handling the message
 * @idx: session table slot index of the reporting fd
 *
 * No-op/defensive if @idx isn't currently GRANTED_WARMUP
 */
static void handle_ready_for_rt(wisps_control_plane_t *cp, int idx);

/**
 * handle_grant_decline() - GRANTED_WARMUP -> NEGOTIATED, offer the slot onward.
 * @cp:  control plane handling the message
 * @idx: session table slot index of the declining fd
 *
 * The client failed its warmup callback or its own scheduling self-promotion and is
 * fast-failing out of the grant rather than making the server wait out a liveness
 * timeout. Drops @idx back to NEGOTIATED (it may re-request activation later) and
 * immediately grants the next queued client, if any.
 */
static void handle_grant_decline(wisps_control_plane_t *cp, int idx);

/**
 * begin_eviction() - EVICT_PENDING the current ACTIVE_RT holder, remember who's next
 * @cp:       control plane state
 * @prev_idx: currently ACTIVE_RT slot to evict
 * @target:   slot to grant once @prev_idx confirms STOPPED or the grace period
 *            elapses
 *
 * Moves @prev_idx to WISPS_SESSION_EVICTING_COOPERATIVE and sends
 * WISP_MSG_EVICT_PENDING{grace_ms}. Stashes @target in cp->pending_grant_target and
 * stamps cp->eviction_deadline_monotonic. The poll loop force-demotes @prev_idx if
 * that deadline passes with no WISP_MSG_STOPPED (see handle_stopped(),
 * wisps_control_plane_run()).
 */
static void begin_eviction(wisps_control_plane_t *cp, int prev_idx, int target);

/**
 * finish_pending_grant() - actually grant cp->pending_grant_target
 * @cp: control plane state
 *
 * Called once the prior ACTIVE_RT holder's cooperative wind-down has resolved,
 * either by WISP_MSG_STOPPED (see handle_stopped()) or by grace-period expiry (see
 * wisps_control_plane_run()) -- the RT slot is free of the previous holder either
 * way by the time this runs. No-op if cp->pending_grant_target is -1 (e.g. the
 * target disconnected while the eviction was in flight).
 */
static void finish_pending_grant(wisps_control_plane_t *cp);

/**
 * handle_stopped() - EVICTING_COOPERATIVE -> reclaimed, grant whoever's pending
 * @cp:  control plane handling the message
 * @idx: session table slot index of the reporting fd
 *
 * No-op (defensive) if @idx isn't the session currently EVICTING_COOPERATIVE (e.g. a
 * stray STOPPED, or one that arrives just after the grace-period deadline already
 * force-demoted it).
 */
static void handle_stopped(wisps_control_plane_t *cp, int idx);

/**
 * maybe_grant_next_queued() - auto-promote the earliest-waiting QUEUED client
 * @cp: control plane state
 *
 * Called whenever the RT slot becomes free with clients still waiting (a grant is
 * declined, a heartbeat times out, an active/granted client disconnects). No-op if no
 * slot is currently QUEUED, or if the RT slot is already spoken for.
 */
static void maybe_grant_next_queued(wisps_control_plane_t *cp);

int wisps_control_plane_init(wisps_control_plane_t *cp, wisp_shm_ring_t *ring,
                             int frame_fd, wisps_data_plane_t *dp,
                             wisps_control_query_channel_t *chan,
                             const wisp_render_mode_t *supported_modes,
                             uint32_t num_supported_modes) {
  if (num_supported_modes == 0) {
    fprintf(stderr, "[pgipc-control] num_supported_modes must be >= 1\n");
    return -1;
  }

  if (num_supported_modes > WISP_MAX_MODES)
    num_supported_modes = WISP_MAX_MODES;

  memset(cp, 0, sizeof(*cp));
  cp->ring = ring;
  cp->frame_fd = frame_fd;
  cp->dp = dp;
  cp->chan = chan;
  cp->num_supported_modes = num_supported_modes;
  cp->pending_grant_target = -1;
  cp->evicting_slot = -1;
  for (uint32_t i = 0; i < num_supported_modes; i++)
    cp->supported_modes[i] = supported_modes[i];

  wisps_session_table_init(&cp->table);
  wisp_atomic_store(&cp->running, true);

  int stop_fds[2];
  if (pipe(stop_fds) != 0) {
    perror("pipe (control plane stop)");
    return -1;
  }
  cp->stop_read_fd = stop_fds[0];
  cp->stop_write_fd = stop_fds[1];

  cp->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (cp->listen_fd < 0) {
    perror("socket (control plane)");
    close(cp->stop_read_fd);
    close(cp->stop_write_fd);
    return -1;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, WISPS_CONTROL_SOCK_PATH, sizeof(addr.sun_path) - 1);

  unlink(WISPS_CONTROL_SOCK_PATH); // stale socket from a previous run

  if (bind(cp->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    perror("bind (control plane)");
    close(cp->listen_fd);
    close(cp->stop_read_fd);
    close(cp->stop_write_fd);
    cp->listen_fd = -1;
    return -1;
  }

  if (listen(cp->listen_fd, /*backlog=*/WISPS_SESSION_MAX_CLIENTS) != 0) {
    perror("listen (control plane)");
    close(cp->listen_fd);
    close(cp->stop_read_fd);
    close(cp->stop_write_fd);
    cp->listen_fd = -1;
    return -1;
  }

  return 0;
}

void *wisps_control_plane_run(void *arg) {
  wisps_control_plane_t *cp = (wisps_control_plane_t *)arg;

  while (wisp_atomic_load(&cp->running)) {
    struct pollfd pfds[WISPS_CONTROL_MAX_POLLFDS];
    int nfds = 0;

    int listen_pos = nfds;
    pfds[nfds].fd = cp->listen_fd;
    pfds[nfds].events = POLLIN;
    nfds++;

    int stop_pos = nfds;
    pfds[nfds].fd = cp->stop_read_fd;
    pfds[nfds].events = POLLIN;
    nfds++;

    int chan_pos = nfds;
    pfds[nfds].fd = wisps_control_query_channel_wake_fd(cp->chan);
    pfds[nfds].events = POLLIN;
    nfds++;

    int slot_pos[WISPS_SESSION_MAX_CLIENTS];
    for (int i = 0; i < WISPS_SESSION_MAX_CLIENTS; i++) {
      slot_pos[i] = -1;
      if (cp->table.slots[i].in_use) {
        slot_pos[i] = nfds;
        pfds[nfds].fd = cp->table.slots[i].ctrl_fd;
        pfds[nfds].events = POLLIN;
        nfds++;
      }
    }

    int rc = poll(pfds, (nfds_t)nfds, WISPS_CONTROL_POLL_TIMEOUT_MS);
    if (rc < 0) {
      if (errno == EINTR)
        continue;
      perror("poll (control plane)");
      break;
    }

    if (pfds[stop_pos].revents & POLLIN)
      break; // wisps_control_plane_stop() was called

    if (pfds[chan_pos].revents & POLLIN)
      drain_admin_query(cp);

    if (pfds[listen_pos].revents & POLLIN) {
      int client_fd = accept(cp->listen_fd, NULL, NULL);

      if (client_fd < 0) {
        if (errno != EINTR)
          perror("accept (control plane)");
      } else if (wisps_session_table_add(&cp->table, client_fd) < 0) {
        // Table full: accept then immediately close with no CONNECT reply.
        // resource limit, not a protocol message.
        close(client_fd);
      }
    }

    for (int i = 0; i < WISPS_SESSION_MAX_CLIENTS; i++) {
      if (slot_pos[i] < 0)
        continue;

      short revents = pfds[slot_pos[i]].revents;
      if (revents & POLLIN)
        handle_readable(cp, i);
      else if (revents & (POLLHUP | POLLERR))
        handle_disconnect(cp, i);
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    int timed_out = wisps_session_table_check_heartbeat_timeouts(&cp->table, now);
    if (timed_out >= 0) {
      // wisps_session_table_check_heartbeat_timeouts() already moved the slot
      // ACTIVE -> NEGOTIATED internally; the ring-generation bump and wire
      // notification are this layer's responsibility.
      wisps_evict_client(cp->ring);
      if (cp->dp)
        wisps_data_plane_kick(cp->dp);
      // LIVENESS_TIMEOUT first, then DEACTIVATE. The fd is very possibly already dead
      // so both sends may silently fail; forced eviction that doesn't wait on
      // cooperation either way.
      wisp_ctrl_send(cp->table.slots[timed_out].ctrl_fd, WISP_MSG_LIVENESS_TIMEOUT,
                     NULL, 0);
      wisp_ctrl_send(cp->table.slots[timed_out].ctrl_fd, WISP_MSG_DEACTIVATE, NULL, 0);
      maybe_grant_next_queued(cp);
    }

    if (cp->evicting_slot >= 0) {
      int64_t sec_diff =
          (int64_t)now.tv_sec - (int64_t)cp->eviction_deadline_monotonic.tv_sec;
      int64_t nsec_diff =
          (int64_t)now.tv_nsec - (int64_t)cp->eviction_deadline_monotonic.tv_nsec;
      if (sec_diff * 1000 + nsec_diff / 1000000 >= 0) {
        // Grace period elapsed with no WISP_MSG_STOPPED: force-demote. Real
        // sched_setattr(SCHED_OTHER)/cpuset-reclaim work is out of scope for this
        // build (no scheduling backend wired up yet); this just settles the state
        // machine and unblocks the pending grant.
        int idx = cp->evicting_slot;
        cp->evicting_slot = -1;
        wisps_session_table_mark_eviction_forced(&cp->table, idx);
        wisps_session_table_reclaim_eviction(&cp->table, idx);
        finish_pending_grant(cp);
        maybe_grant_next_queued(cp);
      }
    }
  }

  return NULL;
}

void wisps_control_plane_stop(wisps_control_plane_t *cp) {
  unsigned char byte = 1;
  ssize_t n;

  wisp_atomic_store(&cp->running, false);
  do {
    n = write(cp->stop_write_fd, &byte, 1);
  } while (n < 0 && errno == EINTR);
}

void wisps_control_plane_close(wisps_control_plane_t *cp) {
  for (int i = 0; i < WISPS_SESSION_MAX_CLIENTS; i++) {
    if (cp->table.slots[i].in_use && cp->table.slots[i].ctrl_fd >= 0)
      close(cp->table.slots[i].ctrl_fd);
  }

  if (cp->listen_fd >= 0)
    close(cp->listen_fd);
  if (cp->stop_read_fd >= 0)
    close(cp->stop_read_fd);
  if (cp->stop_write_fd >= 0)
    close(cp->stop_write_fd);

  cp->listen_fd = -1;
  cp->stop_read_fd = -1;
  cp->stop_write_fd = -1;
  unlink(WISPS_CONTROL_SOCK_PATH);
}

static bool negotiate_mode(wisps_control_plane_t *cp, const wisp_render_mode_t *offered,
                           uint32_t num_offered, wisp_render_mode_t *out_chosen) {
  for (uint32_t i = 0; i < num_offered; i++) {
    for (uint32_t j = 0; j < cp->num_supported_modes; j++) {
      if (offered[i].width == cp->supported_modes[j].width &&
          offered[i].height == cp->supported_modes[j].height &&
          offered[i].fps == cp->supported_modes[j].fps) {
        *out_chosen = cp->supported_modes[j];
        return true;
      }
    }
  }
  return false;
}

static void do_grant(wisps_control_plane_t *cp, int idx) {
  wisps_evict_client(cp->ring); // bumps generation
  if (cp->dp)
    wisps_data_plane_kick(cp->dp);

  uint32_t generation = wisp_atomic_load(&cp->ring->generation);
  wisps_session_table_grant(&cp->table, idx, generation);

  wisp_grant_msg_t grant = {.generation = generation};
  wisp_ctrl_send_fds(cp->table.slots[idx].ctrl_fd, WISP_MSG_ACTIVATE_GRANT, &grant,
                     sizeof(grant), &cp->frame_fd, 1);

  wisp_render_mode_t new_mode = cp->table.slots[idx].negotiated_mode;
  if (cp->dp)
    wisps_data_plane_set_mode(cp->dp, new_mode.width, new_mode.height);
}

static void begin_eviction(wisps_control_plane_t *cp, int prev_idx, int target) {
  wisps_session_table_begin_eviction(&cp->table, prev_idx);

  wisp_evict_pending_msg_t evict = {.grace_ms = WISP_EVICT_GRACE_MS};
  wisp_ctrl_send(cp->table.slots[prev_idx].ctrl_fd, WISP_MSG_EVICT_PENDING, &evict,
                 sizeof(evict));

  cp->evicting_slot = prev_idx;
  cp->pending_grant_target = target;
  clock_gettime(CLOCK_MONOTONIC, &cp->eviction_deadline_monotonic);
  cp->eviction_deadline_monotonic.tv_sec += WISP_EVICT_GRACE_MS / 1000;
  cp->eviction_deadline_monotonic.tv_nsec += (WISP_EVICT_GRACE_MS % 1000) * 1000000L;
  if (cp->eviction_deadline_monotonic.tv_nsec >= 1000000000L) {
    cp->eviction_deadline_monotonic.tv_sec++;
    cp->eviction_deadline_monotonic.tv_nsec -= 1000000000L;
  }
}

static void finish_pending_grant(wisps_control_plane_t *cp) {
  int target = cp->pending_grant_target;
  cp->pending_grant_target = -1;

  if (target < 0)
    return; // e.g. the target disconnected while the eviction was in flight

  wisps_session_t *slot = &cp->table.slots[target];
  if (!slot->in_use ||
      (slot->state != WISPS_SESSION_NEGOTIATED && slot->state != WISPS_SESSION_QUEUED))
    return; // defensive: target's state changed underneath us

  do_grant(cp, target);
}

static void handle_stopped(wisps_control_plane_t *cp, int idx) {
  wisps_session_t *slot = &cp->table.slots[idx];
  if (slot->state != WISPS_SESSION_EVICTING_COOPERATIVE)
    return; // stray STOPPED, or grace period already force-demoted this slot

  wisps_session_table_reclaim_eviction(&cp->table, idx);
  if (cp->evicting_slot == idx)
    cp->evicting_slot = -1;
  finish_pending_grant(cp);
}

static void grant_slot(wisps_control_plane_t *cp, int idx) {
  int prev_active = cp->table.active_slot;

  if (prev_active >= 0 && prev_active != idx) {
    begin_eviction(cp, prev_active, idx);
    return;
  }

  if (wisps_session_table_rt_slot_taken(&cp->table)) {
    // RT slot is occupied by something other than active_slot -- either a
    // GRANTED_WARMUP in flight for a different client, or an eviction already in
    // progress. Don't preempt an in-flight warmup/eviction; just remember @idx as
    // the next target once it resolves (mid-warmup admin-forced-switch edge case).
    cp->pending_grant_target = idx;
    return;
  }

  do_grant(cp, idx);
}

/**
 * translate_state() - internal state -> wire state
 * @state: internal session state
 *
 * Returns: corresponding wire state (wisps_admin_client_state_t)
 */
static wisps_admin_client_state_t translate_state(wisps_session_state_t state) {
  switch (state) {
  case WISPS_SESSION_CONNECTED:
    return WISPS_ADMIN_STATE_CONNECTED;
  case WISPS_SESSION_NEGOTIATED:
    return WISPS_ADMIN_STATE_NEGOTIATED;
  case WISPS_SESSION_QUEUED:
    return WISPS_ADMIN_STATE_QUEUED;
  case WISPS_SESSION_GRANTED_WARMUP:
    return WISPS_ADMIN_STATE_GRANTED_WARMUP;
  case WISPS_SESSION_ACTIVE_RT:
    return WISPS_ADMIN_STATE_ACTIVE_RT;
  case WISPS_SESSION_EVICTING_COOPERATIVE:
    return WISPS_ADMIN_STATE_EVICTING_COOPERATIVE;
  case WISPS_SESSION_EVICTING_FORCED:
    return WISPS_ADMIN_STATE_EVICTING_FORCED;
  case WISPS_SESSION_REJECTED:
  default:
    return WISPS_ADMIN_STATE_REJECTED;
  }
}

static void handle_list_query(wisps_control_plane_t *cp, wisps_control_query_t *query) {
  uint32_t count = 0;

  for (int i = 0; i < WISPS_SESSION_MAX_CLIENTS && count < WISPS_ADMIN_MAX_CLIENTS;
       i++) {
    if (!cp->table.slots[i].in_use)
      continue;

    wisps_admin_client_info_t *out = &query->list_response.clients[count];
    strncpy(out->client_id, cp->table.slots[i].client_id, WISP_CLIENT_ID_LEN - 1);
    out->client_id[WISP_CLIENT_ID_LEN - 1] = '\0';
    out->state = translate_state(cp->table.slots[i].state);
    out->negotiated_mode = cp->table.slots[i].negotiated_mode;
    out->payload_kind = cp->table.slots[i].payload_kind;
    count++;
  }
  query->list_response.count = count;
}

static void handle_switch_query(wisps_control_plane_t *cp,
                                wisps_control_query_t *query) {
  int idx = wisps_session_table_find_by_client_id(&cp->table, query->switch_client_id);
  if (idx < 0) {
    query->switch_response.ok = 0;
    strncpy(query->switch_response.reason, "app not connected",
            WISPS_ADMIN_REASON_LEN - 1);
    query->switch_response.reason[WISPS_ADMIN_REASON_LEN - 1] = '\0';
    return;
  }

  if (cp->table.slots[idx].state == WISPS_SESSION_ACTIVE_RT) {
    query->switch_response.ok = 1; // already active: idempotent no-op
    query->switch_response.reason[0] = '\0';
    return;
  }

  if (cp->table.slots[idx].state != WISPS_SESSION_NEGOTIATED &&
      cp->table.slots[idx].state != WISPS_SESSION_QUEUED) {
    query->switch_response.ok = 0;
    strncpy(query->switch_response.reason, "app not in a switchable state",
            WISPS_ADMIN_REASON_LEN - 1);
    query->switch_response.reason[WISPS_ADMIN_REASON_LEN - 1] = '\0';
    return;
  }

  // Admin's named target jumps the queue explicitly, per the queue-as-derived-view
  // design: forced switch bypasses FIFO order rather than requiring the admin to
  // first drain everyone ahead of it.
  grant_slot(cp, idx);
  query->switch_response.ok = 1;
  query->switch_response.reason[0] = '\0';
}

static void drain_admin_query(wisps_control_plane_t *cp) {
  wisps_control_query_t *query = wisps_control_query_channel_drain(cp->chan);

  if (!query)
    return; // spurious wakeup, defensively handled per control_query.h's contract

  if (query->type == WISPS_CTRL_QUERY_LIST)
    handle_list_query(cp, query);
  else // WISPS_CTRL_QUERY_SWITCH
    handle_switch_query(cp, query);

  wisps_control_query_complete(query);
}

static void handle_disconnect(wisps_control_plane_t *cp, int idx) {
  wisps_session_t *slot = &cp->table.slots[idx];
  bool held_rt_slot = slot->state == WISPS_SESSION_ACTIVE_RT ||
                      slot->state == WISPS_SESSION_GRANTED_WARMUP ||
                      slot->state == WISPS_SESSION_EVICTING_COOPERATIVE ||
                      slot->state == WISPS_SESSION_EVICTING_FORCED;

  if (slot->state == WISPS_SESSION_ACTIVE_RT) {
    wisps_evict_client(cp->ring); // nothing to notify, fd is going away
    if (cp->dp)
      wisps_data_plane_kick(cp->dp);
  } else if (slot->state == WISPS_SESSION_EVICTING_COOPERATIVE ||
             slot->state == WISPS_SESSION_EVICTING_FORCED) {
    // fd is going away mid-wind-down; the grace-period deadline check would
    // eventually notice this too, but there's no need to wait for it once we
    // already know the slot is gone.
    wisps_session_table_reclaim_eviction(&cp->table, idx);
    if (cp->evicting_slot == idx)
      cp->evicting_slot = -1;
  }

  if (idx == cp->pending_grant_target)
    cp->pending_grant_target = -1; // the client waiting for this slot vanished

  close(slot->ctrl_fd);
  wisps_session_table_remove(&cp->table, idx);

  if (held_rt_slot) {
    finish_pending_grant(cp);
    maybe_grant_next_queued(cp);
  }
}

static void handle_connect(wisps_control_plane_t *cp, int idx,
                           const wisp_connect_msg_t *connect) {
  wisps_session_t *slot = &cp->table.slots[idx];

  if (connect->protocol_magic != WISP_PROTOCOL_MAGIC ||
      connect->protocol_major != WISP_PROTOCOL_VERSION_MAJOR) {
    wisp_version_mismatch_msg_t mismatch = {
        .required_magic = WISP_PROTOCOL_MAGIC,
        .required_major = WISP_PROTOCOL_VERSION_MAJOR,
    };
    wisp_ctrl_send(slot->ctrl_fd, WISP_MSG_VERSION_MISMATCH, &mismatch,
                   sizeof(mismatch));
    slot->state = WISPS_SESSION_REJECTED;
    handle_disconnect(cp, idx); // REJECTED -> CLOSED, no mode negotiation attempted
    return;
  }

  wisp_render_mode_t chosen;
  bool matched = negotiate_mode(cp, connect->modes, connect->num_modes, &chosen);

  wisp_mode_msg_t reply = {0};
  reply.accepted = matched ? 1 : 0;
  if (matched)
    reply.chosen = chosen;
  wisp_ctrl_send(slot->ctrl_fd, WISP_MSG_MODE, &reply, sizeof(reply));

  if (!matched) {
    slot->state = WISPS_SESSION_REJECTED;
    handle_disconnect(cp, idx); // REJECTED -> CLOSED
    return;
  }

  strncpy(slot->client_id, connect->client_id, WISP_CLIENT_ID_LEN - 1);
  slot->client_id[WISP_CLIENT_ID_LEN - 1] = '\0';
  slot->num_offered_modes = connect->num_modes;

  for (uint32_t i = 0; i < connect->num_modes; i++)
    slot->offered_modes[i] = connect->modes[i];

  slot->negotiated_mode = chosen;
  slot->negotiated_protocol_minor =
      connect->protocol_minor < WISP_PROTOCOL_VERSION_MINOR
          ? connect->protocol_minor
          : WISP_PROTOCOL_VERSION_MINOR;
  slot->state = WISPS_SESSION_NEGOTIATED;
}

static void handle_activate_request(wisps_control_plane_t *cp, int idx) {
  wisps_session_t *slot = &cp->table.slots[idx];
  if (slot->state != WISPS_SESSION_NEGOTIATED)
    return; // already ACTIVE/QUEUED, or CONNECT hasn't completed yet: defensive no-op

  if (!wisps_session_table_rt_slot_taken(&cp->table)) {
    grant_slot(cp, idx);
    return;
  }

  clock_gettime(CLOCK_MONOTONIC, &slot->activate_requested_monotonic);
  slot->state = WISPS_SESSION_QUEUED;

  wisp_queued_msg_t queued = {
      .queue_position = wisps_session_table_queue_position(&cp->table, idx),
  };
  wisp_ctrl_send(slot->ctrl_fd, WISP_MSG_ACTIVATE_QUEUED, &queued, sizeof(queued));
}

static void handle_ready_for_rt(wisps_control_plane_t *cp, int idx) {
  wisps_session_t *slot = &cp->table.slots[idx];
  if (slot->state != WISPS_SESSION_GRANTED_WARMUP)
    return; // stray/duplicate READY_FOR_RT, or this grant was already superseded

  wisps_session_table_activate(&cp->table, idx);
}

static void handle_grant_decline(wisps_control_plane_t *cp, int idx) {
  wisps_session_t *slot = &cp->table.slots[idx];
  if (slot->state != WISPS_SESSION_GRANTED_WARMUP)
    return; // stray/duplicate decline

  slot->state = WISPS_SESSION_NEGOTIATED;
  maybe_grant_next_queued(cp);
}

static void maybe_grant_next_queued(wisps_control_plane_t *cp) {
  if (wisps_session_table_rt_slot_taken(&cp->table))
    return;

  int next = wisps_session_table_find_next_queued(&cp->table);
  if (next >= 0)
    grant_slot(cp, next);
}

static void handle_heartbeat(wisps_control_plane_t *cp, int idx) {
  wisps_session_t *slot = &cp->table.slots[idx];
  if (slot->state != WISPS_SESSION_ACTIVE_RT)
    return; // NEGOTIATED clients don't heartbeat.

  clock_gettime(CLOCK_MONOTONIC, &slot->last_heartbeat_monotonic);
}

static void handle_dmabuf_announce(wisps_control_plane_t *cp, int idx,
                                   const wisp_dmabuf_announce_msg_t *msg,
                                   const int *fds, int nfds) {
  wisps_session_t *slot = &cp->table.slots[idx];
  wisps_dmabuf_set_t set;

  if (wisps_dmabuf_set_from_announce(&set, msg, fds, nfds) == 0)
    wisps_dmabuf_set_close(&set); // well-formed, but v1 never actually adopts it

  wisp_dmabuf_ack_msg_t ack = {0};
  ack.accepted = 0;
  strncpy(ack.reason, "GPU dmabuf import not implemented in this build",
          WISP_DENY_REASON_LEN - 1);

  wisp_ctrl_send(slot->ctrl_fd, WISP_MSG_DMABUF_ACK, &ack, sizeof(ack));
}

static void handle_readable(wisps_control_plane_t *cp, int idx) {
  wisps_session_t *slot = &cp->table.slots[idx];
  unsigned char buf[WISPS_CONTROL_RECV_BUF_SIZE];
  wisp_msg_kind_t type;
  uint32_t len;
  int fds[WISP_NUM_BUFFERS];
  int nfds = 0;

  int rc = wisp_ctrl_recv_fds(slot->ctrl_fd, &type, buf, sizeof(buf), &len, fds,
                              WISP_NUM_BUFFERS, &nfds);
  if (rc == -1) {
    handle_disconnect(cp, idx);
    return;
  }
  if (rc == -2) {
    // Oversized single frame: log and drop it, keep the connection open.
    fprintf(stderr, "[pgipc-control] oversized message from slot %d, dropping\n", idx);
    return;
  }

  switch (type) {
  case WISP_MSG_CONNECT: {
    wisp_connect_msg_t connect;
    memset(&connect, 0, sizeof(connect));

    if (len == sizeof(connect)) {
      memcpy(&connect, buf, sizeof(connect));
      connect.client_id[WISP_CLIENT_ID_LEN - 1] = '\0';

      if (connect.num_modes > WISP_MAX_MODES)
        connect.num_modes = 0; // malformed -> negotiate_mode guaranteed to reject
    }
    // else: len mismatch leaves connect.num_modes == 0 from the memset above,
    // which also guarantees negotiate_mode() rejects -- same malformed path.
    handle_connect(cp, idx, &connect);
    break;
  }
  case WISP_MSG_ACTIVATE_REQUEST:
    handle_activate_request(cp, idx);
    break;
  case WISP_MSG_READY_FOR_RT:
    handle_ready_for_rt(cp, idx);
    break;
  case WISP_MSG_GRANT_DECLINE:
    handle_grant_decline(cp, idx);
    break;
  case WISP_MSG_STOPPED:
    handle_stopped(cp, idx);
    break;
  case WISP_MSG_HEARTBEAT:
    handle_heartbeat(cp, idx);
    break;
  case WISP_MSG_DISCONNECT:
    handle_disconnect(cp, idx);
    break;
  case WISP_MSG_DMABUF_ANNOUNCE: {
    wisp_dmabuf_announce_msg_t msg;

    if (len != sizeof(wisp_dmabuf_announce_msg_t)) {
      fprintf(stderr, "[pgipc-control] malformed DMABUF_ANNOUNCE from slot %d\n", idx);
      break;
    }

    memcpy(&msg, buf, sizeof(msg));
    handle_dmabuf_announce(cp, idx, &msg, fds, nfds);
    break;
  }
  default:
    fprintf(stderr, "[pgipc-control] unexpected message type %d from slot %d\n",
            (int)type, idx);
    break;
  }
}
