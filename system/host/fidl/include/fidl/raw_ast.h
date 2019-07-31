// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_HOST_FIDL_INCLUDE_FIDL_RAW_AST_H_
#define ZIRCON_SYSTEM_HOST_FIDL_INCLUDE_FIDL_RAW_AST_H_

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "source_location.h"
#include "token.h"
#include "types.h"

// ASTs fresh out of the oven. This is a tree-shaped bunch of nodes
// pretty much exactly corresponding to the grammar of a single fidl
// file. File is the root of the tree, and consists of lists of
// Declarations, and so on down to individual SourceLocations.

// Each node owns its children via unique_ptr and vector. All tokens
// here, like everywhere in the fidl compiler, are backed by a string
// view whose contents are owned by a SourceManager.

// This class has a tight coupling with the TreeVisitor class.  Each node has a
// corresponding method in that class.  Each node type also has an Accept()
// method to help visitors visit the node.  When you add a new node, or add a
// field to an existing node, you must ensure the Accept method works.

// A raw::File is produced by parsing a token stream. All of the
// Files in a library are then flattened out into a Library.

namespace fidl {
namespace raw {

// In order to be able to associate AST nodes with their original source, each
// node is a SourceElement, which contains information about the original
// source.  The AST has a start token, whose previous_end field points to the
// end of the previous AST node, and an end token, which points to the end of
// this syntactic element.
//
// Note: The file may have a tail of whitespace / comment text not explicitly
// associated with any node.  In order to reconstruct that text, raw::File
// contains an end token; the previous_end field of that token points to the end
// of the last interesting token.
class TreeVisitor;

class SourceElement {
 public:
  explicit SourceElement(SourceElement const& element)
      : start_(element.start_), end_(element.end_) {}

  explicit SourceElement(Token start, Token end) : start_(start), end_(end) {}

  SourceLocation location() const {
    assert(&start_.location().source_file() == &end_.location().source_file());
    const char* start_pos = start_.location().data().data();
    const char* end_pos = end_.location().data().data() + end_.location().data().length();
    return SourceLocation(std::string_view(start_pos, end_pos - start_pos),
                          start_.location().source_file());
  }

  virtual ~SourceElement() {}

  Token start_;
  Token end_;
};

class SourceElementMark {
 public:
  SourceElementMark(TreeVisitor* tv, const SourceElement& element);

  ~SourceElementMark();

 private:
  TreeVisitor* tv_;
  const SourceElement& element_;
};

class Identifier final : public SourceElement {
 public:
  explicit Identifier(SourceElement const& element) : SourceElement(element) {}

  void Accept(TreeVisitor* visitor) const;
};

class CompoundIdentifier final : public SourceElement {
 public:
  CompoundIdentifier(SourceElement const& element,
                     std::vector<std::unique_ptr<Identifier>> components)
      : SourceElement(element), components(std::move(components)) {}

  void Accept(TreeVisitor* visitor) const;

  std::vector<std::unique_ptr<Identifier>> components;
};

class Literal : public SourceElement {
 public:
  enum struct Kind {
    kString,
    kNumeric,
    // TODO(pascallouis): should have kBool instead.
    kTrue,
    kFalse,
  };

  explicit Literal(SourceElement const& element, Kind kind) : SourceElement(element), kind(kind) {}

  virtual ~Literal() {}

  const Kind kind;
};

class StringLiteral final : public Literal {
 public:
  explicit StringLiteral(SourceElement const& element) : Literal(element, Kind::kString) {}

  void Accept(TreeVisitor* visitor) const;
};

class NumericLiteral final : public Literal {
 public:
  NumericLiteral(SourceElement const& element) : Literal(element, Kind::kNumeric) {}

  void Accept(TreeVisitor* visitor) const;
};

class Ordinal32 final : public SourceElement {
 public:
  Ordinal32(SourceElement const& element, uint32_t value) : SourceElement(element), value(value) {}

  void Accept(TreeVisitor* visitor) const;

