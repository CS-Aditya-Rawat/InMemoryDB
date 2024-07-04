#include <arpa/inet.h>
#include <cassert>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <iostream>
#include <netinet/ip.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

void die(const char *msg) {
  int err = errno;
  std::cerr << "Error: " << err << " " << msg;
  fprintf(stderr, "[%d] %s\n", err, msg);
  abort();
}

void msg(const char *msg) { std::cout << msg; }

void set_fd_nb(int fd) { // set non blocking conn fd
  errno = 0;
  int flags = fcntl(fd, F_GETFL, 0);
  if (errno) {
    die("fcntl error");
    return;
  }
  flags |= O_NONBLOCK;
  errno = 0;
  if (fcntl(fd, F_SETFL, flags) == -1) {
    die("fcntl error");
  }
}

enum class ConnectionState {
  STATE_REQ = 0,
  STATE_RES = 1,
  STATE_END = 2, // mark the connection for deletion
};

constexpr size_t k_max_msg = 4096;

struct Conn {
  int fd = -1;
  ConnectionState state;
  std::vector<uint8_t> rbuf;
  std::vector<uint8_t> wbuf;
  size_t rbuf_size;
  size_t wbuf_size;
  size_t wbuf_sent;
  Conn(int file_descriptor)
      : fd(file_descriptor), state(ConnectionState::STATE_REQ),
        rbuf(4 + k_max_msg), wbuf(4 + k_max_msg), rbuf_size(0), wbuf_size(0),
        wbuf_sent(0) {}
  ~Conn() {
    if (fd != -1) {
      close(fd);
    }
  }
};

static void conn_put(std::vector<Conn *> &fd2conn, struct Conn *conn){
  if (fd2conn.size() <= (size_t)conn->fd){
    fd2conn.resize(conn->fd + 1);
  }
  fd2conn[conn->fd] = conn;
}

static int32_t accept_new_conn(std::vector<Conn *> &fd2conn, int fd) {
  // accept
  struct sockaddr_in client_addr = {};
  socklen_t socklen = sizeof(client_addr);
  int connfd =
      accept(fd, reinterpret_cast<struct sockaddr *>(&client_addr), &socklen);
  if (connfd < 0) {
    msg("accept() error");
    return -1;
  }
  // set the new connection fd to nonblocking mode
  set_fd_nb(fd);
  try {
    std::cout << "CONN ID:" << connfd << "\n";
    auto conn = new Conn(connfd);
    conn_put(fd2conn, conn);
  } catch (std::exception &e) {
    std::cerr << "Caught exception: " << e.what() << "\n";
    return -1;
  }
  return 0;
}

static void state_req(Conn *conn);
static void state_res(Conn *conn);

bool try_one_request(Conn *conn) {
  // try to parse a request from the buffer
  if (conn->rbuf_size < 4) {
    // not enough data in the buffer. Will retry in the next iteration
    return false;
  }
  // memcpy(&len, &conn->rbuf[0], 4);
  uint32_t len = *reinterpret_cast<uint32_t *>(&conn->rbuf[0]);
  if (len > k_max_msg) {
    msg("too long");
    conn->state = ConnectionState::STATE_END;
    return false;
  }
  if (4 + len > conn->rbuf_size) {
    // not enough data in the buffer. Will retry in the next iteration
    return false;
  }
  std::cout << "Client says: | length " << len << " | message " << &conn->rbuf[4] << "\n";
  // generating echoing response
  memcpy(&conn->wbuf[0], &len, 4);
  memcpy(&conn->wbuf[4], &conn->rbuf[4], len);
  conn->wbuf_size = 4 + len;
  // remove the request from buffer.
  // note: frequent remove is inefficient
  // note: need better handling for production code.
  size_t remain = conn->rbuf_size - 4 - len;
  if (remain) {
    memmove(conn->rbuf.data(), &conn->rbuf[4 + len], remain);
  }
  conn->rbuf_size = remain;

  // change state
  conn->state = ConnectionState::STATE_RES;
  state_res(conn);

  return (conn->state == ConnectionState::STATE_REQ);
}

bool try_fill_buffer(Conn *conn) {
  // try to fill the buffer
  assert(conn->rbuf_size < sizeof(conn->rbuf));
  ssize_t rv = 0;
  do {
    size_t cap = sizeof(conn->rbuf) - conn->rbuf_size;
    rv = read(conn->fd, &conn->rbuf[conn->rbuf_size], cap);
  } while (rv < 0 && errno == EINTR);
  if (rv < 0 && errno == EAGAIN) {
    // got EAGAIN, stop
    return false;
  }
  if (rv < 0) {
    msg("read() error");
    conn->state = ConnectionState::STATE_END;
    return false;
  }
  if (rv == 0) {
    std::cerr << (conn->rbuf_size > 0 ? "Unexpected EOF\n" : "EOF\n");
    conn->state = ConnectionState::STATE_END;
    return false;
  }
  conn->rbuf_size += (size_t)rv;
  assert(conn->rbuf_size <= sizeof(conn->rbuf));
  // Try to process request one by one.
  // Explaination of "Pipelining"
  while (try_one_request(conn)) {
  }
  return (conn->state == ConnectionState::STATE_REQ);
}

