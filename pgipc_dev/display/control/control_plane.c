// control_plane.c - see control_plane.h.
#define LIBPGIPC_READER
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

// Generous fixed buffer for one control-protocol message. The largest non-fd-bearing
// payload today is pgipc_hello_msg_t (well under 1KiB); the only fd-bearing message
// (DMABUF_ANNOUNCE) is handled via pgipc_ctrl_recv_fds() into this same buffer.
#define PGIPC_CONTROL_RECV_BUF_SIZE 4096

// PGIPC_CONTROL_MAX_CLIENTS worth of producer fds, plus listen_fd, stop_read_fd, and
// chan's wake_fd.
#define PGIPC_CONTROL_MAX_POLLFDS (PGIPC_SESSION_MAX_CLIENTS + 3)

// How often the poll() loop wakes up even with nothing to do, so
// pgipc_session_table_check_heartbeat_timeouts() runs regularly instead of only when
// socket activity happens to occur.
#define PGIPC_CONTROL_POLL_TIMEOUT_MS 250

int pgipc_control_plane_init(pgipc_control_plane_t *cp, pgipc_shm_ring_t *ring,
                             pgipc_data_plane_t *dp,
                             pgipc_control_query_channel_t *chan,
                             const pgipc_render_mode_t *supported_modes,
                             uint32_t num_supported_modes) {
  if (num_supported_modes == 0) {
    fprintf(stderr, "[pgipc-control] num_supported_modes must be >= 1\n");
    return -1;
  }

  if (num_supported_modes > PGIPC_MAX_MODES)
    num_supported_modes = PGIPC_MAX_MODES;

  memset(cp, 0, sizeof(*cp));
  cp->ring = ring;
  cp->dp = dp;
  cp->chan = chan;
  cp->num_supported_modes = num_supported_modes;
  for (uint32_t i = 0; i < num_supported_modes; i++)
    cp->supported_modes[i] = supported_modes[i];

  pgipc_session_table_init(&cp->table);
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
  strncpy(addr.sun_path, PGIPC_CONTROL_SOCK_PATH, sizeof(addr.sun_path) - 1);

  unlink(PGIPC_CONTROL_SOCK_PATH); // stale socket from a previous run

  if (bind(cp->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    perror("bind (control plane)");
    close(cp->listen_fd);
    close(cp->stop_read_fd);
    close(cp->stop_write_fd);
    cp->listen_fd = -1;
    return -1;
  }

  if (listen(cp->listen_fd, /*backlog=*/PGIPC_SESSION_MAX_CLIENTS) != 0) {
    perror("listen (control plane)");
    close(cp->listen_fd);
    close(cp->stop_read_fd);
    close(cp->stop_write_fd);
    cp->listen_fd = -1;
    return -1;
  }

  return 0;
}

/** pgipc__control_negotiate_mode() - first-exact-match rule.
 * @cp:          control plane state
 * @offered:     producer's offered modes, in the producer's preference order
 * @num_offered: number of valid entries in @offered
 * @out_chosen:  filled in with the matched mode iff this returns true
 *
 * Return: true iff some offered mode exactly (width, height, fps all equal) matches
 * some entry in @cp->supported_modes.
 */
static bool pgipc__control_negotiate_mode(pgipc_control_plane_t *cp,
                                          const pgipc_render_mode_t *offered,
                                          uint32_t num_offered,
                                          pgipc_render_mode_t *out_chosen) {
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

/** pgipc__control_activate() - evict-then-grant to @idx
 * @cp:  control plane state
 * @idx: slot to grant; caller guarantees this slot is currently
 *       PGIPC_SESSION_NEGOTIATED and is not the current active_slot
 *
 * Unconditionally bumps the ring generation exactly once, then, if a different
 * client was previously ACTIVE, notifies it via PGIPC_MSG_DEACTIVATE before flipping
 * the table over to the new grant. Finally notifies the data plane if the negotiated
 * frame size changed.
 */
static void pgipc__control_activate(pgipc_control_plane_t *cp, int idx) {
  int prev_active = cp->table.active_slot;

  pgipc_render_mode_t prev_mode = {0};
  if (prev_active >= 0)
    prev_mode = cp->table.slots[prev_active].negotiated_mode;

  pgipc_evict_writer(cp->ring); // bumps generation

  if (prev_active >= 0 && prev_active != idx) {
    pgipc_ctrl_send(cp->table.slots[prev_active].ctrl_fd, PGIPC_MSG_DEACTIVATE, NULL,
                    0);
  }

  uint32_t generation = atomic_load(&cp->ring->generation);
  pgipc_session_table_activate(&cp->table, idx, generation);

  pgipc_grant_msg_t grant = {.generation = generation};
  pgipc_ctrl_send(cp->table.slots[idx].ctrl_fd, PGIPC_MSG_ACTIVATE_GRANT, &grant,
                  sizeof(grant));

  pgipc_render_mode_t new_mode = cp->table.slots[idx].negotiated_mode;
  if (cp->dp && (prev_active < 0 || new_mode.width != prev_mode.width ||
                 new_mode.height != prev_mode.height))
    pgipc_data_plane_set_mode(cp->dp, new_mode.width, new_mode.height);
}

/** pgipc__control_translate_state() - internal state -> wire state */
static pgipc_admin_client_state_t
pgipc__control_translate_state(pgipc_session_state_t state) {
  switch (state) {
  case PGIPC_SESSION_CONNECTED:
    return PGIPC_ADMIN_STATE_CONNECTED;
  case PGIPC_SESSION_NEGOTIATED:
    return PGIPC_ADMIN_STATE_NEGOTIATED;
  case PGIPC_SESSION_ACTIVE:
    return PGIPC_ADMIN_STATE_ACTIVE;
  case PGIPC_SESSION_REJECTED:
  default:
    return PGIPC_ADMIN_STATE_REJECTED;
  }
}

/** pgipc__control_handle_list_query() - fill a LIST query from the real table. */
static void pgipc__control_handle_list_query(pgipc_control_plane_t *cp,
                                             pgipc_control_query_t *query) {
  uint32_t count = 0;

  for (int i = 0; i < PGIPC_SESSION_MAX_CLIENTS && count < PGIPC_ADMIN_MAX_CLIENTS;
       i++) {
    if (!cp->table.slots[i].in_use)
      continue;

    pgipc_admin_client_info_t *out = &query->list_response.clients[count];
    strncpy(out->app_id, cp->table.slots[i].app_id, PGIPC_APP_ID_LEN - 1);
    out->app_id[PGIPC_APP_ID_LEN - 1] = '\0';
    out->state = pgipc__control_translate_state(cp->table.slots[i].state);
    out->negotiated_mode = cp->table.slots[i].negotiated_mode;
    out->payload_kind = cp->table.slots[i].payload_kind;
    count++;
  }
  query->list_response.count = count;
}

/** pgipc__control_handle_switch_query() - switch semantics. */
static void pgipc__control_handle_switch_query(pgipc_control_plane_t *cp,
                                               pgipc_control_query_t *query) {
  int idx = pgipc_session_table_find_by_app_id(&cp->table, query->switch_app_id);
  if (idx < 0) {
    query->switch_response.ok = 0;
    strncpy(query->switch_response.reason, "app not connected",
            PGIPC_ADMIN_REASON_LEN - 1);
    query->switch_response.reason[PGIPC_ADMIN_REASON_LEN - 1] = '\0';
    return;
  }

  if (cp->table.slots[idx].state == PGIPC_SESSION_ACTIVE) {
    query->switch_response.ok = 1; // already active: idempotent no-op
    query->switch_response.reason[0] = '\0';
    return;
  }

  if (cp->table.slots[idx].state != PGIPC_SESSION_NEGOTIATED) {
    query->switch_response.ok = 0;
    strncpy(query->switch_response.reason, "app not in a switchable state",
            PGIPC_ADMIN_REASON_LEN - 1);
    query->switch_response.reason[PGIPC_ADMIN_REASON_LEN - 1] = '\0';
    return;
  }

  pgipc__control_activate(cp, idx);
  query->switch_response.ok = 1;
  query->switch_response.reason[0] = '\0';
}

/** pgipc__control_drain_admin_query() - answer whatever the admin thread posted. */
static void pgipc__control_drain_admin_query(pgipc_control_plane_t *cp) {
  pgipc_control_query_t *query = pgipc_control_query_channel_drain(cp->chan);

  if (!query)
    return; // spurious wakeup, defensively handled per control_query.h's contract

  if (query->type == PGIPC_CTRL_QUERY_LIST)
    pgipc__control_handle_list_query(cp, query);
  else // PGIPC_CTRL_QUERY_SWITCH
    pgipc__control_handle_switch_query(cp, query);

  pgipc_control_query_complete(query);
}

/** pgipc__control_handle_disconnect() - close+free a slot.
 *
 * Shared by: an explicit PGIPC_MSG_DISCONNECT, a socket EOF/error, and a rejected
 * CONNECT (REJECTED -> CLOSED is immediate).
 */
static void pgipc__control_handle_disconnect(pgipc_control_plane_t *cp, int idx) {
  pgipc_session_t *slot = &cp->table.slots[idx];

  if (slot->state == PGIPC_SESSION_ACTIVE)
    pgipc_evict_writer(cp->ring); // nothing to notify, fd is going away

  close(slot->ctrl_fd);
  pgipc_session_table_remove(&cp->table, idx);
}

/** pgipc__control_handle_connect() - CONNECT -> mode negotiation. */
static void pgipc__control_handle_connect(pgipc_control_plane_t *cp, int idx,
                                          const pgipc_hello_msg_t *hello) {
  pgipc_session_t *slot = &cp->table.slots[idx];

  pgipc_render_mode_t chosen;
  bool matched =
      pgipc__control_negotiate_mode(cp, hello->modes, hello->num_modes, &chosen);

  pgipc_mode_msg_t reply = {0};
  reply.accepted = matched ? 1 : 0;
  if (matched)
    reply.chosen = chosen;
  pgipc_ctrl_send(slot->ctrl_fd, PGIPC_MSG_MODE, &reply, sizeof(reply));

  if (!matched) {
    slot->state = PGIPC_SESSION_REJECTED;
    pgipc__control_handle_disconnect(cp, idx); // REJECTED -> CLOSED
    return;
  }

  strncpy(slot->app_id, hello->app_id, PGIPC_APP_ID_LEN - 1);
  slot->app_id[PGIPC_APP_ID_LEN - 1] = '\0';
  slot->num_offered_modes = hello->num_modes;

  for (uint32_t i = 0; i < hello->num_modes; i++)
    slot->offered_modes[i] = hello->modes[i];

  slot->negotiated_mode = chosen;
  slot->state = PGIPC_SESSION_NEGOTIATED;
}

/** pgipc__control_handle_activate_request() - granting rules. */
static void pgipc__control_handle_activate_request(pgipc_control_plane_t *cp, int idx) {
  pgipc_session_t *slot = &cp->table.slots[idx];
  if (slot->state != PGIPC_SESSION_NEGOTIATED)
    return; // already ACTIVE, or CONNECT hasn't completed yet: defensive no-op

  if (cp->table.active_slot < 0 || cp->table.active_slot == idx) {
    pgipc__control_activate(cp, idx);
    return;
  }

  pgipc_deny_msg_t deny = {0};
  strncpy(deny.reason, "another app is currently active", PGIPC_DENY_REASON_LEN - 1);
  pgipc_ctrl_send(slot->ctrl_fd, PGIPC_MSG_ACTIVATE_DENY, &deny, sizeof(deny));
}

/** pgipc__control_handle_heartbeat() - refresh the ACTIVE client's liveness stamp. */
static void pgipc__control_handle_heartbeat(pgipc_control_plane_t *cp, int idx) {
  pgipc_session_t *slot = &cp->table.slots[idx];
  if (slot->state != PGIPC_SESSION_ACTIVE)
    return; // NEGOTIATED clients don't heartbeat.

  clock_gettime(CLOCK_MONOTONIC, &slot->last_heartbeat_monotonic);
}

/** pgipc__control_handle_dmabuf_announce() - always refuse in v1.
 *
 * Real KMS/DRM import is out of scope for this version; always responding (never
 * leaving the announcing producer waiting) lets it fall back to its own
 * glReadPixels-into-shm path per libpgipc.h's writer-side fallback note.
 */
static void
pgipc__control_handle_dmabuf_announce(pgipc_control_plane_t *cp, int idx,
                                      const pgipc_dmabuf_announce_msg_t *msg,
                                      const int *fds, int nfds) {
  pgipc_session_t *slot = &cp->table.slots[idx];
  pgipc_dmabuf_set_t set;

  if (pgipc_dmabuf_set_from_announce(&set, msg, fds, nfds) == 0)
    pgipc_dmabuf_set_close(&set); // well-formed, but v1 never actually adopts it

  pgipc_dmabuf_ack_msg_t ack = {0};
  ack.accepted = 0;
  strncpy(ack.reason, "GPU dmabuf import not implemented in this build",
          PGIPC_DENY_REASON_LEN - 1);

  pgipc_ctrl_send(slot->ctrl_fd, PGIPC_MSG_DMABUF_ACK, &ack, sizeof(ack));
}

/** pgipc__control_handle_readable() - one recv+dispatch cycle for slot @idx. */
static void pgipc__control_handle_readable(pgipc_control_plane_t *cp, int idx) {
  pgipc_session_t *slot = &cp->table.slots[idx];
  unsigned char buf[PGIPC_CONTROL_RECV_BUF_SIZE];
  pgipc_msg_type_t type;
  uint32_t len;
  int fds[PGIPC_NUM_BUFFERS];
  int nfds = 0;

  int rc = pgipc_ctrl_recv_fds(slot->ctrl_fd, &type, buf, sizeof(buf), &len, fds,
                               PGIPC_NUM_BUFFERS, &nfds);
  if (rc == -1) {
    pgipc__control_handle_disconnect(cp, idx);
    return;
  }
  if (rc == -2) {
    // Oversized single frame: log and drop it, keep the connection open.
    fprintf(stderr, "[pgipc-control] oversized message from slot %d, dropping\n", idx);
    return;
  }

  switch (type) {
  case PGIPC_MSG_CONNECT: {
    pgipc_hello_msg_t hello;
    memset(&hello, 0, sizeof(hello));

    if (len == sizeof(hello)) {
      memcpy(&hello, buf, sizeof(hello));
      hello.app_id[PGIPC_APP_ID_LEN - 1] = '\0';

      if (hello.num_modes > PGIPC_MAX_MODES)
        hello.num_modes = 0; // malformed -> negotiate_mode guaranteed to reject
    }
    // else: len mismatch leaves hello.num_modes == 0 from the memset above,
    // which also guarantees negotiate_mode() rejects -- same malformed path.
    pgipc__control_handle_connect(cp, idx, &hello);
    break;
  }
  case PGIPC_MSG_ACTIVATE_REQUEST:
    pgipc__control_handle_activate_request(cp, idx);
    break;
  case PGIPC_MSG_HEARTBEAT:
    pgipc__control_handle_heartbeat(cp, idx);
    break;
  case PGIPC_MSG_DISCONNECT:
    pgipc__control_handle_disconnect(cp, idx);
    break;
  case PGIPC_MSG_DMABUF_ANNOUNCE: {
    pgipc_dmabuf_announce_msg_t msg;

    if (len != sizeof(pgipc_dmabuf_announce_msg_t)) {
      fprintf(stderr, "[pgipc-control] malformed DMABUF_ANNOUNCE from slot %d\n", idx);
      break;
    }

    memcpy(&msg, buf, sizeof(msg));
    pgipc__control_handle_dmabuf_announce(cp, idx, &msg, fds, nfds);
    break;
  }
  default:
    fprintf(stderr, "[pgipc-control] unexpected message type %d from slot %d\n",
            (int)type, idx);
    break;
  }
}
void *pgipc_control_plane_run(void *arg) {
  pgipc_control_plane_t *cp = (pgipc_control_plane_t *)arg;

  while (atomic_load(&cp->running)) {
    struct pollfd pfds[PGIPC_CONTROL_MAX_POLLFDS];
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
    pfds[nfds].fd = pgipc_control_query_channel_wake_fd(cp->chan);
    pfds[nfds].events = POLLIN;
    nfds++;

    int slot_pos[PGIPC_SESSION_MAX_CLIENTS];
    for (int i = 0; i < PGIPC_SESSION_MAX_CLIENTS; i++) {
      slot_pos[i] = -1;
      if (cp->table.slots[i].in_use) {
        slot_pos[i] = nfds;
        pfds[nfds].fd = cp->table.slots[i].ctrl_fd;
        pfds[nfds].events = POLLIN;
        nfds++;
      }
    }

    int rc = poll(pfds, (nfds_t)nfds, PGIPC_CONTROL_POLL_TIMEOUT_MS);
    if (rc < 0) {
      if (errno == EINTR)
        continue;
      perror("poll (control plane)");
      break;
    }

    if (pfds[stop_pos].revents & POLLIN)
      break; // pgipc_control_plane_stop() was called

    if (pfds[chan_pos].revents & POLLIN)
      pgipc__control_drain_admin_query(cp);

    if (pfds[listen_pos].revents & POLLIN) {
      int client_fd = accept(cp->listen_fd, NULL, NULL);

      if (client_fd < 0) {
        if (errno != EINTR)
          perror("accept (control plane)");
      } else if (pgipc_session_table_add(&cp->table, client_fd) < 0) {
        // Table full: accept then immediately close with no CONNECT reply.
        // resource limit, not a protocol message.
        close(client_fd);
      }
    }

    for (int i = 0; i < PGIPC_SESSION_MAX_CLIENTS; i++) {
      if (slot_pos[i] < 0)
        continue;

      short revents = pfds[slot_pos[i]].revents;
      if (revents & POLLIN)
        pgipc__control_handle_readable(cp, i);
      else if (revents & (POLLHUP | POLLERR))
        pgipc__control_handle_disconnect(cp, i);
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    int timed_out = pgipc_session_table_check_heartbeat_timeouts(&cp->table, now);
    if (timed_out >= 0) {
      // pgipc_session_table_check_heartbeat_timeouts() already moved the slot
      // ACTIVE -> NEGOTIATED internally; the ring-generation bump and wire
      // notification are this layer's responsibility.
      pgipc_evict_writer(cp->ring);
      pgipc_ctrl_send(cp->table.slots[timed_out].ctrl_fd, PGIPC_MSG_DEACTIVATE, NULL,
                      0);
    }
  }

  return NULL;
}

void pgipc_control_plane_stop(pgipc_control_plane_t *cp) {
  unsigned char byte = 1;
  ssize_t n;

  atomic_store(&cp->running, false);
  do {
    n = write(cp->stop_write_fd, &byte, 1);
  } while (n < 0 && errno == EINTR);
}

void pgipc_control_plane_close(pgipc_control_plane_t *cp) {
  for (int i = 0; i < PGIPC_SESSION_MAX_CLIENTS; i++) {
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
  unlink(PGIPC_CONTROL_SOCK_PATH);
}