  const uint32_t value;
};

class Ordinal64 final : public SourceElement {
 public:
  Ordinal64(SourceElement const& element, uint64_t value) : SourceElement(element), value(value) {}

  void Accept(TreeVisitor* visitor) const;

  const uint64_t value;
};

class TrueLiteral final : public Literal {
 public:
  TrueLiteral(SourceElement const& element) : Literal(element, Kind::kTrue) {}

  void Accept(TreeVisitor* visitor) const;
};

class FalseLiteral final : public Literal {
 public:
  FalseLiteral(SourceElement const& element) : Literal(element, Kind::kFalse) {}

  void Accept(TreeVisitor* visitor) const;
};

class Constant : public SourceElement {
 public:
  enum class Kind {
    kIdentifier,
    kLiteral,
  };

  explicit Constant(Token token, Kind kind) : SourceElement(token, token), kind(kind) {}

  virtual ~Constant() {}

  const Kind kind;
};

class IdentifierConstant final : public Constant {
 public:
  explicit IdentifierConstant(std::unique_ptr<CompoundIdentifier> identifier)
      : Constant(identifier->start_, Kind::kIdentifier), identifier(std::move(identifier)) {}

  std::unique_ptr<CompoundIdentifier> identifier;

  void Accept(TreeVisitor* visitor) const;
};

class LiteralConstant final : public Constant {
 public:
  explicit LiteralConstant(std::unique_ptr<Literal> literal)
      : Constant(literal->start_, Kind::kLiteral), literal(std::move(literal)) {}

  std::unique_ptr<Literal> literal;

  void Accept(TreeVisitor* visitor) const;
};

class Attribute final : public SourceElement {
 public:
  Attribute(SourceElement const& element, std::string name, std::string value)
      : SourceElement(element), name(std::move(name)), value(std::move(value)) {}

  void Accept(TreeVisitor* visitor) const;

  const std::string name;
  const std::string value;
};

class AttributeList final : public SourceElement {
 public:
  AttributeList(SourceElement const& element, std::vector<Attribute> attributes)
      : SourceElement(element), attributes(std::move(attributes)) {}

  bool HasAttribute(std::string name) const {
    for (const auto& attribute : attributes) {
      if (attribute.name == name)
        return true;
    }
    return false;
  }

  void Accept(TreeVisitor* visitor) const;

  std::vector<Attribute> attributes;
};

class TypeConstructor final : public SourceElement {
 public:
  TypeConstructor(SourceElement const& element, std::unique_ptr<CompoundIdentifier> identifier,
                  std::unique_ptr<TypeConstructor> maybe_arg_type_ctor,
                  std::optional<types::HandleSubtype> handle_subtype,
                  std::unique_ptr<Constant> maybe_size, types::Nullability nullability)
      : SourceElement(element),
        identifier(std::move(identifier)),
        maybe_arg_type_ctor(std::move(maybe_arg_type_ctor)),
        handle_subtype(handle_subtype),
        maybe_size(std::move(maybe_size)),
        nullability(nullability) {}

  void Accept(TreeVisitor* visitor) const;

  std::unique_ptr<CompoundIdentifier> identifier;
  std::unique_ptr<TypeConstructor> maybe_arg_type_ctor;
  std::optional<types::HandleSubtype> handle_subtype;
  std::unique_ptr<Constant> maybe_size;
  types::Nullability nullability;
};

class BitsMember final : public SourceElement {
 public:
  BitsMember(SourceElement const& element, std::unique_ptr<Identifier> identifier,
             std::unique_ptr<Constant> value, std::unique_ptr<AttributeList> attributes)
      : SourceElement(element),
        identifier(std::move(identifier)),
        value(std::move(value)),
        attributes(std::move(attributes)) {}

  void Accept(TreeVisitor* visitor) const;

