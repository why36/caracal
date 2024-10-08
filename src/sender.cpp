#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <pcap/pcap.h>
#include <spdlog/fmt/ostr.h>
#include <spdlog/spdlog.h>
#include <tins/tins.h>
#include <chrono>
#include <string>
#include <iostream>

#include <algorithm>
#include <caracal/builder.hpp>
#include <caracal/constants.hpp>
#include <caracal/pretty.hpp>
#include <caracal/probe.hpp>
#include <caracal/sender.hpp>
#include <caracal/timestamp.hpp>
#include <caracal/utilities.hpp>
#include <caracal/prober.hpp>



using std::chrono::system_clock;

namespace caracal {

std::string execCommand(const char* cmd) {
    char buffer[128];
    std::string result;
    FILE* pipe = popen(cmd, "r");
    if (!pipe) return "popen failed!";
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        std::string line(buffer);
        line.erase(line.find_last_not_of("\n") + 1);  // �Ƴ����з�
        result += line;
    }
    pclose(pipe);
    return result;
}

Sender::Sender(const Prober::Config& config)
    : buffer_{},
      l2_protocol_{Protocols::L2::Ethernet},
      src_mac_{},
      dst_mac_{},
      src_ip_v4_{},
      src_ip_v6_{},
      caracal_id_{config.caracal_id},
      handle_{nullptr} {
  // Open pcap interface.
  char pcap_err[PCAP_ERRBUF_SIZE] = {};
  handle_ = pcap_open_live(config.interface.c_str(), 0, 0, 0, pcap_err);
  if (handle_ == nullptr) {
    throw std::runtime_error(pcap_err);
  } else if (strlen(pcap_err) > 0) {
    spdlog::warn("{}", pcap_err);
  }

  switch (pcap_datalink(handle_)) {
    case DLT_EN10MB:
      l2_protocol_ = Protocols::L2::Ethernet;
      break;
    case DLT_NULL:
      l2_protocol_ = Protocols::L2::BSDLoopback;
      break;
    case DLT_RAW:
      l2_protocol_ = Protocols::L2::None;
      break;
    default:
      throw std::runtime_error("Unsupported link type");
  }

  // Find the IPv4/v6 gateway.
  Tins::NetworkInterface interface { config.interface };
  std::string macString = execCommand("ip -6 neigh | awk '{print $5}'");
  Tins::HWAddress<6> gateway_mac(macString);
  //Tins::HWAddress<6> gateway_mac{"fa:16:3e:0b:28:15"};
  // if (l2_protocol_ == Protocols::L2::Ethernet) {
  //   spdlog::info("Resolving the gateway MAC address...");
  //   if (config.ip_version == 6) {
  //     // TODO Libtins does not provide a simple way to get the HW gateway for IPv6,
  //     // so let the user put their own gateway as a future work
  //     // TODO In the future it might be best to resolve both gateway automatically
  //     // and use them depending on the probe protocol. E.g. set gateway_mac_v4 and gateway_mac_v6
  //     std::cerr << "123" << std::endl;
  //     gateway_mac = Tins::HWAddress<6>{"fa:16:3e:0b:28:15"};
  //   } else {
  //     // By default use IPv4
  //     std::cerr << "456" << std::endl;
  //     gateway_mac =
  //         Utilities::gateway_mac_for(interface, Tins::IPv4Address("8.8.8.8"));   
  //   }
  // }

  std::copy(gateway_mac.begin(), gateway_mac.end(), dst_mac_.begin());


  // Set the source/destination MAC addresses.
  auto if_mac = interface.hw_address();
  std::copy(if_mac.begin(), if_mac.end(), src_mac_.begin());

  // Set the source IPv4 address.
  src_ip_v4_.sin_family = AF_INET;
  if (config.source_ipv4.has_value()) {
    inet_pton(AF_INET, config.source_ipv4->to_string().c_str(),
              &src_ip_v4_.sin_addr);
  } else {
    inet_pton(AF_INET, Utilities::source_ipv4_for(interface).to_string().c_str(),
              &src_ip_v4_.sin_addr);
  }

  // Set the source IPv6 address.
  src_ip_v6_.sin6_family = AF_INET6;
  if (config.source_ipv6.has_value()) {
    inet_pton(AF_INET6, config.source_ipv6->to_string().c_str(),
              &src_ip_v6_.sin6_addr);
  } else {
    inet_pton(AF_INET6, Utilities::source_ipv6_for(interface).to_string().c_str(),
              &src_ip_v6_.sin6_addr);
  }


  spdlog::info("dst_mac={:02x}", fmt::join(dst_mac_, ":"));
  spdlog::info("src_ip_v4={} src_ip_v6={}", src_ip_v4_.sin_addr,
               src_ip_v6_.sin6_addr);
}

Sender::~Sender() { pcap_close(handle_); }

void Sender::send(const Probe &probe) {
  const auto l3_protocol = probe.l3_protocol();
  const auto l4_protocol = probe.l4_protocol();

  const uint64_t timestamp =
      Timestamp::cast<Timestamp::tenth_ms>(system_clock::now());
  const uint16_t timestamp_enc = Timestamp::encode(timestamp);

  const uint16_t payload_length = probe.ttl + PAYLOAD_TWEAK_BYTES;
  const Packet packet{buffer_.data(), buffer_.size(), l2_protocol_,
                      l3_protocol,    l4_protocol,    payload_length};

  std::fill(packet.begin(), packet.end(), std::byte{0});

  switch (l2_protocol_) {
    case Protocols::L2::BSDLoopback:
      Builder::Loopback::init(packet);
      break;

    case Protocols::L2::Ethernet:
      Builder::Ethernet::init(packet, src_mac_, dst_mac_);
      break;

    case Protocols::L2::None:
      break;
  }

  switch (l3_protocol) {
    case Protocols::L3::IPv4:
      Builder::IPv4::init(packet, src_ip_v4_.sin_addr,
                          probe.sockaddr4().sin_addr, probe.ttl,
                          probe.checksum(caracal_id_));
      break;

    case Protocols::L3::IPv6:
      Builder::IPv6::init(packet, src_ip_v6_.sin6_addr,
                          probe.sockaddr6().sin6_addr, probe.ttl,
                          probe.flow_label);
      break;
  }

  switch (l4_protocol) {
    case Protocols::L4::ICMP:
      Builder::ICMP::init(packet, probe.src_port, timestamp_enc);
      break;

    case Protocols::L4::ICMPv6:
      Builder::ICMPv6::init(packet, probe.src_port, timestamp_enc);
      break;

    case Protocols::L4::UDP:
      Builder::UDP::init(packet, timestamp_enc, probe.src_port, probe.dst_port);
      break;
  }

  if (pcap_inject(handle_, packet.l2(), packet.l2_size()) == PCAP_ERROR) {
    throw std::runtime_error(pcap_geterr(handle_));
  }
}
}  // namespace caracal