static void state_req(Conn *conn) {
    while (try_fill_buffer(conn)) {}
}

bool try_flush_buffer(Conn *conn) {
  ssize_t rv = 0;
  do {
    size_t remain = conn->wbuf_size - conn->wbuf_sent;
    rv = write(conn->fd, &conn->wbuf[conn->wbuf_sent], remain);
  } while (rv < 0 && errno == EINTR);
  if (rv < 0 && errno == EAGAIN) {
    return false;
  }

  if (rv < 0) {
    msg("write() error");
    conn->state = ConnectionState::STATE_END;
    return false;
  }
  conn->wbuf_sent += (size_t)rv;
  assert(conn->wbuf_sent <= conn->wbuf_size);
  if (conn->wbuf_sent == conn->wbuf_size) {
    // reponse was fully sent, change state back
    conn->state = ConnectionState::STATE_REQ;
    conn->wbuf_sent = 0;
    conn->wbuf_size = 0;
    return false;
  }

  // still got some data in wbuf, could try to write again
  return true;
}

static void state_res(Conn *conn) {
    while (try_flush_buffer(conn)) {}
}

static void connection_io(Conn *conn) {
  if (conn->state == ConnectionState::STATE_REQ) {
    state_req(conn);
  } else if (conn->state == ConnectionState::STATE_RES) {
    state_res(conn);
  } else {
    assert(0);
  }
}
int main() {
  /*
   * AF_INET is for IPv4
   * AF_INET6 for IPv6 | This selects the IP level protocol.
   */
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  std::cout << "CREATED FD: " << fd << "\n";

  int val = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0) {
    std::cerr << "setsockopt failed\n";
    close(fd);
    return 1;
  } // SO_REUSEADDR is enabled for every listening socket. Without
    // it, bind() will fail when you restart your server.
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = ntohs(1234);
  addr.sin_addr.s_addr = ntohl(0); // Wildcard address 0.0.0.0

  if (bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
    die("bind()");
  }
  // SOMAXCONN is a backlog argument, it is the size of the queue.
  // It is defined as 128 in Linux.
  if (listen(fd, SOMAXCONN) < 0) {
    die("listen()");
  }

  std::vector<Conn *> fd2conn;
  set_fd_nb(fd); // set the listen fd to non blocking

  std::vector<struct pollfd> poll_args;
  while (true) {
    // prepare the arguments of the poll()
    poll_args.clear();
    struct pollfd pdf = {fd, POLLIN, 0};
    poll_args.push_back(pdf);
    // connection fds
    for (auto it = fd2conn.begin(); it != fd2conn.end(); ++it) {
      Conn *conn = *it;
      if (!conn) {
        continue;
      }
      struct pollfd pfd = {};
      pfd.fd = conn->fd;
      pfd.events =
          (conn->state == ConnectionState::STATE_REQ) ? POLLIN : POLLOUT;
      pfd.events = pfd.events | POLLERR;
      poll_args.push_back(pfd);
    }

    // poll for active fd's
    // the timeout argument doesn't matter here
    int rv =
        poll(poll_args.data(), static_cast<nfds_t>(poll_args.size()), 1000);
    if (rv < 0) {
      die("poll");
    }

    // try to accept a new connection if the listening fd is active
    if (poll_args[0].revents) {
      if (accept_new_conn(fd2conn, fd) < 0) {
        std::cerr << "accept_new_conn() failed\n";
      }
    }

    // process active connection
    for (size_t i = 1; i < poll_args.size(); ++i) {
      if (poll_args[i].revents) {
        Conn *conn = fd2conn[poll_args[i].fd];
        connection_io(conn);
        if (conn->state == ConnectionState::STATE_END) {
          // client closed normally, or something bad happened.
          delete conn; // Free memory allocated by new in accept_new_conn
          fd2conn[poll_args[i].fd] = nullptr;
        }
      }
    }
  }
  if (poll_args[0].revents) {
    accept_new_conn(fd2conn, fd);
  }
  // Cleanup remaining connections
  for(auto conn: fd2conn){
    if (conn){
      delete conn;
    }
  }
  close(fd);
  return 0;
}
