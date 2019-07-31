// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fidl/ordinals.h"

#include <optional>

#define BORINGSSL_NO_CXX
#include <openssl/sha.h>

namespace fidl {
namespace ordinals {

std::string GetSelector(const raw::AttributeList* attributes, SourceLocation name) {
  if (attributes != nullptr) {
    const size_t size = attributes->attributes.size();
    for (size_t i = 0; i < size; i++) {
      if (attributes->attributes[i].name == "Selector") {
        return attributes->attributes[i].value;
      }
    }
  }
  return std::string(name.data().data(), name.data().size());
}

raw::Ordinal32 GetGeneratedOrdinal32(const std::string_view& full_name,
                                     const raw::SourceElement& source_element) {
  uint8_t digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const uint8_t*>(full_name.data()), full_name.size(), digest);
  // The following dance ensures that we treat the bytes as a little-endian
  // int32 regardless of host byte order.
  uint32_t ordinal = static_cast<uint32_t>(digest[0]) | static_cast<uint32_t>(digest[1]) << 8 |
                     static_cast<uint32_t>(digest[2]) << 16 |
                     static_cast<uint32_t>(digest[3]) << 24;

  ordinal &= 0x7fffffff;
  return raw::Ordinal32(source_element, ordinal);
}

raw::Ordinal32 GetGeneratedOrdinal32(const std::vector<std::string_view>& library_name,
                                     const std::string_view& container_name,
                                     const raw::AttributeList* attributes, SourceLocation name,
                                     const raw::SourceElement& source_element) {
  std::string selector_name = GetSelector(attributes, name);
  // TODO(pascallouis): Move this closer (code wise) to NameFlatName, ideally
  // sharing code.
  std::string full_name;
  bool once = false;
  for (std::string_view id : library_name) {
    if (once) {
      full_name.push_back('.');
    } else {
      once = true;
    }
    full_name.append(id.data(), id.size());
  }
  full_name.append(".");
  full_name.append(container_name.data(), container_name.size());
  full_name.append("/");
  full_name.append(selector_name);

  return GetGeneratedOrdinal32(std::string_view(full_name), source_element);
}

raw::Ordinal64 GetGeneratedOrdinal64(const std::string_view& full_name,
                                     const raw::SourceElement& source_element) {
  uint8_t digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const uint8_t*>(full_name.data()), full_name.size(), digest);
  // The following dance ensures that we treat the bytes as a little-endian
  // int64 regardless of host byte order.
  uint64_t ordinal =
      static_cast<uint64_t>(digest[0]) | static_cast<uint64_t>(digest[1]) << 8 |
      static_cast<uint64_t>(digest[2]) << 16 | static_cast<uint64_t>(digest[3]) << 24 |
      static_cast<uint64_t>(digest[4]) << 32 | static_cast<uint64_t>(digest[5]) << 40 |
      static_cast<uint64_t>(digest[6]) << 48 | static_cast<uint64_t>(digest[7]) << 56;

  ordinal &= 0x7fffffffffffffff;
  return raw::Ordinal64(source_element, ordinal);
}

raw::Ordinal64 GetGeneratedOrdinal64(const std::vector<std::string_view>& library_name,
                                     const std::string_view& container_name,
                                     const raw::AttributeList* attributes, SourceLocation name,
                                     const raw::SourceElement& source_element) {
  std::string selector_name = GetSelector(attributes, name);
  // TODO(pascallouis): Move this closer (code wise) to NameFlatName, ideally
  // sharing code.
  std::string full_name;
  bool once = false;
  for (std::string_view id : library_name) {
    if (once) {
      full_name.push_back('.');
    } else {
      once = true;
    }
    full_name.append(id.data(), id.size());
  }
  full_name.append("/");
  full_name.append(container_name.data(), container_name.size());
  full_name.append(".");
  full_name.append(selector_name);

  return GetGeneratedOrdinal64(std::string_view(full_name), source_element);
}

raw::Ordinal32 GetGeneratedOrdinal32(const std::vector<std::string_view>& library_name,
                                     const std::string_view& protocol_name,
                                     const raw::ProtocolMethod& method) {
  return GetGeneratedOrdinal32(library_name, protocol_name, method.attributes.get(),
                               method.identifier->location(), method);
}

raw::Ordinal64 GetGeneratedOrdinal64(const std::vector<std::string_view>& library_name,
                                     const std::string_view& protocol_name,
                                     const raw::ProtocolMethod& method) {
  return GetGeneratedOrdinal64(library_name, protocol_name, method.attributes.get(),
                               method.identifier->location(), method);
}

raw::Ordinal32 GetGeneratedOrdinal32(const std::vector<std::string_view>& library_name,
                                     const std::string_view& xunion_declaration_name,
                                     const raw::XUnionMember& xunion_member) {
  // Note that this ordinal hashing for xunion members uses the same ordinal
  // hashing algorithm as for FIDL methods, which results in 31 bits, not 32.
  return GetGeneratedOrdinal32(library_name, xunion_declaration_name,
                               xunion_member.attributes.get(), xunion_member.identifier->location(),
                               xunion_member);
}

}  // namespace ordinals
}  // namespace fidl
