/** socket_message.c - Shared control socket message implementation for reusability
 *
 * The wisp protocol is NOT intended to be built as a standalone object, in any fashion
 * whatsoever. This implementation is simply defined to provide a standardized messaging
 * channel over the control socket.
 *
 * NOTE FOR DEVELOPERS:
 * If however, you intend to build a client SDK or server implementation, you SHOULD
 * include this as part of your final executable.
 *
 * To do so for CMAKE, add something similar to below in your CMakeLists.txt:
 * ```cmake
 * add_executable(your_main_executable
 *     src/your_src_files.c
 *     ${CMAKE_SOURCE_DIR}/protocol/src/socket_message.c
 * )
 * target_include_directories(your_main_executable
 *     PRIVATE ${CMAKE_SOURCE_DIR}/protocol/include
 * )
 * ```
 * Do note that the above template _assumes_ the executable you are building is a
 * sibling to wisp/protocol/
 */
#include <wisp/wire.h>
#include <arpa/inet.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/**
 * wisp__send_all() - write() until @len bytes are sent or an error occurs
 * @fd:  where to write
 * @buf: payload to write
 * @len: amount to write from payload
 *
 * Returns 0 on success, -1 on error (EINTR is retried transparently).
 */
static int wisp__send_all(int fd, const void *buf, size_t len) {
  const unsigned char *p = (const unsigned char *)buf;
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = write(fd, p + sent, len - sent);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    sent += (size_t)n;
  }
  return 0;
}

/**
 * wisp__recv_all() - read() until @len bytes are received or an error/EOF
 * @fd:  where to read from
 * @buf: where to read into
 * @len: amount to read
 *
 * Returns 0 on success, -1 on error or peer-closed (EINTR retried).
 */
static int wisp__recv_all(int fd, void *buf, size_t len) {
  unsigned char *p = (unsigned char *)buf;
  size_t got = 0;
  while (got < len) {
    ssize_t n = read(fd, p + got, len - got);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (n == 0)
      return -1; /* peer closed */
    got += (size_t)n;
  }
  return 0;
}

/**
 * wisp_close_fds() - close every valid (>=0) fd in @fds [0..nfds).
 * @fds:  file descriptors to close
 * @nfds: number of fds
 *
 * Used by fd-receiving/bookkeeping paths to avoid leaking fds on error or after a
 * dmabuf set is retired.
 */
void wisp_close_fds(int *fds, int nfds) {
  for (int i = 0; i < nfds; i++)
    if (fds[i] >= 0)
      close(fds[i]);
}

int wisp_ctrl_send(int fd, wisp_msg_kind_t kind, const void *payload, uint32_t len) {
  unsigned char hdr[5];
  uint32_t nlen = htonl(len);

  hdr[0] = (unsigned char)kind;
  memcpy(hdr + 1, &nlen, sizeof(hdr) - 1);

  if (wisp__send_all(fd, hdr, sizeof(hdr)) != 0)
    return -1;

  if (len > 0 && wisp__send_all(fd, payload, len) != 0)
    return -1;

  return 0;
}

int wisp_ctrl_recv(int fd, wisp_msg_kind_t *out_kind, void *buf, uint32_t bufsize,
                   uint32_t *out_len) {
  unsigned char hdr[5];
  uint32_t nlen;

  if (wisp__recv_all(fd, hdr, sizeof(hdr)) != 0)
    return -1;

  *out_kind = (wisp_msg_kind_t)hdr[0];

  memcpy(&nlen, hdr + 1, 4);
  uint32_t len = ntohl(nlen);

  if (len > bufsize)
    return -2;
  if (len > 0 && wisp__recv_all(fd, buf, len) != 0)
    return -1;

  *out_len = len;
  return 0;
}

