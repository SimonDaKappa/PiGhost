#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "client_internal.h"
#include "wispc/client.h"

/**
 * wispc_handle_grant() - shared grant-processing path used by both the synchronous
 * connect()-time handshake and the async ctrl thread.
 * @ctx:       session handle
 * @grant:     grant for session
 * @frame_fd:  frame-ready eventfd received via SCM_RIGHTS alongside @grant, or -1 if
 *             none arrived (treated as a malformed grant)
 *
 * Attaches the shm ring and adopts @frame_fd BEFORE flipping @ctx->active so no caller
 * can observe active==true with a NULL ring / invalid frame_fd (see note on
 * wispc_is_active()). Closes any previously-held frame_fd first, since a
 * re-grant (e.g. after being deactivated and reactivated) carries a fresh fd.
 *
 * Returns 0 on success, -1 if shm attachment failed or @frame_fd < 0 (caller should
 * treat the session as unusable).
 */
static int wispc_handle_grant(wispc_ctx_t *ctx, const wisp_grant_msg_t *grant,
                              int frame_fd) {
  if (!ctx->ring)
    ctx->ring = wispc_shm_attach(50, 100);

  if (!ctx->ring || frame_fd < 0) {
    if (frame_fd >= 0)
      close(frame_fd);
    return -1;
  }

  if (ctx->frame_fd >= 0)
    close(ctx->frame_fd);
  ctx->frame_fd = frame_fd;

  wisp_atomic_store(&ctx->granted_generation, grant->generation);
  wisp_atomic_store(&ctx->active, true);

  return 0;
}

int wispc_write_slot(wispc_ctx_t *ctx) { return wisp_shm_ring_write_slot(ctx->ring); }

/**
 * wispc_control - background ctrl thread for clients.
 * @arg: client session handle
 *
 * While client is running, send heartbeat periodically.
 * While client is active,
 */
static void *wispc_control(void *arg) {
  wispc_ctx_t *ctx = (wispc_ctx_t *)arg;
  struct timespec last_heartbeat = {0};
  struct timespec last_activate_attempt = {0};
  struct timeval tv = {.tv_sec = 0, .tv_usec = WISP_CLIENT_HEARTBEAT_MS * 1000};

  setsockopt(ctx->ctrl_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  while (wisp_atomic_load(&ctx->running)) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    long since_hb_ms = (now.tv_sec - last_heartbeat.tv_sec) * 1000 +
                       (now.tv_nsec - last_heartbeat.tv_nsec) / 1000000;

    if (wisp_atomic_load(&ctx->active) && since_hb_ms >= WISP_CLIENT_HEARTBEAT_MS) {
      wisp_heartbeat_msg_t hb = {
          .generation = wisp_atomic_load(&ctx->granted_generation),
          .frame_counter = ctx->ring ? wisp_atomic_load(&ctx->ring->frame_counter) : 0,
      };

      pthread_mutex_lock(&ctx->send_lock);
      wisp_ctrl_send(ctx->ctrl_fd, WISP_MSG_HEARTBEAT, &hb, sizeof(hb));
      pthread_mutex_unlock(&ctx->send_lock);
      last_heartbeat = now;
    }

    long since_req_ms = (now.tv_sec - last_activate_attempt.tv_sec) * 1000 +
                        (now.tv_nsec - last_activate_attempt.tv_nsec) / 1000000;

    if (!wisp_atomic_load(&ctx->active) &&
        since_req_ms >= WISP_CLIENT_RETRY_ACTIVATE_MS) {
      pthread_mutex_lock(&ctx->send_lock);
      wisp_ctrl_send(ctx->ctrl_fd, WISP_MSG_ACTIVATE_REQUEST, NULL, 0);
      pthread_mutex_unlock(&ctx->send_lock);
      last_activate_attempt = now;
    }

    wisp_msg_kind_t kind;
    unsigned char buf[256];
    uint32_t len;
    int fds[1];
    int nfds = 0;

    int rc =
        wisp_ctrl_recv_fds(ctx->ctrl_fd, &kind, buf, sizeof(buf), &len, fds, 1, &nfds);
    if (rc == -1)
      continue; /* timeout or disconnect */
    if (rc == -2)
      continue; /* oversized message */

    switch (kind) {
    case WISP_MSG_ACTIVATE_GRANT: {
      wisp_grant_msg_t grant;

      memcpy(&grant, buf, sizeof(grant));
      /* Shared helper attaches the shm ring and adopts the received frame-ready
       * eventfd BEFORE setting active=true, so a concurrent wispc_is_active()
       * can never observe active with a NULL ring / invalid frame_fd. */
      wispc_handle_grant(ctx, &grant, nfds > 0 ? fds[0] : -1);
      break;
    }
    case WISP_MSG_ACTIVATE_DENY: {
      wisp_deny_msg_t deny;

      memcpy(&deny, buf, sizeof(deny));
      break;
    }
    case WISP_MSG_DEACTIVATE:
      wisp_atomic_store(&ctx->active, false);
      break;
    case WISP_MSG_DMABUF_ACK: {
      wisp_dmabuf_ack_msg_t ack;

      memcpy(&ack, buf, sizeof(ack) < len ? sizeof(ack) : len);

      if (ack.accepted) {
        wisp_atomic_store(&ctx->payload_kind, WISP_PAYLOAD_DMABUF);
        wisp_atomic_store(&ctx->dmabuf_ack_state, 1);
      } else {
        ack.reason[WISP_DENY_REASON_LEN - 1] = '\0';
        wisp_atomic_store(&ctx->dmabuf_ack_state, -1);
      }
      break;
    }
    default:
      break;
    }
  }
  return NULL;
}