  std::unique_ptr<Identifier> identifier;
  std::unique_ptr<Constant> value;
  std::unique_ptr<AttributeList> attributes;
};

class BitsDeclaration final : public SourceElement {
 public:
  BitsDeclaration(SourceElement const& element, std::unique_ptr<AttributeList> attributes,
                  std::unique_ptr<Identifier> identifier,
                  std::unique_ptr<TypeConstructor> maybe_type_ctor,
                  std::vector<std::unique_ptr<BitsMember>> members)
      : SourceElement(element),
        attributes(std::move(attributes)),
        identifier(std::move(identifier)),
        maybe_type_ctor(std::move(maybe_type_ctor)),
        members(std::move(members)) {}

  void Accept(TreeVisitor* visitor) const;

  std::unique_ptr<AttributeList> attributes;
  std::unique_ptr<Identifier> identifier;
  std::unique_ptr<TypeConstructor> maybe_type_ctor;
  std::vector<std::unique_ptr<BitsMember>> members;
};

class Using final : public SourceElement {
 public:
  Using(SourceElement const& element, std::unique_ptr<AttributeList> attributes,
        std::unique_ptr<CompoundIdentifier> using_path, std::unique_ptr<Identifier> maybe_alias,
        std::unique_ptr<TypeConstructor> maybe_type_ctor)
      : SourceElement(element),
        attributes(std::move(attributes)),
        using_path(std::move(using_path)),
        maybe_alias(std::move(maybe_alias)),
        maybe_type_ctor(std::move(maybe_type_ctor)) {}

  void Accept(TreeVisitor* visitor) const;

  std::unique_ptr<AttributeList> attributes;
  std::unique_ptr<CompoundIdentifier> using_path;
  std::unique_ptr<Identifier> maybe_alias;
  // TODO(FIDL-483): Use a special purpose AST element, as is the case in the
  // flat AST.
  std::unique_ptr<TypeConstructor> maybe_type_ctor;
};

class ConstDeclaration final : public SourceElement {
 public:
  ConstDeclaration(SourceElement const& element, std::unique_ptr<AttributeList> attributes,
                   std::unique_ptr<TypeConstructor> type_ctor,
                   std::unique_ptr<Identifier> identifier, std::unique_ptr<Constant> constant)
      : SourceElement(element),
        attributes(std::move(attributes)),
        type_ctor(std::move(type_ctor)),
        identifier(std::move(identifier)),
        constant(std::move(constant)) {}

  void Accept(TreeVisitor* visitor) const;

  std::unique_ptr<AttributeList> attributes;
  std::unique_ptr<TypeConstructor> type_ctor;
  std::unique_ptr<Identifier> identifier;
  std::unique_ptr<Constant> constant;
};

class EnumMember final : public SourceElement {
 public:
  EnumMember(SourceElement const& element, std::unique_ptr<Identifier> identifier,
             std::unique_ptr<Constant> value, std::unique_ptr<AttributeList> attributes)
      : SourceElement(element),
        identifier(std::move(identifier)),
        value(std::move(value)),
        attributes(std::move(attributes)) {}

  void Accept(TreeVisitor* visitor) const;

  std::unique_ptr<Identifier> identifier;
  std::unique_ptr<Constant> value;
  std::unique_ptr<AttributeList> attributes;
};

class EnumDeclaration final : public SourceElement {
 public:
  EnumDeclaration(SourceElement const& element, std::unique_ptr<AttributeList> attributes,
                  std::unique_ptr<Identifier> identifier,
                  std::unique_ptr<TypeConstructor> maybe_type_ctor,
                  std::vector<std::unique_ptr<EnumMember>> members)
      : SourceElement(element),
        attributes(std::move(attributes)),
        identifier(std::move(identifier)),
        maybe_type_ctor(std::move(maybe_type_ctor)),
        members(std::move(members)) {}

  void Accept(TreeVisitor* visitor) const;

  std::unique_ptr<AttributeList> attributes;
  std::unique_ptr<Identifier> identifier;
  std::unique_ptr<TypeConstructor> maybe_type_ctor;
  std::vector<std::unique_ptr<EnumMember>> members;
};

class Parameter final : public SourceElement {
 public:
  Parameter(SourceElement const& element, std::unique_ptr<TypeConstructor> type_ctor,
            std::unique_ptr<Identifier> identifier)
      : SourceElement(element),
        type_ctor(std::move(type_ctor)),
        identifier(std::move(identifier)) {}

