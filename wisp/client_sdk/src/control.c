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
#include <wispc/client.h>

static int wispc_ctrl_send(wispc_ctx_t *ctx, wisp_msg_kind_t kind, const void *payload,
                           uint32_t len) {
  pthread_mutex_lock(&ctx->send_lock);
  int rc = wisp_ctrl_send(ctx->ctrl_fd, kind, payload, len);
  pthread_mutex_unlock(&ctx->send_lock);
  return rc;
}

static int wispc_ctrl_send_fds(wispc_ctx_t *ctx, wisp_msg_kind_t kind,
                               const void *payload, uint32_t len, const int *fds,
                               int nfds) {
  pthread_mutex_lock(&ctx->send_lock);
  int rc = wisp_ctrl_send_fds(ctx->ctrl_fd, kind, payload, len, fds, nfds);
  pthread_mutex_unlock(&ctx->send_lock);
  return rc;
}

/**
 * wispc_apply_callback_defaults() - fill unset wispc_event_cfg_t fields
 * @ctx:       session handle whose ctx->user_cbs is being populated
 * @callbacks: app-provided callbacks, or NULL to use all defaults
 *
 * Every NOTIFIER field left NULL by the caller stays NULL (a no-op call site just
 * skips invoking it); only ::warmup gets a non-NULL default, since it drives the
 * state machine (see wispc_event_cfg_t's doc comment).
 */
static void wispc_apply_callback_defaults(wispc_ctx_t *ctx,
                                          const wispc_event_cfg_t *callbacks) {
  if (callbacks)
    ctx->events = *callbacks;
  else
    memset(&ctx->events, 0, sizeof(ctx->events));
}

/**
 * wispc_self_promote() - the client's own scheduling promotion syscall(s)
 * @ctx:           session handle
 * @priority_hint: server-suggested priority, informs the syscall parameters
 *
 * TODO(rt-state-machine): no real sched_setattr()/sched_setscheduler() call is made
 * yet -- there is no RT scheduling backend wired up in this build (mirrors the
 * server's own force-demote stub in control_plane.c's grace-period deadline path).
 * This always succeeds so the state machine can still exercise the full
 * WARMUP -> PROMOTING -> READY_FOR_RT_SENT -> ACTIVE_RT chain end to end.
 *
 * Returns 0 on success, or an errno-style value on failure.
 */
static int wispc_self_promote(wispc_ctx_t *ctx, uint32_t priority_hint) {
  (void)ctx;
  (void)priority_hint;
  return 0;
}

/**
 * wispc_self_demote() - undo wispc_self_promote(), returning threads to SCHED_OTHER
 * @ctx: session handle
 *
 * TODO(rt-state-machine): no real sched_setattr(SCHED_OTHER)/cpuset-migration call is
 * made yet, mirroring wispc_self_promote()'s stub status -- always succeeds. Unlike
 * promotion, demotion has no meaningful failure path worth modeling yet: the server
 * force-reclaims the cpuset regardless once STOPPED (or the grace timeout) fires, so a
 * client that somehow can't self-demote is no worse off than a force-evicted one.
 */
static void wispc_self_demote(wispc_ctx_t *ctx) { (void)ctx; }

/**
 * Fallback drain deadline (ms) used by wispc_run_eviction_windown() when @grace_ms is
 * 0 -- i.e. a client-initiated voluntary stop, where there is no server-offered
 * window to honor but wispc still must not wait on ::render_frame forever.
 */
#define WISPC_RENDER_DRAIN_FALLBACK_MS 1000u

