#include <client_internal.h>
#include <clock_internal.h>

#include <errno.h>

/* ------------------------------------------------------------------------- */
/* HEADLESS                                                                  */
/* ------------------------------------------------------------------------- */

static int clock_init_headless(wispc_clock_t *clock, wispc_clock_spec_t *spec) {
  wispc_clock_headless_t *headless = &clock->iface.headless;

  if (spec->kind != WISPC_CLOCK_KIND_HEADLESS)
    return -EINVAL;

  clock->kind = spec->kind;
  wisp_atomic_store(&headless->force_wake, false);
  return 0;
}

static void clock_reset_headless(wispc_clock_t *clock) {
  wispc_clock_headless_t *headless = &clock->iface.headless;
  wisp_atomic_store(&headless->force_wake, false);
}

static bool clock_wait_next_headless(wispc_clock_t *clock, uint32_t *missed_frames) {
  wispc_clock_headless_t *headless = &clock->iface.headless;
  *missed_frames = 0;

  /* No pacing and no blocking, so the only thing to do is honour the abort
   * latch. There is no lost-wakeup window here because we never sleep. */
  return !wisp_atomic_load(&headless->force_wake);
}

static void clock_force_wake_headless(wispc_clock_t *clock) {
  wispc_clock_headless_t *headless = &clock->iface.headless;
  wisp_atomic_store(&headless->force_wake, true);
}

static const wispc_clock_ops_t clock_ops_headless = {
    .init = clock_init_headless,
    .destroy = NULL,
    .reset = clock_reset_headless,
    .tick = NULL,
    .wait_next = clock_wait_next_headless,
    .frame_done = NULL,
    .force_wake = clock_force_wake_headless,
};

/* ------------------------------------------------------------------------- */
/* PACED                                                                     */
/* ------------------------------------------------------------------------- */

/* Returns 0 or a positive pthread error code. */
static int clock_init_paced(wispc_clock_t *clock, wispc_clock_spec_t *spec) {
  wispc_clock_paced_t *paced = &clock->iface.paced;
  pthread_condattr_t attr;
  int rc;

  if (spec->kind != WISPC_CLOCK_KIND_PACED)
    return -EINVAL;

  clock->kind = spec->kind;
  paced->rt_period_ns = spec->iface.paced.period_ns;
  paced->pace_active = false;
  paced->force_wake = false;

  rc = pthread_mutex_init(&paced->lock, NULL);
  if (rc != 0)
    return rc;

  rc = pthread_condattr_init(&attr);
  if (rc != 0) {
    pthread_mutex_destroy(&paced->lock);
    return rc;
  }

  /* Must match the clock we compute deadlines against, or a wall-clock
   * adjustment will slew every timedwait. */
  rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  if (rc == 0)
    rc = pthread_cond_init(&paced->wake_cond, &attr);
  pthread_condattr_destroy(&attr);

  if (rc != 0)
    pthread_mutex_destroy(&paced->lock);
  return rc;
}

static void clock_destroy_paced(wispc_clock_t *clock) {
  wispc_clock_paced_t *paced = &clock->iface.paced;

  pthread_cond_destroy(&paced->wake_cond);
  pthread_mutex_destroy(&paced->lock);
}

static void clock_reset_paced(wispc_clock_t *clock) {
  wispc_clock_paced_t *paced = &clock->iface.paced;
  struct timespec now;

  pthread_mutex_lock(&paced->lock);
  paced->force_wake = false;
  clock_gettime(CLOCK_MONOTONIC, &now);
  paced->next_deadline = now;
  paced->pace_active = false;
  pthread_cond_broadcast(&paced->wake_cond);
  pthread_mutex_unlock(&paced->lock);
}