  void Accept(TreeVisitor* visitor) const;

  std::unique_ptr<TypeConstructor> type_ctor;
  std::unique_ptr<Identifier> identifier;
};

class ParameterList final : public SourceElement {
 public:
  ParameterList(SourceElement const& element,
                std::vector<std::unique_ptr<Parameter>> parameter_list)
      : SourceElement(element), parameter_list(std::move(parameter_list)) {}

  void Accept(TreeVisitor* visitor) const;

  std::vector<std::unique_ptr<Parameter>> parameter_list;
};

class ProtocolMethod : public SourceElement {
 public:
  ProtocolMethod(SourceElement const& element, std::unique_ptr<AttributeList> attributes,
                 std::unique_ptr<Identifier> identifier,
                 std::unique_ptr<ParameterList> maybe_request,
                 std::unique_ptr<ParameterList> maybe_response,
                 std::unique_ptr<TypeConstructor> maybe_error_ctor)
      : SourceElement(element),
        attributes(std::move(attributes)),
        identifier(std::move(identifier)),
        maybe_request(std::move(maybe_request)),
        maybe_response(std::move(maybe_response)),
        maybe_error_ctor(std::move(maybe_error_ctor)) {}

  void Accept(TreeVisitor* visitor) const;

  std::unique_ptr<AttributeList> attributes;
  std::unique_ptr<Identifier> identifier;
  std::unique_ptr<ParameterList> maybe_request;
  std::unique_ptr<ParameterList> maybe_response;
  std::unique_ptr<TypeConstructor> maybe_error_ctor;
};

class ComposeProtocol final : public SourceElement {
 public:
  ComposeProtocol(SourceElement const& element, std::unique_ptr<CompoundIdentifier> protocol_name)
      : SourceElement(element), protocol_name(std::move(protocol_name)) {}

  void Accept(TreeVisitor* visitor) const;

  std::unique_ptr<CompoundIdentifier> protocol_name;
};

class ProtocolDeclaration final : public SourceElement {
 public:
  ProtocolDeclaration(SourceElement const& element, std::unique_ptr<AttributeList> attributes,
                      std::unique_ptr<Identifier> identifier,
                      std::vector<std::unique_ptr<ComposeProtocol>> composed_protocols,
                      std::vector<std::unique_ptr<ProtocolMethod>> methods)
      : SourceElement(element),
        attributes(std::move(attributes)),
        identifier(std::move(identifier)),
        composed_protocols(std::move(composed_protocols)),
        methods(std::move(methods)) {}

  void Accept(TreeVisitor* visitor) const;

  std::unique_ptr<AttributeList> attributes;
  std::unique_ptr<Identifier> identifier;
  std::vector<std::unique_ptr<ComposeProtocol>> composed_protocols;
  std::vector<std::unique_ptr<ProtocolMethod>> methods;
};

class StructMember final : public SourceElement {
 public:
  StructMember(SourceElement const& element, std::unique_ptr<TypeConstructor> type_ctor,
               std::unique_ptr<Identifier> identifier,
               std::unique_ptr<Constant> maybe_default_value,
               std::unique_ptr<AttributeList> attributes)
      : SourceElement(element),
        type_ctor(std::move(type_ctor)),
        identifier(std::move(identifier)),
        maybe_default_value(std::move(maybe_default_value)),
        attributes(std::move(attributes)) {}

  void Accept(TreeVisitor* visitor) const;

  std::unique_ptr<TypeConstructor> type_ctor;
  std::unique_ptr<Identifier> identifier;
  std::unique_ptr<Constant> maybe_default_value;
  std::unique_ptr<AttributeList> attributes;
};

class StructDeclaration final : public SourceElement {
 public:
  // Note: A nullptr passed to attributes means an empty attribute list.
  StructDeclaration(SourceElement const& element, std::unique_ptr<AttributeList> attributes,
                    std::unique_ptr<Identifier> identifier,
                    std::vector<std::unique_ptr<StructMember>> members)
      : SourceElement(element),
        attributes(std::move(attributes)),
        identifier(std::move(identifier)),
        members(std::move(members)) {}

