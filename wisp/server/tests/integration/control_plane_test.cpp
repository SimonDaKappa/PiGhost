// control_plane_test.cpp - GoogleTest end-to-end test for control_plane.c,
// exercising it against a real fake-producer speaking raw wire-protocol
// messages (wisp/wire.h) and a real admin client dialing WISPS_ADMIN_SOCK_PATH.
//
// Unlike admin_plane_test.cpp's FakeControlThread, this drives the actual
// control_plane.c poll() loop: real CONNECT/MODE negotiation, real
// ACTIVATE_REQUEST/GRANT, real session table, real LIST/SWITCH answering.
//
// Deliberately does NOT link wispc: the fake producer here plays its role by
// hand-crafting wire messages via wisp_ctrl_send()/wisp_ctrl_recv_fds() directly,
// mirroring what the real client SDK does internally, so this suite only depends on
// wisp_protocol + wisp_server_core (matching the server's own dependency shape).
#include "admin/admin_plane.h"
#include "control/control_plane.h"
#include "control/control_query.h"
#include <wisp/wire.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

namespace {

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

  for (int attempt = 0; attempt < 50; attempt++) {
    if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0)
      return fd;
    struct timespec retry_delay = {0, 10000000L}; // 10ms
    nanosleep(&retry_delay, nullptr);
  }
  close(fd);
  return -1;
}

// FakeProducer - minimal hand-rolled producer stand-in that speaks the wire protocol
// directly (wisp_ctrl_send/recv), mirroring what the real client SDK's control thread
// does internally, without linking wispc at all.
struct FakeProducer {
  int fd = -1;
  std::atomic<bool> active{false};
  std::atomic<bool> running{false};
  wisp_render_mode_t negotiated_mode{};
  pthread_t thread{};
};