/**
 * wispc_run_eviction_windown() - EVICT_PENDING -> DEMOTING -> STOPPED_ACK_SENT ->
 * EVICTED_IDLE
 * @ctx:      session handle
 * @grace_ms: server-offered cooperative wind-down window, or 0 for a
 *            client-initiated voluntary stop (no grace period to honor -- wispc is
 *            already draining by choice, not under server pressure)
 *
 * Runs entirely on the control thread. Asks the render thread to stop (if it hasn't
 * already stopped itself -- see wispc_render_thread()) and waits for it to actually
 * exit, bounded by @grace_ms (or ::WISPC_RENDER_DRAIN_FALLBACK_MS for a
 * client-initiated stop, where @grace_ms is 0 and there is no server-offered window to
 * honor). If ::render_frame returns within the deadline, wispc joins the thread
 * normally and proceeds -- the common case. If it does not, wispc gives up waiting,
 * detaches the still-running thread (never force-kills it -- see
 * ::wispc_event_cfg_t::on_render_timeout's doc for why), fires ::on_render_timeout,
 * and proceeds through DEMOTING/STOPPED anyway; the app is misbehaving (blocking
 * render_frame past its own wind-down window) and wispc must not let one bad tick hang
 * the whole eviction sequence.
 */
static void wispc_run_eviction_windown(wispc_ctx_t *ctx, uint32_t grace_ms) {
  wispc_renderer_stop(&ctx->renderer, WISPC_RENDER_STOP_EVICTED, 0);

  ctx->state = WISPC_STATE_EVICT_PENDING;
  if (ctx->events.on_evict_pending)
    ctx->events.on_evict_pending(ctx->events.user, grace_ms);

  uint32_t deadline_ms = grace_ms > 0 ? grace_ms : WISPC_RENDER_DRAIN_FALLBACK_MS;
  uint32_t waited_ms = 0;
  struct timespec poll_ts = {.tv_sec = 0, .tv_nsec = 5000000L};
  while (!wisp_atomic_load(&ctx->renderer.exited) && waited_ms < deadline_ms) {
    nanosleep(&poll_ts, NULL);
    waited_ms += 5;
  }

  if (wisp_atomic_load(&ctx->renderer.exited)) {
    pthread_join(ctx->renderer.thread, NULL);
  } else {
    pthread_detach(ctx->renderer.thread);
    if (ctx->events.on_evict_timeout)
      ctx->events.on_evict_timeout(ctx->events.user);
  }

  ctx->state = WISPC_STATE_DEMOTING;
  wispc_self_demote(ctx);

  ctx->state = WISPC_STATE_STOPPED_ACK_SENT;
  wispc_ctrl_send(ctx, WISP_MSG_STOPPED, NULL, 0);

  ctx->state = WISPC_STATE_EVICTED_IDLE;
  if (ctx->events.on_evicted)
    ctx->events.on_evicted(ctx->events.user);
}

/**
 * wispc_run_warmup_and_promote() - WARMUP -> PROMOTING -> READY_FOR_RT_SENT ->
 * ACTIVE_RT
 * @ctx:   session handle, already holding the RT slot (WISP_MSG_ACTIVATE_GRANT
 *         processed, shm ring attached, frame_fd adopted)
 * @grant: the grant just processed; carries the scheduling hints ::warmup and
 *         wispc_self_promote() both consume
 *
 * Runs entirely on the control thread. Drives every state in the chain, invoking
 * ::warmup (GATED/REQUIRED) and firing ::on_warmup_failed/::on_promotion_failed/
 * ::on_rt_active as appropriate. On any failure, sends WISP_MSG_GRANT_DECLINE and
 * leaves @ctx inactive/NEGOTIATED rather than ACTIVE_RT -- the caller must not
 * assume the grant "took" just because wispc_handle_grant() succeeded.
 */
