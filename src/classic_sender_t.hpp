#pragma once

#include <netinet/in.h>
#include <sys/socket.h>
#include <tins/tins.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "rate_limiter.hpp"

namespace fs = std::filesystem;

class classic_sender_t {
 public:
  classic_sender_t(uint8_t family, const std::string protocol,
                   const Tins::NetworkInterface interface, const int pps,
                   const std::optional<fs::path> ofile);
  ~classic_sender_t();
  void send(int n_packets, in_addr destination, uint8_t ttl, uint16_t sport,
            uint16_t dport);

 private:
  void dump_reference_time();

  int m_socket;
  uint8_t m_family;
  uint8_t m_proto;
  sockaddr_in m_src_addr;
  uint8_t *m_buffer;
  std::string m_payload;

  timeval m_start;
  timeval m_now;

  std::ofstream m_start_time_log_file;
  RateLimiter m_rl;
};
