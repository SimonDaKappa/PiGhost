// control_plane_test.cpp - GoogleTest end-to-end test for control_plane.c,
// exercising it against a real producer via pgipc_writer_connect() (writer
// side of libpgipc.h) and a real admin client dialing PGIPC_ADMIN_SOCK_PATH.
//
// Unlike admin_plane_test.cpp's FakeControlThread, this drives the actual
// control_plane.c poll() loop: real CONNECT/MODE negotiation, real
// ACTIVATE_REQUEST/GRANT, real session table, real LIST/SWITCH answering.
// Both sides of libpgipc.h are needed in this TU: control_plane.h requires
// LIBPGIPC_READER (display-side shm ring create/evict), while this test also
// drives a real producer via pgipc_writer_connect() (LIBPGIPC_WRITER). Both
// must be defined before libpgipc.h's *first* #include -- it has a single
// top-level include guard, so a second #include after only flipping
// LIBPGIPC_WRITER on would be a silent no-op.
#define LIBPGIPC_READER
#define LIBPGIPC_WRITER
#include "admin/admin_plane.h"
#include "control/control_plane.h"
#include "control/control_query.h"
#include "libpgipc.h"

#include <gtest/gtest.h>

#include <cstring>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

namespace {

// Dials PGIPC_ADMIN_SOCK_PATH, as a real admin client (CLI/orchestrator)
// would. Returns -1 (never asserts) so callers can ASSERT_GE with a clear
// gtest failure message instead of a bare abort().
int AdminClientConnect() {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, PGIPC_ADMIN_SOCK_PATH, sizeof(addr.sun_path) - 1);

  for (int attempt = 0; attempt < 50; attempt++) {
    if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0)
      return fd;
    struct timespec retry_delay = {0, 10000000L}; // 10ms
    nanosleep(&retry_delay, nullptr);
  }
  close(fd);
  return -1;
}

class ControlPlaneTest : public ::testing::Test {
protected:
  void SetUp() override {
    ring_ = pgipc_shm_ring_create();
    ASSERT_NE(ring_, nullptr);
    frame_fd_ = pgipc_frame_fd_create();
    ASSERT_GE(frame_fd_, 0);

    ASSERT_EQ(pgipc_control_query_channel_init(&chan_), 0);
    ASSERT_EQ(pgipc_admin_plane_init(&ap_, &chan_), 0);

    pgipc_render_mode_t modes[1] = {{320, 240, 60}};
    ASSERT_EQ(pgipc_control_plane_init(&cp_, ring_, frame_fd_, nullptr, &chan_, modes, 1),
              0);

    ASSERT_EQ(pthread_create(&control_thread_, nullptr, pgipc_control_plane_run, &cp_),
              0);
    ASSERT_EQ(pthread_create(&admin_thread_, nullptr, pgipc_admin_plane_run, &ap_), 0);
  }

  void TearDown() override {
    pgipc_admin_plane_stop(&ap_);
    pthread_join(admin_thread_, nullptr);
    pgipc_admin_plane_close(&ap_);

    pgipc_control_plane_stop(&cp_);
    pthread_join(control_thread_, nullptr);
    pgipc_control_plane_close(&cp_);

    pgipc_control_query_channel_close(&chan_);
    pgipc_shm_ring_destroy(ring_);
    if (frame_fd_ >= 0)
      close(frame_fd_);
  }

  pgipc_shm_ring_t *ring_ = nullptr;
  int frame_fd_ = -1;
  pgipc_control_query_channel_t chan_;
  pgipc_admin_plane_t ap_;
  pgipc_control_plane_t cp_;
  pthread_t control_thread_;
  pthread_t admin_thread_;
};

TEST_F(ControlPlaneTest, WriterConnectsAndActivatesImmediately) {
  pgipc_render_mode_t modes[1] = {{320, 240, 60}};
  pgipc_writer_ctx_t *w = pgipc_writer_connect("sine_wave_cpu", modes, 1);
  ASSERT_NE(w, nullptr);
  EXPECT_TRUE(pgipc_writer_is_active(w));
  EXPECT_EQ(pgipc_writer_negotiated_mode(w).width, 320u);
  pgipc_writer_disconnect(w);
}

