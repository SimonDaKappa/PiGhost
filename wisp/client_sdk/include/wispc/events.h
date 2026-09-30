#pragma once

#include "render.h"
#include <wisp/types.h>

/**
 * typedef wispc_on_negotiated - notify negotation success notification
 * @user: opaque user data
 * @mode: negotiated client-server resolution and capabilities
 */
typedef void (*wispc_on_negotiated)(void *user, wisp_resolution_t mode);

/**
 * typedef wispc_on_activation_queued - notify client placed in queue notification
 * @user:     opaque user data
 * @position: client position in queue (1-indexed)
 */
typedef void (*wispc_on_activation_queued)(void *user, uint32_t position);

/**
 * typedef wispc_on_activation_granted - notify client granted active position
 * @user:              opaque user data
 * @target_core:       core the client's render cb and state machine will run on
 * @priority_hint:     priority the client's render cb is ran at
 * @period_ns:         recommended render clock tick period
 * @frame_render_budget_ns: per-frame render budget
 */
typedef void (*wispc_on_activation_granted)(void *user, uint32_t target_core,
                                            uint32_t priority_hint, uint64_t period_ns,
                                            uint64_t frame_render_budget_ns);

/**
 * typedef on_activation_denied - notify client activation denial
 * @user:   opaque user data
 * @reason: human readable denial reason
 */
typedef void (*wispc_on_activation_denied)(void *user, const char *reason);

/**
 * typedef wispc_on_render_warmup_failed - notify real-time warmup failure
 * @user: opaque user data
 * @err:  warmup error code
 */
typedef void (*wispc_on_render_warmup_failed)(void *user, int err);

/**
 * typedef wispc_on_promotion_failed - notify real-time promotion failure
 * @user: opaque user data
 * @err:  promotion error code
 */
typedef void (*wispc_on_promotion_failed)(void *user, int err);

/**
 * typedef wispc_on_rt_active - notify real-time start
 * @user: opaque user data
 */
typedef void (*wispc_on_rt_active)(void *user);

/**
 * typedef wispc_on_evict_timeout - notify frame render exceeded eviction budget timeout
 * @user: opaque user data
 */
typedef void (*wispc_on_evict_timeout)(void *user);

/**
 * typedef wispc_on_evict_pending - notify client eviction imminent
 * @user:     opaque user data
 * @grace_ms: window to wind dwon down before absolute eviction
 */
typedef void (*wispc_on_evict_pending)(void *user, uint32_t grace_ms);

/**
 * typedef wispc_on_evicted - notify client evicted
 * @user: opaque user data
 */
typedef void (*wispc_on_evicted)(void *user);

/**
 * typedef wispc_event_cfg_t - optional event callback hooks, registered at connect
 * @user: opaque user data passed through each callback
 *
 * Control thread callbacks:
 * @on_negotiated:         negotiation completes
 * @on_activation_queued:  the client is queued for activation
 * @on_activation_denied:  the client is denied activation
 * @on_activation_granted: the client is granted activation
 * @on_promotion_failed:   the client fails promotion
 * @on_rt_active:          the client becomes real-time active
 * @on_evict_pending:      the client is about to be evicted
 * @on_evicted:            the client is evicted
 * @on_evict_timeout:      the client exceeds the eviction timeout
 *
 * Render thread callbacks:
 * @on_warmup_render_failed: the real-time render warmup fails
 * @on_render_published:     a frame is successfully published
 * @on_render_error:         a render error occurs
 * @on_render_backpressure:  the render thread experiences clock and/or tick
 *                           backpressure
 *
 * All callback fields are optional.
 */
typedef struct {
  void *user;

  /* Control thread */
  wispc_on_negotiated on_negotiated;
  wispc_on_activation_queued on_activation_queued;
  wispc_on_activation_denied on_activation_denied;
  wispc_on_activation_granted on_activation_granted;
  wispc_on_promotion_failed on_promotion_failed;
  wispc_on_rt_active on_rt_active;
  wispc_on_evict_pending on_evict_pending;
  wispc_on_evicted on_evicted;
  wispc_on_evict_timeout on_evict_timeout;

  /* Render thread */
  wispc_on_render_warmup_failed on_warmup_failed;
  wispc_on_render_published on_render_published;
  wispc_on_render_error on_render_error;
  wispc_on_render_backpressure on_render_backpressure;
} wispc_event_cfg_t;
