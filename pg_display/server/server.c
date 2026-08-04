// reader.c - the server service's real entrypoint.
//
// Wires up all three threads (admin, control, data) around the shm ring +
// frame-ready eventfd, using fb_sink_file.c as a stand-in output sink
// until real Pi hardware (fb_sink_linuxfb.c) is available.
#define LIBPGDP_SERVER
#include "libpgdp.h"

#include "admin/admin_plane.h"
#include "control/control_plane.h"
#include "control/control_query.h"
#include "dataplane/data_plane.h"
#include "dataplane/fb_sink_file.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

/* Server's advertised render modes, most-preferred first.
 * Hardcoded for now since no config-file/CLI plumbing exists yet; the first
 * entry also seeds the data-plane's initial frame size before any client
 * has negotiated a mode. */
static const pgdp_render_mode_t g_supported_modes[] = {
    {320, 240, 60},
    {640, 480, 60},
    {1280, 720, 30},
};
#define NUM_SUPPORTED_MODES (sizeof(g_supported_modes) / sizeof(g_supported_modes[0]))

/* fb_sink_file output directory; a developer (or an image-viewer with
 * auto-reload) can watch <dir>/latest.ppm. Swap for fb_sink_linuxfb once
 * real hardware is available. */
static const char *g_frame_out_dir = "/tmp/pgdp_frames";

int main(void) {
  pgdp_shm_ring_t *ring = pgdps_shm_ring_create();
  if (!ring) {
    fprintf(stderr, "[pgipc-reader] failed to create shm ring\n");
    return 1;
  }

  int frame_fd = pgdps_frame_fd_create();
  if (frame_fd < 0) {
    fprintf(stderr, "[pgipc-reader] failed to create frame-ready eventfd\n");
    pgdps_shm_ring_destroy(ring);
    return 1;
  }

  pgdps_fb_sink_t sink;
  pgdps_fb_sink_file_opts_t sink_opts = {
      .out_dir = g_frame_out_dir,
      .snapshot_interval = 0,
      .max_snapshots = 0,
  };
  if (pgdps_fb_sink_file_create(&sink, &sink_opts) != 0) {
    fprintf(stderr, "[pgipc-reader] failed to create file sink at %s\n",
            g_frame_out_dir);
    close(frame_fd);
    pgdps_shm_ring_destroy(ring);
    return 1;
  }

  pgdps_data_plane_t dp;
  if (pgdps_data_plane_init(&dp, ring, frame_fd, &sink, g_supported_modes[0].width,
                            g_supported_modes[0].height) != 0) {
    fprintf(stderr, "[pgipc-reader] failed to init data plane\n");
    pgdps_fb_sink_close(&sink);
    close(frame_fd);
    pgdps_shm_ring_destroy(ring);
    return 1;
  }

  pgdps_control_query_channel_t chan;
  if (pgdps_control_query_channel_init(&chan) != 0) {
    fprintf(stderr, "[pgipc-reader] failed to init control-query channel\n");
    pgdps_fb_sink_close(&sink);
    close(frame_fd);
    pgdps_shm_ring_destroy(ring);
    return 1;
  }

  pgdps_admin_plane_t ap;
  if (pgdps_admin_plane_init(&ap, &chan) != 0) {
    fprintf(stderr, "[pgipc-reader] failed to init admin plane\n");
    pgdps_control_query_channel_close(&chan);
    pgdps_fb_sink_close(&sink);
    close(frame_fd);
    pgdps_shm_ring_destroy(ring);
    return 1;
  }

  pgdps_control_plane_t cp;
  if (pgdps_control_plane_init(&cp, ring, frame_fd, &dp, &chan, g_supported_modes,
                               NUM_SUPPORTED_MODES) != 0) {
    fprintf(stderr, "[pgipc-reader] failed to init control plane\n");
    pgdps_admin_plane_close(&ap);
    pgdps_control_query_channel_close(&chan);
    pgdps_fb_sink_close(&sink);
    close(frame_fd);
    pgdps_shm_ring_destroy(ring);
    return 1;
  }

  /* Block SIGINT/SIGTERM here, before spawning any threads, so every thread
   * inherits the same mask and none of their blocking syscalls (poll(),
   * epoll_wait(), accept()) get interrupted by them. The main thread alone
   * waits for one of these signals via sigwait(). */
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &mask, NULL);

  pthread_t admin_thread, control_thread, data_thread;
  pthread_create(&admin_thread, NULL, pgdps_admin_plane_run, &ap);
  pthread_create(&control_thread, NULL, pgdps_control_plane_run, &cp);
  pthread_create(&data_thread, NULL, pgdps_data_plane_run, &dp);

  printf("[pgipc-reader] running: admin=%s control=%s shm=%s frames=%s "
         "(SIGINT/SIGTERM to stop)\n",
         PGDPS_ADMIN_SOCK_PATH, PGDPS_CONTROL_SOCK_PATH, PGDP_SHM_NAME,
         g_frame_out_dir);
  fflush(stdout);

  int sig = 0;
  sigwait(&mask, &sig);
  printf("[pgipc-reader] received signal %d, shutting down\n", sig);
  fflush(stdout);

  /* Stop order matches the one exercised (and TSan-validated) in
   * tests/integration/control_plane_test.cpp's fixture: admin first (so no
   * new admin queries can be submitted), then control (safe to stop even
   * mid-drain, since the admin thread is already gone), then data plane. */
  pgdps_admin_plane_stop(&ap);
  pthread_join(admin_thread, NULL);
  pgdps_admin_plane_close(&ap);

  pgdps_control_plane_stop(&cp);
  pthread_join(control_thread, NULL);
  pgdps_control_plane_close(&cp);

  pgdps_data_plane_stop(&dp);
  pthread_join(data_thread, NULL);
  pgdps_fb_sink_close(&sink);
  close(dp.abort_fd);
  close(dp.epoll_fd);

  pgdps_control_query_channel_close(&chan);
  close(frame_fd);
  pgdps_shm_ring_destroy(ring);

  return 0;
}
