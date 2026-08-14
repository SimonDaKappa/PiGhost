// control_plane.c - see control_plane.h.
#define LIBPGDP_SERVER
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
 * PGDPS_CONTROL_RECV_BUF_SIZE - max buffer size for control message
 *
 * Generous fixed buffer for one control-protocol message. The largest non-fd-bearing
 * payload today is pgdp_connect_msg_t (well under 1KiB); the only fd-bearing message
 * (DMABUF_ANNOUNCE) is handled via pgdp_ctrl_recv_fds() into this same buffer.
 */
#define PGDPS_CONTROL_RECV_BUF_SIZE 4096

/**
 * PGDPS_CONTROL_MAX_POLLFDS - max fds to concurrently poll
 *
 * PGDPS_CONTROL_MAX_CLIENTS worth of producer fds, plus listen_fd, stop_read_fd, and
 * schan's wake_fd.
 */
#define PGDPS_CONTROL_MAX_POLLFDS (PGDPS_SESSION_MAX_CLIENTS + 3)

/**
 * PGDPS_CONTROL_POLL_TIMEOUT_MS - how often the poll() loop wakes
 *
 * Wakes up even with nothing to do, so pgdps_session_table_check_heartbeat_timeouts()
 * runs regularly instead of only when socket activity happens to occur.
 */
#define PGDPS_CONTROL_POLL_TIMEOUT_MS 250

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
static bool negotiate_mode(pgdps_control_plane_t *cp, const pgdp_render_mode_t *offered,
                           uint32_t num_offered, pgdp_render_mode_t *out_chosen);

/**
 * activate() - evict-then-grant to @idx
 * @cp:  control plane state
 * @idx: slot to grant; caller guarantees this slot is currently
 *       PGDPS_SESSION_NEGOTIATED and is not the current active_slot
 *
 * Unconditionally bumps the ring generation exactly once, then, if a different
 * client was previously ACTIVE, notifies it via PGDP_MSG_DEACTIVATE before flipping
 * the table over to the new grant. Finally notifies the data plane if the negotiated
 * frame size changed.
 */
static void activate(pgdps_control_plane_t *cp, int idx);

/**
 * handle_readable() - one recv+dispatch cycle for slot @idx.
 * @cp:  control plane handling the message
 * @idx: session table slot index of the readable fd
 */
static void handle_readable(pgdps_control_plane_t *cp, int idx);

/**
 * handle_list_query() - fill a LIST query from the real table.
 * @cp:    control plane handling the query
 * @query: the LIST query to fill
 */
static void handle_list_query(pgdps_control_plane_t *cp, pgdps_control_query_t *query);

/**
 * handle_switch_query() - switch semantics.
 * @cp:    control plane handling the query
 * @query: the SWITCH query to handle
 */
static void handle_switch_query(pgdps_control_plane_t *cp,
                                pgdps_control_query_t *query);

/**
 * drain_admin_query() - answer whatever the admin thread posted.
 * @cp: control plane handling the admin query
 */
static void drain_admin_query(pgdps_control_plane_t *cp);

/**
 * handle_disconnect() - close+free a slot.
 * @cp:  control plane handling the message
 * @idx: session table slot index of the disconnecting fd
 *
 * Shared by: an explicit PGDP_MSG_DISCONNECT, a socket EOF/error, and a rejected
 * CONNECT (REJECTED -> CLOSED is immediate).
 */
static void handle_disconnect(pgdps_control_plane_t *cp, int idx);

/**
 * handle_connect() - CONNECT -> mode negotiation.
 * @cp:      control plane handling the message
 * @idx:     session table slot index of the connecting fd
 * @connect: the CONNECT message payload
 */
static void handle_connect(pgdps_control_plane_t *cp, int idx,
                           const pgdp_connect_msg_t *connect);

/**
 * handle_activate_request() - granting rules.
 * @cp:  control plane handling the message
 * @idx: session table slot index of the requesting fd
 */
static void handle_activate_request(pgdps_control_plane_t *cp, int idx);

