// admin_plane.c - see admin_plane.h.
#define LIBPGIPC_READER
#include "admin_plane.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// Generous fixed buffer: the largest admin payload today is pgipc_admin_list_response_t
// (PGIPC_ADMIN_MAX_CLIENTS entries), well under 4KiB. Revisit if
// PGIPC_ADMIN_MAX_CLIENTS grows a lot.
#define PGIPC_ADMIN_RECV_BUF_SIZE 4096

int pgipc_admin_plane_init(pgipc_admin_plane_t *ap,
                           pgipc_control_query_channel_t *chan) {
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
  strncpy(addr.sun_path, PGIPC_ADMIN_SOCK_PATH, sizeof(addr.sun_path) - 1);

  unlink(PGIPC_ADMIN_SOCK_PATH); // stale socket from a previous run

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

/** pgipc__admin_handle_list() - LIST_REQUEST -> a filled query -> response. */
static void pgipc__admin_handle_list(pgipc_admin_plane_t *ap, int client_fd) {
  pgipc_control_query_t query;

  memset(&query, 0, sizeof(query));
  query.type = PGIPC_CTRL_QUERY_LIST;

  pgipc_control_query_submit(ap->chan, &query);

  pgipc_ctrl_send(client_fd, (pgipc_msg_type_t)PGIPC_ADMIN_MSG_LIST_RESPONSE,
                  &query.list_response, sizeof(query.list_response));
}

/** pgipc__admin_handle_switch() - SWITCH_REQUEST -> a filled query -> response. */
static void pgipc__admin_handle_switch(pgipc_admin_plane_t *ap, int client_fd,
                                       const pgipc_admin_switch_request_t *req) {
  pgipc_control_query_t query;
  memset(&query, 0, sizeof(query));
  query.type = PGIPC_CTRL_QUERY_SWITCH;
  strncpy(query.switch_app_id, req->app_id, PGIPC_APP_ID_LEN - 1);

  pgipc_control_query_submit(ap->chan, &query);

  pgipc_ctrl_send(client_fd, (pgipc_msg_type_t)PGIPC_ADMIN_MSG_SWITCH_RESPONSE,
                  &query.switch_response, sizeof(query.switch_response));
}

/** pgipc__admin_handle_connection() - one connect->request->response->close cycle.
 *
 * Never crashes on malformed input. An unrecognized/oversized/short message just closes
 * the connection with no reply, exactly like the producer control protocol's own
 * malformed- input stance. 
 */
static void pgipc__admin_handle_connection(pgipc_admin_plane_t *ap, int client_fd) {
  unsigned char buf[PGIPC_ADMIN_RECV_BUF_SIZE];
  pgipc_msg_type_t type;
  uint32_t len;

  int rc = pgipc_ctrl_recv(client_fd, &type, buf, sizeof(buf), &len);
  if (rc != 0) {
    if (rc == -2)
      fprintf(stderr, "[pgipc-admin] oversized request, dropping connection\n");
    close(client_fd);
    return;
  }

  switch ((pgipc_admin_msg_type_t)type) {
  case PGIPC_ADMIN_MSG_LIST_REQUEST:
    pgipc__admin_handle_list(ap, client_fd);
    break;

  case PGIPC_ADMIN_MSG_SWITCH_REQUEST: {
    if (len != sizeof(pgipc_admin_switch_request_t)) {
      fprintf(stderr, "[pgipc-admin] malformed SWITCH_REQUEST (len=%u)\n", len);
      break;
    }
    pgipc_admin_switch_request_t req;
    memcpy(&req, buf, sizeof(req));
    req.app_id[PGIPC_APP_ID_LEN - 1] = '\0'; // never trust the wire's NUL
    pgipc__admin_handle_switch(ap, client_fd, &req);
    break;
  }

  default:
    fprintf(stderr, "[pgipc-admin] unrecognized admin message type %d\n", (int)type);
    break;
  }

  close(client_fd);
}

void *pgipc_admin_plane_run(void *arg) {
  pgipc_admin_plane_t *ap = (pgipc_admin_plane_t *)arg;

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
      break; // pgipc_admin_plane_stop() was called

    if (!(pfds[0].revents & POLLIN))
      continue;

    int client_fd = accept(ap->listen_fd, NULL, NULL);
    if (client_fd < 0) {
      if (errno == EINTR)
        continue;
      perror("accept (admin plane)");
      continue;
    }

    pgipc__admin_handle_connection(ap, client_fd);
  }

  return NULL;
}

void pgipc_admin_plane_stop(pgipc_admin_plane_t *ap) {
  atomic_store(&ap->running, false);
  unsigned char byte = 1;
  ssize_t n;
  do {
    n = write(ap->stop_write_fd, &byte, 1);
  } while (n < 0 && errno == EINTR);
}

void pgipc_admin_plane_close(pgipc_admin_plane_t *ap) {
  if (ap->listen_fd >= 0)
    close(ap->listen_fd);
  if (ap->stop_read_fd >= 0)
    close(ap->stop_read_fd);
  if (ap->stop_write_fd >= 0)
    close(ap->stop_write_fd);
  ap->listen_fd = -1;
  ap->stop_read_fd = -1;
  ap->stop_write_fd = -1;
  unlink(PGIPC_ADMIN_SOCK_PATH);
}
