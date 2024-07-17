#include <arpa/inet.h>
#include <assert.h>
#include <stdint.h>
#include <errno.h>
#include <cstdlib>
#include <iostream>
#include <netinet/ip.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>
#include <string>
#include <cassert>

constexpr uint32_t k_max_msg = 4096;

void die(const char *msg) {
  int err = errno;
  std::cerr << "Error: " << err << " " << msg;
  fprintf(stderr, "[%d] %s\n", err, msg);
  abort();
}

void msg(const char *msg) { std::cout << msg; }

int32_t read_full(int fd, char *buf, size_t n) {
  while (n > 0) {
    ssize_t rv = read(fd, buf, n);
    if (rv <= 0) {
      return -1; // error, or unexptected EOF
    }
    assert((size_t)rv <= n);
    n -= (size_t)rv;
    buf += rv;
  }
  return 0;
}

int32_t write_all(int fd, const char *buf, size_t n) {
  while (n > 0) {
    ssize_t rv = write(fd, buf, n);
    if (rv <= 0) {
      return -1; // error, or unexptected EOF
    }
    assert((size_t)rv <= n);
    n -= (size_t)rv;
    buf += rv;
  }
  return 0;
}

int32_t send_req(int fd, std::vector<std::string>& cmd) {
  uint32_t len = 4;
  for (const std::string &s: cmd){
    len += 4 + s.size();
  }
  if (len > k_max_msg) {
    return -1;
  }

  char wbuf[4 + k_max_msg];
  memcpy(wbuf, &len, 4); // assume little endian

  uint32_t n = cmd.size();
  memcpy(&wbuf[4], &n, 4); // writing nstr
  size_t cur = 8;
  for(const std::string &s: cmd){
    uint32_t p = (uint32_t)s.size();
    memcpy(&wbuf[cur], &p, 4);
    memcpy(&wbuf[cur + 4], s.data(), s.size());
    cur += 4 + s.size();
  }

  return write_all(fd, wbuf, 4 + len);
}

int32_t read_res(int fd) {
  // 4 bytes header
  char rbuf[4 + k_max_msg + 1];
  errno = 0;
  int32_t err = read_full(fd, rbuf, 4);
  if (err) {
    if (errno == 0) {
      msg("EOF");
    } else {
      msg("read() error");
    }
    return err;
  }
  uint32_t len = 0;
  memcpy(&len, rbuf, 4); // assume little endian
  if (len > k_max_msg) {
    msg("too long");
    return -1;
  }

  // reply body
  err = read_full(fd, &rbuf[4], len);
  if (err){
    msg("read() error");
    return err;
  }
  // print the result
  uint32_t rescode = 0;
  if (len < 4){
    msg("bad response");
    return -1;
  }
  memcpy(&rescode, &rbuf[4], 4);
  std::cout << "Server says: [" << rescode <<"]"<<"\n";
  return 0;
}

int main(int argc, char *argv[]) {
  try {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      die("Socket()");
    }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = ntohs(1234);
    addr.sin_addr.s_addr = ntohl(INADDR_LOOPBACK); // 127.0.0.1
    if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) <
        0) {
      close(fd);
      die("connect()");
    }
    std::vector<std::string> cmd;
    for(int i=1; i<argc; ++i){
      cmd.push_back(argv[i]);
    }
    if (send_req(fd, cmd)) close(fd);
    if (read_res(fd)) close(fd);

  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}