/**
 * handle_heartbeat() - refresh the ACTIVE client's liveness stamp. 
 * @cp:  control plane handling the message
 * @idx: session table slot index of the heartbeat fd
 */
static void handle_heartbeat(pgdps_control_plane_t *cp, int idx);

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
 * glReadPixels-into-shm path per libpgdp.h's client-side fallback note.
 */
static void handle_dmabuf_announce(pgdps_control_plane_t *cp, int idx,
                                   const pgdp_dmabuf_announce_msg_t *msg,
                                   const int *fds, int nfds);

int pgdps_control_plane_init(pgdps_control_plane_t *cp, pgdp_shm_ring_t *ring,
                             int frame_fd, pgdps_data_plane_t *dp,
                             pgdps_control_query_channel_t *chan,
                             const pgdp_render_mode_t *supported_modes,
                             uint32_t num_supported_modes) {
  if (num_supported_modes == 0) {
    fprintf(stderr, "[pgipc-control] num_supported_modes must be >= 1\n");
    return -1;
  }

  if (num_supported_modes > PGDP_MAX_MODES)
    num_supported_modes = PGDP_MAX_MODES;

  memset(cp, 0, sizeof(*cp));
  cp->ring = ring;
  cp->frame_fd = frame_fd;
  cp->dp = dp;
  cp->chan = chan;
  cp->num_supported_modes = num_supported_modes;
  for (uint32_t i = 0; i < num_supported_modes; i++)
    cp->supported_modes[i] = supported_modes[i];

  pgdps_session_table_init(&cp->table);
  atomic_store(&cp->running, true);

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
  strncpy(addr.sun_path, PGDPS_CONTROL_SOCK_PATH, sizeof(addr.sun_path) - 1);

  unlink(PGDPS_CONTROL_SOCK_PATH); // stale socket from a previous run

  if (bind(cp->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    perror("bind (control plane)");
    close(cp->listen_fd);
    close(cp->stop_read_fd);
    close(cp->stop_write_fd);
    cp->listen_fd = -1;
    return -1;
  }

  if (listen(cp->listen_fd, /*backlog=*/PGDPS_SESSION_MAX_CLIENTS) != 0) {
    perror("listen (control plane)");
    close(cp->listen_fd);
    close(cp->stop_read_fd);
    close(cp->stop_write_fd);
    cp->listen_fd = -1;
    return -1;
  }

  return 0;
}

void *pgdps_control_plane_run(void *arg) {
  pgdps_control_plane_t *cp = (pgdps_control_plane_t *)arg;

  while (atomic_load(&cp->running)) {
    struct pollfd pfds[PGDPS_CONTROL_MAX_POLLFDS];
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
    pfds[nfds].fd = pgdps_control_query_channel_wake_fd(cp->chan);
    pfds[nfds].events = POLLIN;
    nfds++;

    int slot_pos[PGDPS_SESSION_MAX_CLIENTS];
    for (int i = 0; i < PGDPS_SESSION_MAX_CLIENTS; i++) {
      slot_pos[i] = -1;
      if (cp->table.slots[i].in_use) {
        slot_pos[i] = nfds;
        pfds[nfds].fd = cp->table.slots[i].ctrl_fd;
        pfds[nfds].events = POLLIN;
        nfds++;
      }
    }

    int rc = poll(pfds, (nfds_t)nfds, PGDPS_CONTROL_POLL_TIMEOUT_MS);
    if (rc < 0) {
      if (errno == EINTR)
        continue;
      perror("poll (control plane)");
      break;
    }

    if (pfds[stop_pos].revents & POLLIN)
      break; // pgdps_control_plane_stop() was called

    if (pfds[chan_pos].revents & POLLIN)
      drain_admin_query(cp);

    if (pfds[listen_pos].revents & POLLIN) {
      int client_fd = accept(cp->listen_fd, NULL, NULL);

      if (client_fd < 0) {
        if (errno != EINTR)
          perror("accept (control plane)");
      } else if (pgdps_session_table_add(&cp->table, client_fd) < 0) {
        // Table full: accept then immediately close with no CONNECT reply.
        // resource limit, not a protocol message.
        close(client_fd);
      }
    }

    for (int i = 0; i < PGDPS_SESSION_MAX_CLIENTS; i++) {
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

    int timed_out = pgdps_session_table_check_heartbeat_timeouts(&cp->table, now);
    if (timed_out >= 0) {
      // pgdps_session_table_check_heartbeat_timeouts() already moved the slot
      // ACTIVE -> NEGOTIATED internally; the ring-generation bump and wire
      // notification are this layer's responsibility.
      pgdps_evict_client(cp->ring);
      if (cp->dp)
        pgdps_data_plane_kick(cp->dp);
      pgdp_ctrl_send(cp->table.slots[timed_out].ctrl_fd, PGDP_MSG_DEACTIVATE, NULL, 0);
    }
  }

  return NULL;
}

void pgdps_control_plane_stop(pgdps_control_plane_t *cp) {
  unsigned char byte = 1;
  ssize_t n;

  atomic_store(&cp->running, false);
  do {
    n = write(cp->stop_write_fd, &byte, 1);
  } while (n < 0 && errno == EINTR);
}

void pgdps_control_plane_close(pgdps_control_plane_t *cp) {
  for (int i = 0; i < PGDPS_SESSION_MAX_CLIENTS; i++) {
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
  unlink(PGDPS_CONTROL_SOCK_PATH);
}

static bool negotiate_mode(pgdps_control_plane_t *cp, const pgdp_render_mode_t *offered,
                           uint32_t num_offered, pgdp_render_mode_t *out_chosen) {
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

static void activate(pgdps_control_plane_t *cp, int idx) {
  int prev_active = cp->table.active_slot;

  pgdp_render_mode_t prev_mode = {0};
  if (prev_active >= 0)
    prev_mode = cp->table.slots[prev_active].negotiated_mode;

  pgdps_evict_client(cp->ring); // bumps generation
  if (cp->dp)
    pgdps_data_plane_kick(cp->dp);

  if (prev_active >= 0 && prev_active != idx) {
    pgdp_ctrl_send(cp->table.slots[prev_active].ctrl_fd, PGDP_MSG_DEACTIVATE, NULL, 0);
  }

  uint32_t generation = pgdp_atomic_load(&cp->ring->generation);
  pgdps_session_table_activate(&cp->table, idx, generation);

  pgdp_grant_msg_t grant = {.generation = generation};
  pgdp_ctrl_send_fds(cp->table.slots[idx].ctrl_fd, PGDP_MSG_ACTIVATE_GRANT, &grant,
                     sizeof(grant), &cp->frame_fd, 1);

  pgdp_render_mode_t new_mode = cp->table.slots[idx].negotiated_mode;
  if (cp->dp && (prev_active < 0 || new_mode.width != prev_mode.width ||
                 new_mode.height != prev_mode.height))
    pgdps_data_plane_set_mode(cp->dp, new_mode.width, new_mode.height);
}

/**
 * translate_state() - internal state -> wire state 
 * @state: internal session state
 *
 * Returns: corresponding wire state (pgdps_admin_client_state_t)
 */
static pgdps_admin_client_state_t translate_state(pgdps_session_state_t state) {
  switch (state) {
  case PGDPS_SESSION_CONNECTED:
    return PGDPS_ADMIN_STATE_CONNECTED;
  case PGDPS_SESSION_NEGOTIATED:
    return PGDPS_ADMIN_STATE_NEGOTIATED;
  case PGDPS_SESSION_ACTIVE:
    return PGDPS_ADMIN_STATE_ACTIVE;
  case PGDPS_SESSION_REJECTED:
  default:
    return PGDPS_ADMIN_STATE_REJECTED;
  }
}

static void handle_list_query(pgdps_control_plane_t *cp, pgdps_control_query_t *query) {
  uint32_t count = 0;

  for (int i = 0; i < PGDPS_SESSION_MAX_CLIENTS && count < PGDPS_ADMIN_MAX_CLIENTS;
       i++) {
    if (!cp->table.slots[i].in_use)
      continue;

    pgdps_admin_client_info_t *out = &query->list_response.clients[count];
    strncpy(out->client_id, cp->table.slots[i].client_id, PGDP_CLIENT_ID_LEN - 1);
    out->client_id[PGDP_CLIENT_ID_LEN - 1] = '\0';
    out->state = translate_state(cp->table.slots[i].state);
    out->negotiated_mode = cp->table.slots[i].negotiated_mode;
    out->payload_kind = cp->table.slots[i].payload_kind;
    count++;
  }
  query->list_response.count = count;
}

static void handle_switch_query(pgdps_control_plane_t *cp,
                                pgdps_control_query_t *query) {
  int idx = pgdps_session_table_find_by_client_id(&cp->table, query->switch_client_id);
  if (idx < 0) {
    query->switch_response.ok = 0;
    strncpy(query->switch_response.reason, "app not connected",
            PGDPS_ADMIN_REASON_LEN - 1);
    query->switch_response.reason[PGDPS_ADMIN_REASON_LEN - 1] = '\0';
    return;
  }

  if (cp->table.slots[idx].state == PGDPS_SESSION_ACTIVE) {
    query->switch_response.ok = 1; // already active: idempotent no-op
    query->switch_response.reason[0] = '\0';
    return;
  }

  if (cp->table.slots[idx].state != PGDPS_SESSION_NEGOTIATED) {
    query->switch_response.ok = 0;
    strncpy(query->switch_response.reason, "app not in a switchable state",
            PGDPS_ADMIN_REASON_LEN - 1);
    query->switch_response.reason[PGDPS_ADMIN_REASON_LEN - 1] = '\0';
    return;
  }

  activate(cp, idx);
  query->switch_response.ok = 1;
  query->switch_response.reason[0] = '\0';
}

static void drain_admin_query(pgdps_control_plane_t *cp) {
  pgdps_control_query_t *query = pgdps_control_query_channel_drain(cp->chan);

  if (!query)
    return; // spurious wakeup, defensively handled per control_query.h's contract

  if (query->type == PGDPS_CTRL_QUERY_LIST)
    handle_list_query(cp, query);
  else // PGDPS_CTRL_QUERY_SWITCH
    handle_switch_query(cp, query);

  pgdps_control_query_complete(query);
}

static void handle_disconnect(pgdps_control_plane_t *cp, int idx) {
  pgdps_session_t *slot = &cp->table.slots[idx];

  if (slot->state == PGDPS_SESSION_ACTIVE) {
    pgdps_evict_client(cp->ring); // nothing to notify, fd is going away
    if (cp->dp)
      pgdps_data_plane_kick(cp->dp);
  }

  close(slot->ctrl_fd);
  pgdps_session_table_remove(&cp->table, idx);
}

static void handle_connect(pgdps_control_plane_t *cp, int idx,
                           const pgdp_connect_msg_t *connect) {
  pgdps_session_t *slot = &cp->table.slots[idx];

  pgdp_render_mode_t chosen;
  bool matched = negotiate_mode(cp, connect->modes, connect->num_modes, &chosen);

  pgdp_mode_msg_t reply = {0};
  reply.accepted = matched ? 1 : 0;
  if (matched)
    reply.chosen = chosen;
  pgdp_ctrl_send(slot->ctrl_fd, PGDP_MSG_MODE, &reply, sizeof(reply));

  if (!matched) {
    slot->state = PGDPS_SESSION_REJECTED;
    handle_disconnect(cp, idx); // REJECTED -> CLOSED
    return;
  }

  strncpy(slot->client_id, connect->client_id, PGDP_CLIENT_ID_LEN - 1);
  slot->client_id[PGDP_CLIENT_ID_LEN - 1] = '\0';
  slot->num_offered_modes = connect->num_modes;

  for (uint32_t i = 0; i < connect->num_modes; i++)
    slot->offered_modes[i] = connect->modes[i];

  slot->negotiated_mode = chosen;
  slot->state = PGDPS_SESSION_NEGOTIATED;
}

static void handle_activate_request(pgdps_control_plane_t *cp, int idx) {
  pgdps_session_t *slot = &cp->table.slots[idx];
  if (slot->state != PGDPS_SESSION_NEGOTIATED)
    return; // already ACTIVE, or CONNECT hasn't completed yet: defensive no-op

  if (cp->table.active_slot < 0 || cp->table.active_slot == idx) {
    activate(cp, idx);
    return;
  }

  pgdp_deny_msg_t deny = {0};
  strncpy(deny.reason, "another app is currently active", PGDP_DENY_REASON_LEN - 1);
  pgdp_ctrl_send(slot->ctrl_fd, PGDP_MSG_ACTIVATE_DENY, &deny, sizeof(deny));
}

static void handle_heartbeat(pgdps_control_plane_t *cp, int idx) {
  pgdps_session_t *slot = &cp->table.slots[idx];
  if (slot->state != PGDPS_SESSION_ACTIVE)
    return; // NEGOTIATED clients don't heartbeat.

  clock_gettime(CLOCK_MONOTONIC, &slot->last_heartbeat_monotonic);
}

static void handle_dmabuf_announce(pgdps_control_plane_t *cp, int idx,
                                   const pgdp_dmabuf_announce_msg_t *msg,
                                   const int *fds, int nfds) {
  pgdps_session_t *slot = &cp->table.slots[idx];
  pgdps_dmabuf_set_t set;

  if (pgdps_dmabuf_set_from_announce(&set, msg, fds, nfds) == 0)
    pgdps_dmabuf_set_close(&set); // well-formed, but v1 never actually adopts it

  pgdp_dmabuf_ack_msg_t ack = {0};
  ack.accepted = 0;
  strncpy(ack.reason, "GPU dmabuf import not implemented in this build",
          PGDP_DENY_REASON_LEN - 1);

  pgdp_ctrl_send(slot->ctrl_fd, PGDP_MSG_DMABUF_ACK, &ack, sizeof(ack));
}

static void handle_readable(pgdps_control_plane_t *cp, int idx) {
  pgdps_session_t *slot = &cp->table.slots[idx];
  unsigned char buf[PGDPS_CONTROL_RECV_BUF_SIZE];
  pgdp_msg_type_t type;
  uint32_t len;
  int fds[PGDP_NUM_BUFFERS];
  int nfds = 0;

  int rc = pgdp_ctrl_recv_fds(slot->ctrl_fd, &type, buf, sizeof(buf), &len, fds,
                              PGDP_NUM_BUFFERS, &nfds);
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
  case PGDP_MSG_CONNECT: {
    pgdp_connect_msg_t connect;
    memset(&connect, 0, sizeof(connect));

    if (len == sizeof(connect)) {
      memcpy(&connect, buf, sizeof(connect));
      connect.client_id[PGDP_CLIENT_ID_LEN - 1] = '\0';

      if (connect.num_modes > PGDP_MAX_MODES)
        connect.num_modes = 0; // malformed -> negotiate_mode guaranteed to reject
    }
    // else: len mismatch leaves connect.num_modes == 0 from the memset above,
    // which also guarantees negotiate_mode() rejects -- same malformed path.
    handle_connect(cp, idx, &connect);
    break;
  }
  case PGDP_MSG_ACTIVATE_REQUEST:
    handle_activate_request(cp, idx);
    break;
  case PGDP_MSG_HEARTBEAT:
    handle_heartbeat(cp, idx);
    break;
  case PGDP_MSG_DISCONNECT:
    handle_disconnect(cp, idx);
    break;
  case PGDP_MSG_DMABUF_ANNOUNCE: {
    pgdp_dmabuf_announce_msg_t msg;

    if (len != sizeof(pgdp_dmabuf_announce_msg_t)) {
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
