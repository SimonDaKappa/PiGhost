// admin_plane.c - see admin_plane.h.
#define LIBPGDP_SERVER
#include "admin_plane.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/**
 * PGDSP_ADMIN_RECV_BUF_SIZE - max size of an admin message
 *
 * Generous fixed buffer: the largest admin payload today is pgdps_admin_list_response_t
 * (PGDPS_ADMIN_MAX_CLIENTS entries), well under 4KiB. Revisit if
 * PGDPS_ADMIN_MAX_CLIENTS grows a lot.
 */
#define PGDPS_ADMIN_RECV_BUF_SIZE 4096

/**
 * handle_list() - LIST_REQUEST -> a filled query -> response. 
 */
static void handle_list(pgdps_admin_plane_t *ap, int client_fd);

/**
 * handle_switch() - SWITCH_REQUEST -> a filled query -> response.
 */
static void handle_switch(pgdps_admin_plane_t *ap, int client_fd,
                          const pgdps_admin_switch_request_t *req);

/**
 * handle_connection() - one connect->request->response->close cycle.
 *
 * Never crashes on malformed input. An unrecognized/oversized/short message just closes
 * the connection with no reply, exactly like the producer control protocol's own
 * malformed- input stance.
 */
static void handle_connection(pgdps_admin_plane_t *ap, int client_fd);

int pgdps_admin_plane_init(pgdps_admin_plane_t *ap,
                           pgdps_control_query_channel_t *chan) {
  ap->chan = chan;
  atomic_store(&ap->running, true);

  int stop_fds[2];
  if (pipe(stop_fds) != 0) {
    perror("pipe (admin plane stop)");
    return -1;
  }
  ap->stop_read_fd = stop_fds[0];
  ap->stop_write_fd = stop_fds[1];

  ap->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (ap->listen_fd < 0) {
    perror("socket (admin plane)");
    close(ap->stop_read_fd);
    close(ap->stop_write_fd);
    return -1;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, PGDPS_ADMIN_SOCK_PATH, sizeof(addr.sun_path) - 1);

  unlink(PGDPS_ADMIN_SOCK_PATH); // stale socket from a previous run

  if (bind(ap->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    perror("bind (admin plane)");
    close(ap->listen_fd);
    close(ap->stop_read_fd);
    close(ap->stop_write_fd);
    ap->listen_fd = -1;
    return -1;
  }

  if (listen(ap->listen_fd, /*backlog=*/4) != 0) {
    perror("listen (admin plane)");
    close(ap->listen_fd);
    close(ap->stop_read_fd);
    close(ap->stop_write_fd);
    ap->listen_fd = -1;
    return -1;
  }

  return 0;
}

void *pgdps_admin_plane_run(void *arg) {
  pgdps_admin_plane_t *ap = (pgdps_admin_plane_t *)arg;

  struct pollfd pfds[2];
  pfds[0].fd = ap->listen_fd;
  pfds[0].events = POLLIN;
  pfds[1].fd = ap->stop_read_fd;
  pfds[1].events = POLLIN;

  while (atomic_load(&ap->running)) {
    int rc = poll(pfds, 2, -1);
    if (rc < 0) {
      if (errno == EINTR)
        continue;
      perror("poll (admin plane)");
      break;
    }

    if (pfds[1].revents & POLLIN)
      break; // pgdps_admin_plane_stop() was called

    if (!(pfds[0].revents & POLLIN))
      continue;

    int client_fd = accept(ap->listen_fd, NULL, NULL);
    if (client_fd < 0) {
      if (errno == EINTR)
        continue;
      perror("accept (admin plane)");
      continue;
    }

    handle_connection(ap, client_fd);
  }

  return NULL;
}

void pgdps_admin_plane_stop(pgdps_admin_plane_t *ap) {
  atomic_store(&ap->running, false);
  unsigned char byte = 1;
  ssize_t n;
  do {
    n = write(ap->stop_write_fd, &byte, 1);
  } while (n < 0 && errno == EINTR);
}

void pgdps_admin_plane_close(pgdps_admin_plane_t *ap) {
  if (ap->listen_fd >= 0)
    close(ap->listen_fd);
  if (ap->stop_read_fd >= 0)
    close(ap->stop_read_fd);
  if (ap->stop_write_fd >= 0)
    close(ap->stop_write_fd);
  ap->listen_fd = -1;
  ap->stop_read_fd = -1;
  ap->stop_write_fd = -1;
  unlink(PGDPS_ADMIN_SOCK_PATH);
}

static void handle_list(pgdps_admin_plane_t *ap, int client_fd) {
  pgdps_control_query_t query;

  memset(&query, 0, sizeof(query));
  query.type = PGDPS_CTRL_QUERY_LIST;

  pgdps_control_query_submit(ap->chan, &query);

  pgdp_ctrl_send(client_fd, (pgdp_msg_type_t)PGDPS_ADMIN_MSG_LIST_RESPONSE,
                  &query.list_response, sizeof(query.list_response));
}

static void handle_switch(pgdps_admin_plane_t *ap, int client_fd,
                          const pgdps_admin_switch_request_t *req) {
  pgdps_control_query_t query;
  memset(&query, 0, sizeof(query));
  query.type = PGDPS_CTRL_QUERY_SWITCH;
  strncpy(query.switch_client_id, req->client_id, PGDP_CLIENT_ID_LEN - 1);

  pgdps_control_query_submit(ap->chan, &query);

  pgdp_ctrl_send(client_fd, (pgdp_msg_type_t)PGDPS_ADMIN_MSG_SWITCH_RESPONSE,
                  &query.switch_response, sizeof(query.switch_response));
}

static void handle_connection(pgdps_admin_plane_t *ap, int client_fd) {
  unsigned char buf[PGDPS_ADMIN_RECV_BUF_SIZE];
  pgdp_msg_type_t type;
  uint32_t len;

  int rc = pgdp_ctrl_recv(client_fd, &type, buf, sizeof(buf), &len);
  if (rc != 0) {
    if (rc == -2)
      fprintf(stderr, "[pgipc-admin] oversized request, dropping connection\n");
    close(client_fd);
    return;
  }

  switch ((pgdps_admin_msg_type_t)type) {
  case PGDPS_ADMIN_MSG_LIST_REQUEST:
    handle_list(ap, client_fd);
    break;

  case PGDPS_ADMIN_MSG_SWITCH_REQUEST: {
    if (len != sizeof(pgdps_admin_switch_request_t)) {
      fprintf(stderr, "[pgipc-admin] malformed SWITCH_REQUEST (len=%u)\n", len);
      break;
    }
    pgdps_admin_switch_request_t req;
    memcpy(&req, buf, sizeof(req));
    req.client_id[PGDP_CLIENT_ID_LEN - 1] = '\0'; // never trust the wire's NUL
    handle_switch(ap, client_fd, &req);
    break;
  }

  default:
    fprintf(stderr, "[pgipc-admin] unrecognized admin message type %d\n", (int)type);
    break;
  }

  close(client_fd);
}
