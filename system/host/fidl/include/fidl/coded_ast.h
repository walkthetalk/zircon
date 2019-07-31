// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_HOST_FIDL_INCLUDE_FIDL_CODED_AST_H_
#define ZIRCON_SYSTEM_HOST_FIDL_INCLUDE_FIDL_CODED_AST_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "types.h"

// The types in this file define structures that much more closely map
// the coding tables (i.e., fidl_type_t) for (de)serialization,
// defined at ulib/fidl/include/coding.h and so on.

// In particular, compared to the flat_ast version:
// - All files in the library are resolved together
// - Names have been unnested and fully qualified
// - All data structure sizes and layouts have been computed

namespace fidl {
namespace coded {

enum struct CodingContext {
  // The coding table of this type will be used to represent data within
  // an envelope. This will affect the 'coding needed'.
  kInsideEnvelope,

  // The coding table of this type will be used to represent data outside
  // of an envelope, and default 'coding needed' is appropriate here.
  kOutsideEnvelope,
};

enum struct CodingNeeded {
  // There is interesting coding information about the location of
  // pointers, allocations, or handles for this type.
  kAlways,

  // The type contains no pointers or handles. However, we should generate
  // corresponding coding information when it is wrapped in an envelope,
  // to support encoding/decoding of xunions and tables.
  kEnvelopeOnly,
};

namespace {

CodingNeeded WhichCodingNeeded(CodingContext context, CodingNeeded coding_needed) {
  switch (context) {
    case CodingContext::kInsideEnvelope:
      return CodingNeeded::kAlways;
    case CodingContext::kOutsideEnvelope:
      return coding_needed;
  }
}

}  // namespace

struct Type;

struct StructField {
  StructField(const Type* type, uint32_t size, uint32_t offset, uint32_t padding)
      : type(type), size(size), offset(offset), padding(padding) {}

  const Type* type;
  const uint32_t size;
  const uint32_t offset;
  const uint32_t padding;
};

struct UnionField {
  UnionField(const Type* type, uint32_t padding)
      : type(type), padding(padding) {}

  const Type* type;
  const uint32_t padding;
};

// This carries the same information as the XUnionField struct below and
// arguably violates DRY, but it's useful to make it a different type to
// distinguish its use-case in code, and also to make it easier to change later
// if necessary. (Gotta do something at least three times before we abstract it
// out, right?)
struct TableField {
  TableField(const Type* type, uint32_t ordinal)
      : type(type), ordinal(ordinal) {}

  const Type* type;
  const uint32_t ordinal;
};

// This carries the same information as the TableField struct above and arguably
// violates DRY, but it's useful to make it a different type to distinguish its
// use-case in code, and also to make it easier to change later if necessary.
// (Gotta do something at least three times before we abstract it out, right?)
struct XUnionField {
  XUnionField(const Type* type, uint32_t ordinal)
      : type(type), ordinal(ordinal) {}

  const Type* type;
  const uint32_t ordinal;
};

struct Type {
  virtual ~Type() = default;

  enum struct Kind {
    kPrimitive,
    kEnum,
    kBits,
    kHandle,
    kProtocolHandle,
    kRequestHandle,
    kStruct,
    kTable,
    kUnion,
    kXUnion,
    kPointer,
    kMessage,
    kProtocol,
    kArray,
    kString,
    kVector,
  };

  Type(Kind kind, std::string coded_name, uint32_t size, CodingNeeded coding_needed)
      : kind(kind), coded_name(std::move(coded_name)), size(size), coding_needed(coding_needed) {}

  const Kind kind;
  const std::string coded_name;
  uint32_t size;
  const CodingNeeded coding_needed;
};

struct PrimitiveType : public Type {
  PrimitiveType(std::string name, types::PrimitiveSubtype subtype, uint32_t size,
                CodingContext context)
      : Type(Kind::kPrimitive, std::move(name), size,
             WhichCodingNeeded(context, CodingNeeded::kEnvelopeOnly)),
        subtype(subtype) {}

  const types::PrimitiveSubtype subtype;
};

struct EnumType : public Type {
  EnumType(std::string name, types::PrimitiveSubtype subtype, uint32_t size,
           std::vector<uint64_t> members)
      : Type(Kind::kEnum, std::move(name), size, CodingNeeded::kAlways),
        subtype(subtype),
        members(std::move(members)) {}

  const types::PrimitiveSubtype subtype;
  const std::vector<uint64_t> members;
};

struct BitsType : public Type {
  BitsType(std::string name, types::PrimitiveSubtype subtype, uint32_t size, uint64_t mask)
      : Type(Kind::kBits, std::move(name), size, CodingNeeded::kAlways),
        subtype(subtype),
        mask(mask) {}

  const types::PrimitiveSubtype subtype;
  const uint64_t mask;
};

struct HandleType : public Type {
  HandleType(std::string name, types::HandleSubtype subtype, types::Nullability nullability)
      : Type(Kind::kHandle, std::move(name), 4u, CodingNeeded::kAlways),
        subtype(subtype),
        nullability(nullability) {}

