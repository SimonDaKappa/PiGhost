// session_table.c - see session_table.h.
#include "session_table.h"

#include <string.h>

void wisps_session_table_init(wisps_session_table_t *table) {
  memset(table, 0, sizeof(*table));
  for (int i = 0; i < WISPS_SESSION_MAX_CLIENTS; i++) {
    table->slots[i].in_use = false;
    table->slots[i].ctrl_fd = -1;
  }
  table->active_slot = -1;
}

int wisps_session_table_add(wisps_session_table_t *table, int fd) {
  for (int i = 0; i < WISPS_SESSION_MAX_CLIENTS; i++) {
    if (!table->slots[i].in_use) {
      /* memset first: a slot may be reused after wisps_session_table_remove() and must
       * not leak the previous occupant's client_id/dmabuf_set/etc. */
      memset(&table->slots[i], 0, sizeof(table->slots[i]));
      table->slots[i].in_use = true;
      table->slots[i].ctrl_fd = fd;
      table->slots[i].state = WISPS_SESSION_CONNECTED;
      table->slots[i].payload_kind = WISP_PAYLOAD_PIXELS;
      return i;
    }
  }
  return -1;
}

int wisps_session_table_find_by_fd(wisps_session_table_t *table, int fd) {
  for (int i = 0; i < WISPS_SESSION_MAX_CLIENTS; i++) {
    if (table->slots[i].in_use && table->slots[i].ctrl_fd == fd)
      return i;
  }
  return -1;
}

int wisps_session_table_find_by_client_id(wisps_session_table_t *table,
                                          const char *client_id) {
  for (int i = 0; i < WISPS_SESSION_MAX_CLIENTS; i++) {
    if (table->slots[i].in_use &&
        strncmp(table->slots[i].client_id, client_id, WISP_CLIENT_ID_LEN) == 0)
      return i;
  }
  return -1;
}

void wisps_session_table_grant(wisps_session_table_t *table, int idx,
                               uint32_t generation) {
  table->slots[idx].state = WISPS_SESSION_GRANTED_WARMUP;
  table->slots[idx].granted_generation = generation;
}

void wisps_session_table_activate(wisps_session_table_t *table, int idx) {
  if (table->active_slot >= 0 && table->active_slot != idx)
    wisps_session_table_deactivate(table, table->active_slot);

  table->slots[idx].state = WISPS_SESSION_ACTIVE_RT;
  clock_gettime(CLOCK_MONOTONIC, &table->slots[idx].last_heartbeat_monotonic);
  table->active_slot = idx;
}

void wisps_session_table_deactivate(wisps_session_table_t *table, int idx) {
  if (table->slots[idx].state != WISPS_SESSION_ACTIVE_RT)
    return;

  table->slots[idx].state = WISPS_SESSION_NEGOTIATED;
  if (table->active_slot == idx)
    table->active_slot = -1;
}

void wisps_session_table_begin_eviction(wisps_session_table_t *table, int idx) {
  if (table->slots[idx].state != WISPS_SESSION_ACTIVE_RT)
    return;

  table->slots[idx].state = WISPS_SESSION_EVICTING_COOPERATIVE;
  if (table->active_slot == idx)
    table->active_slot = -1;
}

void wisps_session_table_mark_eviction_forced(wisps_session_table_t *table, int idx) {
  if (table->slots[idx].state == WISPS_SESSION_EVICTING_COOPERATIVE)
    table->slots[idx].state = WISPS_SESSION_EVICTING_FORCED;
}

void wisps_session_table_reclaim_eviction(wisps_session_table_t *table, int idx) {
  if (table->slots[idx].state != WISPS_SESSION_EVICTING_COOPERATIVE &&
      table->slots[idx].state != WISPS_SESSION_EVICTING_FORCED)
    return;

  table->slots[idx].state = WISPS_SESSION_NEGOTIATED;
}

bool wisps_session_table_rt_slot_taken(wisps_session_table_t *table) {
  for (int i = 0; i < WISPS_SESSION_MAX_CLIENTS; i++) {
    if (table->slots[i].in_use &&
        (table->slots[i].state == WISPS_SESSION_GRANTED_WARMUP ||
         table->slots[i].state == WISPS_SESSION_ACTIVE_RT ||
         table->slots[i].state == WISPS_SESSION_EVICTING_COOPERATIVE ||
         table->slots[i].state == WISPS_SESSION_EVICTING_FORCED))
      return true;
  }
  return false;
}

void wisps_session_table_remove(wisps_session_table_t *table, int idx) {
  if (!table->slots[idx].in_use)
    return;

  wisps_session_table_deactivate(table, idx);
  if (table->slots[idx].dmabuf_set.valid)
    wisps_dmabuf_set_close(&table->slots[idx].dmabuf_set);

  table->slots[idx].in_use = false;
  table->slots[idx].ctrl_fd = -1;
}

int wisps_session_table_check_heartbeat_timeouts(wisps_session_table_t *table,
                                                 struct timespec now) {
  if (table->active_slot < 0)
    return -1;

  int idx = table->active_slot;
  int64_t sec_diff =
      (int64_t)now.tv_sec - (int64_t)table->slots[idx].last_heartbeat_monotonic.tv_sec;
  int64_t nsec_diff = (int64_t)now.tv_nsec -
                      (int64_t)table->slots[idx].last_heartbeat_monotonic.tv_nsec;
  int64_t elapsed_ms = sec_diff * 1000 + nsec_diff / 1000000;

  if (elapsed_ms > WISP_CLIENT_HEARTBEAT_TIMEOUT_MS) {
    wisps_session_table_deactivate(table, idx);
    return idx;
  }
  return -1;
}

/**
 * ts_less() - true iff @a is strictly earlier than @b
 * @a: left-hand timestamp
 * @b: right-hand timestamp
 */
static bool ts_less(struct timespec a, struct timespec b) {
  if (a.tv_sec != b.tv_sec)
    return a.tv_sec < b.tv_sec;
  return a.tv_nsec < b.tv_nsec;
}

int wisps_session_table_find_next_queued(wisps_session_table_t *table) {
  int best = -1;

  for (int i = 0; i < WISPS_SESSION_MAX_CLIENTS; i++) {
    if (!table->slots[i].in_use || table->slots[i].state != WISPS_SESSION_QUEUED)
      continue;

    if (best < 0 ||
        ts_less(table->slots[i].activate_requested_monotonic,
                table->slots[best].activate_requested_monotonic))
      best = i;
  }
  return best;
}

uint32_t wisps_session_table_queue_position(wisps_session_table_t *table, int idx) {
  if (!table->slots[idx].in_use || table->slots[idx].state != WISPS_SESSION_QUEUED)
    return 0;

  uint32_t position = 1;
  for (int i = 0; i < WISPS_SESSION_MAX_CLIENTS; i++) {
    if (i == idx || !table->slots[i].in_use || table->slots[i].state != WISPS_SESSION_QUEUED)
      continue;

    if (ts_less(table->slots[i].activate_requested_monotonic,
                table->slots[idx].activate_requested_monotonic) ||
        (table->slots[i].activate_requested_monotonic.tv_sec ==
             table->slots[idx].activate_requested_monotonic.tv_sec &&
         table->slots[i].activate_requested_monotonic.tv_nsec ==
             table->slots[idx].activate_requested_monotonic.tv_nsec &&
         i < idx))
      position++;
  }
  return position;
}
