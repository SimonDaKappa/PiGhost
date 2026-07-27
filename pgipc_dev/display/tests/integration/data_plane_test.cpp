// data_plane_test.cpp - GoogleTest harness proving the ring/checkout/blit path works
// end-to-end, entirely without the control-plane.
//
// Plays BOTH roles itself:
//   - the "display": creates the shm ring + semaphore (LIBPGIPC_READER
//     symbols) and runs the real data-plane loop against a file-backed
//     sink (fb_sink_file.c), so every frame lands as a viewable PPM.
//   - a "fake writer": bypasses the control protocol entirely and drives
//     the shm ring's low-level writer primitives directly
//     (pgipc_shm_ring_pick_write_slot/publish, LIBPGIPC_WRITER symbols) to
//     push a moving sine wave, then posts the frame-ready semaphore itself
//     (pgipc_shm_ring_publish() does not do this on its own -- that's
//     normally pgipc_writer_publish()'s job, but that requires a live
//     control-socket session, which this test deliberately has none of).
#define LIBPGIPC_READER
#define LIBPGIPC_WRITER
#define LIBPGIPC_NO_SIDE_WARNING
#include "libpgipc.h"

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

namespace {

constexpr uint32_t kWidth = 320;
constexpr uint32_t kHeight = 240;
constexpr int kFps = 60;
constexpr int kDurationSec = 2; // short enough to keep `ctest --repeat` cheap
constexpr double kPi = 3.14159265358979323846;
constexpr const char *kOutDir = "./pgipc_test_frames";

void SetPixelXrgb(unsigned char *buf, uint32_t width, uint32_t x, uint32_t y,
                  unsigned char r, unsigned char g, unsigned char b) {
  unsigned char *px =
      buf + (static_cast<size_t>(y) * width + x) * PGIPC_BYTES_PER_PIXEL;
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
    ring_ = pgipc_shm_ring_create();
    ASSERT_NE(ring_, nullptr) << "shm ring create failed -- is /dev/shm writable?";
    fsem_ = pgipc_shm_sem_create();
    ASSERT_NE(fsem_, nullptr);
  }

  void TearDown() override {
    if (fsem_) {
      sem_close(fsem_);
      sem_unlink(PGIPC_SEM_NAME);
    }
    if (ring_)
      pgipc_shm_ring_destroy(ring_);
  }

  pgipc_shm_ring_t *ring_ = nullptr;
  sem_t *fsem_ = nullptr;
};

TEST_F(DataPlaneTest, SineWaveRendersWithZeroDropsAndProducesViewableFrames) {
  pgipc_fb_sink_t sink;
  pgipc_fb_sink_file_opts_t sink_opts{};
  sink_opts.out_dir = kOutDir;
  sink_opts.snapshot_interval = kFps; // one numbered snapshot per second
  sink_opts.max_snapshots = 10;
  ASSERT_EQ(pgipc_fb_sink_file_create(&sink, &sink_opts), 0);

  pgipc_data_plane_t dp;
  ASSERT_EQ(pgipc_data_plane_init(&dp, ring_, fsem_, &sink, kWidth, kHeight), 0);

  pthread_t dp_thread;
  ASSERT_EQ(pthread_create(&dp_thread, nullptr, pgipc_data_plane_run, &dp), 0);

  struct timespec frame_period = {0, 1000000000L / kFps};
  double phase = 0.0;
  uint64_t frame_id = 0;
  const int total_frames = kFps * kDurationSec;

  for (int i = 0; i < total_frames; i++) {
    // fake writer side: identical shape to what pgipc_writer_publish() does
    // internally, minus the control-plane generation check (there is no
    // control-plane session here to evict).
    int slot = pgipc_shm_ring_pick_write_slot(ring_);
    RenderSineFrame(ring_->frame_bufs[slot], kWidth, kHeight, phase);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    pgipc_shm_ring_publish(ring_, slot, frame_id++, now);
    sem_post(fsem_);

    phase += 0.15;
    nanosleep(&frame_period, nullptr);
  }

  pgipc_data_plane_stop(&dp);
  pthread_join(dp_thread, nullptr);
  pgipc_fb_sink_close(&sink);

  uint64_t rendered = atomic_load(&dp.stats.frames_rendered);
  uint64_t dropped = atomic_load(&dp.stats.frames_dropped);
  uint64_t blit_errors = atomic_load(&dp.stats.blit_errors);

  EXPECT_GT(rendered, 0u);
  EXPECT_EQ(dropped, 0u) << "no writer contention in this test -- any drop is a bug";
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
