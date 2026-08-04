// data_plane_test.cpp - GoogleTest harness proving the ring/checkout/blit path works
// end-to-end, entirely without the control-plane.
//
// Plays BOTH roles itself:
//   - the "server": creates the shm ring + frame-ready eventfd
//     (LIBPGDP_SERVER symbols) and runs the real data-plane loop against a
//     file-backed sink (fb_sink_file.c), so every frame lands as a viewable
//     PPM.
//   - a "fake client": bypasses the control protocol entirely and drives
//     the shm ring's low-level client primitives directly
//     (pgdpc_write_slot/publish, LIBPGDP_CLIENT symbols) to
//     push a moving sine wave, then writes to the frame-ready eventfd itself
//     (pgdpc__shm_ring_publish() does not do this on its own -- that's
//     normally pgdpc_publish()'s job, but that requires a live
//     control-socket session, which this test deliberately has none of).
#define LIBPGDP_SERVER
#define LIBPGDP_CLIENT
#define LIBPGDP_IMPLEMENTATION
#define LIBPGDP_NO_SIDE_WARNING
#include "libpgdp.h"

#include "dataplane/data_plane.h"
#include "dataplane/fb_sink_file.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace {

constexpr uint32_t kWidth = 320;
constexpr uint32_t kHeight = 240;
constexpr int kFps = 60;
constexpr int kDurationSec = 2; // short enough to keep `ctest --repeat` cheap
constexpr double kPi = 3.14159265358979323846;
constexpr const char *kOutDir = "./pgdp_test_frames";

void SetPixelXrgb(unsigned char *buf, uint32_t width, uint32_t x, uint32_t y,
                  unsigned char r, unsigned char g, unsigned char b) {
  unsigned char *px =
      buf + (static_cast<size_t>(y) * width + x) * PGDP_BYTES_PER_PIXEL;
  px[0] = b;
  px[1] = g;
  px[2] = r;
  px[3] = 0xFF;
}

// Draws a moving cyan sine wave over a dark x-gradient background, same
// visual as the original harness (manually verified once via PPM->PNG
// inspection during initial development -- see checkpoint history).
void RenderSineFrame(unsigned char *buf, uint32_t width, uint32_t height,
                     double phase) {
  double amplitude = height * 0.3;
  double wavelength = width / 2.0;
  double mid = height / 2.0;

  for (uint32_t x = 0; x < width; x++) {
    double wave_y =
        mid + amplitude * sin(2.0 * kPi * static_cast<double>(x) / wavelength + phase);
    unsigned char bg_r = static_cast<unsigned char>(x * 255 / width);
    unsigned char bg_b = static_cast<unsigned char>(255 - bg_r);

    for (uint32_t y = 0; y < height; y++) {
      double d = fabs(static_cast<double>(y) - wave_y);
      if (d < 4.0)
        SetPixelXrgb(buf, width, x, y, 40, 220, 255);
      else
        SetPixelXrgb(buf, width, x, y, static_cast<unsigned char>(bg_r / 6), 10,
                     static_cast<unsigned char>(bg_b / 6));
    }
  }
}

class DataPlaneTest : public ::testing::Test {
protected:
  void SetUp() override {
    ring_ = pgdps_shm_ring_create();
    ASSERT_NE(ring_, nullptr) << "shm ring create failed -- is /dev/shm writable?";
    frame_fd_ = pgdps_frame_fd_create();
    ASSERT_GE(frame_fd_, 0);
  }

  void TearDown() override {
    if (frame_fd_ >= 0)
      close(frame_fd_);
    if (ring_)
      pgdps_shm_ring_destroy(ring_);
  }

  pgdp_shm_ring_t *ring_ = nullptr;
  int frame_fd_ = -1;
};

TEST_F(DataPlaneTest, SineWaveRendersWithZeroDropsAndProducesViewableFrames) {
  pgdps_fb_sink_t sink;
  pgdps_fb_sink_file_opts_t sink_opts{};
  sink_opts.out_dir = kOutDir;
  sink_opts.snapshot_interval = kFps; // one numbered snapshot per second
  sink_opts.max_snapshots = 10;
  ASSERT_EQ(pgdps_fb_sink_file_create(&sink, &sink_opts), 0);

  pgdps_data_plane_t dp;
  ASSERT_EQ(pgdps_data_plane_init(&dp, ring_, frame_fd_, &sink, kWidth, kHeight), 0);

  pthread_t dp_thread;
  ASSERT_EQ(pthread_create(&dp_thread, nullptr, pgdps_data_plane_run, &dp), 0);

  struct timespec frame_period = {0, 1000000000L / kFps};
  double phase = 0.0;
  uint64_t frame_id = 0;
  const int total_frames = kFps * kDurationSec;

  struct _pgdpc_ctx_t fake_ctx = {};
  fake_ctx.ring = ring_;
  pgdp_atomic_store(&fake_ctx.active, true);
  pgdp_atomic_store(&fake_ctx.granted_generation, 1);
  pgdp_atomic_store(&fake_ctx.ring->generation, 1);

  for (int i = 0; i < total_frames; i++) {
    int slot = pgdpc_write_slot(&fake_ctx);
    RenderSineFrame(ring_->frame_bufs[slot], kWidth, kHeight, phase);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    ASSERT_GE(pgdpc_publish(&fake_ctx, slot, frame_id), 0);
    
    frame_id++;
    uint64_t v = 1;
    write(frame_fd_, &v, sizeof(v));

    phase += 0.15;
    nanosleep(&frame_period, nullptr);
  }

  pgdps_data_plane_stop(&dp);
  pthread_join(dp_thread, nullptr);
  pgdps_fb_sink_close(&sink);
  close(dp.abort_fd);
  close(dp.epoll_fd);

  uint64_t rendered = atomic_load(&dp.stats.frames_rendered);
  uint64_t dropped = atomic_load(&dp.stats.frames_dropped);
  uint64_t blit_errors = atomic_load(&dp.stats.blit_errors);

  EXPECT_GT(rendered, 0u);
  EXPECT_EQ(dropped, 0u) << "no client contention in this test -- any drop is a bug";
  EXPECT_EQ(blit_errors, 0u);

  // Confirm the sink actually produced a viewable, correctly-sized frame.
  std::string latest_path = std::string(kOutDir) + "/latest.ppm";
  struct stat st;
  ASSERT_EQ(stat(latest_path.c_str(), &st), 0)
      << "expected " << latest_path << " to exist after the run";

  FILE *f = fopen(latest_path.c_str(), "rb");
  ASSERT_NE(f, nullptr);
  char magic[3] = {};
  int w = 0, h = 0, maxval = 0;
  ASSERT_EQ(fscanf(f, "%2s %d %d %d", magic, &w, &h, &maxval), 4);
  fclose(f);
  EXPECT_STREQ(magic, "P6");
  EXPECT_EQ(w, static_cast<int>(kWidth));
  EXPECT_EQ(h, static_cast<int>(kHeight));
}

} // namespace
