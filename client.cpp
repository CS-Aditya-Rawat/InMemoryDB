#include <arpa/inet.h>
#include <array>
#include <cassert>
#include <cstdint>
#include <errno.h>
#include <iostream>
#include <netinet/ip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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

int32_t send_req(int fd, const std::string& message) {
  uint32_t len = static_cast<uint32_t>(message.size());
  if (len > k_max_msg) {
    return -1;
  }

  char wbuf[4 + k_max_msg];
  memcpy(wbuf, &len, 4); // assume little endian
  memcpy(&wbuf[4], message.data(), len);
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
      std::cout<<"ERROR HERE"<<'\n';
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
  rbuf[4 + len] = '\0';
  std::cout << "Server says: " << &rbuf[4]<<"\n";
  return 0;
}

int main() {
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
    // multiple pipelined requests
    std::array<std::string, 4> query_list = {"hello", "see", "you", "again"};

    for (const auto &query : query_list) {
      send_req(fd, query);
    }
    for (const auto &query : query_list) {
      int32_t response = read_res(fd);
      std::cout << "Response: " << response << std::endl;
    }
    close(fd);
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}