static bool clock_wait_next_paced(wispc_clock_t *clock, uint32_t *missed_frames) {
  wispc_clock_paced_t *paced = &clock->iface.paced;
  const uint64_t period_ns = paced->rt_period_ns;
  struct timespec now;
  bool ticked;
  int rc = 0;
  *missed_frames = 0;

  pthread_mutex_lock(&paced->lock);

  if (paced->force_wake) {
    pthread_mutex_unlock(&paced->lock);
    return false;
  }

  clock_gettime(CLOCK_MONOTONIC, &now);

  if (!paced->pace_active) {
    paced->next_deadline = now;
    paced->pace_active = true;
  }

  timespec_add_ns(&paced->next_deadline, period_ns);

  uint64_t now_ns = timespec_to_ns(&now);
  uint64_t deadline_ns = timespec_to_ns(&paced->next_deadline);

  if (deadline_ns <= now_ns) {
    /* Already past (at least) this deadline: the previous frame overran the
     * period. Count whole periods missed and re-baseline from now instead of
     * sleeping, so we never burst-replay a backlog of ticks. */
    *missed_frames = (uint32_t)((now_ns - deadline_ns) / period_ns);
    paced->next_deadline = now;
    pthread_mutex_unlock(&paced->lock);
    return true;
  }

  /* rc == 0 means signalled or spurious: re-check and keep waiting on the
   * same absolute deadline. Anything else (ETIMEDOUT) ends the wait. */
  while (rc == 0 && !paced->force_wake)
    rc = pthread_cond_timedwait(&paced->wake_cond, &paced->lock, &paced->next_deadline);

  /* Re-check after a timeout as well: a force_wake that raced the deadline
   * must still win over the tick. */
  ticked = !paced->force_wake;

  pthread_mutex_unlock(&paced->lock);
  return ticked;
}

static void clock_force_wake_paced(wispc_clock_t *clock) {
  wispc_clock_paced_t *paced = &clock->iface.paced;

  pthread_mutex_lock(&paced->lock);
  paced->force_wake = true;
  pthread_cond_broadcast(&paced->wake_cond);
  pthread_mutex_unlock(&paced->lock);
}

static const wispc_clock_ops_t clock_ops_paced = {
    .init = clock_init_paced,
    .destroy = clock_destroy_paced,
    .reset = clock_reset_paced,
    .tick = NULL,
    .wait_next = clock_wait_next_paced,
    .frame_done = NULL,
    .force_wake = clock_force_wake_paced,
};

/* ------------------------------------------------------------------------- */
/* TICKED                                                                    */
/* ------------------------------------------------------------------------- */

/* Returns 0 or a positive pthread error code. */
static int clock_init_ticked(wispc_clock_t *clock, wispc_clock_spec_t *spec) {
  wispc_clock_ticked_t *ticked = &clock->iface.ticked;
  int rc;

  if (spec->kind != WISPC_CLOCK_KIND_TICKED)
    return -EINVAL;

  clock->kind = spec->kind;
  ticked->tick_pending = 0;
  ticked->force_wake = false;

  rc = pthread_mutex_init(&ticked->tick_lock, NULL);
  if (rc != 0)
    return rc;

  rc = pthread_cond_init(&ticked->tick_cond, NULL);
  if (rc != 0)
    pthread_mutex_destroy(&ticked->tick_lock);
  return rc;
}

static void clock_destroy_ticked(wispc_clock_t *clock) {
  wispc_clock_ticked_t *ticked = &clock->iface.ticked;

  pthread_cond_destroy(&ticked->tick_cond);
  pthread_mutex_destroy(&ticked->tick_lock);
}

static void clock_reset_ticked(wispc_clock_t *clock) {
  wispc_clock_ticked_t *ticked = &clock->iface.ticked;

  pthread_mutex_lock(&ticked->tick_lock);
  ticked->force_wake = false;
  ticked->tick_pending = 0;
  pthread_cond_broadcast(&ticked->tick_cond);
  pthread_mutex_unlock(&ticked->tick_lock);
}

