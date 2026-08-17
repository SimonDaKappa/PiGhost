// admin_plane.c - see admin_plane.h.
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "admin_plane.h"



/*
 * PGDSP_ADMIN_RECV_BUF_SIZE - max size of an admin message
 *
 * Generous fixed buffer: the largest admin payload today is wisps_admin_list_response_t
 * (WISPS_ADMIN_MAX_CLIENTS entries), well under 4KiB. Revisit if
 * WISPS_ADMIN_MAX_CLIENTS grows a lot.
 */
#define WISPS_ADMIN_RECV_BUF_SIZE 4096

/**
 * handle_list() - LIST_REQUEST -> a filled query -> response. 
 * @ap:        admin plane handling the list
 * @client_fd: the requesting client's fd
 */
static void handle_list(wisps_admin_plane_t *ap, int client_fd);

/**
 * handle_switch() - SWITCH_REQUEST -> a filled query -> response.
 * @ap:        admin plane handling the switch
 * @client_fd: the requesting client's fd
 * @req:       the switch request
 */
static void handle_switch(wisps_admin_plane_t *ap, int client_fd,
                          const wisps_admin_switch_request_t *req);

/**
 * handle_connection() - one connect->request->response->close cycle.
 * @ap:        admin plane handling the connection
 * @client_fd: the connected client's fd
 *
 * Never crashes on malformed input. An unrecognized/oversized/short message just closes
 * the connection with no reply, exactly like the producer control protocol's own
 * malformed- input stance.
 */
static void handle_connection(wisps_admin_plane_t *ap, int client_fd);

int wisps_admin_plane_init(wisps_admin_plane_t *ap,
                           wisps_control_query_channel_t *chan) {
  ap->chan = chan;
  wisp_atomic_store(&ap->running, true);

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
  strncpy(addr.sun_path, WISPS_ADMIN_SOCK_PATH, sizeof(addr.sun_path) - 1);

  unlink(WISPS_ADMIN_SOCK_PATH); // stale socket from a previous run

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

void *wisps_admin_plane_run(void *arg) {
  wisps_admin_plane_t *ap = (wisps_admin_plane_t *)arg;

  struct pollfd pfds[2];
  pfds[0].fd = ap->listen_fd;
  pfds[0].events = POLLIN;
  pfds[1].fd = ap->stop_read_fd;
  pfds[1].events = POLLIN;

  while (wisp_atomic_load(&ap->running)) {
    int rc = poll(pfds, 2, -1);
    if (rc < 0) {
      if (errno == EINTR)
        continue;
      perror("poll (admin plane)");
      break;
    }

    if (pfds[1].revents & POLLIN)
      break; // wisps_admin_plane_stop() was called

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

void wisps_admin_plane_stop(wisps_admin_plane_t *ap) {
  wisp_atomic_store(&ap->running, false);
  unsigned char byte = 1;
  ssize_t n;
  do {
    n = write(ap->stop_write_fd, &byte, 1);
  } while (n < 0 && errno == EINTR);
}

void wisps_admin_plane_close(wisps_admin_plane_t *ap) {
  if (ap->listen_fd >= 0)
    close(ap->listen_fd);
  if (ap->stop_read_fd >= 0)
    close(ap->stop_read_fd);
  if (ap->stop_write_fd >= 0)
    close(ap->stop_write_fd);
  ap->listen_fd = -1;
  ap->stop_read_fd = -1;
  ap->stop_write_fd = -1;
  unlink(WISPS_ADMIN_SOCK_PATH);
}

static void handle_list(wisps_admin_plane_t *ap, int client_fd) {
  wisps_control_query_t query;

  memset(&query, 0, sizeof(query));
  query.type = WISPS_CTRL_QUERY_LIST;

  wisps_control_query_submit(ap->chan, &query);

  wisp_ctrl_send(client_fd, (wisp_msg_kind_t)WISPS_ADMIN_MSG_LIST_RESPONSE,
                  &query.list_response, sizeof(query.list_response));
}

static void handle_switch(wisps_admin_plane_t *ap, int client_fd,
                          const wisps_admin_switch_request_t *req) {
  wisps_control_query_t query;
  memset(&query, 0, sizeof(query));
  query.type = WISPS_CTRL_QUERY_SWITCH;
  strncpy(query.switch_client_id, req->client_id, WISP_CLIENT_ID_LEN - 1);

  wisps_control_query_submit(ap->chan, &query);

  wisp_ctrl_send(client_fd, (wisp_msg_kind_t)WISPS_ADMIN_MSG_SWITCH_RESPONSE,
                  &query.switch_response, sizeof(query.switch_response));
}

static void handle_connection(wisps_admin_plane_t *ap, int client_fd) {
  unsigned char buf[WISPS_ADMIN_RECV_BUF_SIZE];
  wisp_msg_kind_t type;
  uint32_t len;

  int rc = wisp_ctrl_recv(client_fd, &type, buf, sizeof(buf), &len);
  if (rc != 0) {
    if (rc == -2)
      fprintf(stderr, "[pgipc-admin] oversized request, dropping connection\n");
    close(client_fd);
    return;
  }

  switch ((wisps_admin_msg_type_t)type) {
  case WISPS_ADMIN_MSG_LIST_REQUEST:
    handle_list(ap, client_fd);
    break;

  case WISPS_ADMIN_MSG_SWITCH_REQUEST: {
    if (len != sizeof(wisps_admin_switch_request_t)) {
      fprintf(stderr, "[pgipc-admin] malformed SWITCH_REQUEST (len=%u)\n", len);
      break;
    }
    wisps_admin_switch_request_t req;
    memcpy(&req, buf, sizeof(req));
    req.client_id[WISP_CLIENT_ID_LEN - 1] = '\0'; // never trust the wire's NUL
    handle_switch(ap, client_fd, &req);
    break;
  }

  default:
    fprintf(stderr, "[pgipc-admin] unrecognized admin message type %d\n", (int)type);
    break;
  }

  close(client_fd);
}
