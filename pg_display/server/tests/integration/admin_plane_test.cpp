// admin_plane_test.cpp - GoogleTest end-to-end smoke test for admin_plane.c +
// control_query.c.
//
// Plays two stand-in roles since control_plane.c doesn't exist yet:
//   - a "fake control thread": a tiny poll() loop that drains queries from
//     the channel and answers them with canned data (one fake NEGOTIATED
//     client "sine_wave_cpu", and a SWITCH_REQUEST outcome that succeeds
//     only for that exact app_id) -- stands in for what control_plane.c
//     will eventually do against the real session table.
//   - a "fake admin client": connects to PGIPC_ADMIN_SOCK_PATH exactly the
//     way a future CLI/orchestrator would, sends real wire-format
//     requests, and asserts on the real wire-format responses.
#define LIBPGIPC_READER
#include "admin/admin_plane.h"
#include "control/control_query.h"

#include <gtest/gtest.h>

#include <cstring>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

namespace {

atomic_bool g_fake_control_running;

// Stands in for control_plane.c's future poll() loop; only handles the one
// wake fd this test cares about.
void *FakeControlThread(void *arg) {
  auto *chan = static_cast<pgipc_control_query_channel_t *>(arg);
  int wake_fd = pgipc_control_query_channel_wake_fd(chan);

  struct pollfd pfd = {.fd = wake_fd, .events = POLLIN, .revents = 0};

  while (atomic_load(&g_fake_control_running)) {
    int rc = poll(&pfd, 1, 100 /* ms */);
    if (rc <= 0)
      continue;

    pgipc_control_query_t *q = pgipc_control_query_channel_drain(chan);
    if (!q)
      continue;

    if (q->type == PGIPC_CTRL_QUERY_LIST) {
      q->list_response.count = 1;
      strncpy(q->list_response.clients[0].app_id, "sine_wave_cpu",
              PGIPC_APP_ID_LEN - 1);
      q->list_response.clients[0].state = PGIPC_ADMIN_STATE_NEGOTIATED;
      q->list_response.clients[0].negotiated_mode =
          pgipc_render_mode_t{.width = 320, .height = 240, .fps = 60};
      q->list_response.clients[0].payload_kind = PGIPC_PAYLOAD_PIXELS;
    } else { // PGIPC_CTRL_QUERY_SWITCH
      if (strcmp(q->switch_app_id, "sine_wave_cpu") == 0) {
        q->switch_response.ok = 1;
        q->switch_response.reason[0] = '\0';
      } else {
        q->switch_response.ok = 0;
        strncpy(q->switch_response.reason, "app not connected",
                PGIPC_ADMIN_REASON_LEN - 1);
      }
    }

    pgipc_control_query_complete(q);
  }

  return nullptr;
}

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

  // Short retry loop: the admin thread's listen() may not have happened
  // yet the instant the test thread starts.
  for (int attempt = 0; attempt < 50; attempt++) {
    if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0)
      return fd;
    struct timespec retry_delay = {0, 10000000L}; // 10ms
    nanosleep(&retry_delay, nullptr);
  }
  close(fd);
  return -1;
}

class AdminPlaneTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_EQ(pgipc_control_query_channel_init(&chan_), 0);
    ASSERT_EQ(pgipc_admin_plane_init(&ap_, &chan_), 0);

    atomic_store(&g_fake_control_running, true);
    ASSERT_EQ(pthread_create(&control_thread_, nullptr, FakeControlThread, &chan_), 0);
    ASSERT_EQ(pthread_create(&admin_thread_, nullptr, pgipc_admin_plane_run, &ap_), 0);
  }

  void TearDown() override {
    atomic_store(&g_fake_control_running, false);
    pthread_join(control_thread_, nullptr);

    pgipc_admin_plane_stop(&ap_);
    pthread_join(admin_thread_, nullptr);
    pgipc_admin_plane_close(&ap_);
    pgipc_control_query_channel_close(&chan_);
  }

  pgipc_control_query_channel_t chan_;
  pgipc_admin_plane_t ap_;
  pthread_t control_thread_;
  pthread_t admin_thread_;
};

TEST_F(AdminPlaneTest, ListRequestReturnsSnapshotFromControlThread) {
  int fd = AdminClientConnect();
  ASSERT_GE(fd, 0);
  ASSERT_EQ(pgipc_ctrl_send(fd,
                            static_cast<pgipc_msg_type_t>(PGIPC_ADMIN_MSG_LIST_REQUEST),
                            nullptr, 0),
            0);

  unsigned char buf[4096];
  pgipc_msg_type_t type;
  uint32_t len;
  ASSERT_EQ(pgipc_ctrl_recv(fd, &type, buf, sizeof(buf), &len), 0);
  EXPECT_EQ(static_cast<pgipc_admin_msg_type_t>(type), PGIPC_ADMIN_MSG_LIST_RESPONSE);
  ASSERT_EQ(len, sizeof(pgipc_admin_list_response_t));

  pgipc_admin_list_response_t resp;
  memcpy(&resp, buf, sizeof(resp));
  EXPECT_EQ(resp.count, 1u);
  EXPECT_STREQ(resp.clients[0].app_id, "sine_wave_cpu");
  EXPECT_EQ(resp.clients[0].state, PGIPC_ADMIN_STATE_NEGOTIATED);
  EXPECT_EQ(resp.clients[0].negotiated_mode.width, 320u);

  close(fd);
}

TEST_F(AdminPlaneTest, SwitchRequestForKnownAppSucceeds) {
  int fd = AdminClientConnect();
  ASSERT_GE(fd, 0);

  pgipc_admin_switch_request_t req{};
  strncpy(req.app_id, "sine_wave_cpu", PGIPC_APP_ID_LEN - 1);
  ASSERT_EQ(
      pgipc_ctrl_send(fd, static_cast<pgipc_msg_type_t>(PGIPC_ADMIN_MSG_SWITCH_REQUEST),
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
}

TEST_F(AdminPlaneTest, SwitchRequestForUnknownAppFailsWithReason) {
  int fd = AdminClientConnect();
  ASSERT_GE(fd, 0);

  pgipc_admin_switch_request_t req{};
  strncpy(req.app_id, "nonexistent_app", PGIPC_APP_ID_LEN - 1);
  ASSERT_EQ(
      pgipc_ctrl_send(fd, static_cast<pgipc_msg_type_t>(PGIPC_ADMIN_MSG_SWITCH_REQUEST),
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
