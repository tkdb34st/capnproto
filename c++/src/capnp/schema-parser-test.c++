// Copyright (c) 2013, Kenton Varda <temporal@gmail.com>
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "schema-parser.h"
#include <gtest/gtest.h>
#include "test-util.h"
#include <kj/debug.h>
#include <map>

namespace capnp {
namespace {

class FakeFileReader final: public SchemaFile::FileReader {
public:
  void add(kj::StringPtr name, kj::StringPtr content) {
    files[name] = content;
  }

  bool exists(kj::StringPtr path) const override {
    return files.count(path) > 0;
  }

  kj::Array<const char> read(kj::StringPtr path) const override {
    auto iter = files.find(path);
    KJ_ASSERT(iter != files.end(), "FakeFileReader has no such file.", path);
    auto result = kj::heapArray<char>(iter->second.size());
    memcpy(result.begin(), iter->second.begin(), iter->second.size());
    return kj::mv(result);
  }

private:
  std::map<kj::StringPtr, kj::StringPtr> files;
};

static uint64_t getFieldTypeFileId(StructSchema::Field field) {
  return field.getContainingStruct()
      .getDependency(field.getProto().getNonGroup().getType().getStruct())
      .getProto().getScopeId();
}

TEST(SchemaParser, Basic) {
  SchemaParser parser;
  FakeFileReader reader;

  reader.add("src/foo/bar.capnp",
      "@0x8123456789abcdef;\n"
      "struct Bar {\n"
      "  baz @0: import \"baz.capnp\".Baz;\n"
      "  corge @1: import \"../qux/corge.capnp\".Corge;\n"
      "  grault @2: import \"/grault.capnp\".Grault;\n"
      "  garply @3: import \"/garply.capnp\".Garply;\n"
      "}\n");
  reader.add("src/foo/baz.capnp",
      "@0x823456789abcdef1;\n"
      "struct Baz {}\n");
  reader.add("src/qux/corge.capnp",
      "@0x83456789abcdef12;\n"
      "struct Corge {}\n");
  reader.add("/usr/include/grault.capnp",
      "@0x8456789abcdef123;\n"
      "struct Grault {}\n");
  reader.add("/opt/include/grault.capnp",
      "@0x8000000000000001;\n"
      "struct WrongGrault {}\n");
  reader.add("/usr/local/include/garply.capnp",
      "@0x856789abcdef1234;\n"
      "struct Garply {}\n");

  kj::StringPtr importPath[] = {
    "/usr/include", "/usr/local/include", "/opt/include"
  };

  ParsedSchema barSchema = parser.parseFile(SchemaFile::newDiskFile(
      "foo2/bar2.capnp", "src/foo/bar.capnp", importPath, reader));

  auto barProto = barSchema.getProto();
  EXPECT_EQ(0x8123456789abcdefull, barProto.getId());
  EXPECT_EQ("foo2/bar2.capnp", barProto.getDisplayName());

  auto barStruct = barSchema.getNested("Bar");
  auto barFields = barStruct.asStruct().getFields();
  ASSERT_EQ(4u, barFields.size());
  EXPECT_EQ("baz", barFields[0].getProto().getName());
  EXPECT_EQ(0x823456789abcdef1ull, getFieldTypeFileId(barFields[0]));
  EXPECT_EQ("corge", barFields[1].getProto().getName());
  EXPECT_EQ(0x83456789abcdef12ull, getFieldTypeFileId(barFields[1]));
  EXPECT_EQ("grault", barFields[2].getProto().getName());
  EXPECT_EQ(0x8456789abcdef123ull, getFieldTypeFileId(barFields[2]));
  EXPECT_EQ("garply", barFields[3].getProto().getName());
  EXPECT_EQ(0x856789abcdef1234ull, getFieldTypeFileId(barFields[3]));

  auto bazSchema = parser.parseFile(SchemaFile::newDiskFile(
      "not/used/because/already/loaded",
      "src/foo/baz.capnp", importPath, reader));
  EXPECT_EQ(0x823456789abcdef1ull, bazSchema.getProto().getId());
  EXPECT_EQ("foo2/baz.capnp", bazSchema.getProto().getDisplayName());
  auto bazStruct = bazSchema.getNested("Baz").asStruct();
  EXPECT_EQ(bazStruct, barStruct.getDependency(bazStruct.getProto().getId()));

  auto corgeSchema = parser.parseFile(SchemaFile::newDiskFile(
      "not/used/because/already/loaded",
      "src/qux/corge.capnp", importPath, reader));
  EXPECT_EQ(0x83456789abcdef12ull, corgeSchema.getProto().getId());
  EXPECT_EQ("qux/corge.capnp", corgeSchema.getProto().getDisplayName());
  auto corgeStruct = corgeSchema.getNested("Corge").asStruct();
  EXPECT_EQ(corgeStruct, barStruct.getDependency(corgeStruct.getProto().getId()));

  auto graultSchema = parser.parseFile(SchemaFile::newDiskFile(
      "not/used/because/already/loaded",
      "/usr/include/grault.capnp", importPath, reader));
  EXPECT_EQ(0x8456789abcdef123ull, graultSchema.getProto().getId());
  EXPECT_EQ("grault.capnp", graultSchema.getProto().getDisplayName());
  auto graultStruct = graultSchema.getNested("Grault").asStruct();
  EXPECT_EQ(graultStruct, barStruct.getDependency(graultStruct.getProto().getId()));

  // Try importing the other grault.capnp directly.  It'll get the display name we specify since
  // it wasn't imported before.
  auto wrongGraultSchema = parser.parseFile(SchemaFile::newDiskFile(
      "weird/display/name.capnp",
      "/opt/include/grault.capnp", importPath, reader));
  EXPECT_EQ(0x8000000000000001ull, wrongGraultSchema.getProto().getId());
  EXPECT_EQ("weird/display/name.capnp", wrongGraultSchema.getProto().getDisplayName());
}

TEST(SchemaParser, Constants) {
  // This is actually a test of the full dynamic API stack for constants, because the schemas for
  // constants are not actually accessible from the generated code API, so the only way to ever
  // get a ConstSchema is by parsing it.

  SchemaParser parser;
  FakeFileReader reader;

  reader.add("const.capnp",
      "@0x8123456789abcdef;\n"
      "const uint32Const :UInt32 = 1234;\n"
      "const listConst :List(Float32) = [1.25, 2.5, 3e4];\n"
      "const structConst :Foo = (bar = 123, baz = \"qux\");\n"
      "struct Foo {\n"
      "  bar @0 :Int16;\n"
      "  baz @1 :Text;\n"
      "}\n");

  ParsedSchema barSchema = parser.parseFile(SchemaFile::newDiskFile(
      "const.capnp", "const.capnp", nullptr, reader));

  EXPECT_EQ(1234, barSchema.getNested("uint32Const").asConst().as<uint32_t>());

  auto list = barSchema.getNested("listConst").asConst().as<DynamicList>();
  ASSERT_EQ(3u, list.size());
  EXPECT_EQ(1.25, list[0].as<float>());
  EXPECT_EQ(2.5, list[1].as<float>());
  EXPECT_EQ(3e4f, list[2].as<float>());

  auto structConst = barSchema.getNested("structConst").asConst().as<DynamicStruct>();
  EXPECT_EQ(123, structConst.get("bar").as<int16_t>());
  EXPECT_EQ("qux", structConst.get("baz").as<Text>());
}

}  // namespace
}  // namespace capnp
