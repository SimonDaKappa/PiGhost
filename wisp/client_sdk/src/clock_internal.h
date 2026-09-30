#pragma once
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <wisp/utils.h>
#include <wispc/client.h>
#include <wispc/clock.h>

/**
 * typedef wispc_clock_ops_t - per-kind clock behaviour, one static const
 * instance per kind.
 * @init:        initialize clock with given configuration.
 * @destroy:     release per-kind resources (mutexes, condvars)
 * @reset:       reset the clock to its initial state. DO NOT CALL while render loop is
 *               active
 * @tick:        advance the clock by one tick, typically used by ticked clocks.
 * @wait_next:   block until the next frame should render. Returns false if
 *               the clock was force-woken or the ctx is shutting down.
 * @frame_done:  post-frame-publishing hook
 * @force_wake:  thread-safe preemption of wait_next
 */
typedef struct {
  int (*init)(wispc_clock_t *clock, wispc_clock_spec_t *spec);
  void (*destroy)(wispc_clock_t *clock);
  void (*reset)(wispc_clock_t *clock);
  void (*tick)(wispc_clock_t *clock);
  bool (*wait_next)(wispc_clock_t *clock, uint32_t *missed_frames);
  void (*frame_done)(wispc_clock_t *clock);
  void (*force_wake)(wispc_clock_t *clock);
} wispc_clock_ops_t;

/**
 * typedef wispc_clock_headless_t - headless clock state.
 * @force_wake: indicates if the clock should be force-woken.
 *
 * A clock that runs as fast as possible without any pacing.
 */
typedef struct {
  WISP_ATOMIC bool force_wake;
} wispc_clock_headless_t;

/**
 * typedef wispc_clock_paced_t - paced clock state.
 * @lock:          mutex protecting the schedule and force_wake.
 * @wake_cond:     condition variable for waking the clock thread.
 * @rt_period_ns:  real-time period in nanoseconds, immutable after init.
 * @next_deadline: next scheduled deadline, guarded by lock.
 * @pace_active:   indicates if the pacing is active, guarded by lock.
 * @force_wake:    indicates if the clock should be force-woken, guarded by lock.
 *
 * A clock that paces its frames according to a fixed real-time period.
 */
typedef struct {
  pthread_mutex_t lock;
  pthread_cond_t wake_cond;
  uint64_t rt_period_ns;
  struct timespec next_deadline;
  bool pace_active;
  bool force_wake;
} wispc_clock_paced_t;

/**
 * typedef wispc_clock_ticked_t - ticked clock state.
 * @tick_lock:    mutex protecting the tick state.
 * @tick_cond:    condition variable for waking the clock thread.
 * @tick_pending: indicates if a tick is pending, guarded by tick_lock.
 * @force_wake:   indicates if the clock should be force-woken, guarded by tick_lock.
 *
 * A clock that advances in discrete ticks, typically driven by an external source.
 */
typedef struct {
  pthread_mutex_t tick_lock;
  pthread_cond_t tick_cond;
  uint32_t tick_pending;
  bool force_wake;
} wispc_clock_ticked_t;

/**
 * typedef wispc_clock_t - clock state, embedded by value in wispc_ctx_t.
 * @kind:  indicates the kind of clock currently active.
 * @ops:   selects which union member is live. NULL when uninitialized.
 * @iface: contains the state for the currently active clock kind.
 *
 * The storage for every kind lives here, so no allocation is needed and the
 * clock lives exactly as long as the owning lifetime. Because the mutexes and condvars
 * are stored inline, the ctx must not be copied or moved after
 * wispc_clock_init().
 */
typedef struct {
  wispc_clock_kind_t kind;
  const wispc_clock_ops_t *ops;
  union {
    wispc_clock_headless_t headless;
    wispc_clock_paced_t paced;
    wispc_clock_ticked_t ticked;
  } iface;
} wispc_clock_t;

/**
 * typedef wispc_clock_spec_t - clock configuration.
 * @kind:  indicates the kind of clock to configure.
 * @iface: contains the configuration for the specified clock kind.
 *
 * The configuration is used to initialize the clock with the desired behavior.
 */
typedef struct {
  wispc_clock_kind_t kind;
  union {
    struct {
      uint64_t period_ns;
    } paced;
  } iface;
} wispc_clock_spec_t;

/**
 * wispc_clock_init() - initialize a clock with the specified configuration.
 * @clock: pointer to the clock to initialize.
 * @spec:   pointer to the configuration to use for initialization.
 *
 * Returns 0 on success or a negative errno on failure.
 */
int wispc_clock_init(wispc_clock_t *clock, wispc_clock_spec_t *spec);

/**
 * wispc_clock_destroy() - clean up and release resources associated with the clock.
 * @clock: pointer to the clock to destroy.
 */
void wispc_clock_destroy(wispc_clock_t *clock);

/**
 * wispc_clock_reset() - reset the clock to its initial (but configured) state.
 * @clock: pointer to the clock to reset.
 */
void wispc_clock_reset(wispc_clock_t *clock);

/**
 * wispc_clock_wait_next() - wait for the next clock tick.
 * @clock:         pointer to the clock to wait on.
 * @missed_frames: pointer to a variable to store the number of missed frames.
 *
 * Returns true if the clock ticked, false if it was force-woken.
 */
bool wispc_clock_wait_next(wispc_clock_t *clock, uint32_t *missed_frames);

/**
 * wispc_clock_frame_done() - notify the clock that a frame has been completed.
 * @clock: pointer to the clock to notify.
 */
void wispc_clock_frame_done(wispc_clock_t *clock);

/**
 * wispc_clock_force_wake() - force the clock to wake.
 * @clock: pointer to the clock to wake.
 */
void wispc_clock_force_wake(wispc_clock_t *clock);

/**
 * wispc_clock_tick() - notify the clock that a tick has occurred, if supported.
 * @clock: pointer to the clock to notify.
 */
void wispc_clock_tick(wispc_clock_t *clock);