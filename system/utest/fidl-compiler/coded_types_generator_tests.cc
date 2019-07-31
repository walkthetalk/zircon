// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/tables_generator.h>
#include <unittest/unittest.h>

#include "test_library.h"

namespace {

bool CodedTypesOfArrays() {
  BEGIN_TEST;

  TestLibrary library(R"FIDL(
library example;

struct Arrays {
  array<uint8>:7 prime;
  array<array<uint8>:7>:11 next_prime;
  array<array<array<uint8>:7>:11>:13 next_next_prime;
};
)FIDL");
  ASSERT_TRUE(library.Compile());
  fidl::CodedTypesGenerator gen(library.library());
  gen.CompileCodedTypes();

  ASSERT_EQ(4, gen.coded_types().size());

  auto type0 = gen.coded_types().at(0).get();
  ASSERT_STR_EQ("uint8", type0->coded_name.c_str());
  ASSERT_EQ(fidl::coded::CodingNeeded::kEnvelopeOnly, type0->coding_needed);
  ASSERT_EQ(fidl::coded::Type::Kind::kPrimitive, type0->kind);
  auto type0_primitive = static_cast<const fidl::coded::PrimitiveType*>(type0);
  ASSERT_EQ(fidl::types::PrimitiveSubtype::kUint8, type0_primitive->subtype);

  auto type1 = gen.coded_types().at(1).get();
  ASSERT_STR_EQ("Arrayuint87", type1->coded_name.c_str());
  ASSERT_EQ(fidl::coded::CodingNeeded::kEnvelopeOnly, type1->coding_needed);
  ASSERT_EQ(fidl::coded::Type::Kind::kArray, type1->kind);
  auto type1_array = static_cast<const fidl::coded::ArrayType*>(type1);
  ASSERT_EQ(1, type1_array->element_size);
  ASSERT_EQ(type0, type1_array->element_type);

  auto type2 = gen.coded_types().at(2).get();
  ASSERT_STR_EQ("ArrayArrayuint8777", type2->coded_name.c_str());
  ASSERT_EQ(fidl::coded::CodingNeeded::kEnvelopeOnly, type2->coding_needed);
  ASSERT_EQ(fidl::coded::Type::Kind::kArray, type2->kind);
  auto type2_array = static_cast<const fidl::coded::ArrayType*>(type2);
  ASSERT_EQ(7 * 1, type2_array->element_size);
  ASSERT_EQ(type1, type2_array->element_type);

  auto type3 = gen.coded_types().at(3).get();
  ASSERT_STR_EQ("ArrayArrayArrayuint87771001", type3->coded_name.c_str());
  ASSERT_EQ(fidl::coded::CodingNeeded::kEnvelopeOnly, type3->coding_needed);
  ASSERT_EQ(fidl::coded::Type::Kind::kArray, type3->kind);
  auto type3_array = static_cast<const fidl::coded::ArrayType*>(type3);
  ASSERT_EQ(11 * 7 * 1, type3_array->element_size);
  ASSERT_EQ(type2, type3_array->element_type);

  END_TEST;
}

bool CodedTypesOfVectors() {
  BEGIN_TEST;

  TestLibrary library(R"FIDL(
library example;

struct SomeStruct {};

struct Vectors {
  vector<SomeStruct>:10 bytes1;
  vector<vector<SomeStruct>:10>:20 bytes12;
};
)FIDL");
  ASSERT_TRUE(library.Compile());
  fidl::CodedTypesGenerator gen(library.library());
  gen.CompileCodedTypes();

  auto name_some_struct = fidl::flat::Name(library.library(), "SomeStruct");
  auto type_some_struct = gen.CodedTypeFor(&name_some_struct);
  ASSERT_NONNULL(type_some_struct);
  ASSERT_STR_EQ("example_SomeStruct", type_some_struct->coded_name.c_str());
  ASSERT_EQ(fidl::coded::CodingNeeded::kAlways, type_some_struct->coding_needed);
  ASSERT_EQ(fidl::coded::Type::Kind::kStruct, type_some_struct->kind);
  auto type_some_struct_struct = static_cast<const fidl::coded::StructType*>(type_some_struct);
  ASSERT_EQ(0, type_some_struct_struct->fields.size());
  ASSERT_STR_EQ("example/SomeStruct", type_some_struct_struct->qname.c_str());
  ASSERT_NULL(type_some_struct_struct->maybe_reference_type);

  ASSERT_EQ(2, gen.coded_types().size());

  auto type0 = gen.coded_types().at(0).get();
  ASSERT_STR_EQ("Vectorexample_SomeStruct10nonnullable", type0->coded_name.c_str());
  ASSERT_EQ(fidl::coded::CodingNeeded::kAlways, type0->coding_needed);
  ASSERT_EQ(fidl::coded::Type::Kind::kVector, type0->kind);
  auto type0_vector = static_cast<const fidl::coded::VectorType*>(type0);
  ASSERT_EQ(type_some_struct, type0_vector->element_type);
  ASSERT_EQ(10, type0_vector->max_count);
  ASSERT_EQ(1, type0_vector->element_size);
  ASSERT_EQ(fidl::types::Nullability::kNonnullable, type0_vector->nullability);

  auto type1 = gen.coded_types().at(1).get();
  ASSERT_STR_EQ("VectorVectorexample_SomeStruct10nonnullable20nonnullable",
                type1->coded_name.c_str());
  ASSERT_EQ(fidl::coded::CodingNeeded::kAlways, type1->coding_needed);
  ASSERT_EQ(fidl::coded::Type::Kind::kVector, type1->kind);
  auto type1_vector = static_cast<const fidl::coded::VectorType*>(type1);
  ASSERT_EQ(type0, type1_vector->element_type);
  ASSERT_EQ(20, type1_vector->max_count);
  ASSERT_EQ(16, type1_vector->element_size);
  ASSERT_EQ(fidl::types::Nullability::kNonnullable, type1_vector->nullability);

  END_TEST;
}

bool CodedTypesOfProtocol() {
  BEGIN_TEST;

  TestLibrary library(R"FIDL(
library example;

protocol SomeProtocol {};

protocol UseOfProtocol {
    Call(SomeProtocol arg);
};
)FIDL");
  ASSERT_TRUE(library.Compile());
  fidl::CodedTypesGenerator gen(library.library());
  gen.CompileCodedTypes();

  ASSERT_EQ(2, gen.coded_types().size());

  auto type0 = gen.coded_types().at(0).get();
  ASSERT_STR_EQ("example_SomeProtocolProtocolnonnullable", type0->coded_name.c_str());
  ASSERT_EQ(fidl::coded::CodingNeeded::kAlways, type0->coding_needed);
  ASSERT_EQ(fidl::coded::Type::Kind::kProtocolHandle, type0->kind);
  ASSERT_EQ(4, type0->size);
  auto type0_ihandle = static_cast<const fidl::coded::ProtocolHandleType*>(type0);
  ASSERT_EQ(fidl::types::Nullability::kNonnullable, type0_ihandle->nullability);

  auto type1 = gen.coded_types().at(1).get();
  ASSERT_STR_EQ("example_UseOfProtocolCallRequest", type1->coded_name.c_str());
  ASSERT_EQ(fidl::coded::CodingNeeded::kAlways, type1->coding_needed);
  ASSERT_EQ(fidl::coded::Type::Kind::kMessage, type1->kind);
  ASSERT_EQ(24, type1->size);
  auto type1_message = static_cast<const fidl::coded::MessageType*>(type1);
  ASSERT_STR_EQ("example/UseOfProtocolCallRequest", type1_message->qname.c_str());
  ASSERT_EQ(1, type1_message->fields.size());

  auto type1_message_field0 = type1_message->fields.at(0);
  ASSERT_EQ(16, type1_message_field0.offset);
  ASSERT_EQ(type0, type1_message_field0.type);

  END_TEST;
}

bool CodedTypesOfRequestOfProtocol() {
  BEGIN_TEST;

  TestLibrary library(R"FIDL(
library example;

protocol SomeProtocol {};

protocol UseOfRequestOfProtocol {
    Call(request<SomeProtocol> arg);
};
)FIDL");
  ASSERT_TRUE(library.Compile());
  fidl::CodedTypesGenerator gen(library.library());
  gen.CompileCodedTypes();

  ASSERT_EQ(2, gen.coded_types().size());

  auto type0 = gen.coded_types().at(0).get();
  ASSERT_STR_EQ("example_SomeProtocolRequestnonnullable", type0->coded_name.c_str());
  ASSERT_EQ(fidl::coded::CodingNeeded::kAlways, type0->coding_needed);
  ASSERT_EQ(fidl::coded::Type::Kind::kRequestHandle, type0->kind);
  ASSERT_EQ(4, type0->size);
  auto type0_ihandle = static_cast<const fidl::coded::RequestHandleType*>(type0);
  ASSERT_EQ(fidl::types::Nullability::kNonnullable, type0_ihandle->nullability);

  auto type1 = gen.coded_types().at(1).get();
  ASSERT_STR_EQ("example_UseOfRequestOfProtocolCallRequest", type1->coded_name.c_str());
  ASSERT_EQ(fidl::coded::CodingNeeded::kAlways, type1->coding_needed);
  ASSERT_EQ(fidl::coded::Type::Kind::kMessage, type1->kind);
  ASSERT_EQ(24, type1->size);
  auto type1_message = static_cast<const fidl::coded::MessageType*>(type1);
  ASSERT_STR_EQ("example/UseOfRequestOfProtocolCallRequest", type1_message->qname.c_str());
  ASSERT_EQ(1, type1_message->fields.size());

  auto type1_message_field0 = type1_message->fields.at(0);
  ASSERT_EQ(16, type1_message_field0.offset);
  ASSERT_EQ(type0, type1_message_field0.type);

  END_TEST;
}

bool CodedTypesOfXUnions() {
  BEGIN_TEST;

  TestLibrary library(R"FIDL(
library example;

xunion MyXUnion {
  bool foo;
  int32 bar;
};
)FIDL");
  ASSERT_TRUE(library.Compile());
  fidl::CodedTypesGenerator gen(library.library());
  gen.CompileCodedTypes();

  ASSERT_EQ(2, gen.coded_types().size());

  auto type0 = gen.coded_types().at(0).get();
  ASSERT_STR_EQ("int32", type0->coded_name.c_str());
  ASSERT_EQ(fidl::coded::CodingNeeded::kAlways, type0->coding_needed);
  ASSERT_EQ(fidl::coded::Type::Kind::kPrimitive, type0->kind);
  auto type0_primitive = static_cast<const fidl::coded::PrimitiveType*>(type0);
  ASSERT_EQ(fidl::types::PrimitiveSubtype::kInt32, type0_primitive->subtype);

  auto type1 = gen.coded_types().at(1).get();
  ASSERT_STR_EQ("bool", type1->coded_name.c_str());
  ASSERT_EQ(fidl::coded::CodingNeeded::kAlways, type1->coding_needed);
  ASSERT_EQ(fidl::coded::Type::Kind::kPrimitive, type1->kind);
  auto type1_primitive = static_cast<const fidl::coded::PrimitiveType*>(type1);
  ASSERT_EQ(fidl::types::PrimitiveSubtype::kBool, type1_primitive->subtype);

  auto name_xunion = fidl::flat::Name(library.library(), "MyXUnion");
  auto type_xunion = gen.CodedTypeFor(&name_xunion);
  ASSERT_NONNULL(type_xunion);
  ASSERT_STR_EQ("example_MyXUnion", type_xunion->coded_name.c_str());
  ASSERT_EQ(fidl::coded::CodingNeeded::kAlways, type_xunion->coding_needed);
  ASSERT_EQ(fidl::coded::Type::Kind::kXUnion, type_xunion->kind);
  auto type_xunion_xunion = static_cast<const fidl::coded::XUnionType*>(type_xunion);
  ASSERT_EQ(2, type_xunion_xunion->fields.size());
  auto xunion_field0 = type_xunion_xunion->fields.at(0);
  ASSERT_EQ(fidl::coded::Type::Kind::kPrimitive, xunion_field0.type->kind);
  auto xunion_field0_primitive = static_cast<const fidl::coded::PrimitiveType*>(xunion_field0.type);
  ASSERT_EQ(fidl::types::PrimitiveSubtype::kInt32, xunion_field0_primitive->subtype);
  auto xunion_field1 = type_xunion_xunion->fields.at(1);
  ASSERT_EQ(fidl::coded::Type::Kind::kPrimitive, xunion_field1.type->kind);
  auto xunion_field1_primitive = static_cast<const fidl::coded::PrimitiveType*>(xunion_field1.type);
  ASSERT_EQ(fidl::types::PrimitiveSubtype::kBool, xunion_field1_primitive->subtype);
  ASSERT_STR_EQ("example/MyXUnion", type_xunion_xunion->qname.c_str());
  ASSERT_EQ(fidl::types::Nullability::kNonnullable, type_xunion_xunion->nullability);
  ASSERT_NULL(type_xunion_xunion->maybe_reference_type);

  END_TEST;
}

// This mostly exists to make sure that the same nullable objects aren't
// represented more than once in the coding tables.
bool CodedTypesOfNullablePointers() {
  BEGIN_TEST;

  TestLibrary library(R"FIDL(
library example;

struct MyStruct {
  bool foo;
  int32 bar;
};

union MyUnion {
  bool foo;
  int32 bar;
};

xunion MyXUnion {
  bool foo;
  int32 bar;
};

struct Wrapper1 {
  MyStruct? ms;
  MyUnion? mu;
  MyXUnion? xu;
};

// This ensures that MyXUnion? doesn't show up twice in the coded types.
struct Wrapper2 {
  MyStruct? ms;
  MyUnion? mu;
  MyXUnion? xu;
};

)FIDL");
  ASSERT_TRUE(library.Compile());
  fidl::CodedTypesGenerator gen(library.library());
  gen.CompileCodedTypes();

  // 7 == size of {bool-outside-of-envelope, bool-inside-of-envelope,
  // int32-outside-of-envelope, int32-inside-of-envelope, MyStruct?, MyUnion?,
  // MyXUnion?}, which is all the coded types in the example.
  ASSERT_EQ(7, gen.coded_types().size());

  END_TEST;
}

bool CodedTypesOfStructsWithPaddings() {
  BEGIN_TEST;

  TestLibrary library(R"FIDL(
library example;

struct BoolAndInt32 {
  bool foo;
  // 3 bytes of padding here.
  int32 bar;
};

struct Complex {
  int32 i32;
  bool b1;
  // 3 bytes of padding here.
  int64 i64;
  int16 i16;
  // 6 bytes of padding here.
};

)FIDL");
  ASSERT_TRUE(library.Compile());
  fidl::CodedTypesGenerator gen(library.library());
  gen.CompileCodedTypes();

  ASSERT_EQ(4, gen.coded_types().size());

  auto type0 = gen.coded_types().at(0).get();
  EXPECT_STR_EQ("int32", type0->coded_name.c_str());
  EXPECT_EQ(fidl::coded::CodingNeeded::kEnvelopeOnly, type0->coding_needed);
  auto type1 = gen.coded_types().at(1).get();
  EXPECT_STR_EQ("bool", type1->coded_name.c_str());
  EXPECT_EQ(fidl::coded::CodingNeeded::kEnvelopeOnly, type1->coding_needed);
  auto type2 = gen.coded_types().at(2).get();
  EXPECT_STR_EQ("int64", type2->coded_name.c_str());
  EXPECT_EQ(fidl::coded::CodingNeeded::kEnvelopeOnly, type2->coding_needed);
  auto type3 = gen.coded_types().at(3).get();
  EXPECT_STR_EQ("int16", type3->coded_name.c_str());
  EXPECT_EQ(fidl::coded::CodingNeeded::kEnvelopeOnly, type3->coding_needed);

  auto name_bool_and_int32 = fidl::flat::Name(library.library(), "BoolAndInt32");
  auto type_bool_and_int32 = gen.CodedTypeFor(&name_bool_and_int32);
  ASSERT_NONNULL(type_bool_and_int32);
  ASSERT_STR_EQ("example_BoolAndInt32", type_bool_and_int32->coded_name.c_str());
  auto type_bool_and_int32_struct =
      static_cast<const fidl::coded::StructType*>(type_bool_and_int32);
  EXPECT_EQ(type_bool_and_int32_struct->fields.size(), 1);
  EXPECT_EQ(type_bool_and_int32_struct->fields[0].type, nullptr);
  EXPECT_EQ(type_bool_and_int32_struct->fields[0].offset, 0);
  EXPECT_EQ(type_bool_and_int32_struct->fields[0].padding, 3);

  auto name_complex = fidl::flat::Name(library.library(), "Complex");
  auto type_complex = gen.CodedTypeFor(&name_complex);
  ASSERT_NONNULL(type_complex);
  ASSERT_STR_EQ("example_Complex", type_complex->coded_name.c_str());
  auto type_complex_struct = static_cast<const fidl::coded::StructType*>(type_complex);
  EXPECT_EQ(type_complex_struct->fields.size(), 2);
  EXPECT_EQ(type_complex_struct->fields[0].type, nullptr);
  EXPECT_EQ(type_complex_struct->fields[0].offset, 4);
  EXPECT_EQ(type_complex_struct->fields[0].padding, 3);
  EXPECT_EQ(type_complex_struct->fields[1].type, nullptr);
  EXPECT_EQ(type_complex_struct->fields[1].offset, 16);
  EXPECT_EQ(type_complex_struct->fields[1].padding, 6);

  END_TEST;
}

bool CodedTypesOfNullableXUnions() {
  BEGIN_TEST;

  TestLibrary library(R"FIDL(
library example;

xunion MyXUnion {
  bool foo;
  int32 bar;
};

struct Wrapper1 {
  MyXUnion? xu;
};

// This ensures that MyXUnion? doesn't show up twice in the coded types.
struct Wrapper2 {
  MyXUnion? xu;
};

)FIDL");
  ASSERT_TRUE(library.Compile());
  fidl::CodedTypesGenerator gen(library.library());
  gen.CompileCodedTypes();

  // 3 == size of {bool, int32, MyXUnion?}, which is all of the types used in
  // the example.
  ASSERT_EQ(3, gen.coded_types().size());

  auto type0 = gen.coded_types().at(0).get();
  ASSERT_STR_EQ("int32", type0->coded_name.c_str());
  ASSERT_EQ(fidl::coded::CodingNeeded::kAlways, type0->coding_needed);
  ASSERT_EQ(fidl::coded::Type::Kind::kPrimitive, type0->kind);
  auto type0_primitive = static_cast<const fidl::coded::PrimitiveType*>(type0);
  ASSERT_EQ(fidl::types::PrimitiveSubtype::kInt32, type0_primitive->subtype);

  auto type1 = gen.coded_types().at(1).get();
  ASSERT_STR_EQ("bool", type1->coded_name.c_str());
  ASSERT_EQ(fidl::coded::CodingNeeded::kAlways, type1->coding_needed);
  ASSERT_EQ(fidl::coded::Type::Kind::kPrimitive, type1->kind);
  auto type1_primitive = static_cast<const fidl::coded::PrimitiveType*>(type1);
  ASSERT_EQ(fidl::types::PrimitiveSubtype::kBool, type1_primitive->subtype);

  auto type2 = gen.coded_types().at(2).get();
  ASSERT_STR_EQ("example_MyXUnionNullableRef", type2->coded_name.c_str());
  ASSERT_EQ(fidl::coded::CodingNeeded::kAlways, type2->coding_needed);
  ASSERT_EQ(fidl::coded::Type::Kind::kXUnion, type2->kind);
  auto type_nullable_xunion_xunion = static_cast<const fidl::coded::XUnionType*>(type2);
  ASSERT_EQ(fidl::types::Nullability::kNullable, type_nullable_xunion_xunion->nullability);

  auto name_xunion = fidl::flat::Name(library.library(), "MyXUnion");
  auto type_xunion = gen.CodedTypeFor(&name_xunion);
  ASSERT_NONNULL(type_xunion);
  ASSERT_STR_EQ("example_MyXUnion", type_xunion->coded_name.c_str());
  ASSERT_EQ(fidl::coded::Type::Kind::kXUnion, type_xunion->kind);
  auto type_xunion_xunion = static_cast<const fidl::coded::XUnionType*>(type_xunion);
  ASSERT_EQ(type_xunion_xunion->maybe_reference_type, type_nullable_xunion_xunion);
  ASSERT_TRUE(type_nullable_xunion_xunion->qname == type_xunion_xunion->qname);
  ASSERT_EQ(fidl::types::Nullability::kNonnullable, type_xunion_xunion->nullability);

  const auto& fields_nullable = type_nullable_xunion_xunion->fields;
  const auto& fields = type_xunion_xunion->fields;
  ASSERT_EQ(fields_nullable.size(), fields.size());
  for (size_t i = 0; i < fields_nullable.size(); i++) {
    ASSERT_EQ(fields_nullable[i].ordinal, fields[i].ordinal);
    ASSERT_EQ(fields_nullable[i].type, fields[i].type);
  }

  END_TEST;
}

bool CodedTypesOfTables() {
  BEGIN_TEST;

  TestLibrary library(R"FIDL(
library example;

table MyTable {
  1: bool foo;
  2: int32 bar;
  3: array<bool>:42 baz;
};
)FIDL");
  ASSERT_TRUE(library.Compile());
  fidl::CodedTypesGenerator gen(library.library());
  gen.CompileCodedTypes();

  ASSERT_EQ(4, gen.coded_types().size());

  // This bool is used in the coding table of the MyTable table.
  auto type0 = gen.coded_types().at(0).get();
  ASSERT_STR_EQ("bool", type0->coded_name.c_str());
  ASSERT_EQ(fidl::coded::CodingNeeded::kAlways, type0->coding_needed);
  ASSERT_EQ(fidl::coded::Type::Kind::kPrimitive, type0->kind);
  auto type0_primitive = static_cast<const fidl::coded::PrimitiveType*>(type0);
  ASSERT_EQ(fidl::types::PrimitiveSubtype::kBool, type0_primitive->subtype);

  auto type1 = gen.coded_types().at(1).get();
  ASSERT_STR_EQ("int32", type1->coded_name.c_str());
  ASSERT_EQ(fidl::coded::CodingNeeded::kAlways, type1->coding_needed);
  ASSERT_EQ(fidl::coded::Type::Kind::kPrimitive, type1->kind);
  auto type1_primitive = static_cast<const fidl::coded::PrimitiveType*>(type1);
  ASSERT_EQ(fidl::types::PrimitiveSubtype::kInt32, type1_primitive->subtype);

  // This bool is part of array<bool>; it will not map to any coding table.
  auto type2 = gen.coded_types().at(2).get();
  ASSERT_STR_EQ("bool", type2->coded_name.c_str());
  ASSERT_EQ(fidl::coded::CodingNeeded::kEnvelopeOnly, type2->coding_needed);
  ASSERT_EQ(fidl::coded::Type::Kind::kPrimitive, type2->kind);
  auto type2_primitive = static_cast<const fidl::coded::PrimitiveType*>(type0);
  ASSERT_EQ(fidl::types::PrimitiveSubtype::kBool, type2_primitive->subtype);

  auto type3 = gen.coded_types().at(3).get();
  ASSERT_STR_EQ("Arraybool42", type3->coded_name.c_str());
  ASSERT_EQ(fidl::coded::CodingNeeded::kAlways, type3->coding_needed);
  ASSERT_EQ(fidl::coded::Type::Kind::kArray, type3->kind);
  auto type3_array = static_cast<const fidl::coded::ArrayType*>(type3);
  ASSERT_EQ(42, type3_array->size);
  ASSERT_EQ(fidl::coded::Type::Kind::kPrimitive, type3_array->element_type->kind);
  auto type3_array_element_type =
      static_cast<const fidl::coded::PrimitiveType*>(type3_array->element_type);
  ASSERT_EQ(fidl::types::PrimitiveSubtype::kBool, type3_array_element_type->subtype);

  auto name_table = fidl::flat::Name(library.library(), "MyTable");
  auto type_table = gen.CodedTypeFor(&name_table);
  ASSERT_NONNULL(type_table);
  ASSERT_STR_EQ("example_MyTable", type_table->coded_name.c_str());
  ASSERT_EQ(fidl::coded::CodingNeeded::kAlways, type_table->coding_needed);
  ASSERT_EQ(fidl::coded::Type::Kind::kTable, type_table->kind);
  auto type_table_table = static_cast<const fidl::coded::TableType*>(type_table);
  ASSERT_EQ(3, type_table_table->fields.size());
  auto table_field0 = type_table_table->fields.at(0);
  ASSERT_EQ(fidl::coded::Type::Kind::kPrimitive, table_field0.type->kind);
  auto table_field0_primitive = static_cast<const fidl::coded::PrimitiveType*>(table_field0.type);
  ASSERT_EQ(fidl::types::PrimitiveSubtype::kBool, table_field0_primitive->subtype);
  auto table_field1 = type_table_table->fields.at(1);
  ASSERT_EQ(fidl::coded::Type::Kind::kPrimitive, table_field1.type->kind);
  auto table_field1_primitive = static_cast<const fidl::coded::PrimitiveType*>(table_field1.type);
  ASSERT_EQ(fidl::types::PrimitiveSubtype::kInt32, table_field1_primitive->subtype);
  auto table_field2 = type_table_table->fields.at(2);
  ASSERT_EQ(fidl::coded::Type::Kind::kArray, table_field2.type->kind);
  ASSERT_STR_EQ("example/MyTable", type_table_table->qname.c_str());

  END_TEST;
}

bool CodedTypesOfBits() {
  BEGIN_TEST;

  TestLibrary library(R"FIDL(
library example;

bits MyBits : uint8 {
    HELLO = 0x1;
    WORLD = 0x10;
};

)FIDL");
  ASSERT_TRUE(library.Compile());
  fidl::CodedTypesGenerator gen(library.library());
  gen.CompileCodedTypes();

  ASSERT_EQ(0, gen.coded_types().size());
  auto name_bits = fidl::flat::Name(library.library(), "MyBits");
  auto type_bits = gen.CodedTypeFor(&name_bits);
  ASSERT_NONNULL(type_bits);
  ASSERT_STR_EQ("example_MyBits", type_bits->coded_name.c_str());
  ASSERT_EQ(fidl::coded::CodingNeeded::kAlways, type_bits->coding_needed);
  ASSERT_EQ(fidl::coded::Type::Kind::kBits, type_bits->kind);
  auto type_bits_bits = static_cast<const fidl::coded::BitsType*>(type_bits);
  ASSERT_EQ(fidl::types::PrimitiveSubtype::kUint8, type_bits_bits->subtype);
  ASSERT_EQ(0x1u | 0x10u, type_bits_bits->mask);

  END_TEST;
}

bool CodedTypesOfEnum() {
  BEGIN_TEST;

  TestLibrary library(R"FIDL(
library example;

enum MyEnum : uint16 {
    HELLO = 0x1;
    WORLD = 0x10;
};

)FIDL");
  ASSERT_TRUE(library.Compile());
  fidl::CodedTypesGenerator gen(library.library());
  gen.CompileCodedTypes();

  ASSERT_EQ(0, gen.coded_types().size());

  ASSERT_EQ(0, gen.coded_types().size());
  auto name_enum = fidl::flat::Name(library.library(), "MyEnum");
  auto type_enum = gen.CodedTypeFor(&name_enum);
  ASSERT_NONNULL(type_enum);
  ASSERT_STR_EQ("example_MyEnum", type_enum->coded_name.c_str());
  ASSERT_EQ(fidl::coded::CodingNeeded::kAlways, type_enum->coding_needed);
  ASSERT_EQ(fidl::coded::Type::Kind::kEnum, type_enum->kind);
  auto type_enum_enum = static_cast<const fidl::coded::EnumType*>(type_enum);
  ASSERT_EQ(fidl::types::PrimitiveSubtype::kUint16, type_enum_enum->subtype);

  ASSERT_EQ(2, type_enum_enum->members.size());
  ASSERT_EQ(0x1, type_enum_enum->members[0]);
  ASSERT_EQ(0x10, type_enum_enum->members[1]);

  END_TEST;
}

}  // namespace

BEGIN_TEST_CASE(coded_types_generator_tests)

RUN_TEST(CodedTypesOfArrays);
RUN_TEST(CodedTypesOfVectors);
RUN_TEST(CodedTypesOfProtocol);
RUN_TEST(CodedTypesOfRequestOfProtocol);
RUN_TEST(CodedTypesOfXUnions);
RUN_TEST(CodedTypesOfNullablePointers);
RUN_TEST(CodedTypesOfStructsWithPaddings);
RUN_TEST(CodedTypesOfNullableXUnions);
RUN_TEST(CodedTypesOfTables);
RUN_TEST(CodedTypesOfBits);
RUN_TEST(CodedTypesOfEnum);

END_TEST_CASE(coded_types_generator_tests)
