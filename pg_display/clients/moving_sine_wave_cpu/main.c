// main.c - moving_sine_wave_cpu: a reference CPU-pixel producer app.
//
// Draws a single animated sine wave curve scrolling left-to-right across
// the negotiated frame, entirely in software (no GPU/GBM involved), and
// publishes it into the display's shm ring on every tick at the
// negotiated fps. This is the simplest possible libpgipc producer:
// establish a session, loop forever, write pixels, publish, repeat.
// Everything else (mode negotiation, activation/eviction, reconnect
// handling) is handled by libpgipc.h itself.
//
// Run alongside pgipc_reader (see pgipc_dev/display/) and watch
// <frames-dir>/latest.ppm with an auto-reloading image viewer to see it
// live. See README.md in this directory for full testing instructions.
#define LIBPGIPC_IMPLEMENTATION
#define LIBPGIPC_WRITER
#include "libpgipc.h"

#include <math.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static volatile sig_atomic_t g_stop = 0;

static void handle_signal(int sig) {
  (void)sig;
  g_stop = 1;
}

/**
 * render_sine_frame() - fill one XRGB8888 frame with a scrolling sine curve
 * @pixels: destination buffer, @height rows of @width pixels, tightly
 *          packed, byte order [B, G, R, X] per pixel (DRM XR24 in memory)
 * @width:  frame width in pixels
 * @height: frame height in pixels
 * @phase:  radians to offset the curve by this frame, advanced by the
 *          caller each tick to animate the scroll
 *
 * Background is a dark vertical gradient; the curve itself is drawn a few
 * pixels thick in a bright color so it's clearly visible even at small
 * frame sizes.
 */
static void render_sine_frame(unsigned char *pixels, uint32_t width, uint32_t height,
                              double phase) {
  const double amplitude = (double)height * 0.35;
  const double mid_y = (double)height / 2.0;
  const double wavelength_px = (double)width / 2.0; /* two full cycles across */
  const int stroke_half_px = 2;

  for (uint32_t y = 0; y < height; y++) {
    unsigned char *row = pixels + (size_t)y * width * 4;
    unsigned char bg = (unsigned char)(20 + (40 * y) / (height ? height : 1));
    for (uint32_t x = 0; x < width; x++) {
      row[x * 4 + 0] = bg;     /* B */
      row[x * 4 + 1] = bg / 2; /* G */
      row[x * 4 + 2] = bg / 4; /* R */
      row[x * 4 + 3] = 0xFF;   /* X (unused) */
    }
  }

  for (uint32_t x = 0; x < width; x++) {
    double angle = (2.0 * M_PI * (double)x / wavelength_px) + phase;
    int curve_y = (int)lround(mid_y - amplitude * sin(angle));

    for (int dy = -stroke_half_px; dy <= stroke_half_px; dy++) {
      int y = curve_y + dy;
      if (y < 0 || y >= (int)height)
        continue;
      unsigned char *px = pixels + ((size_t)y * width + x) * 4;
      px[0] = 0xE0; /* B */
      px[1] = 0xC0; /* G */
      px[2] = 0x20; /* R cyan-ish/gold curve, bright against the dark bg */
      px[3] = 0xFF; /* X */
    }
  }
}

