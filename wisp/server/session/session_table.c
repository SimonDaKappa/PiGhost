// session_table.c - see session_table.h.
#define LIBPGDP_SERVER
#include "session_table.h"

#include <string.h>

void pgdps_session_table_init(pgdps_session_table_t *table) {
  memset(table, 0, sizeof(*table));
  for (int i = 0; i < PGDPS_SESSION_MAX_CLIENTS; i++) {
    table->slots[i].in_use = false;
    table->slots[i].ctrl_fd = -1;
  }
  table->active_slot = -1;
}

int pgdps_session_table_add(pgdps_session_table_t *table, int fd) {
  for (int i = 0; i < PGDPS_SESSION_MAX_CLIENTS; i++) {
    if (!table->slots[i].in_use) {
      /* memset first: a slot may be reused after pgdps_session_table_remove() and must
       * not leak the previous occupant's client_id/dmabuf_set/etc. */
      memset(&table->slots[i], 0, sizeof(table->slots[i]));
      table->slots[i].in_use = true;
      table->slots[i].ctrl_fd = fd;
      table->slots[i].state = PGDPS_SESSION_CONNECTED;
      table->slots[i].payload_kind = PGDP_PAYLOAD_PIXELS;
      return i;
    }
  }
  return -1;
}

int pgdps_session_table_find_by_fd(pgdps_session_table_t *table, int fd) {
  for (int i = 0; i < PGDPS_SESSION_MAX_CLIENTS; i++) {
    if (table->slots[i].in_use && table->slots[i].ctrl_fd == fd)
      return i;
  }
  return -1;
}

int pgdps_session_table_find_by_client_id(pgdps_session_table_t *table,
                                          const char *client_id) {
  for (int i = 0; i < PGDPS_SESSION_MAX_CLIENTS; i++) {
    if (table->slots[i].in_use &&
        strncmp(table->slots[i].client_id, client_id, PGDP_CLIENT_ID_LEN) == 0)
      return i;
  }
  return -1;
}

void pgdps_session_table_activate(pgdps_session_table_t *table, int idx,
                                  uint32_t generation) {
  if (table->active_slot >= 0 && table->active_slot != idx)
    pgdps_session_table_deactivate(table, table->active_slot);

  table->slots[idx].state = PGDPS_SESSION_ACTIVE;
  table->slots[idx].granted_generation = generation;
  clock_gettime(CLOCK_MONOTONIC, &table->slots[idx].last_heartbeat_monotonic);
  table->active_slot = idx;
}

void pgdps_session_table_deactivate(pgdps_session_table_t *table, int idx) {
  if (table->slots[idx].state != PGDPS_SESSION_ACTIVE)
    return;

  table->slots[idx].state = PGDPS_SESSION_NEGOTIATED;
  if (table->active_slot == idx)
    table->active_slot = -1;
}

void pgdps_session_table_remove(pgdps_session_table_t *table, int idx) {
  if (!table->slots[idx].in_use)
    return;

  pgdps_session_table_deactivate(table, idx);
  if (table->slots[idx].dmabuf_set.valid)
    pgdps_dmabuf_set_close(&table->slots[idx].dmabuf_set);

  table->slots[idx].in_use = false;
  table->slots[idx].ctrl_fd = -1;
}

int pgdps_session_table_check_heartbeat_timeouts(pgdps_session_table_t *table,
                                                 struct timespec now) {
  if (table->active_slot < 0)
    return -1;

  int idx = table->active_slot;
  int64_t sec_diff =
      (int64_t)now.tv_sec - (int64_t)table->slots[idx].last_heartbeat_monotonic.tv_sec;
  int64_t nsec_diff = (int64_t)now.tv_nsec -
                      (int64_t)table->slots[idx].last_heartbeat_monotonic.tv_nsec;
  int64_t elapsed_ms = sec_diff * 1000 + nsec_diff / 1000000;

  if (elapsed_ms > PGDPS_HEARTBEAT_TIMEOUT_MS) {
    pgdps_session_table_deactivate(table, idx);
    return idx;
  }
  return -1;
}