wispc_ctx_t *wispc_connect(const char *client_id, const wisp_render_mode_t *modes,
                           int num_modes) {
  wispc_ctx_t *ctx = (wispc_ctx_t *)calloc(1, sizeof(wispc_ctx_t));
  if (!ctx)
    return NULL;

  strncpy(ctx->client_id, client_id, WISP_CLIENT_ID_LEN - 1);
  pthread_mutex_init(&ctx->send_lock, NULL);
  wisp_atomic_store(&ctx->running, true);
  wisp_atomic_store(&ctx->active, false);
  wisp_atomic_store(&ctx->payload_kind, WISP_PAYLOAD_PIXELS);
  wisp_atomic_store(&ctx->dmabuf_ack_state, 0);
  ctx->frame_fd = -1;

  int fd = -1;
  for (int i = 0; i < 100; i++) {
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
      break;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, WISPS_CONTROL_SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
      break;

    close(fd);
    fd = -1;

    struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000L};
    nanosleep(&ts, NULL);
  }

  if (fd < 0) {
    free(ctx);
    return NULL;
  }
  ctx->ctrl_fd = fd;

  wisp_connect_msg_t connect;
  memset(&connect, 0, sizeof(connect));
  strncpy(connect.client_id, ctx->client_id, WISP_CLIENT_ID_LEN - 1);
  connect.num_modes =
      (uint32_t)(num_modes > WISP_MAX_MODES ? WISP_MAX_MODES : num_modes);
  for (uint32_t i = 0; i < connect.num_modes; i++)
    connect.modes[i] = modes[i];
  wisp_ctrl_send(ctx->ctrl_fd, WISP_MSG_CONNECT, &connect, sizeof(connect));

  wisp_msg_kind_t kind;
  unsigned char buf[256];
  uint32_t len;

  if (wisp_ctrl_recv(ctx->ctrl_fd, &kind, buf, sizeof(buf), &len) == 0 &&
      kind == WISP_MSG_MODE) {
    wisp_mode_msg_t mode;

    memcpy(&mode, buf, sizeof(mode));
    if (!mode.accepted) {
      close(fd);
      pthread_mutex_destroy(&ctx->send_lock);
      free(ctx);
      return NULL;
    }

    ctx->mode = mode.chosen;
  }

  kind = (wisp_msg_kind_t)-1;
  len = (uint32_t)-1;
  memset(buf, 0, sizeof(buf));

  /* --- Initial activation handshake (synchronous) ----------------------- */
  /* Send the first request here so the ctrl thread starts in a known state. */
  wisp_ctrl_send(ctx->ctrl_fd, WISP_MSG_ACTIVATE_REQUEST, NULL, 0);

  int grant_fds[1];
  int grant_nfds = 0;

  if (wisp_ctrl_recv_fds(ctx->ctrl_fd, &kind, buf, sizeof(buf), &len, grant_fds, 1,
                         &grant_nfds) == 0) {
    if (kind == WISP_MSG_ACTIVATE_GRANT) {
      wisp_grant_msg_t grant;

      memcpy(&grant, buf, sizeof(grant));

      /* Same helper the ctrl thread uses for later grants -- keeps the
       * attach-before-active ordering in exactly one place. */
      if (wispc_handle_grant(ctx, &grant, grant_nfds > 0 ? grant_fds[0] : -1) != 0) {
        close(fd);
        pthread_mutex_destroy(&ctx->send_lock);
        free(ctx);
        return NULL;
      }
    } else if (kind == WISP_MSG_ACTIVATE_DENY) {
      /* Resources stay NULL; ctrl thread will retry and attach on grant. */
      wisp_deny_msg_t deny;
      memcpy(&deny, buf, sizeof(deny));
    }
    /* Any other message (e.g. unexpected MSG_MODE) is ignored; the ctrl
     * thread will recover via the periodic retry logic. */
  }

  pthread_create(&ctx->ctrl_thread, NULL, wispc_control, ctx);
  return ctx;
}