static void clock_tick_ticked(wispc_clock_t *clock) {
  wispc_clock_ticked_t *ticked = &clock->iface.ticked;

  pthread_mutex_lock(&ticked->tick_lock);
  ticked->tick_pending++;
  pthread_cond_signal(&ticked->tick_cond);
  pthread_mutex_unlock(&ticked->tick_lock);
}

static bool clock_wait_next_ticked(wispc_clock_t *clock, uint32_t *missed_frames) {
  wispc_clock_ticked_t *ticked = &clock->iface.ticked;

  *missed_frames = 0;

  pthread_mutex_lock(&ticked->tick_lock);

  while (ticked->tick_pending == 0 && !ticked->force_wake)
    pthread_cond_wait(&ticked->tick_cond, &ticked->tick_lock);

  /* force_wake wins over a pending tick: once evicted we do not run another
   * frame, however many ticks are queued. */
  if (ticked->force_wake) {
    pthread_mutex_unlock(&ticked->tick_lock);
    return false;
  }

  *missed_frames = ticked->tick_pending - 1; /* one is being consumed */
  ticked->tick_pending = 0;

  pthread_mutex_unlock(&ticked->tick_lock);
  return true;
}

static void clock_force_wake_ticked(wispc_clock_t *clock) {
  wispc_clock_ticked_t *ticked = &clock->iface.ticked;

  pthread_mutex_lock(&ticked->tick_lock);
  ticked->force_wake = true;
  pthread_cond_broadcast(&ticked->tick_cond);
  pthread_mutex_unlock(&ticked->tick_lock);
}

static const wispc_clock_ops_t clock_ops_ticked = {
    .init = clock_init_ticked,
    .destroy = clock_destroy_ticked,
    .reset = clock_reset_ticked,
    .tick = clock_tick_ticked,
    .wait_next = clock_wait_next_ticked,
    .frame_done = NULL,
    .force_wake = clock_force_wake_ticked,
};

/* ------------------------------------------------------------------------- */
/* Lifecycle and dispatch                                                    */
/* ------------------------------------------------------------------------- */

int wispc_clock_init(wispc_clock_t *clock, wispc_clock_spec_t *spec) {
  const wispc_clock_ops_t *ops;
  int rc;

  if (clock->ops != NULL)
    return -EBUSY;

  switch (spec->kind) {
  case WISPC_CLOCK_KIND_HEADLESS:
    clock->ops = &clock_ops_headless;
    break;
  case WISPC_CLOCK_KIND_PACED:
    clock->ops = &clock_ops_paced;
    break;
  case WISPC_CLOCK_KIND_TICKED:
    clock->ops = &clock_ops_ticked;
    break;
  default:
    return -EINVAL;
  }

  rc = clock->ops->init(clock, spec);
  if (rc != 0)
    return (rc < 0) ? rc : -rc; /* pthread codes are positive; normalize to -errno */

  return 0;
}

void wispc_clock_destroy(wispc_clock_t *clock) {
  if (clock->ops && clock->ops->destroy)
    clock->ops->destroy(clock);
}

void wispc_clock_reset(wispc_clock_t *clock) {
  if (clock->ops && clock->ops->reset)
    clock->ops->reset(clock);
}

bool wispc_clock_wait_next(wispc_clock_t *clock, uint32_t *missed_frames) {
  if (clock->ops && clock->ops->wait_next)
    return clock->ops->wait_next(clock, missed_frames);
  return false;
}

void wispc_clock_frame_done(wispc_clock_t *clock) {
  if (clock->ops && clock->ops->frame_done)
    clock->ops->frame_done(clock);
}

void wispc_clock_force_wake(wispc_clock_t *clock) {
  if (clock->ops && clock->ops->force_wake)
    clock->ops->force_wake(clock);
}

void wispc_clock_tick(wispc_clock_t *clock) {
  if (clock->ops && clock->ops->tick)
    clock->ops->tick(clock);
}