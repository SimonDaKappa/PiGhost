// session_table.c - see session_table.h.
#define LIBPGIPC_READER
#include "session_table.h"

#include <string.h>

void pgipc_session_table_init(pgipc_session_table_t *table) {
  memset(table, 0, sizeof(*table));
  for (int i = 0; i < PGIPC_SESSION_MAX_CLIENTS; i++) {
    table->slots[i].in_use = false;
    table->slots[i].ctrl_fd = -1;
  }
  table->active_slot = -1;
}

int pgipc_session_table_add(pgipc_session_table_t *table, int fd) {
  for (int i = 0; i < PGIPC_SESSION_MAX_CLIENTS; i++) {
    if (!table->slots[i].in_use) {
      /* memset first: a slot may be reused after pgipc_session_table_remove() and must
       * not leak the previous occupant's app_id/dmabuf_set/etc. */
      memset(&table->slots[i], 0, sizeof(table->slots[i]));
      table->slots[i].in_use = true;
      table->slots[i].ctrl_fd = fd;
      table->slots[i].state = PGIPC_SESSION_CONNECTED;
      table->slots[i].payload_kind = PGIPC_PAYLOAD_PIXELS;
      return i;
    }
  }
  return -1;
}

int pgipc_session_table_find_by_fd(pgipc_session_table_t *table, int fd) {
  for (int i = 0; i < PGIPC_SESSION_MAX_CLIENTS; i++) {
    if (table->slots[i].in_use && table->slots[i].ctrl_fd == fd)
      return i;
  }
  return -1;
}

int pgipc_session_table_find_by_app_id(pgipc_session_table_t *table,
                                       const char *app_id) {
  for (int i = 0; i < PGIPC_SESSION_MAX_CLIENTS; i++) {
    if (table->slots[i].in_use &&
        strncmp(table->slots[i].app_id, app_id, PGIPC_APP_ID_LEN) == 0)
      return i;
  }
  return -1;
}

void pgipc_session_table_activate(pgipc_session_table_t *table, int idx,
                                  uint32_t generation) {
  if (table->active_slot >= 0 && table->active_slot != idx)
    pgipc_session_table_deactivate(table, table->active_slot);

  table->slots[idx].state = PGIPC_SESSION_ACTIVE;
  table->slots[idx].granted_generation = generation;
  clock_gettime(CLOCK_MONOTONIC, &table->slots[idx].last_heartbeat_monotonic);
  table->active_slot = idx;
}

void pgipc_session_table_deactivate(pgipc_session_table_t *table, int idx) {
  if (table->slots[idx].state != PGIPC_SESSION_ACTIVE)
    return;

  table->slots[idx].state = PGIPC_SESSION_NEGOTIATED;
  if (table->active_slot == idx)
    table->active_slot = -1;
}

void pgipc_session_table_remove(pgipc_session_table_t *table, int idx) {
  if (!table->slots[idx].in_use)
    return;

  pgipc_session_table_deactivate(table, idx);
  if (table->slots[idx].dmabuf_set.valid)
    pgipc_dmabuf_set_close(&table->slots[idx].dmabuf_set);
  
  table->slots[idx].in_use = false;
  table->slots[idx].ctrl_fd = -1;
}

int pgipc_session_table_check_heartbeat_timeouts(pgipc_session_table_t *table,
                                                 struct timespec now) {
  if (table->active_slot < 0)
    return -1;

  int idx = table->active_slot;
  int64_t sec_diff =
      (int64_t)now.tv_sec - (int64_t)table->slots[idx].last_heartbeat_monotonic.tv_sec;
  int64_t nsec_diff = (int64_t)now.tv_nsec -
                      (int64_t)table->slots[idx].last_heartbeat_monotonic.tv_nsec;
  int64_t elapsed_ms = sec_diff * 1000 + nsec_diff / 1000000;

  if (elapsed_ms > PGIPC_HEARTBEAT_TIMEOUT_MS) {
    pgipc_session_table_deactivate(table, idx);
    return idx;
  }
  return -1;
}