bool wispc_is_active(wispc_ctx_t *ctx) { return wisp_atomic_load(&ctx->active); }

wisp_render_mode_t wispc_negotiated_mode(wispc_ctx_t *ctx) { return ctx->mode; }

wisp_payload_kind_t wispc_payload_kind(wispc_ctx_t *ctx) {
  return (wisp_payload_kind_t)wisp_atomic_load(&ctx->payload_kind);
}

int wispc_announce_dmabufs(wispc_ctx_t *ctx, const int fds[WISP_NUM_BUFFERS],
                           const wisp_dmabuf_desc_t desc[WISP_NUM_BUFFERS],
                           int timeout_ms) {
  wisp_dmabuf_announce_msg_t msg;
  memset(&msg, 0, sizeof(msg));

  msg.nbufs = WISP_NUM_BUFFERS;
  for (int i = 0; i < WISP_NUM_BUFFERS; i++)
    msg.desc[i] = desc[i];

  wisp_atomic_store(&ctx->dmabuf_ack_state, 0);

  pthread_mutex_lock(&ctx->send_lock);
  int rc = wisp_ctrl_send_fds(ctx->ctrl_fd, WISP_MSG_DMABUF_ANNOUNCE, &msg, sizeof(msg),
                              fds, WISP_NUM_BUFFERS);
  pthread_mutex_unlock(&ctx->send_lock);
  if (rc != 0)
    return -1;

  /* The ctrl thread owns the socket's read side, so wait for it to flip the
   * ack flag rather than reading here ourselves. Polling at 5ms granularity
   * is plenty for a one-time setup handshake. */
  struct timespec poll_ts = {.tv_sec = 0, .tv_nsec = 5 * 1000000L};
  long waited_ms = 0;
  for (;;) {
    int st = wisp_atomic_load(&ctx->dmabuf_ack_state);

    if (st == 1)
      return 0;
    if (st == -1)
      return -2;
    if (timeout_ms >= 0 && waited_ms >= timeout_ms)
      return -1;
    nanosleep(&poll_ts, NULL);
    waited_ms += 5;
  }
}

void wispc_publish(wispc_ctx_t *ctx, int idx, uint64_t frame_id) {
  if (!wisp_atomic_load(&ctx->active))
    return;
  if (wisp_atomic_load(&ctx->ring->generation) !=
      wisp_atomic_load(&ctx->granted_generation)) {
    wisp_atomic_store(&ctx->active, false);
    return;
  }

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  uint64_t now_ns = ((uint64_t)now.tv_sec * 1000000000ULL) + now.tv_nsec;

  wispc_shm_publish(ctx->ring, idx, frame_id, now_ns);
  if (ctx->frame_fd >= 0) {
    uint64_t v = 1;
    ssize_t n;
    do {
      n = write(ctx->frame_fd, &v, sizeof(v));
    } while (n < 0 && errno == EINTR);
  }
}

void wispc_disconnect(wispc_ctx_t *ctx) {
  wisp_atomic_store(&ctx->running, false);

  pthread_join(ctx->ctrl_thread, NULL);
  wisp_ctrl_send(ctx->ctrl_fd, WISP_MSG_DISCONNECT, NULL, 0);
  close(ctx->ctrl_fd);

  if (ctx->ring)
    munmap(ctx->ring, sizeof(wisp_shm_ring_t));
  if (ctx->frame_fd >= 0)
    close(ctx->frame_fd);

  pthread_mutex_destroy(&ctx->send_lock);
  free(ctx);
}