TEST_F(ControlPlaneTest, AdminListReflectsRealConnectedWriter) {
  pgipc_render_mode_t modes[1] = {{320, 240, 60}};
  pgipc_writer_ctx_t *w = pgipc_writer_connect("sine_wave_cpu", modes, 1);
  ASSERT_NE(w, nullptr);

  int fd = AdminClientConnect();
  ASSERT_GE(fd, 0);
  ASSERT_EQ(pgipc_ctrl_send(fd, static_cast<pgipc_msg_type_t>(PGIPC_ADMIN_MSG_LIST_REQUEST),
                            nullptr, 0),
            0);

  unsigned char buf[4096];
  pgipc_msg_type_t type;
  uint32_t len;
  ASSERT_EQ(pgipc_ctrl_recv(fd, &type, buf, sizeof(buf), &len), 0);
  EXPECT_EQ(static_cast<pgipc_admin_msg_type_t>(type), PGIPC_ADMIN_MSG_LIST_RESPONSE);

  pgipc_admin_list_response_t resp;
  memcpy(&resp, buf, sizeof(resp));
  ASSERT_EQ(resp.count, 1u);
  EXPECT_STREQ(resp.clients[0].app_id, "sine_wave_cpu");
  EXPECT_EQ(resp.clients[0].state, PGIPC_ADMIN_STATE_ACTIVE);
  EXPECT_EQ(resp.clients[0].negotiated_mode.width, 320u);

  close(fd);
  pgipc_writer_disconnect(w);
}

TEST_F(ControlPlaneTest, AdminSwitchBetweenTwoWritersSucceeds) {
  pgipc_render_mode_t modes[1] = {{320, 240, 60}};
  pgipc_writer_ctx_t *w1 = pgipc_writer_connect("app_one", modes, 1);
  ASSERT_NE(w1, nullptr);
  pgipc_writer_ctx_t *w2 = pgipc_writer_connect("app_two", modes, 1);
  ASSERT_NE(w2, nullptr);

  // w1 was first, so it's still active; w2 only negotiated.
  EXPECT_TRUE(pgipc_writer_is_active(w1));

  int fd = AdminClientConnect();
  ASSERT_GE(fd, 0);

  pgipc_admin_switch_request_t req{};
  strncpy(req.app_id, "app_two", PGIPC_APP_ID_LEN - 1);
  ASSERT_EQ(pgipc_ctrl_send(fd, static_cast<pgipc_msg_type_t>(PGIPC_ADMIN_MSG_SWITCH_REQUEST),
                            &req, sizeof(req)),
            0);

  unsigned char buf[4096];
  pgipc_msg_type_t type;
  uint32_t len;
  ASSERT_EQ(pgipc_ctrl_recv(fd, &type, buf, sizeof(buf), &len), 0);
  EXPECT_EQ(static_cast<pgipc_admin_msg_type_t>(type), PGIPC_ADMIN_MSG_SWITCH_RESPONSE);

  pgipc_admin_switch_response_t resp;
  memcpy(&resp, buf, sizeof(resp));
  EXPECT_EQ(resp.ok, 1);
  close(fd);

  // Give the writer ctrl threads a moment to process their DEACTIVATE/GRANT messages.
  struct timespec wait_ts = {0, 200000000L}; // 200ms
  nanosleep(&wait_ts, nullptr);

  EXPECT_TRUE(pgipc_writer_is_active(w2));
  EXPECT_FALSE(pgipc_writer_is_active(w1));

  pgipc_writer_disconnect(w1);
  pgipc_writer_disconnect(w2);
}

TEST_F(ControlPlaneTest, AdminSwitchForUnknownAppFails) {
  int fd = AdminClientConnect();
  ASSERT_GE(fd, 0);

  pgipc_admin_switch_request_t req{};
  strncpy(req.app_id, "nonexistent_app", PGIPC_APP_ID_LEN - 1);
  ASSERT_EQ(pgipc_ctrl_send(fd, static_cast<pgipc_msg_type_t>(PGIPC_ADMIN_MSG_SWITCH_REQUEST),
                            &req, sizeof(req)),
            0);

  unsigned char buf[4096];
  pgipc_msg_type_t type;
  uint32_t len;
  ASSERT_EQ(pgipc_ctrl_recv(fd, &type, buf, sizeof(buf), &len), 0);

  pgipc_admin_switch_response_t resp;
  memcpy(&resp, buf, sizeof(resp));
  EXPECT_EQ(resp.ok, 0);
  EXPECT_GT(strlen(resp.reason), 0u);

  close(fd);
}

} // namespace