  void Accept(TreeVisitor* visitor) const;

  std::unique_ptr<AttributeList> attributes;
  std::unique_ptr<Identifier> identifier;
  std::vector<std::unique_ptr<StructMember>> members;
};

struct TableMember final : public SourceElement {
  TableMember(SourceElement const& element, std::unique_ptr<Ordinal32> ordinal,
              std::unique_ptr<TypeConstructor> type_ctor, std::unique_ptr<Identifier> identifier,
              std::unique_ptr<Constant> maybe_default_value,
              std::unique_ptr<AttributeList> attributes)
      : SourceElement(element),
        ordinal(std::move(ordinal)),
        maybe_used(std::make_unique<Used>(std::move(type_ctor), std::move(identifier),
                                          std::move(maybe_default_value), std::move(attributes))) {}

  TableMember(SourceElement const& element, std::unique_ptr<Ordinal32> ordinal)
      : SourceElement(element), ordinal(std::move(ordinal)) {}

  void Accept(TreeVisitor* visitor) const;

  std::unique_ptr<Ordinal32> ordinal;
  // A used member is not 'reserved'
  struct Used {
    Used(std::unique_ptr<TypeConstructor> type_ctor, std::unique_ptr<Identifier> identifier,
         std::unique_ptr<Constant> maybe_default_value, std::unique_ptr<AttributeList> attributes)
        : type_ctor(std::move(type_ctor)),
          identifier(std::move(identifier)),
          maybe_default_value(std::move(maybe_default_value)),
          attributes(std::move(attributes)) {}
    std::unique_ptr<TypeConstructor> type_ctor;
    std::unique_ptr<Identifier> identifier;
    std::unique_ptr<Constant> maybe_default_value;
    std::unique_ptr<AttributeList> attributes;
  };
  std::unique_ptr<Used> maybe_used;
};

struct TableDeclaration final : public SourceElement {
  TableDeclaration(SourceElement const& element, std::unique_ptr<AttributeList> attributes,
                   std::unique_ptr<Identifier> identifier,
                   std::vector<std::unique_ptr<TableMember>> members)
      : SourceElement(element),
        attributes(std::move(attributes)),
        identifier(std::move(identifier)),
        members(std::move(members)) {}

  void Accept(TreeVisitor* visitor) const;

  std::unique_ptr<AttributeList> attributes;
  std::unique_ptr<Identifier> identifier;
  std::vector<std::unique_ptr<TableMember>> members;
};

class UnionMember final : public SourceElement {
 public:
  UnionMember(SourceElement const& element, std::unique_ptr<TypeConstructor> type_ctor,
              std::unique_ptr<Identifier> identifier, std::unique_ptr<AttributeList> attributes)
      : SourceElement(element),
        type_ctor(std::move(type_ctor)),
        identifier(std::move(identifier)),
        attributes(std::move(attributes)) {}

  void Accept(TreeVisitor* visitor) const;

  std::unique_ptr<TypeConstructor> type_ctor;
  std::unique_ptr<Identifier> identifier;
  std::unique_ptr<AttributeList> attributes;
};

class UnionDeclaration final : public SourceElement {
 public:
  UnionDeclaration(SourceElement const& element, std::unique_ptr<AttributeList> attributes,
                   std::unique_ptr<Identifier> identifier,
                   std::vector<std::unique_ptr<UnionMember>> members)
      : SourceElement(element),
        attributes(std::move(attributes)),
        identifier(std::move(identifier)),
        members(std::move(members)) {}

  void Accept(TreeVisitor* visitor) const;

