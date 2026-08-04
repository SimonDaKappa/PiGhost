// session_table_test.cpp - GoogleTest port of the original
// test_session_table.c assertions, against SPEC.md §3-4.
//
// A pure unit test: no threads, no sockets, no shm -- just the
// session_table.c state machine in isolation. Fast enough to run on every
// build; contrast with tests/integration/, which exercises real threads
// and IPC and is expected to be slower and occasionally sanitizer-checked
// (see tests/CMakeLists.txt's PGDP_SANITIZE wiring).
#define LIBPGDP_SERVER
#include "session/session_table.h"

#include <gtest/gtest.h>

#include <cstring>

namespace {

pgdp_render_mode_t Mode(uint32_t w, uint32_t h, uint32_t fps) {
  return pgdp_render_mode_t{.width = w, .height = h, .fps = fps};
}

class SessionTableTest : public ::testing::Test {
protected:
  void SetUp() override { pgdps_session_table_init(&table_); }

  pgdps_session_table_t table_;
};

TEST_F(SessionTableTest, InitStartsEmptyWithNoActiveSlot) {
  EXPECT_EQ(table_.active_slot, -1);
}

TEST_F(SessionTableTest, AddFillsTableThenRejectsOverflow) {
  int slots[PGDPS_SESSION_MAX_CLIENTS];
  for (int i = 0; i < PGDPS_SESSION_MAX_CLIENTS; i++) {
    slots[i] = pgdps_session_table_add(&table_, 100 + i);
    ASSERT_EQ(slots[i], i);
    EXPECT_EQ(table_.slots[i].state, PGDPS_SESSION_CONNECTED);
  }
  EXPECT_EQ(pgdps_session_table_add(&table_, 999), -1);
}

TEST_F(SessionTableTest, RemoveFreesSlotWithoutLeakingStaleFields) {
  int idx = pgdps_session_table_add(&table_, 42);
  ASSERT_GE(idx, 0);
  strncpy(table_.slots[idx].client_id, "stale_app", PGDP_CLIENT_ID_LEN - 1);

  pgdps_session_table_remove(&table_, idx);
  EXPECT_FALSE(table_.slots[idx].in_use);

  int reused = pgdps_session_table_add(&table_, 500);
  EXPECT_EQ(reused, idx);
  EXPECT_EQ(table_.slots[idx].client_id[0], '\0')
      << "previous occupant's client_id must not leak into a reused slot";
}

TEST_F(SessionTableTest, FindByAppIdLocatesNegotiatedClient) {
  int idx = pgdps_session_table_add(&table_, 10);
  ASSERT_GE(idx, 0);
  strncpy(table_.slots[idx].client_id, "sine_wave_cpu", PGDP_CLIENT_ID_LEN - 1);
  table_.slots[idx].offered_modes[0] = Mode(320, 240, 60);
  table_.slots[idx].num_offered_modes = 1;
  table_.slots[idx].negotiated_mode = Mode(320, 240, 60);
  table_.slots[idx].state = PGDPS_SESSION_NEGOTIATED;

  EXPECT_EQ(pgdps_session_table_find_by_client_id(&table_, "sine_wave_cpu"), idx);
  EXPECT_EQ(pgdps_session_table_find_by_client_id(&table_, "nonexistent"), -1);
}

TEST_F(SessionTableTest, ActivateSetsActiveSlotStateAndGeneration) {
  int idx = pgdps_session_table_add(&table_, 10);
  ASSERT_GE(idx, 0);
  table_.slots[idx].state = PGDPS_SESSION_NEGOTIATED;

  pgdps_session_table_activate(&table_, idx, /*generation=*/1);

  EXPECT_EQ(table_.active_slot, idx);
  EXPECT_EQ(table_.slots[idx].state, PGDPS_SESSION_ACTIVE);
  EXPECT_EQ(table_.slots[idx].granted_generation, 1u);
}

TEST_F(SessionTableTest, ActivatingNewSlotDeactivatesPreviousActiveOne) {
  int a = pgdps_session_table_add(&table_, 10);
  int b = pgdps_session_table_add(&table_, 11);
  ASSERT_GE(a, 0);
  ASSERT_GE(b, 0);
  table_.slots[a].state = PGDPS_SESSION_NEGOTIATED;
  table_.slots[b].state = PGDPS_SESSION_NEGOTIATED;

  pgdps_session_table_activate(&table_, a, /*generation=*/1);
  pgdps_session_table_activate(&table_, b, /*generation=*/2);

  EXPECT_EQ(table_.active_slot, b);
  EXPECT_EQ(table_.slots[b].state, PGDPS_SESSION_ACTIVE);
  EXPECT_EQ(table_.slots[a].state, PGDPS_SESSION_NEGOTIATED)
      << "only one slot may be ACTIVE at a time (SPEC.md §4)";
}

TEST_F(SessionTableTest, FreshlyActivatedSlotIsNotImmediatelyTimedOut) {
  int idx = pgdps_session_table_add(&table_, 10);
  ASSERT_GE(idx, 0);
  table_.slots[idx].state = PGDPS_SESSION_NEGOTIATED;
  pgdps_session_table_activate(&table_, idx, /*generation=*/1);

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  EXPECT_EQ(pgdps_session_table_check_heartbeat_timeouts(&table_, now), -1);
}

TEST_F(SessionTableTest, StaleHeartbeatEvictsActiveSlotBackToNegotiated) {
  int idx = pgdps_session_table_add(&table_, 10);
  ASSERT_GE(idx, 0);
  table_.slots[idx].state = PGDPS_SESSION_NEGOTIATED;
  pgdps_session_table_activate(&table_, idx, /*generation=*/1);

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  struct timespec stale = now;
  stale.tv_sec -= (PGDPS_HEARTBEAT_TIMEOUT_MS / 1000) + 1;
  table_.slots[idx].last_heartbeat_monotonic = stale;

  EXPECT_EQ(pgdps_session_table_check_heartbeat_timeouts(&table_, now), idx);
  EXPECT_EQ(table_.slots[idx].state, PGDPS_SESSION_NEGOTIATED);
  EXPECT_EQ(table_.active_slot, -1);
}

TEST_F(SessionTableTest, TimeoutCheckIsNoOpWhenNoSlotIsActive) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  EXPECT_EQ(pgdps_session_table_check_heartbeat_timeouts(&table_, now), -1);
}

TEST_F(SessionTableTest, FindByFdWorksAfterChurn) {
  int idx = pgdps_session_table_add(&table_, 500);
  ASSERT_GE(idx, 0);
  EXPECT_EQ(pgdps_session_table_find_by_fd(&table_, 500), idx);
  EXPECT_EQ(pgdps_session_table_find_by_fd(&table_, 12345), -1);
}

} // namespace
