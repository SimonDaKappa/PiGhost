// control_query.c - see control_query.h.
#define LIBPGDP_SERVER
#include "control_query.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int pgdps_control_query_channel_init(pgdps_control_query_channel_t *chan) {
  int fds[2];
  if (pipe(fds) != 0) {
    perror("pipe (control_query channel)");
    return -1;
  }

  // Non-blocking on the read side: the control thread's poll() loop only ever reads
  // after poll() says the fd is readable, but a non-blocking read still protects
  // against a spurious wakeup racing a concurrent drain from ever hanging that loop.
  int flags = fcntl(fds[0], F_GETFL, 0);
  fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);

  chan->wake_read_fd = fds[0];
  chan->wake_write_fd = fds[1];
  chan->pending = NULL;
  pthread_mutex_init(&chan->queue_lock, NULL);
  return 0;
}

void pgdps_control_query_channel_close(pgdps_control_query_channel_t *chan) {
  if (chan->wake_read_fd >= 0)
    close(chan->wake_read_fd);
  if (chan->wake_write_fd >= 0)
    close(chan->wake_write_fd);

  chan->wake_read_fd = -1;
  chan->wake_write_fd = -1;
  pthread_mutex_destroy(&chan->queue_lock);
}

void pgdps_control_query_submit(pgdps_control_query_channel_t *chan,
                                pgdps_control_query_t *query) {
  unsigned char byte = 1;
  ssize_t n;

  pthread_mutex_init(&query->lock, NULL);
  pthread_cond_init(&query->done_cond, NULL);
  query->done = false;

  pthread_mutex_lock(&chan->queue_lock);
  chan->pending = query;
  pthread_mutex_unlock(&chan->queue_lock);

  do {
    n = write(chan->wake_write_fd, &byte, 1);
  } while (n < 0 && errno == EINTR);

  pthread_mutex_lock(&query->lock);
  while (!query->done)
    pthread_cond_wait(&query->done_cond, &query->lock);
  pthread_mutex_unlock(&query->lock);

  pthread_mutex_destroy(&query->lock);
  pthread_cond_destroy(&query->done_cond);
}

int pgdps_control_query_channel_wake_fd(pgdps_control_query_channel_t *chan) {
  return chan->wake_read_fd;
}

pgdps_control_query_t *
pgdps_control_query_channel_drain(pgdps_control_query_channel_t *chan) {
  unsigned char byte;
  ssize_t n;

  do {
    n = read(chan->wake_read_fd, &byte, 1);
  } while (n < 0 && errno == EINTR);
  // n <= 0 (EAGAIN on a non-blocking fd, or a genuinely spurious wakeup):
  // fall through and return whatever is pending (may legitimately be NULL).

  pthread_mutex_lock(&chan->queue_lock);
  pgdps_control_query_t *query = chan->pending;
  chan->pending = NULL;
  pthread_mutex_unlock(&chan->queue_lock);

  return query;
}

void pgdps_control_query_complete(pgdps_control_query_t *query) {
  pthread_mutex_lock(&query->lock);
  query->done = true;
  pthread_cond_signal(&query->done_cond);
  pthread_mutex_unlock(&query->lock);
}