  std::unique_ptr<AttributeList> attributes;
  std::unique_ptr<Identifier> identifier;
  std::vector<std::unique_ptr<UnionMember>> members;
};

class XUnionMember final : public SourceElement {
 public:
  XUnionMember(SourceElement const& element, std::unique_ptr<TypeConstructor> type_ctor,
               std::unique_ptr<Identifier> identifier, std::unique_ptr<AttributeList> attributes)
      : SourceElement(element),
        type_ctor(std::move(type_ctor)),
        identifier(std::move(identifier)),
        attributes(std::move(attributes)) {}

  void Accept(TreeVisitor* visitor) const;

  std::unique_ptr<TypeConstructor> type_ctor;
  std::unique_ptr<Identifier> identifier;
  std::unique_ptr<AttributeList> attributes;
};

class XUnionDeclaration final : public SourceElement {
 public:
  XUnionDeclaration(SourceElement const& element, std::unique_ptr<AttributeList> attributes,
                    std::unique_ptr<Identifier> identifier,
                    std::vector<std::unique_ptr<XUnionMember>> members,
                    types::Strictness strictness)
      : SourceElement(element),
        attributes(std::move(attributes)),
        identifier(std::move(identifier)),
        members(std::move(members)),
        strictness(strictness) {}

  void Accept(TreeVisitor* visitor) const;

  std::unique_ptr<AttributeList> attributes;
  std::unique_ptr<Identifier> identifier;
  std::vector<std::unique_ptr<XUnionMember>> members;
  const types::Strictness strictness;
};

class File final : public SourceElement {
 public:
  File(SourceElement const& element, Token end, std::unique_ptr<AttributeList> attributes,
       std::unique_ptr<CompoundIdentifier> library_name,
       std::vector<std::unique_ptr<Using>> using_list,
       std::vector<std::unique_ptr<BitsDeclaration>> bits_declaration_list,
       std::vector<std::unique_ptr<ConstDeclaration>> const_declaration_list,
       std::vector<std::unique_ptr<EnumDeclaration>> enum_declaration_list,
       std::vector<std::unique_ptr<ProtocolDeclaration>> protocol_declaration_list,
       std::vector<std::unique_ptr<StructDeclaration>> struct_declaration_list,
       std::vector<std::unique_ptr<TableDeclaration>> table_declaration_list,
       std::vector<std::unique_ptr<UnionDeclaration>> union_declaration_list,
       std::vector<std::unique_ptr<XUnionDeclaration>> xunion_declaration_list)
      : SourceElement(element),
        attributes(std::move(attributes)),
        library_name(std::move(library_name)),
        using_list(std::move(using_list)),
        bits_declaration_list(std::move(bits_declaration_list)),
        const_declaration_list(std::move(const_declaration_list)),
        enum_declaration_list(std::move(enum_declaration_list)),
        protocol_declaration_list(std::move(protocol_declaration_list)),
        struct_declaration_list(std::move(struct_declaration_list)),
        table_declaration_list(std::move(table_declaration_list)),
        union_declaration_list(std::move(union_declaration_list)),
        xunion_declaration_list(std::move(xunion_declaration_list)),
        end_(end) {}

  void Accept(TreeVisitor* visitor) const;

  std::unique_ptr<AttributeList> attributes;
  std::unique_ptr<CompoundIdentifier> library_name;
  std::vector<std::unique_ptr<Using>> using_list;
  std::vector<std::unique_ptr<BitsDeclaration>> bits_declaration_list;
  std::vector<std::unique_ptr<ConstDeclaration>> const_declaration_list;
  std::vector<std::unique_ptr<EnumDeclaration>> enum_declaration_list;
  std::vector<std::unique_ptr<ProtocolDeclaration>> protocol_declaration_list;
  std::vector<std::unique_ptr<StructDeclaration>> struct_declaration_list;
  std::vector<std::unique_ptr<TableDeclaration>> table_declaration_list;
  std::vector<std::unique_ptr<UnionDeclaration>> union_declaration_list;
  std::vector<std::unique_ptr<XUnionDeclaration>> xunion_declaration_list;
  Token end_;
};

}  // namespace raw
}  // namespace fidl

#endif  // ZIRCON_SYSTEM_HOST_FIDL_INCLUDE_FIDL_RAW_AST_H_