  const types::HandleSubtype subtype;
  const types::Nullability nullability;
};

struct ProtocolHandleType : public Type {
  ProtocolHandleType(std::string name, types::Nullability nullability)
      : Type(Kind::kProtocolHandle, std::move(name), 4u, CodingNeeded::kAlways),
        nullability(nullability) {}

  const types::Nullability nullability;
};

struct RequestHandleType : public Type {
  RequestHandleType(std::string name, types::Nullability nullability)
      : Type(Kind::kRequestHandle, std::move(name), 4u, CodingNeeded::kAlways),
        nullability(nullability) {}

  const types::Nullability nullability;
};

struct PointerType : public Type {
  PointerType(std::string name, const Type* type)
      : Type(Kind::kPointer, std::move(name), 8u, CodingNeeded::kAlways), element_type(type) {}

  const Type* element_type;
};

struct StructType : public Type {
  StructType(std::string name, std::vector<StructField> fields, uint32_t size, std::string qname)
      : Type(Kind::kStruct, std::move(name), size, CodingNeeded::kAlways),
        fields(std::move(fields)),
        qname(std::move(qname)) {}

  std::vector<StructField> fields;
  std::string qname;
  PointerType* maybe_reference_type = nullptr;
};

struct UnionType : public Type {
  UnionType(std::string name, std::vector<UnionField> members, uint32_t data_offset, uint32_t size,
            std::string qname)
      : Type(Kind::kUnion, std::move(name), size, CodingNeeded::kAlways),
        members(std::move(members)),
        data_offset(data_offset),
        qname(std::move(qname)) {}

  std::vector<UnionField> members;
  const uint32_t data_offset;
  std::string qname;
  PointerType* maybe_reference_type = nullptr;
};

struct TableType : public Type {
  TableType(std::string name, std::vector<TableField> fields, uint32_t size, std::string qname)
      : Type(Kind::kTable, std::move(name), size, CodingNeeded::kAlways),
        fields(std::move(fields)),
        qname(std::move(qname)) {}

  std::vector<TableField> fields;
  std::string qname;
};

struct XUnionType : public Type {
  XUnionType(std::string name, std::vector<XUnionField> fields, std::string qname,
             types::Nullability nullability, types::Strictness strictness)
      : Type(Kind::kXUnion, std::move(name), 24u, CodingNeeded::kAlways),
        fields(std::move(fields)),
        qname(std::move(qname)),
        nullability(nullability),
        strictness(strictness) {}

  std::vector<XUnionField> fields;
  const std::string qname;
  types::Nullability nullability;
  XUnionType* maybe_reference_type = nullptr;
  types::Strictness strictness;
};

struct MessageType : public Type {
  MessageType(std::string name, std::vector<StructField> fields, uint32_t size, std::string qname)
      : Type(Kind::kMessage, std::move(name), size, CodingNeeded::kAlways),
        fields(std::move(fields)),
        qname(std::move(qname)) {}

  std::vector<StructField> fields;
  std::string qname;
};

struct ProtocolType : public Type {
  explicit ProtocolType(std::vector<std::unique_ptr<MessageType>> messages)
      // N.B. ProtocolTypes are never used in the eventual coding table generation.
      : Type(Kind::kProtocol, "", 0, CodingNeeded::kEnvelopeOnly), messages(std::move(messages)) {}

  std::vector<std::unique_ptr<MessageType>> messages;
};

struct ArrayType : public Type {
  ArrayType(std::string name, const Type* element_type, uint32_t array_size, uint32_t element_size,
            CodingContext context)
      : Type(Kind::kArray, std::move(name), array_size,
             WhichCodingNeeded(context, element_type->coding_needed)),
        element_type(element_type),
        element_size(element_size) {}

  const Type* const element_type;
  const uint32_t element_size;
};

struct StringType : public Type {
  StringType(std::string name, uint32_t max_size, types::Nullability nullability)
      : Type(Kind::kString, std::move(name), 16u, CodingNeeded::kAlways),
        max_size(max_size),
        nullability(nullability) {}

  const uint32_t max_size;
  const types::Nullability nullability;
};

struct VectorType : public Type {
  VectorType(std::string name, const Type* element_type, uint32_t max_count, uint32_t element_size,
             types::Nullability nullability)
      : Type(Kind::kVector, std::move(name), 16u, CodingNeeded::kAlways),
        element_type(element_type),
        max_count(max_count),
        element_size(element_size),
        nullability(nullability) {}

  const Type* const element_type;
  const uint32_t max_count;
  const uint32_t element_size;
  const types::Nullability nullability;
};

}  // namespace coded
}  // namespace fidl

#endif  // ZIRCON_SYSTEM_HOST_FIDL_INCLUDE_FIDL_CODED_AST_H_