static void wispc_run_warmup_and_promote(wispc_ctx_t *ctx,
                                         const wisp_grant_msg_t *grant) {
  ctx->state = WISPC_STATE_WARMUP;

  if (ctx->renderer.clock.kind == WISPC_CLOCK_KIND_PACED && grant->period_ns == 0) {
    /* Nothing to pace against -- period_ns negotiation isn't wired up server-side
     * yet (see the TODO in control_plane.c's do_grant()), so fail loud rather than
     * silently behaving like TICKLESS. */
    wisp_atomic_store(&ctx->active, false);
    ctx->state = WISPC_STATE_EVICTED_IDLE;

    wisp_grant_decline_msg_t decline = {.reason = WISP_GRANT_DECLINE_INVALID_PACING};
    wispc_ctrl_send(ctx, WISP_MSG_GRANT_DECLINE, &decline, sizeof(decline));
    return;
  }

  ctx->state = WISPC_STATE_PROMOTING;
  int err = wispc_self_promote(ctx, grant->priority_hint);
  if (err != 0) {
    wisp_atomic_store(&ctx->active, false);
    ctx->state = WISPC_STATE_EVICTED_IDLE;
    if (ctx->events.on_promotion_failed)
      ctx->events.on_promotion_failed(ctx->events.user, err);

    wisp_grant_decline_msg_t decline = {.reason = WISP_GRANT_DECLINE_PROMOTION_FAILED};
    wispc_ctrl_send(ctx, WISP_MSG_GRANT_DECLINE, &decline, sizeof(decline));
    return;
  }

  int32_t warmup_err;
  bool warmup_ok = ctx->events.warmup(ctx->events.user, &warmup_err);
  if (!warmup_ok) {
    wisp_atomic_store(&ctx->active, false);
    ctx->state = WISPC_STATE_EVICTED_IDLE;
    if (ctx->events.on_warmup_failed)
      ctx->events.on_warmup_failed(ctx->events.user, warmup_err);

    wisp_grant_decline_msg_t decline = {.reason = WISP_GRANT_DECLINE_WARMUP_FAILED};
    wispc_ctrl_send(ctx, WISP_MSG_GRANT_DECLINE, &decline, sizeof(decline));
    return;
  }

  ctx->state = WISPC_STATE_READY_FOR_RT_SENT;
  wispc_ctrl_send(ctx, WISP_MSG_READY_FOR_RT, NULL, 0);

  ctx->state = WISPC_STATE_ACTIVE_RT;
  if (ctx->events.on_rt_active)
    ctx->events.on_rt_active(ctx->events.user);

  // $$$SIMON Need to figure out ordering between warmup/promotion/render init(-> clock
  // init)/render start
  ctx->rt_period_ns = grant->period_ns;
  ctx->clocked_schedule_initialized = false;
  pthread_mutex_lock(&ctx->tick_lock);
  ctx->tick_pending = 0;
  pthread_mutex_unlock(&ctx->tick_lock);

  wisp_atomic_store(&ctx->render_should_stop, false);
  wisp_atomic_store(&ctx->render_stop_reason, WISPC_RENDER_STOP_REASON_NONE);
  wisp_atomic_store(&ctx->render_thread_exited, false);
  pthread_create(&ctx->renderer.thread, NULL, wispc_render_thread, ctx);
}

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
 * Fires ::on_activation_granted, then runs the full WARMUP -> ... -> ACTIVE_RT chain
 * (see wispc_run_warmup_and_promote()) before returning -- @ctx->active may end up
 * false again by the time this returns if warmup/promotion declined the grant.
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

  if (ctx->events.on_activation_granted)
    ctx->events.on_activation_granted(ctx->events.user, grant->target_core,
                                        grant->priority_hint, grant->period_ns,
                                        grant->runtime_budget _ns);

  wispc_run_warmup_and_promote(ctx, grant);

  return 0;
}

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
  struct timeval tv = {.tv_sec = 0, .tv_usec = WISP_HEARTBEAT_MS * 1000};

  setsockopt(ctx->ctrl_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  while (wisp_atomic_load(&ctx->running)) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    long since_hb_ms = (now.tv_sec - last_heartbeat.tv_sec) * 1000 +
                       (now.tv_nsec - last_heartbeat.tv_nsec) / 1000000;

    if (wisp_atomic_load(&ctx->active) && since_hb_ms >= WISP_HEARTBEAT_MS) {
      wisp_heartbeat_msg_t hb = {
          .generation = wisp_atomic_load(&ctx->granted_generation),
          .frame_counter = ctx->ring ? wisp_atomic_load(&ctx->ring->frame_counter) : 0,
      };

      wispc_ctrl_send(ctx, WISP_MSG_HEARTBEAT, &hb, sizeof(hb));
      last_heartbeat = now;
    }

    long since_req_ms = (now.tv_sec - last_activate_attempt.tv_sec) * 1000 +
                        (now.tv_nsec - last_activate_attempt.tv_nsec) / 1000000;

    if (!wisp_atomic_load(&ctx->active) &&
        since_req_ms >= WISP_CLIENT_RETRY_ACTIVATE_MS) {
      wispc_ctrl_send(ctx, WISP_MSG_ACTIVATE_REQUEST, NULL, 0);
      last_activate_attempt = now;
    }

    /* render_frame() decided to stop on its own (WISPC_RENDER_STOP/ERROR) --
     * synthesize the same eviction wind-down path a server-initiated
     * EVICT_PENDING uses, with grace_ms=0 since there is no server grace
     * period to honor for a client-initiated stop. */
    if (ctx->state == WISPC_STATE_ACTIVE_RT &&
        wisp_atomic_load(&ctx->render_stop_reason) != WISPC_RENDER_STOP_REASON_NONE) {
      wispc_run_eviction_windown(ctx, 0);
      continue;
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
    case WISP_MSG_ACTIVATE_QUEUED: {
      wisp_queued_msg_t queued;

      memcpy(&queued, buf, sizeof(queued) < len ? sizeof(queued) : len);
      ctx->state = WISPC_STATE_QUEUED;
      if (ctx->events.on_activation_queued)
        ctx->events.on_activation_queued(ctx->events.user, queued.queue_position);
      break;
    }
    case WISP_MSG_ACTIVATE_DENY: {
      wisp_deny_msg_t deny;

      memcpy(&deny, buf, sizeof(deny));
      deny.reason[WISP_DENY_REASON_LEN - 1] = '\0';
      ctx->state = WISPC_STATE_EVICTED_IDLE;
      if (ctx->events.on_activation_denied)
        ctx->events.on_activation_denied(ctx->events.user, deny.reason);
      break;
    }
    case WISP_MSG_DEACTIVATE:
      wisp_atomic_store(&ctx->active, false);
      break;
    case WISP_MSG_EVICT_PENDING: {
      wisp_evict_pending_msg_t evict;

      memcpy(&evict, buf, sizeof(evict) < len ? sizeof(evict) : len);
      wispc_run_eviction_windown(ctx, evict.grace_ms);
      break;
    }
    case WISP_MSG_DMABUF_ACK: {
      wisp_dmabuf_ack_msg_t ack;

      memcpy(&ack, buf, sizeof(ack) < len ? sizeof(ack) : len);

      if (ack.accepted) {
        wisp_atomic_store(&ctx->render_kind, WISP_PAYLOAD_DMABUF);
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

wispc_ctx_t *wispc_connect(const char *client_id, const wisp_resolution_t *modes,
                           int num_modes, const wispc_event_cfg_t *callbacks) {
  if (!callbacks || !callbacks->render_frame)
    return NULL;

  wispc_ctx_t *ctx = (wispc_ctx_t *)calloc(1, sizeof(wispc_ctx_t));
  if (!ctx)
    return NULL;

  strncpy(ctx->client_id, client_id, WISP_CLIENT_ID_LEN - 1);
  wispc_apply_callback_defaults(ctx, callbacks);
  ctx->state = WISPC_STATE_CONNECTING;
  pthread_mutex_init(&ctx->send_lock, NULL);
  pthread_mutex_init(&ctx->tick_lock, NULL);
  pthread_cond_init(&ctx->tick_cond, NULL);
  wisp_atomic_store(&ctx->running, true);
  wisp_atomic_store(&ctx->active, false);
  wisp_atomic_store(&ctx->render_kind, WISP_PAYLOAD_PIXELS);
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
    strncpy(addr.sun_path, WISP_CONTROL_SOCK_PATH, sizeof(addr.sun_path) - 1);

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
  connect.protocol_magic = WISP_PROTOCOL_MAGIC;
  connect.protocol_major = WISP_PROTOCOL_VERSION_MAJOR;
  connect.protocol_minor = WISP_PROTOCOL_VERSION_MINOR;
  strncpy(connect.client_id, ctx->client_id, WISP_CLIENT_ID_LEN - 1);
  connect.num_modes =
      (uint32_t)(num_modes > WISP_MAX_MODES ? WISP_MAX_MODES : num_modes);
  for (uint32_t i = 0; i < connect.num_modes; i++)
    connect.modes[i] = modes[i];
  wispc_ctrl_send(ctx, WISP_MSG_CONNECT, &connect, sizeof(connect));

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
      pthread_mutex_destroy(&ctx->tick_lock);
      pthread_cond_destroy(&ctx->tick_cond);
      free(ctx);
      return NULL;
    }

    ctx->mode = mode.chosen;
    ctx->state = WISPC_STATE_NEGOTIATED;
    if (ctx->events.on_mode_negotiated)
      ctx->events.on_mode_negotiated(ctx->events.user, ctx->mode);
  } else if (kind == WISP_MSG_VERSION_MISMATCH) {
    /* Server speaks an incompatible protocol magic/major; rebuild against the
     * matching wisp_protocol release. No mode negotiation was attempted. */
    close(fd);
    pthread_mutex_destroy(&ctx->send_lock);
    pthread_mutex_destroy(&ctx->tick_lock);
    pthread_cond_destroy(&ctx->tick_cond);
    free(ctx);
    return NULL;
  }

  kind = (wisp_msg_kind_t)-1;
  len = (uint32_t)-1;
  memset(buf, 0, sizeof(buf));

  /* --- Initial activation handshake (synchronous) ----------------------- */
  /* Send the first request here so the ctrl thread starts in a known state. */
  ctx->state = WISPC_STATE_ACTIVATE_REQUESTED;
  wispc_ctrl_send(ctx, WISP_MSG_ACTIVATE_REQUEST, NULL, 0);

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
        pthread_mutex_destroy(&ctx->tick_lock);
        pthread_cond_destroy(&ctx->tick_cond);
        free(ctx);
        return NULL;
      }
    } else if (kind == WISP_MSG_ACTIVATE_QUEUED) {
      wisp_queued_msg_t queued;

      memcpy(&queued, buf, sizeof(queued) < len ? sizeof(queued) : len);
      ctx->state = WISPC_STATE_QUEUED;
      if (ctx->events.on_activation_queued)
        ctx->events.on_activation_queued(ctx->events.user, queued.queue_position);
    } else if (kind == WISP_MSG_ACTIVATE_DENY) {
      /* Resources stay NULL; ctrl thread will retry and attach on grant. */
      wisp_deny_msg_t deny;
      memcpy(&deny, buf, sizeof(deny));
      deny.reason[WISP_DENY_REASON_LEN - 1] = '\0';
      ctx->state = WISPC_STATE_EVICTED_IDLE;
      if (ctx->events.on_activation_denied)
        ctx->events.on_activation_denied(ctx->events.user, deny.reason);
    }
    /* Any other message (e.g. unexpected MSG_MODE) is ignored; the ctrl
     * thread will recover via the periodic retry logic. */
  }

  pthread_create(&ctx->ctrl_thread, NULL, wispc_control, ctx);
  return ctx;
}

