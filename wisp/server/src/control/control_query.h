// control_query.h - internal (in-process only, never on any wire) request/
// response bridge between the admin thread and the control-plane thread.
//
// The control-plane thread is the ONLY thread allowed to touch the session table
// (single-client invariant enforced by having exactly one thread own it). The admin
// thread therefore cannot satisfy a LIST or SWITCH request by reading/writing the table
// itself. It builds a wisps_control_query_t describing what it wants, hands it to the
// control thread via a self-pipe wakeup, and blocks until the control thread fills in
// the result and signals back.
//
// Since every admin connection is strictly connect -> request -> response -> close, the
// admin thread never has more than one query in flight at a time. This channel is
// deliberately NOT a general queue, just a single-slot rendezvous.
#ifndef WISPS_CONTROL_QUERY_H
#define WISPS_CONTROL_QUERY_H

#include <pthread.h>
#include <stdbool.h>

#include "admin/admin_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * enum wisps_control_query_type_t - what kind of request is pending
 * @WISPS_CTRL_QUERY_LIST:   list all connected/negotiated clients
 * @WISPS_CTRL_QUERY_SWITCH: switch out the active negotiated client
 */
typedef enum {
  WISPS_CTRL_QUERY_LIST = 0,
  WISPS_CTRL_QUERY_SWITCH = 1,
} wisps_control_query_type_t;

/**
 * struct wisps_control_query_t - one in-flight admin request
 * @type:            which request this is
 * @switch_client_id:   input, valid when type == WISPS_CTRL_QUERY_SWITCH
 * @list_response:   output, filled in when type == WISPS_CTRL_QUERY_LIST
 * @switch_response: output, filled in when type == WISPS_CTRL_QUERY_SWITCH
 * @lock:            guards @done and the condvar wait below
 * @done_cond:       signaled by the control thread once outputs are valid
 * @done:            set true by the control thread when finished
 *
 * Allocated on the admin thread's stack for the lifetime of one admin
 * connection.
 */
typedef struct {
  wisps_control_query_type_t type;
  char switch_client_id[WISP_CLIENT_ID_LEN];

  wisps_admin_list_response_t list_response;
  wisps_admin_switch_response_t switch_response;

  pthread_mutex_t lock;
  pthread_cond_t done_cond;
  bool done;
} wisps_control_query_t;

/**
 * struct wisps_control_query_channel_t - the admin<->control rendezvous point
 * @wake_read_fd:  control thread adds this to its poll() set
 * @wake_write_fd: admin thread writes one byte here per submitted query
 * @queue_lock:    guards @pending; also serializes concurrent submitters (today
 *                 there's only ever one admin thread, but the lock costs nothing and
 *                 removes that as an assumption later)
 * @pending:       the current in-flight query, or NULL if none
 *
 * One instance shared between server's admin thread and control thread, created once
 * at startup before either thread runs.
 */
typedef struct {
  int wake_read_fd;
  int wake_write_fd;
  pthread_mutex_t queue_lock;
  wisps_control_query_t *pending;
} wisps_control_query_channel_t;

/**
 * wisps_control_query_channel_init() - create the self-pipe + locks
 * @chan: channel to initialize
 *
 * Return: 0 on success, -1 on error (e.g. pipe() failed).
 */
int wisps_control_query_channel_init(wisps_control_query_channel_t *chan);

/**
 * wisps_control_query_channel_close() - release the self-pipe fds
 * @chan: channel previously initialized by wisps_control_query_channel_init()
 */
void wisps_control_query_channel_close(wisps_control_query_channel_t *chan);

/**
 * wisps_control_query_submit() - admin thread: post a query and block for it
 * @chan:  channel shared with the control thread
 * @query: caller-owned, stack-allocated query; must be zero-initialized except for
 *         @type and (for SWITCH) @switch_client_id before calling
 *
 * Blocks until the control thread has filled in the relevant output field(s) and called
 * wisps_control_query_complete(). Safe to call from the admin thread only.
 */
void wisps_control_query_submit(wisps_control_query_channel_t *chan,
                                wisps_control_query_t *query);

/**
 * wisps_control_query_channel_wake_fd() - fd for the control thread's poll()
 * @chan: channel
 *
 * Return: the read end of the self-pipe; readable exactly when a query is waiting to be
 * drained via wisps_control_query_channel_drain().
 */
int wisps_control_query_channel_wake_fd(wisps_control_query_channel_t *chan);

/**
 * wisps_control_query_channel_drain() - control thread: claim the pending query
 * @chan: channel
 *
 * Call after poll() reports @chan's wake fd is readable. Consumes the wakeup byte and
 * returns the pending query for the control thread to populate in-place; the control
 * thread must call wisps_control_query_complete() when done, exactly once, on the
 * pointer returned here.
 *
 * Return: the pending query, or NULL if the wakeup was spurious (should not happen in
 * practice, but callers must handle it defensively).
 */
wisps_control_query_t *
wisps_control_query_channel_drain(wisps_control_query_channel_t *chan);

/**
 * wisps_control_query_complete() - control thread: signal a query is done
 * @query: query returned by wisps_control_query_channel_drain(), with its output
 *         field(s) already filled in
 *
 * Wakes the admin thread blocked in wisps_control_query_submit().
 */
void wisps_control_query_complete(wisps_control_query_t *query);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif // WISPS_CONTROL_QUERY_H