int main(void) {
  /* Offer the same mode list the reference display advertises
   * (pgipc_dev/display/reader.c), most-preferred first. Any producer app
   * can offer a different/smaller list; the display picks the first
   * mutual match (SPEC.md §5). */
  const pgipc_render_mode_t offered_modes[] = {
      {320, 240, 60},
      {640, 480, 60},
      {1280, 720, 30},
  };
  const int num_offered = (int)(sizeof(offered_modes) / sizeof(offered_modes[0]));

  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);
  /* Ignore SIGPIPE: this is a long-running daemon-style app, and a broken
   * stdout/stderr pipe (e.g. the terminal/log collector on the other end
   * start failing, which is harmless here.) */
  signal(SIGPIPE, SIG_IGN);

  printf("[sine_wave_cpu] connecting to display...\n");
  pgipc_writer_ctx_t *ctx =
      pgipc_writer_connect("sine_wave_cpu", offered_modes, num_offered);
  if (!ctx) {
    fprintf(stderr, "[sine_wave_cpu] failed to connect. is pgipc_reader running?\n");
    return 1;
  }

  pgipc_render_mode_t mode = pgipc_writer_negotiated_mode(ctx);
  double frame_interval_s = mode.fps > 0 ? 1.0 / (double)mode.fps : 1.0 / 60.0;
  printf("[sine_wave_cpu] negotiated %ux%u@%ufps, frame interval=%.3fms\n", mode.width,
         mode.height, mode.fps, frame_interval_s * 1000.0);

  double phase = 0.0;
  const double phase_speed = 2.5; /* radians/sec */
  uint64_t frame_id = 0;
  uint64_t frames_published = 0;
  uint64_t frames_skipped_inactive = 0;
  bool was_active = false;

  struct timespec last_report;
  clock_gettime(CLOCK_MONOTONIC, &last_report);

  while (!g_stop) {
    struct timespec tick_start;
    clock_gettime(CLOCK_MONOTONIC, &tick_start);

    bool active = pgipc_writer_is_active(ctx);
    if (active != was_active) {
      printf("[sine_wave_cpu] %s\n", active ? "ACTIVATED: now driving the display"
                                            : "DEACTIVATED: paused, holding session");
      was_active = active;
    }

    if (active) {
      /* Mode may have changed since connect (e.g. admin switched us in at
       * a different negotiated size than our first activation); re-read
       * it every tick since it's cheap and this is the source of truth
       * for the buffer size we're about to write. */
      mode = pgipc_writer_negotiated_mode(ctx);
      frame_interval_s = mode.fps > 0 ? 1.0 / (double)mode.fps : 1.0 / 60.0;

      int idx = pgipc_writer_write_slot(ctx);
      if (idx >= 0) {
        render_sine_frame(ctx->ring->frame_bufs[idx], mode.width, mode.height, phase);
        pgipc_writer_publish(ctx, idx, frame_id++);
        frames_published++;
        phase += phase_speed * frame_interval_s;
        if (phase > 2.0 * M_PI)
          phase -= 2.0 * M_PI;
      }
    } else {
      frames_skipped_inactive++;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed_since_report = (double)(now.tv_sec - last_report.tv_sec) +
                                  (double)(now.tv_nsec - last_report.tv_nsec) / 1e9;
    if (elapsed_since_report >= 5.0) {
      printf("[sine_wave_cpu] status: active=%d published=%llu skipped=%llu\n", active,
             (unsigned long long)frames_published,
             (unsigned long long)frames_skipped_inactive);
      last_report = now;
    }

    /* Naively pace to the negotiated fps regardless of how long rendering took. 
     * Future (e.g., v2) implementation should have a "scheduler" of sorts that allows
     * background threads to heuristically run during idle times. Should be abstracted 
     * to be simple */
    struct timespec tick_end;
    clock_gettime(CLOCK_MONOTONIC, &tick_end);
    double tick_elapsed_s = (double)(tick_end.tv_sec - tick_start.tv_sec) +
                            (double)(tick_end.tv_nsec - tick_start.tv_nsec) / 1e9;
    double sleep_s = frame_interval_s - tick_elapsed_s;
    if (sleep_s > 0.0) {
      struct timespec ts;
      ts.tv_sec = (time_t)sleep_s;
      ts.tv_nsec = (long)((sleep_s - (double)ts.tv_sec) * 1e9);
      nanosleep(&ts, NULL);
    }
  }

  printf("[sine_wave_cpu] shutting down (published=%llu, skipped=%llu)\n",
         (unsigned long long)frames_published,
         (unsigned long long)frames_skipped_inactive);
  pgipc_writer_disconnect(ctx);
  return 0;
}
