#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/ip.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#include <stddef.h>

#include "hashtable.h"

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

void die(const char* msg) {
  int err = errno;
  std::cerr << "Error: " << err << " " << msg;
  fprintf(stderr, "[%d] %s\n", err, msg);
  abort();
}

void msg(const char* msg) {
  std::cout << msg;
}

void set_fd_nb(int fd) {  // set non blocking conn fd
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
  STATE_END = 2,  // mark the connection for deletion
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
      : fd(file_descriptor),
        state(ConnectionState::STATE_REQ),
        rbuf_size(0),
        rbuf(4 + k_max_msg),
        wbuf(4 + k_max_msg),
        wbuf_size(0),
        wbuf_sent(0) {}
  ~Conn() {
    if (fd != -1) {
      close(fd);
    }
  }
};

static void conn_put(std::vector<Conn*>& fd2conn, struct Conn* conn) {
  if (fd2conn.size() <= (size_t)conn->fd) {
    fd2conn.resize(conn->fd + 1);
  }
  fd2conn[conn->fd] = conn;
}

static int32_t accept_new_conn(std::vector<Conn*>& fd2conn, int fd) {
  // accept
  struct sockaddr_in client_addr = {};
  socklen_t socklen = sizeof(client_addr);
  int connfd =
      accept(fd, reinterpret_cast<struct sockaddr*>(&client_addr), &socklen);
  if (connfd < 0) {
    msg("accept() error");
    return -1;
  }
  // set the new connection fd to nonblocking mode
  set_fd_nb(fd);
  try {
    auto conn = new Conn(connfd);
    conn_put(fd2conn, conn);
  } catch (std::exception& e) {
    std::cerr << "Caught exception: " << e.what() << "\n";
    return -1;
  }
  return 0;
}

static void state_req(Conn* conn);
static void state_res(Conn* conn);

const size_t k_max_args = 1024;

static int32_t parse_req(const uint8_t* data, size_t len,
                         std::vector<std::string>& out) {
  if (len < 4) {
    return -1;
  }
  uint32_t n = 0;
  memcpy(&n, &data[0], 4);
  if (n > k_max_args) {
    return -1;
  }
  size_t pos = 4;
  while (n--) {
    if (pos + 4 > len) {
      return -1;
    }
    uint32_t sz = 0;
    memcpy(&sz, &data[pos], 4);
    if (pos + 4 + sz > len) {
      return -1;
    }
    out.push_back(std::string((char*)&data[pos + 4], sz));
    pos += 4 + sz;
  }
  if (pos != len)
    return -1;  // tailing garbage
  return 0;
}

enum { RES_OK = 0, RES_ERR = 1, RES_NX = 2 };

// The data structure for the key space
static struct {
  HashMap db;
} g_data;

// the structure for the key
struct Entry {
  struct HashNode node;
  std::string key;
  std::string val;
};

static bool entry_eq(HashNode* lhs, HashNode* rhs) {
  struct Entry* le = container_of(lhs, struct Entry, node);
  struct Entry* re = container_of(rhs, struct Entry, node);
  return le->key == re->key;
}

static uint64_t str_hash(const uint8_t* data, size_t len) {
  uint32_t h = 0x811C9DC5;
  for (size_t i = 0; i < len; i++) {
    h = (h + data[i]) * 0x01000193;
  }
  return h;
}

static uint32_t do_get(std::vector<std::string>& cmd, uint8_t* res,
                       uint32_t* reslen) {
  Entry key;
  key.key.swap(cmd[1]);
  key.node.hcode = str_hash((uint8_t*)key.key.data(), key.key.size());

  HashNode* node = hm_lookup(&g_data.db, &key.node, &entry_eq);
  if (!node) {
    return RES_NX;
  }
  const std::string& val = container_of(node, Entry, node)->val;
  assert(val.size() <= k_max_msg);
  memcpy(res, val.data(), val.size());
  *reslen = (uint32_t)val.size();
  return RES_OK;
}

static uint32_t do_set(std::vector<std::string>& cmd) {
  Entry key;
  key.key.swap(cmd[1]);
  key.node.hcode = str_hash((uint8_t*)key.key.data(), key.key.size());

  HashNode* node = hm_lookup(&g_data.db, &key.node, &entry_eq);
  if (node) {
    container_of(node, Entry, node)->val.swap(cmd[2]);
  } else {
    Entry* ent = new Entry();
    ent->key.swap(key.key);
    ent->node.hcode = key.node.hcode;
    ent->val.swap(cmd[2]);
    hm_insert(&g_data.db, &ent->node);
  }
  return RES_OK;
}

static uint32_t do_del(std::vector<std::string>& cmd) {
  Entry key;
  key.key.swap(cmd[1]);
  key.node.hcode = str_hash((uint8_t*)key.key.data(), key.key.size());

  HashNode* node = hm_pop(&g_data.db, &key.node, &entry_eq);
  if (node) {
    delete container_of(node, Entry, node);
  }
  return RES_OK;
}

static bool cmd_is(const std::string& word, const char* cmd) {
  return 0 == strcasecmp(word.c_str(), cmd);
}

