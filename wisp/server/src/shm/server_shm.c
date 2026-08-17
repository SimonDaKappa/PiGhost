#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>

#include "server_shm.h"

wisp_shm_ring_t *wisps_shm_ring_create(void) {
  if (!__atomic_always_lock_free(sizeof(int32_t), 0) ||
      !__atomic_always_lock_free(sizeof(uint32_t), 0)) {
    fprintf(stderr,
            "[pgipc] WARNING: 32-bit atomics are not always lock-free on this "
            "platform; cross-process atomics may not work as intended. Exiting!\n");
    exit(1);
  }

  shm_unlink(WISP_SHM_NAME);

  int fd = shm_open(WISP_SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0666);
  if (fd < 0) {
    perror("shm_open (create)");
    return NULL;
  }

  if (ftruncate(fd, (off_t)sizeof(wisp_shm_ring_t)) != 0) {
    perror("ftruncate");
    close(fd);
    return NULL;
  }

  wisp_shm_ring_t *ring = (wisp_shm_ring_t *)mmap(
      NULL, sizeof(wisp_shm_ring_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  close(fd);
  if (ring == MAP_FAILED) {
    perror("mmap");
    return NULL;
  }

  memset(ring, 0, sizeof(wisp_shm_ring_t));
  wisp_atomic_store(&ring->latest_ready, -1);
  wisp_atomic_store(&ring->server_locked, -1);
  wisp_atomic_store(&ring->client_locked, -1);
  wisp_atomic_store(&ring->frame_counter, 0);
  wisp_atomic_store(&ring->generation, 0);

  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
  pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
  pthread_mutex_init(&ring->bookkeeping_lock, &attr);
  pthread_mutexattr_destroy(&attr);

  return ring;
}

void wisps_shm_ring_destroy(wisp_shm_ring_t *ring) {
  if (ring) {
    pthread_mutex_destroy(&ring->bookkeeping_lock);
    munmap(ring, sizeof(wisp_shm_ring_t));
  }
  shm_unlink(WISP_SHM_NAME);
}

int wisps_shm_frame_fd_create(void) {
  int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (fd < 0)
    perror("eventfd (frame_fd create)");
  return fd;
}

int wisps_shm_ring_checkout(wisp_shm_ring_t *ring) {
  wisp_shm_ring_lock(ring);
  int idx = wisp_atomic_load(&ring->latest_ready);
  if (idx >= 0)
    wisp_atomic_store(&ring->server_locked, idx);
  wisp_shm_ring_unlock(ring);
  return idx;
}

void wisps_shm_ring_release(wisp_shm_ring_t *ring) {
  wisp_shm_ring_lock(ring);
  wisp_atomic_store(&ring->server_locked, -1);
  wisp_shm_ring_unlock(ring);
}

void wisps_evict_client(wisp_shm_ring_t *ring) {
  wisp_shm_ring_lock(ring);
  wisp_atomic_fetch_add(&ring->generation, 1);
  wisp_atomic_store(&ring->latest_ready, -1);
  wisp_atomic_store(&ring->server_locked, -1);
  wisp_atomic_store(&ring->client_locked, -1);
  wisp_shm_ring_unlock(ring);
}

int wisps_dmabuf_set_from_announce(wisps_dmabuf_set_t *set,
                                   const wisp_dmabuf_announce_msg_t *msg,
                                   const int *fds, int nfds) {
  memset(set, 0, sizeof(*set));
  for (int i = 0; i < WISP_NUM_BUFFERS; i++)
    set->fds[i] = -1;

  if (msg->nbufs != WISP_NUM_BUFFERS || nfds != WISP_NUM_BUFFERS) {
    int tmp[WISP_NUM_BUFFERS];

    for (int i = 0; i < nfds && i < WISP_NUM_BUFFERS; i++)
      tmp[i] = fds[i];

    wisp_close_fds(tmp, nfds < WISP_NUM_BUFFERS ? nfds : WISP_NUM_BUFFERS);
    return -1;
  }

  for (int i = 0; i < WISP_NUM_BUFFERS; i++) {
    const wisp_dmabuf_desc_t *d = &msg->desc[i];

    /* stride is in bytes; must cover the row */
    if (fds[i] < 0 || d->width == 0 || d->height == 0 || d->stride == 0 ||
        d->stride < d->width) {
      wisp_close_fds((int *)fds, WISP_NUM_BUFFERS);
      return -1;
    }
  }

  for (int i = 0; i < WISP_NUM_BUFFERS; i++) {
    set->fds[i] = fds[i];
    set->desc[i] = msg->desc[i];
  }

  set->valid = true;
  return 0;
}

void wisps_dmabuf_set_close(wisps_dmabuf_set_t *set) {
  if (!set)
    return;

  wisp_close_fds(set->fds, WISP_NUM_BUFFERS);

  for (int i = 0; i < WISP_NUM_BUFFERS; i++)
    set->fds[i] = -1;
  set->valid = false;
}