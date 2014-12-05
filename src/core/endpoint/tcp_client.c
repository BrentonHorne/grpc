/*
 *
 * Copyright 2014, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "src/core/endpoint/tcp_client.h"

#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>

#include "src/core/endpoint/socket_utils.h"
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/time.h>

typedef struct {
  void (*cb)(void *arg, grpc_endpoint *tcp);
  void *cb_arg;
  grpc_em_fd *fd;
  gpr_timespec deadline;
} async_connect;

static int prepare_socket(int fd) {
  if (fd < 0) {
    goto error;
  }

  if (!grpc_set_socket_nonblocking(fd, 1) || !grpc_set_socket_cloexec(fd, 1) ||
      !grpc_set_socket_low_latency(fd, 1)) {
    gpr_log(GPR_ERROR, "Unable to configure socket %d: %s", fd,
            strerror(errno));
    goto error;
  }

  return 1;

error:
  if (fd >= 0) {
    close(fd);
  }
  return 0;
}

static void on_writable(void *acp, grpc_em_cb_status status) {
  async_connect *ac = acp;
  int so_error = 0;
  socklen_t so_error_size;
  int err;
  int fd = grpc_em_fd_get(ac->fd);

  if (status == GRPC_CALLBACK_SUCCESS) {
    do {
      so_error_size = sizeof(so_error);
      err = getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_size);
    } while (err < 0 && errno == EINTR);
    if (err < 0) {
      gpr_log(GPR_ERROR, "getsockopt(ERROR): %s", strerror(errno));
      goto error;
    } else if (so_error != 0) {
      if (so_error == ENOBUFS) {
        /* We will get one of these errors if we have run out of
           memory in the kernel for the data structures allocated
           when you connect a socket.  If this happens it is very
           likely that if we wait a little bit then try again the
           connection will work (since other programs or this
           program will close their network connections and free up
           memory).  This does _not_ indicate that there is anything
           wrong with the server we are connecting to, this is a
           local problem.

           If you are looking at this code, then chances are that
           your program or another program on the same computer
           opened too many network connections.  The "easy" fix:
           don't do that! */
        gpr_log(GPR_ERROR, "kernel out of buffers");
        grpc_em_fd_notify_on_write(ac->fd, on_writable, ac, ac->deadline);
        return;
      } else {
        goto error;
      }
    } else {
      goto great_success;
    }
  } else {
    gpr_log(GPR_ERROR, "on_writable failed during connect: status=%d", status);
    goto error;
  }

  abort();

error:
  ac->cb(ac->cb_arg, NULL);
  grpc_em_fd_destroy(ac->fd);
  gpr_free(ac->fd);
  gpr_free(ac);
  return;

great_success:
  ac->cb(ac->cb_arg, grpc_tcp_create_emfd(ac->fd));
  gpr_free(ac);
}

void grpc_tcp_client_connect(void (*cb)(void *arg, grpc_endpoint *ep),
                             void *arg, grpc_em *em,
                             const struct sockaddr *addr, int addr_len,
                             gpr_timespec deadline) {
  int fd;
  grpc_dualstack_mode dsmode;
  int err;
  async_connect *ac;
  struct sockaddr_in6 addr6_v4mapped;
  struct sockaddr_in addr4_copy;

  /* Use dualstack sockets where available. */
  if (grpc_sockaddr_to_v4mapped(addr, &addr6_v4mapped)) {
    addr = (const struct sockaddr *)&addr6_v4mapped;
    addr_len = sizeof(addr6_v4mapped);
  }

  fd = grpc_create_dualstack_socket(addr, SOCK_STREAM, 0, &dsmode);
  if (fd < 0) {
    gpr_log(GPR_ERROR, "Unable to create socket: %s", strerror(errno));
  }
  if (dsmode == GRPC_DSMODE_IPV4) {
    /* If we got an AF_INET socket, map the address back to IPv4. */
    GPR_ASSERT(grpc_sockaddr_is_v4mapped(addr, &addr4_copy));
    addr = (struct sockaddr *)&addr4_copy;
    addr_len = sizeof(addr4_copy);
  }
  if (!prepare_socket(fd)) {
    cb(arg, NULL);
    return;
  }

  do {
    err = connect(fd, addr, addr_len);
  } while (err < 0 && errno == EINTR);

  if (err >= 0) {
    cb(arg, grpc_tcp_create(fd, em));
    return;
  }

  if (errno != EWOULDBLOCK && errno != EINPROGRESS) {
    gpr_log(GPR_ERROR, "connect error: %s", strerror(errno));
    close(fd);
    cb(arg, NULL);
    return;
  }

  ac = gpr_malloc(sizeof(async_connect));
  ac->cb = cb;
  ac->cb_arg = arg;
  ac->deadline = deadline;
  ac->fd = gpr_malloc(sizeof(grpc_em_fd));
  grpc_em_fd_init(ac->fd, em, fd);
  grpc_em_fd_notify_on_write(ac->fd, on_writable, ac, deadline);
}