bool wispc_is_active(wispc_ctx_t *ctx) { return wisp_atomic_load(&ctx->active); }

wisp_resolution_t wispc_resolution(wispc_ctx_t *ctx) { return ctx->mode; }

wisp_render_kind_t wispc_render_kind(wispc_ctx_t *ctx) {
  return (wisp_render_kind_t)wisp_atomic_load(&ctx->render_kind);
}

void wispc_notify_tick(wispc_ctx_t *ctx) {
  wispc_clock_tick(&ctx->renderer.clock);
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

  int rc = wisp_ctrl_send_fds(ctx, WISP_MSG_DMABUF_ANNOUNCE, &msg, sizeof(msg), fds,
                              WISP_NUM_BUFFERS);
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

void wispc_disconnect(wispc_ctx_t *ctx) {
  wisp_atomic_store(&ctx->running, false);

  pthread_join(ctx->ctrl_thread, NULL);
  wisp_ctrl_send(ctx, WISP_MSG_DISCONNECT, NULL, 0);
  close(ctx->ctrl_fd);

  if (ctx->ring)
    munmap(ctx->ring, sizeof(wisp_shm_ring_t));
  if (ctx->frame_fd >= 0)
    close(ctx->frame_fd);

  pthread_mutex_destroy(&ctx->send_lock);
  pthread_mutex_destroy(&ctx->tick_lock);
  pthread_cond_destroy(&ctx->tick_cond);
  free(ctx);
}