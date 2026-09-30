#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "client_internal.h"

/**
 * wispc_shm_attach() - attach to an existing shared ring
 * @max_retries:    number of attempts before giving up
 * @retry_delay_ms: delay between attempts, in milliseconds
 *
 * The server service creates the ring before any client starts, so a client racing
 * server startup should retry rather than fail immediately.
 *
 * Return: pointer to the mapped ring, or NULL if it never appeared.
 */
wisp_shm_ring_t *wispc_shm_attach(int max_retries, int retry_delay_ms) {
  int fd = -1;

  for (int i = 0; i < max_retries; i++) {
    fd = shm_open(WISP_SHM_NAME, O_RDWR, 0666);
    if (fd >= 0)
      break;
    struct timespec ts = {.tv_sec = retry_delay_ms / 1000,
                          .tv_nsec = (retry_delay_ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
  }

  if (fd < 0) {
    fprintf(stderr, "[pgipc] gave up waiting for shm segment %s\n", WISP_SHM_NAME);
    return NULL;
  }

  wisp_shm_ring_t *ring = (wisp_shm_ring_t *)mmap(
      NULL, sizeof(wisp_shm_ring_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  close(fd);

  if (ring == MAP_FAILED) {
    perror("mmap");
    return NULL;
  }
  return ring;
}