// Background loop: after the initial synchronous CONNECT/MODE/ACTIVATE handshake,
// keeps reading for async GRANT/DEACTIVATE messages (e.g. triggered by an admin
// SWITCH) so tests can poll FakeProducer::active from the main thread. Any fd
// attached to a GRANT (the frame-ready eventfd) is silently discarded by the kernel
// since this reads via wisp_ctrl_recv() rather than wisp_ctrl_recv_fds() -- this
// suite never needs to actually publish frames.
void *FakeProducerLoop(void *arg) {
  auto *fp = static_cast<FakeProducer *>(arg);
  struct timeval tv = {0, 50 * 1000}; // 50ms
  setsockopt(fp->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  while (fp->running.load()) {
    wisp_msg_kind_t kind;
    unsigned char buf[256];
    uint32_t len;
    int rc = wisp_ctrl_recv(fp->fd, &kind, buf, sizeof(buf), &len);
    if (rc != 0)
      continue; // timeout or transient error

    if (kind == WISP_MSG_ACTIVATE_GRANT) {
      // Mirrors wispc_handle_grant()'s immediate READY_FOR_RT: this fake producer
      // has no warmup/self-promotion logic of its own, same TODO as client_control.c.
      wisp_ctrl_send(fp->fd, WISP_MSG_READY_FOR_RT, nullptr, 0);
      fp->active.store(true);
    } else if (kind == WISP_MSG_DEACTIVATE) {
      fp->active.store(false);
    } else if (kind == WISP_MSG_EVICT_PENDING) {
      // Mirrors client_control.c's immediate STOPPED ack: no real wind-down here
      // either.
      fp->active.store(false);
      wisp_ctrl_send(fp->fd, WISP_MSG_STOPPED, nullptr, 0);
    }
  }
  return nullptr;
}

// Connects to WISPS_CONTROL_SOCK_PATH, performs the CONNECT/MODE negotiation and the
// initial ACTIVATE_REQUEST/GRANT-or-DENY handshake synchronously (mirroring
// wispc_connect()'s shape, minus the shm ring attach this suite doesn't need), then
// spawns a background thread to track subsequent async GRANT/DEACTIVATE messages.
// Returns nullptr (never asserts) on any failure so callers can ASSERT_NE with a clear
// gtest failure message.
FakeProducer *FakeProducerConnect(const char *client_id, const wisp_render_mode_t *modes,
                                  int num_modes) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return nullptr;

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, WISPS_CONTROL_SOCK_PATH, sizeof(addr.sun_path) - 1);

  bool connected = false;
  for (int attempt = 0; attempt < 100; attempt++) {
    if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0) {
      connected = true;
      break;
    }
    struct timespec retry_delay = {0, 10000000L}; // 10ms
    nanosleep(&retry_delay, nullptr);
  }
  if (!connected) {
    close(fd);
    return nullptr;
  }

  wisp_connect_msg_t connect_msg{};
  connect_msg.protocol_magic = WISP_PROTOCOL_MAGIC;
  connect_msg.protocol_major = WISP_PROTOCOL_VERSION_MAJOR;
  connect_msg.protocol_minor = WISP_PROTOCOL_VERSION_MINOR;
  strncpy(connect_msg.client_id, client_id, WISP_CLIENT_ID_LEN - 1);
  connect_msg.num_modes =
      static_cast<uint32_t>(num_modes > WISP_MAX_MODES ? WISP_MAX_MODES : num_modes);
  for (uint32_t i = 0; i < connect_msg.num_modes; i++)
    connect_msg.modes[i] = modes[i];

  if (wisp_ctrl_send(fd, WISP_MSG_CONNECT, &connect_msg, sizeof(connect_msg)) != 0) {
    close(fd);
    return nullptr;
  }

  auto *fp = new FakeProducer();
  fp->fd = fd;

  wisp_msg_kind_t kind;
  unsigned char buf[256];
  uint32_t len;

  if (wisp_ctrl_recv(fd, &kind, buf, sizeof(buf), &len) == 0 && kind == WISP_MSG_MODE) {
    wisp_mode_msg_t mode;
    memcpy(&mode, buf, sizeof(mode));
    if (!mode.accepted) {
      close(fd);
      delete fp;
      return nullptr;
    }
    fp->negotiated_mode = mode.chosen;
  }

  if (wisp_ctrl_send(fd, WISP_MSG_ACTIVATE_REQUEST, nullptr, 0) != 0) {
    close(fd);
    delete fp;
    return nullptr;
  }

  if (wisp_ctrl_recv(fd, &kind, buf, sizeof(buf), &len) == 0 &&
      kind == WISP_MSG_ACTIVATE_GRANT) {
    // Mirrors wispc_handle_grant()'s immediate READY_FOR_RT -- see the TODO on that
    // function; this fake producer has no real warmup logic either.
    wisp_ctrl_send(fd, WISP_MSG_READY_FOR_RT, nullptr, 0);
    fp->active.store(true);
  }
  /* WISP_MSG_ACTIVATE_QUEUED or WISP_MSG_ACTIVATE_DENY: stays inactive. The
   * background thread below will pick up the eventual GRANT once the RT slot frees
   * up (queued) or this producer is admin-switched to active (denied). */

  fp->running.store(true);
  pthread_create(&fp->thread, nullptr, FakeProducerLoop, fp);
  return fp;
}

// Tears down a session opened by FakeProducerConnect(): stops the background thread,
// sends WISP_MSG_DISCONNECT, closes the socket, and frees @fp (mirrors
// wispc_disconnect()'s contract -- @fp must not be used again).
void FakeProducerDisconnect(FakeProducer *fp) {
  if (!fp)
    return;
  fp->running.store(false);
  pthread_join(fp->thread, nullptr);
  wisp_ctrl_send(fp->fd, WISP_MSG_DISCONNECT, nullptr, 0);
  close(fp->fd);
  delete fp;
}

class ControlPlaneTest : public ::testing::Test {
protected:
  void SetUp() override {
    ring_ = wisps_shm_ring_create();
    ASSERT_NE(ring_, nullptr);
    frame_fd_ = wisps_shm_frame_fd_create();
    ASSERT_GE(frame_fd_, 0);

    ASSERT_EQ(wisps_control_query_channel_init(&chan_), 0);
    ASSERT_EQ(wisps_admin_plane_init(&ap_, &chan_), 0);

    wisp_render_mode_t modes[1] = {{320, 240, 60}};
    ASSERT_EQ(wisps_control_plane_init(&cp_, ring_, frame_fd_, nullptr, &chan_, modes, 1),
              0);

    ASSERT_EQ(pthread_create(&control_thread_, nullptr, wisps_control_plane_run, &cp_),
              0);
    ASSERT_EQ(pthread_create(&admin_thread_, nullptr, wisps_admin_plane_run, &ap_), 0);
  }

