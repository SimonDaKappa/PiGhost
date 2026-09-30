// admin_plane_test.cpp - GoogleTest end-to-end smoke test for admin_plane.c +
// control_query.c.
//
// Plays two stand-in roles since control_plane.c doesn't exist yet:
//   - a "fake control thread": a tiny poll() loop that drains queries from
//     the channel and answers them with canned data (one fake NEGOTIATED
//     client "sine_wave_cpu", and a SWITCH_REQUEST outcome that succeeds
//     only for that exact client_id) -- stands in for what control_plane.c
//     will eventually do against the real session table.
//   - a "fake admin client": connects to WISPS_ADMIN_SOCK_PATH exactly the
//     way a future CLI/orchestrator would, sends real wire-format
//     requests, and asserts on the real wire-format responses.
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

WISP_ATOMIC bool g_fake_control_running;

// Stands in for control_plane.c's future poll() loop; only handles the one
// wake fd this test cares about.
void *FakeControlThread(void *arg) {
  auto *chan = static_cast<wisps_control_query_channel_t *>(arg);
  int wake_fd = wisps_control_query_channel_wake_fd(chan);

  struct pollfd pfd = {.fd = wake_fd, .events = POLLIN, .revents = 0};

  while (wisp_atomic_load(&g_fake_control_running)) {
    int rc = poll(&pfd, 1, 100 /* ms */);
    if (rc <= 0)
      continue;

    wisps_control_query_t *q = wisps_control_query_channel_drain(chan);
    if (!q)
      continue;

    if (q->type == WISPS_CTRL_QUERY_LIST) {
      q->list_response.count = 1;
      strncpy(q->list_response.clients[0].client_id, "sine_wave_cpu",
              WISP_CLIENT_ID_LEN - 1);
      q->list_response.clients[0].state = WISPS_ADMIN_STATE_NEGOTIATED;
      q->list_response.clients[0].negotiated_mode =
          wisp_resolution_t{.width = 320, .height = 240};
      q->list_response.clients[0].render_kind = WISP_PAYLOAD_PIXELS;
    } else { // WISPS_CTRL_QUERY_SWITCH
      if (strcmp(q->switch_client_id, "sine_wave_cpu") == 0) {
        q->switch_response.ok = 1;
        q->switch_response.reason[0] = '\0';
      } else {
        q->switch_response.ok = 0;
        strncpy(q->switch_response.reason, "app not connected",
                WISPS_ADMIN_REASON_LEN - 1);
      }
    }

    wisps_control_query_complete(q);
  }

  return nullptr;
}

// Dials WISPS_ADMIN_SOCK_PATH, as a real admin client (CLI/orchestrator)
// would. Returns -1 (never asserts) so callers can ASSERT_GE with a clear
// gtest failure message instead of a bare abort().
int AdminClientConnect() {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, WISPS_ADMIN_SOCK_PATH, sizeof(addr.sun_path) - 1);

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
    ASSERT_EQ(wisps_control_query_channel_init(&chan_), 0);
    ASSERT_EQ(wisps_admin_plane_init(&ap_, &chan_), 0);

    wisp_atomic_store(&g_fake_control_running, true);
    ASSERT_EQ(pthread_create(&control_thread_, nullptr, FakeControlThread, &chan_), 0);
    ASSERT_EQ(pthread_create(&admin_thread_, nullptr, wisps_admin_plane_run, &ap_), 0);
  }

  void TearDown() override {
    wisp_atomic_store(&g_fake_control_running, false);
    pthread_join(control_thread_, nullptr);

    wisps_admin_plane_stop(&ap_);
    pthread_join(admin_thread_, nullptr);
    wisps_admin_plane_close(&ap_);
    wisps_control_query_channel_close(&chan_);
  }

  wisps_control_query_channel_t chan_;
  wisps_admin_plane_t ap_;
  pthread_t control_thread_;
  pthread_t admin_thread_;
};

TEST_F(AdminPlaneTest, ListRequestReturnsSnapshotFromControlThread) {
  int fd = AdminClientConnect();
  ASSERT_GE(fd, 0);
  ASSERT_EQ(wisp_ctrl_send(fd,
                           static_cast<wisp_msg_kind_t>(WISPS_ADMIN_MSG_LIST_REQUEST),
                           nullptr, 0),
            0);

  unsigned char buf[4096];
  wisp_msg_kind_t type;
  uint32_t len;
  ASSERT_EQ(wisp_ctrl_recv(fd, &type, buf, sizeof(buf), &len), 0);
  EXPECT_EQ(static_cast<wisps_admin_msg_type_t>(type), WISPS_ADMIN_MSG_LIST_RESPONSE);
  ASSERT_EQ(len, sizeof(wisps_admin_list_response_t));

  wisps_admin_list_response_t resp;
  memcpy(&resp, buf, sizeof(resp));
  EXPECT_EQ(resp.count, 1u);
  EXPECT_STREQ(resp.clients[0].client_id, "sine_wave_cpu");
  EXPECT_EQ(resp.clients[0].state, WISPS_ADMIN_STATE_NEGOTIATED);
  EXPECT_EQ(resp.clients[0].negotiated_mode.width, 320u);

  close(fd);
}

TEST_F(AdminPlaneTest, SwitchRequestForKnownAppSucceeds) {
  int fd = AdminClientConnect();
  ASSERT_GE(fd, 0);

  wisps_admin_switch_request_t req{};
  strncpy(req.client_id, "sine_wave_cpu", WISP_CLIENT_ID_LEN - 1);
  ASSERT_EQ(wisp_ctrl_send(fd,
                           static_cast<wisp_msg_kind_t>(WISPS_ADMIN_MSG_SWITCH_REQUEST),
                           &req, sizeof(req)),
            0);

  unsigned char buf[4096];
  wisp_msg_kind_t type;
  uint32_t len;
  ASSERT_EQ(wisp_ctrl_recv(fd, &type, buf, sizeof(buf), &len), 0);
  EXPECT_EQ(static_cast<wisps_admin_msg_type_t>(type), WISPS_ADMIN_MSG_SWITCH_RESPONSE);

  wisps_admin_switch_response_t resp;
  memcpy(&resp, buf, sizeof(resp));
  EXPECT_EQ(resp.ok, 1);

  close(fd);
}

TEST_F(AdminPlaneTest, SwitchRequestForUnknownAppFailsWithReason) {
  int fd = AdminClientConnect();
  ASSERT_GE(fd, 0);

  wisps_admin_switch_request_t req{};
  strncpy(req.client_id, "nonexistent_app", WISP_CLIENT_ID_LEN - 1);
  ASSERT_EQ(wisp_ctrl_send(fd,
                           static_cast<wisp_msg_kind_t>(WISPS_ADMIN_MSG_SWITCH_REQUEST),
                           &req, sizeof(req)),
            0);

  unsigned char buf[4096];
  wisp_msg_kind_t type;
  uint32_t len;
  ASSERT_EQ(wisp_ctrl_recv(fd, &type, buf, sizeof(buf), &len), 0);

  wisps_admin_switch_response_t resp;
  memcpy(&resp, buf, sizeof(resp));
  EXPECT_EQ(resp.ok, 0);
  EXPECT_GT(strlen(resp.reason), 0u);

  close(fd);
}

} // namespace