static int32_t do_request(const uint8_t* req, uint32_t reqlen,
                          uint32_t* rescode, uint8_t* res, uint32_t* reslen) {
  std::vector<std::string> cmd;
  if (0 != parse_req(req, reqlen, cmd)) {
    msg("bad req");
    return -1;
  }
  if (cmd.size() == 2 && cmd_is(cmd[0], "get")) {
    *rescode = do_get(cmd, res, reslen);
  } else if (cmd.size() == 3 && cmd_is(cmd[0], "set")) {
    *rescode = do_set(cmd);
  } else if (cmd.size() == 2 && cmd_is(cmd[0], "del")) {
    *rescode = do_del(cmd);
  } else {
    *rescode = RES_ERR;
    const char* msg = "Unknown cmd";
    strcpy((char*)res, msg);
    *reslen = strlen(msg);
    return 0;
  }
  return 0;
}

bool try_one_request(Conn* conn) {
  // try to parse a request from the buffer
  if (conn->rbuf_size < 4) {
    // not enough data in the buffer. Will retry in the next iteration
    return false;
  }
  // uint32_t len = *reinterpret_cast<uint32_t*>(&conn->rbuf[0]);
  uint32_t len = 0;
  memcpy(&len, &conn->rbuf[0], 4);
  if (len > k_max_msg) {
    msg("too long");
    conn->state = ConnectionState::STATE_END;
    return false;
  }
  if (4 + len > conn->rbuf_size) {
    // not enough data in the buffer. Will retry in the next iteration
    return false;
  }

  // got one request, generate the response
  std::cout << "client says: | length " << len << " | message "
            << &conn->rbuf[12] << "\n";
  uint32_t rescode = 0;
  uint32_t wlen = 0;
  int32_t err =
      do_request(&conn->rbuf[4], len, &rescode, &conn->wbuf[4 + 4], &wlen);
  if (err) {
    conn->state = ConnectionState::STATE_END;
    return false;
  }
  wlen += 4;
  memcpy(&conn->wbuf[0], &wlen, 4);
  memcpy(&conn->wbuf[4], &rescode, 4);
  conn->wbuf_size = 4 + wlen;

  // remove the request from buffer.
  // note: frequent remove is inefficient
  // note: need better handling for production code.
  size_t remain = conn->rbuf_size - 4 - len;
  if (remain) {
    memmove(conn->rbuf.data(), &conn->rbuf[4 + len], remain);
    // memmove(conn->rbuf, &conn->rbuf[4 + len], remain);
  }
  conn->rbuf_size = remain;

  // change state
  conn->state = ConnectionState::STATE_RES;
  state_res(conn);

  return (conn->state == ConnectionState::STATE_REQ);
}

bool try_fill_buffer(Conn* conn) {
  // try to fill the buffer
  // assert(conn->rbuf_size < sizeof(conn->rbuf));
  assert(conn->rbuf_size < conn->rbuf.size());
  ssize_t rv = 0;
  do {
    // size_t cap = sizeof(conn->rbuf) - conn->rbuf_size;
    size_t cap = conn->rbuf.size() - conn->rbuf_size;
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
  // assert(conn->rbuf_size <= sizeof(conn->rbuf));
  assert(conn->rbuf_size <= conn->rbuf.size());
  // Try to process request one by one.
  // Explaination of "Pipelining"
  while (try_one_request(conn)) {}
  return (conn->state == ConnectionState::STATE_REQ);
}

static void state_req(Conn* conn) {
  while (try_fill_buffer(conn)) {}
}

bool try_flush_buffer(Conn* conn) {
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

static void state_res(Conn* conn) {
  while (try_flush_buffer(conn)) {}
}

static void connection_io(Conn* conn) {
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

  int val = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0) {
    std::cerr << "setsockopt failed\n";
    close(fd);
    return 1;
  }  // SO_REUSEADDR is enabled for every listening socket. Without
  // it, bind() will fail when you restart your server.
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = ntohs(1234);
  addr.sin_addr.s_addr = ntohl(0);  // Wildcard address 0.0.0.0

  if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    die("bind()");
  }
  // SOMAXCONN is a backlog argument, it is the size of the queue.
  // It is defined as 128 in Linux.
  if (listen(fd, SOMAXCONN) < 0) {
    die("listen()");
  }

  std::vector<Conn*> fd2conn;
  set_fd_nb(fd);  // set the listen fd to non blocking

  std::vector<struct pollfd> poll_args;
  while (true) {
    // prepare the arguments of the poll()
    poll_args.clear();
    struct pollfd pdf = {fd, POLLIN, 0};
    poll_args.push_back(pdf);
    // connection fds
    for (auto it = fd2conn.begin(); it != fd2conn.end(); ++it) {
      Conn* conn = *it;
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
        Conn* conn = fd2conn[poll_args[i].fd];
        connection_io(conn);
        if (conn->state == ConnectionState::STATE_END) {
          // client closed normally, or something bad happened.
          delete conn;  // Free memory allocated by new in accept_new_conn
          fd2conn[poll_args[i].fd] = nullptr;
        }
      }
    }
  }
  if (poll_args[0].revents && POLLIN) {
    accept_new_conn(fd2conn, fd);
  }
  // Cleanup remaining connections
  for (auto conn : fd2conn) {
    if (conn) {
      delete conn;
    }
  }
  close(fd);
  return 0;
}
