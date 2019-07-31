// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "filter.h"

#include <zircon/assert.h>

namespace netdump {

static inline bool match_port(PortFieldType type, uint16_t src, uint16_t dst, uint16_t spec) {
  if (type & SRC_PORT) {
    if (src == spec) {
      return true;
    }
  }
  return (type & DST_PORT) && (dst == spec);
}

static inline bool match_address(AddressFieldType type, uint32_t src, uint32_t dst, uint32_t spec) {
  if (type & SRC_ADDR) {
    if (src == spec) {
      return true;
    }
  }
  return (type & DST_ADDR) && (dst == spec);
}

// The address data are in network byte order.
template <size_t N>
static bool match_address(AddressFieldType type, const uint8_t src[], const uint8_t dst[],
                          const std::array<uint8_t, N> spec) {
  if ((type & SRC_ADDR) && std::equal(spec.begin(), spec.end(), src)) {
    return true;
  }
  return (type & DST_ADDR) && (std::equal(spec.begin(), spec.end(), dst));
}

FrameLengthFilter::FrameLengthFilter(uint16_t frame_len, LengthComparator comp) {
  frame_len = ntohs(frame_len);
  switch (comp) {
    case LengthComparator::LEQ:
      match_fn_ = [frame_len](const Packet& packet) { return packet.frame_length <= frame_len; };
      break;
    case LengthComparator::GEQ:
      match_fn_ = [frame_len](const Packet& packet) { return packet.frame_length >= frame_len; };
      break;
    default:
      ZX_DEBUG_ASSERT_MSG(false, "Unexpected comparator: %u", static_cast<uint8_t>(comp));
      break;
  }
}

bool FrameLengthFilter::match(const Packet& packet) { return match_fn_(packet); }

EthFilter::EthFilter(const MacAddress& mac, AddressFieldType type) {
  spec_ = Spec(std::in_place_type_t<Address>());
  Address* addr = std::get_if<Address>(&spec_);
  addr->type = type;
  addr->mac = mac;  // Copy assignment.
}

bool EthFilter::match(const Packet& packet) {
  if (packet.frame == nullptr) {
    return false;
  }
  if (auto addr = std::get_if<Address>(&spec_)) {
    return match_address<ETH_ALEN>(addr->type, packet.frame->h_source, packet.frame->h_dest,
                                   addr->mac);
  }
  return std::get<EthType>(spec_) == packet.frame->h_proto;
}

IpFilter::IpFilter(uint8_t version) : version_(version) {
  ZX_DEBUG_ASSERT_MSG(version == 4 || version == 6, "Unsupported IP version: %u", version);
  // The version in the packet itself is always checked in the `match` method.
  match_fn_ = [](const Packet& /*packet*/) { return true; };
}

IpFilter::IpFilter(uint8_t version, uint16_t ip_pkt_len, LengthComparator comp)
    : version_(version) {
  ip_pkt_len = ntohs(ip_pkt_len);
  // We can avoid the per-packet `version` and `comp` branching at match time if we choose
  // the right lambda now.
  switch (version) {
    case 4:
      switch (comp) {
        case LengthComparator::LEQ:
          match_fn_ = [ip_pkt_len](const Packet& packet) {
            return ntohs(packet.ip->tot_len) <= ip_pkt_len;
          };
          break;
        case LengthComparator::GEQ:
          match_fn_ = [ip_pkt_len](const Packet& packet) {
            return ntohs(packet.ip->tot_len) >= ip_pkt_len;
          };
          break;
        default:
          ZX_DEBUG_ASSERT_MSG(false, "Unexpected comparator: %u", static_cast<uint8_t>(comp));
          break;
      }
      break;
    case 6:
      switch (comp) {
        case LengthComparator::LEQ:
          match_fn_ = [ip_pkt_len](const Packet& packet) {
            return ntohs(packet.ipv6->length) <= ip_pkt_len;
          };
          break;
        case LengthComparator::GEQ:
          match_fn_ = [ip_pkt_len](const Packet& packet) {
            return ntohs(packet.ipv6->length) >= ip_pkt_len;
          };
          break;
        default:
          ZX_DEBUG_ASSERT_MSG(false, "Unexpected comparator: %u", static_cast<uint8_t>(comp));
          break;
      }
      break;
    default:
      ZX_DEBUG_ASSERT_MSG(version == 4 || version == 6, "Unsupported IP version: %u", version);
      break;
  }
}

IpFilter::IpFilter(uint8_t version, uint8_t protocol) : version_(version) {
  switch (version) {
    case 4:
      match_fn_ = [protocol](const Packet& packet) { return packet.ip->protocol == protocol; };
      break;
    case 6:
      match_fn_ = [protocol](const Packet& packet) { return packet.ipv6->next_header == protocol; };
      break;
    default:
      ZX_DEBUG_ASSERT_MSG(version == 4 || version == 6, "Unsupported IP version: %u", version);
  }
}

IpFilter::IpFilter(uint32_t ipv4_addr, AddressFieldType type) : version_(4) {
  match_fn_ = [ipv4_addr, type](const Packet& packet) {
    return match_address(type, packet.ip->saddr, packet.ip->daddr, ipv4_addr);
  };
}

IpFilter::IpFilter(const IPv6Address& ipv6_addr, AddressFieldType type) : version_(6) {
  // Capture of `ipv6_addr` of type `std::array` is by-copy.
  match_fn_ = [ipv6_addr, type](const Packet& packet) {
    return match_address<IP6_ADDR_LEN>(type, packet.ipv6->src.u8, packet.ipv6->dst.u8, ipv6_addr);
  };
}

bool IpFilter::match(const Packet& packet) {
  if (packet.frame == nullptr || packet.ip == nullptr) {
    return false;
  }
  switch (version_) {
    case 4:
      // Check that `h_proto` and `version` in IP header are consistent.
      // If they are not, this is a malformed packet and the filter should reject gracefully
      // by returning false.
      if (packet.frame->h_proto == ETH_P_IP_NETWORK_BYTE_ORDER && packet.ip->version == 4) {
        return match_fn_(packet);
      }
      break;
    case 6:
      if (packet.frame->h_proto == ETH_P_IPV6_NETWORK_BYTE_ORDER && packet.ip->version == 6) {
        return match_fn_(packet);
      }
      break;
    default:
      // Should not happen as `version_` is guarded in the constructors.
      ZX_DEBUG_ASSERT_MSG(version_ == 4 || version_ == 6, "Unsupported IP version: %u", version_);
      break;
  }
  return false;  // Filter IP version does not match packet.
}

static inline bool port_in_range(uint16_t begin, uint16_t end, uint16_t port) {
  port = ntohs(port);
  return begin <= port && port <= end;
}

PortFilter::PortFilter(std::vector<PortRange> ports, PortFieldType type) : type_(type) {
  // Modify `ports` in place.
  for (auto range_it = ports.begin(); range_it < ports.end(); ++range_it) {
    range_it->first = ntohs(range_it->first);
    range_it->second = ntohs(range_it->second);
  }
  ports_ = std::move(ports);
}

bool PortFilter::match_ports(uint16_t src_port, uint16_t dst_port) {
  for (const PortRange& range : ports_) {
    if (((type_ & SRC_PORT) && port_in_range(range.first, range.second, src_port)) ||
        (((type_ & DST_PORT) && port_in_range(range.first, range.second, dst_port)))) {
      return true;
    }
  }
  return false;
}

bool PortFilter::match(const Packet& packet) {
  if (packet.frame == nullptr || packet.ip == nullptr || packet.transport == nullptr) {
    return false;
  }
  uint8_t transport_protocol = 0;
  if (packet.frame->h_proto == ETH_P_IP_NETWORK_BYTE_ORDER && packet.ip->version == 4) {
    transport_protocol = packet.ip->protocol;
  } else if (packet.frame->h_proto == ETH_P_IPV6_NETWORK_BYTE_ORDER && packet.ip->version == 6) {
    transport_protocol = packet.ipv6->next_header;
  } else {
    return false;  // Unhandled IP version
  }
  switch (transport_protocol) {
    case IPPROTO_TCP:
      return match_ports(packet.tcp->source, packet.tcp->dest);
    case IPPROTO_UDP:
      return match_ports(packet.udp->uh_sport, packet.udp->uh_dport);
    default:
      return false;  // Unhandled transport protocol
  }
}

bool NegFilter::match(const Packet& packet) { return !(filter_->match(packet)); }

bool ConjFilter::match(const Packet& packet) {
  return left_->match(packet) && right_->match(packet);
}

bool DisjFilter::match(const Packet& packet) {
  return left_->match(packet) || right_->match(packet);
}

}  // namespace netdump