  void TearDown() override {
    wisps_admin_plane_stop(&ap_);
    pthread_join(admin_thread_, nullptr);
    wisps_admin_plane_close(&ap_);

    wisps_control_plane_stop(&cp_);
    pthread_join(control_thread_, nullptr);
    wisps_control_plane_close(&cp_);

    wisps_control_query_channel_close(&chan_);
    wisps_shm_ring_destroy(ring_);
    if (frame_fd_ >= 0)
      close(frame_fd_);
  }

  wisp_shm_ring_t *ring_ = nullptr;
  int frame_fd_ = -1;
  wisps_control_query_channel_t chan_;
  wisps_admin_plane_t ap_;
  wisps_control_plane_t cp_;
  pthread_t control_thread_;
  pthread_t admin_thread_;
};

TEST_F(ControlPlaneTest, ClientConnectsAndActivatesImmediately) {
  wisp_render_mode_t modes[1] = {{320, 240, 60}};
  FakeProducer *w = FakeProducerConnect("sine_wave_cpu", modes, 1);
  ASSERT_NE(w, nullptr);
  EXPECT_TRUE(w->active.load());
  EXPECT_EQ(w->negotiated_mode.width, 320u);
  FakeProducerDisconnect(w);
}

TEST_F(ControlPlaneTest, AdminListReflectsRealConnectedClient) {
  wisp_render_mode_t modes[1] = {{320, 240, 60}};
  FakeProducer *w = FakeProducerConnect("sine_wave_cpu", modes, 1);
  ASSERT_NE(w, nullptr);

  int fd = AdminClientConnect();
  ASSERT_GE(fd, 0);
  ASSERT_EQ(wisp_ctrl_send(fd, static_cast<wisp_msg_kind_t>(WISPS_ADMIN_MSG_LIST_REQUEST),
                            nullptr, 0),
            0);

  unsigned char buf[4096];
  wisp_msg_kind_t type;
  uint32_t len;
  ASSERT_EQ(wisp_ctrl_recv(fd, &type, buf, sizeof(buf), &len), 0);
  EXPECT_EQ(static_cast<wisps_admin_msg_type_t>(type), WISPS_ADMIN_MSG_LIST_RESPONSE);

  wisps_admin_list_response_t resp;
  memcpy(&resp, buf, sizeof(resp));
  ASSERT_EQ(resp.count, 1u);
  EXPECT_STREQ(resp.clients[0].client_id, "sine_wave_cpu");
  EXPECT_EQ(resp.clients[0].state, WISPS_ADMIN_STATE_ACTIVE_RT);
  EXPECT_EQ(resp.clients[0].negotiated_mode.width, 320u);

  close(fd);
  FakeProducerDisconnect(w);
}

TEST_F(ControlPlaneTest, AdminSwitchBetweenTwoClientsSucceeds) {
  wisp_render_mode_t modes[1] = {{320, 240, 60}};
  FakeProducer *w1 = FakeProducerConnect("app_one", modes, 1);
  ASSERT_NE(w1, nullptr);
  FakeProducer *w2 = FakeProducerConnect("app_two", modes, 1);
  ASSERT_NE(w2, nullptr);

  // w1 was first, so it's still active; w2 only negotiated.
  EXPECT_TRUE(w1->active.load());

  int fd = AdminClientConnect();
  ASSERT_GE(fd, 0);

  wisps_admin_switch_request_t req{};
  strncpy(req.client_id, "app_two", WISP_CLIENT_ID_LEN - 1);
  ASSERT_EQ(wisp_ctrl_send(fd, static_cast<wisp_msg_kind_t>(WISPS_ADMIN_MSG_SWITCH_REQUEST),
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

  // Give the fake producers' background threads a moment to process their
  // DEACTIVATE/GRANT messages.
  struct timespec wait_ts = {0, 200000000L}; // 200ms
  nanosleep(&wait_ts, nullptr);

  EXPECT_TRUE(w2->active.load());
  EXPECT_FALSE(w1->active.load());

  FakeProducerDisconnect(w1);
  FakeProducerDisconnect(w2);
}

TEST_F(ControlPlaneTest, AdminSwitchForUnknownAppFails) {
  int fd = AdminClientConnect();
  ASSERT_GE(fd, 0);

  wisps_admin_switch_request_t req{};
  strncpy(req.client_id, "nonexistent_app", WISP_CLIENT_ID_LEN - 1);
  ASSERT_EQ(wisp_ctrl_send(fd, static_cast<wisp_msg_kind_t>(WISPS_ADMIN_MSG_SWITCH_REQUEST),
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