int wisp_ctrl_send_fds(int fd, wisp_msg_kind_t kind, const void *payload, uint32_t len,
                       const int *fds, int nfds) {
  unsigned char hdr[5];
  uint32_t nlen = htonl(len);
  hdr[0] = (unsigned char)kind;
  memcpy(hdr + 1, &nlen, sizeof(hdr) - 1);

  if (nfds <= 0)
    return wisp_ctrl_send(fd, kind, payload, len);
  if (nfds > WISP_NUM_BUFFERS)
    return -1;

  /* Ancillary data must ride with actual bytes; attach it to the header. */
  struct iovec iov = {.iov_base = hdr, .iov_len = sizeof(hdr)};
  union { /* aligned cmsg buffer */
    char buf[CMSG_SPACE(sizeof(int) * WISP_NUM_BUFFERS)];
    struct cmsghdr align;
  } u;
  memset(&u, 0, sizeof(u));

  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = u.buf;
  msg.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)nfds);

  struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
  c->cmsg_level = SOL_SOCKET;
  c->cmsg_type = SCM_RIGHTS;
  c->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nfds);
  memcpy(CMSG_DATA(c), fds, sizeof(int) * (size_t)nfds);

  ssize_t n;
  do {
    n = sendmsg(fd, &msg, 0);
  } while (n < 0 && errno == EINTR);
  if (n < 0)
    return -1;

  /* Extremely unlikely for a 5-byte header on SOCK_STREAM, but finish it (the cmsg was
   * already consumed with the first byte). */
  if ((size_t)n < sizeof(hdr) &&
      wisp__send_all(fd, hdr + n, sizeof(hdr) - (size_t)n) != 0)
    return -1;

  if (len > 0 && wisp__send_all(fd, payload, len) != 0)
    return -1;

  return 0;
}



int wisp_ctrl_recv_fds(int fd, wisp_msg_kind_t *out_kind, void *buf, uint32_t bufsize,
                       uint32_t *out_len, int *out_fds, int max_fds, int *out_nfds) {
  unsigned char hdr[5];
  uint32_t nlen, len;

  /* Handle nullable fds to ignore incoming on recv (basically subset functionality so
   * it functions like wisp_ctrl_recv() ) $$$TODO SIMON */
  // int *fds, *nfds;
  // if (out_fds)
  //   fds = out_fds;
  // if (out_nfds)
  //   nfds = out_nfds;

  *out_nfds = 0;

  struct iovec iov = {.iov_base = hdr, .iov_len = sizeof(hdr)};
  union {
    char buf[CMSG_SPACE(sizeof(int) * WISP_NUM_BUFFERS)];
    struct cmsghdr align;
  } u;
  memset(&u, 0, sizeof(u));

  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = u.buf;
  msg.msg_controllen = sizeof(u.buf);

  ssize_t n;
  do {
    n = recvmsg(fd, &msg, 0);
  } while (n < 0 && errno == EINTR);
  if (n <= 0)
    return -1; /* error, timeout (EAGAIN), or peer closed */

  /* Harvest any passed fds (they arrive with the first chunk). */
  for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
      int cnt = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
      const unsigned char *src = CMSG_DATA(c);

      for (int i = 0; i < cnt; i++) {
        int newfd;
        memcpy(&newfd, src + (size_t)i * sizeof(int), sizeof(int));
        if (*out_nfds < max_fds)
          out_fds[(*out_nfds)++] = newfd;
        else
          close(newfd); /* more fds than caller can take: don't leak */
      }
    }
  }

  if ((size_t)n < sizeof(hdr) &&
      wisp__recv_all(fd, hdr + n, sizeof(hdr) - (size_t)n) != 0)
    goto fail;

  *out_kind = (wisp_msg_kind_t)hdr[0];

  memcpy(&nlen, hdr + 1, 4);
  len = ntohl(nlen);

  if (len > bufsize) {
    wisp_close_fds(out_fds, *out_nfds);
    *out_nfds = 0;
    return -2;
  }
  if (len > 0 && wisp__recv_all(fd, buf, len) != 0)
    goto fail;

  *out_len = len;
  return 0;

fail:
  wisp_close_fds(out_fds, *out_nfds);
  *out_nfds = 0;
  return -1;
}