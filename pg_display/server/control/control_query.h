// control_query.h - internal (in-process only, never on any wire) request/
// response bridge between the admin thread and the control-plane thread.
//
// The control-plane thread is the ONLY thread allowed to touch the session table
// (single-client invariant enforced by having exactly one thread own it). The admin
// thread therefore cannot satisfy a LIST or SWITCH request by reading/writing the table
// itself. It builds a pgdps_control_query_t describing what it wants, hands it to the
// control thread via a self-pipe wakeup, and blocks until the control thread fills in
// the result and signals back.
//
// Since every admin connection is strictly connect -> request -> response -> close, the
// admin thread never has more than one query in flight at a time. This channel is
// deliberately NOT a general queue, just a single-slot rendezvous.
#ifndef PGDPS_CONTROL_QUERY_H
#define PGDPS_CONTROL_QUERY_H

#include <pthread.h>
#include <stdbool.h>

#include "admin/admin_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * enum pgdps_control_query_type_t - what kind of request is pending
 * @PGDPS_CTRL_QUERY_LIST:   list all connected/negotiated clients
 * @PGDPS_CTRL_QUERY_SWITCH: switch out the active negotiated client
 */
typedef enum {
  PGDPS_CTRL_QUERY_LIST = 0,
  PGDPS_CTRL_QUERY_SWITCH = 1,
} pgdps_control_query_type_t;

/**
 * struct pgdps_control_query_t - one in-flight admin request
 * @type:            which request this is
 * @switch_client_id:   input, valid when type == PGDPS_CTRL_QUERY_SWITCH
 * @list_response:   output, filled in when type == PGDPS_CTRL_QUERY_LIST
 * @switch_response: output, filled in when type == PGDPS_CTRL_QUERY_SWITCH
 * @lock:            guards @done and the condvar wait below
 * @done_cond:       signaled by the control thread once outputs are valid
 * @done:            set true by the control thread when finished
 *
 * Allocated on the admin thread's stack for the lifetime of one admin
 * connection.
 */
typedef struct {
  pgdps_control_query_type_t type;
  char switch_client_id[PGDP_CLIENT_ID_LEN];

  pgdps_admin_list_response_t list_response;
  pgdps_admin_switch_response_t switch_response;

  pthread_mutex_t lock;
  pthread_cond_t done_cond;
  bool done;
} pgdps_control_query_t;

/**
 * struct pgdps_control_query_channel_t - the admin<->control rendezvous point
 * @wake_read_fd:  control thread adds this to its poll() set
 * @wake_write_fd: admin thread writes one byte here per submitted query
 * @queue_lock:    guards @pending; also serializes concurrent submitters (today
 *                 there's only ever one admin thread, but the lock costs nothing and
 *                 removes that as an assumption later)
 * @pending:       the current in-flight query, or NULL if none
 *
 * One instance shared between reader's admin thread and control thread, created once
 * at startup before either thread runs.
 */
typedef struct {
  int wake_read_fd;
  int wake_write_fd;
  pthread_mutex_t queue_lock;
  pgdps_control_query_t *pending;
} pgdps_control_query_channel_t;

/**
 * pgdps_control_query_channel_init() - create the self-pipe + locks
 * @chan: channel to initialize
 *
 * Return: 0 on success, -1 on error (e.g. pipe() failed).
 */
int pgdps_control_query_channel_init(pgdps_control_query_channel_t *chan);

/**
 * pgdps_control_query_channel_close() - release the self-pipe fds
 * @chan: channel previously initialized by pgdps_control_query_channel_init()
 */
void pgdps_control_query_channel_close(pgdps_control_query_channel_t *chan);

/**
 * pgdps_control_query_submit() - admin thread: post a query and block for it
 * @chan:  channel shared with the control thread
 * @query: caller-owned, stack-allocated query; must be zero-initialized except for
 *         @type and (for SWITCH) @switch_client_id before calling
 *
 * Blocks until the control thread has filled in the relevant output field(s) and called
 * pgdps_control_query_complete(). Safe to call from the admin thread only.
 */
void pgdps_control_query_submit(pgdps_control_query_channel_t *chan,
                                pgdps_control_query_t *query);

/**
 * pgdps_control_query_channel_wake_fd() - fd for the control thread's poll()
 * @chan: channel
 *
 * Return: the read end of the self-pipe; readable exactly when a query is waiting to be
 * drained via pgdps_control_query_channel_drain().
 */
int pgdps_control_query_channel_wake_fd(pgdps_control_query_channel_t *chan);

/**
 * pgdps_control_query_channel_drain() - control thread: claim the pending query
 * @chan: channel
 *
 * Call after poll() reports @chan's wake fd is readable. Consumes the wakeup byte and
 * returns the pending query for the control thread to populate in-place; the control
 * thread must call pgdps_control_query_complete() when done, exactly once, on the
 * pointer returned here.
 *
 * Return: the pending query, or NULL if the wakeup was spurious (should not happen in
 * practice, but callers must handle it defensively).
 */
pgdps_control_query_t *
pgdps_control_query_channel_drain(pgdps_control_query_channel_t *chan);

/**
 * pgdps_control_query_complete() - control thread: signal a query is done
 * @query: query returned by pgdps_control_query_channel_drain(), with its output
 *         field(s) already filled in
 *
 * Wakes the admin thread blocked in pgdps_control_query_submit().
 */
void pgdps_control_query_complete(pgdps_control_query_t *query);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif // PGDPS_CONTROL_QUERY_H
