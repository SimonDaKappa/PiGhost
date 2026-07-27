// reader.c - the display service's real entrypoint.
//
// Wires up all three threads (admin, control, data) around the shm ring +
// frame-ready semaphore, using fb_sink_file.c as a stand-in output sink
// until real Pi hardware (fb_sink_linuxfb.c) is available.
#define LIBPGIPC_READER
#include "libpgipc.h"

#include "admin/admin_plane.h"
#include "control/control_plane.h"
#include "control/control_query.h"
#include "dataplane/data_plane.h"
#include "dataplane/fb_sink_file.h"

#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>

/* Display's advertised render modes, most-preferred first.
 * Hardcoded for now since no config-file/CLI plumbing exists yet; the first
 * entry also seeds the data-plane's initial frame size before any writer
 * has negotiated a mode. */
static const pgipc_render_mode_t kSupportedModes[] = {
    {320, 240, 60},
    {640, 480, 60},
    {1280, 720, 30},
};
#define NUM_SUPPORTED_MODES (sizeof(kSupportedModes) / sizeof(kSupportedModes[0]))

/* fb_sink_file output directory; a developer (or an image-viewer with
 * auto-reload) can watch <dir>/latest.ppm. Swap for fb_sink_linuxfb once
 * real hardware is available. */
static const char *kFrameOutDir = "/tmp/pgipc_frames";

int main(void) {
  pgipc_shm_ring_t *ring = pgipc_shm_ring_create();
  if (!ring) {
    fprintf(stderr, "[pgipc-reader] failed to create shm ring\n");
    return 1;
  }

  sem_t *fsem = pgipc_shm_sem_create();
  if (!fsem) {
    fprintf(stderr, "[pgipc-reader] failed to create frame-ready semaphore\n");
    pgipc_shm_ring_destroy(ring);
    return 1;
  }

  pgipc_fb_sink_t sink;
  pgipc_fb_sink_file_opts_t sink_opts = {
      .out_dir = kFrameOutDir,
      .snapshot_interval = 0,
      .max_snapshots = 0,
  };
  if (pgipc_fb_sink_file_create(&sink, &sink_opts) != 0) {
    fprintf(stderr, "[pgipc-reader] failed to create file sink at %s\n", kFrameOutDir);
    sem_close(fsem);
    pgipc_shm_ring_destroy(ring);
    return 1;
  }

  pgipc_data_plane_t dp;
  if (pgipc_data_plane_init(&dp, ring, fsem, &sink, kSupportedModes[0].width,
                            kSupportedModes[0].height) != 0) {
    fprintf(stderr, "[pgipc-reader] failed to init data plane\n");
    pgipc_fb_sink_close(&sink);
    sem_close(fsem);
    pgipc_shm_ring_destroy(ring);
    return 1;
  }

  pgipc_control_query_channel_t chan;
  if (pgipc_control_query_channel_init(&chan) != 0) {
    fprintf(stderr, "[pgipc-reader] failed to init control-query channel\n");
    pgipc_fb_sink_close(&sink);
    sem_close(fsem);
    pgipc_shm_ring_destroy(ring);
    return 1;
  }

  pgipc_admin_plane_t ap;
  if (pgipc_admin_plane_init(&ap, &chan) != 0) {
    fprintf(stderr, "[pgipc-reader] failed to init admin plane\n");
    pgipc_control_query_channel_close(&chan);
    pgipc_fb_sink_close(&sink);
    sem_close(fsem);
    pgipc_shm_ring_destroy(ring);
    return 1;
  }

  pgipc_control_plane_t cp;
  if (pgipc_control_plane_init(&cp, ring, &dp, &chan, kSupportedModes,
                               NUM_SUPPORTED_MODES) != 0) {
    fprintf(stderr, "[pgipc-reader] failed to init control plane\n");
    pgipc_admin_plane_close(&ap);
    pgipc_control_query_channel_close(&chan);
    pgipc_fb_sink_close(&sink);
    sem_close(fsem);
    pgipc_shm_ring_destroy(ring);
    return 1;
  }

  /* Block SIGINT/SIGTERM here, before spawning any threads, so every thread
   * inherits the same mask and none of their blocking syscalls (poll(),
   * sem_wait(), accept()) get interrupted by them. The main thread alone
   * waits for one of these signals via sigwait(). */
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &mask, NULL);

  pthread_t admin_thread, control_thread, data_thread;
  pthread_create(&admin_thread, NULL, pgipc_admin_plane_run, &ap);
  pthread_create(&control_thread, NULL, pgipc_control_plane_run, &cp);
  pthread_create(&data_thread, NULL, pgipc_data_plane_run, &dp);

  printf("[pgipc-reader] running: admin=%s control=%s shm=%s frames=%s "
         "(SIGINT/SIGTERM to stop)\n",
         PGIPC_ADMIN_SOCK_PATH, PGIPC_CONTROL_SOCK_PATH, PGIPC_SHM_NAME, kFrameOutDir);
  fflush(stdout);

  int sig = 0;
  sigwait(&mask, &sig);
  printf("[pgipc-reader] received signal %d, shutting down\n", sig);
  fflush(stdout);

  /* Stop order matches the one exercised (and TSan-validated) in
   * tests/integration/control_plane_test.cpp's fixture: admin first (so no
   * new admin queries can be submitted), then control (safe to stop even
   * mid-drain, since the admin thread is already gone), then data plane. */
  pgipc_admin_plane_stop(&ap);
  pthread_join(admin_thread, NULL);
  pgipc_admin_plane_close(&ap);

  pgipc_control_plane_stop(&cp);
  pthread_join(control_thread, NULL);
  pgipc_control_plane_close(&cp);

  pgipc_data_plane_stop(&dp);
  pthread_join(data_thread, NULL);
  pgipc_fb_sink_close(&sink);

  pgipc_control_query_channel_close(&chan);
  sem_close(fsem);
  pgipc_shm_ring_destroy(ring);

  return 0;
}
