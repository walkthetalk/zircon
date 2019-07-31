// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/flat_ast.h>
#include <fidl/lexer.h>
#include <fidl/parser.h>
#include <fidl/source_file.h>
#include <unittest/unittest.h>

#include <fstream>

#include "examples.h"
#include "test_library.h"

namespace {

// We repeat each test in a loop in order to catch situations where memory layout
// determines what JSON is produced (this is often manifested due to using a std::map<Foo*,...>
// in compiler source code).
static const int kRepeatTestCount = 100;

static inline void trim(std::string& s) {
  s.erase(s.begin(),
          std::find_if(s.begin(), s.end(), [](int ch) { return !std::isspace(ch) && ch != '\n'; }));
  s.erase(std::find_if(s.rbegin(), s.rend(), [](int ch) { return !std::isspace(ch) && ch != '\n'; })
              .base(),
          s.end());
}

bool checkJSONGenerator(TestLibrary library, std::string expected_json) {
  ASSERT_TRUE(library.Compile());

  // actual
  auto actual = library.GenerateJSON();
  trim(actual);

  // expected
  trim(expected_json);

  if (actual.compare(expected_json) == 0) {
    return true;
  }

  // On error, we output both the actual and expected to allow simple
  // diffing to debug the test.

  std::ofstream output_actual("json_generator_tests_actual.txt");
  output_actual << actual;
  output_actual.close();

  std::ofstream output_expected("json_generator_tests_expected.txt");
  output_expected << expected_json;
  output_expected.close();

  return false;
}

bool checkJSONGenerator(std::string raw_source_code, std::string expected_json) {
  return checkJSONGenerator(TestLibrary("json.fidl", raw_source_code), std::move(expected_json));
}

bool json_generator_test_struct() {
  BEGIN_TEST;

  for (int i = 0; i < kRepeatTestCount; i++) {
    EXPECT_TRUE(checkJSONGenerator(R"FIDL(
library fidl.test.json;

struct Simple {
    uint8 f1;
    bool f2;
};

)FIDL",
                                   R"JSON(
{
  "version": "0.0.1",
  "name": "fidl.test.json",
  "library_dependencies": [],
  "bits_declarations": [],
  "const_declarations": [],
  "enum_declarations": [],
  "interface_declarations": [],
  "struct_declarations": [
    {
      "name": "fidl.test.json/Simple",
      "location": {
        "filename": "json.fidl",
        "line": 4,
        "column": 8
      },
      "anonymous": false,
      "members": [
        {
          "type": {
            "kind": "primitive",
            "subtype": "uint8"
          },
          "name": "f1",
          "location": {
            "filename": "json.fidl",
            "line": 5,
            "column": 11
          },
          "size": 1,
          "max_out_of_line": 0,
          "alignment": 1,
          "offset": 0,
          "max_handles": 0
        },
        {
          "type": {
            "kind": "primitive",
            "subtype": "bool"
          },
          "name": "f2",
          "location": {
            "filename": "json.fidl",
            "line": 6,
            "column": 10
          },
          "size": 1,
          "max_out_of_line": 0,
          "alignment": 1,
          "offset": 1,
          "max_handles": 0
        }
      ],
      "size": 2,
      "max_out_of_line": 0,
      "alignment": 1,
      "max_handles": 0,
      "has_padding": false
    }
  ],
  "table_declarations": [],
  "union_declarations": [],
  "xunion_declarations": [],
  "type_alias_declarations": [],
  "declaration_order": [
    "fidl.test.json/Simple"
  ],
  "declarations": {
    "fidl.test.json/Simple": "struct"
  }
}
)JSON"));
  }

  END_TEST;
}

bool json_generator_test_empty_struct() {
  BEGIN_TEST;

  for (int i = 0; i < kRepeatTestCount; i++) {
    EXPECT_TRUE(checkJSONGenerator(R"FIDL(
library fidl.test.json;

struct Empty {
};

protocol EmptyProtocol {
  Send(Empty e);
  -> Receive (Empty e);
  SendAndReceive(Empty e) -> (Empty e);
};
)FIDL",
                                   R"JSON(
{
  "version": "0.0.1",
  "name": "fidl.test.json",
  "library_dependencies": [],
  "bits_declarations": [],
  "const_declarations": [],
  "enum_declarations": [],
  "interface_declarations": [
    {
      "name": "fidl.test.json/EmptyProtocol",
      "location": {
        "filename": "json.fidl",
        "line": 7,
        "column": 10
      },
      "methods": [
        {
          "ordinal": 285845058,
          "generated_ordinal": 285845058,
          "name": "Send",
          "location": {
            "filename": "json.fidl",
            "line": 8,
            "column": 3
          },
          "has_request": true,
          "maybe_request": [
            {
              "type": {
                "kind": "identifier",
                "identifier": "fidl.test.json/Empty",
                "nullable": false
              },
              "name": "e",
              "location": {
                "filename": "json.fidl",
                "line": 8,
                "column": 14
              },
              "size": 1,
              "max_out_of_line": 0,
              "alignment": 1,
              "offset": 16,
              "max_handles": 0
            }
          ],
          "maybe_request_size": 24,
          "maybe_request_alignment": 8,
          "maybe_request_has_padding": true,
          "has_response": false,
          "is_composed": false
        },
        {
          "ordinal": 388770671,
          "generated_ordinal": 388770671,
          "name": "Receive",
          "location": {
            "filename": "json.fidl",
            "line": 9,
            "column": 6
          },
          "has_request": false,
          "has_response": true,
          "maybe_response": [
            {
              "type": {
                "kind": "identifier",
                "identifier": "fidl.test.json/Empty",
                "nullable": false
              },
              "name": "e",
              "location": {
                "filename": "json.fidl",
                "line": 9,
                "column": 21
              },
              "size": 1,
              "max_out_of_line": 0,
              "alignment": 1,
              "offset": 16,
              "max_handles": 0
            }
          ],
          "maybe_response_size": 24,
          "maybe_response_alignment": 8,
          "maybe_response_has_padding": true,
          "is_composed": false
        },
        {
          "ordinal": 2071775804,
          "generated_ordinal": 2071775804,
          "name": "SendAndReceive",
          "location": {
            "filename": "json.fidl",
            "line": 10,
            "column": 3
          },
          "has_request": true,
          "maybe_request": [
            {
              "type": {
                "kind": "identifier",
                "identifier": "fidl.test.json/Empty",
                "nullable": false
              },
              "name": "e",
              "location": {
                "filename": "json.fidl",
                "line": 10,
                "column": 24
              },
              "size": 1,
              "max_out_of_line": 0,
              "alignment": 1,
              "offset": 16,
              "max_handles": 0
            }
          ],
          "maybe_request_size": 24,
          "maybe_request_alignment": 8,
          "maybe_request_has_padding": true,
          "has_response": true,
          "maybe_response": [
            {
              "type": {
                "kind": "identifier",
                "identifier": "fidl.test.json/Empty",
                "nullable": false
              },
              "name": "e",
              "location": {
                "filename": "json.fidl",
                "line": 10,
                "column": 37
              },
              "size": 1,
              "max_out_of_line": 0,
              "alignment": 1,
              "offset": 16,
              "max_handles": 0
            }
          ],
          "maybe_response_size": 24,
          "maybe_response_alignment": 8,
          "maybe_response_has_padding": true,
          "is_composed": false
        }
      ]
    }
  ],
  "struct_declarations": [
    {
      "name": "fidl.test.json/Empty",
      "location": {
        "filename": "json.fidl",
        "line": 4,
        "column": 8
      },
      "anonymous": false,
      "members": [],
      "size": 1,
      "max_out_of_line": 0,
      "alignment": 1,
      "max_handles": 0,
      "has_padding": false
    }
  ],
  "table_declarations": [],
  "union_declarations": [],
  "xunion_declarations": [],
  "type_alias_declarations": [],
  "declaration_order": [
    "fidl.test.json/Empty",
    "fidl.test.json/EmptyProtocol"
  ],
  "declarations": {
    "fidl.test.json/EmptyProtocol": "interface",
    "fidl.test.json/Empty": "struct"
  }
}
)JSON"));
  }

  END_TEST;
}

bool json_generator_test_table() {
  BEGIN_TEST;

  for (int i = 0; i < kRepeatTestCount; i++) {
    EXPECT_TRUE(checkJSONGenerator(R"FIDL(
library fidl.test.json;

table Simple {
    1: uint8 f1;
    2: bool f2;
    3: reserved;
};

)FIDL",
                                   R"JSON(
{
  "version": "0.0.1",
  "name": "fidl.test.json",
  "library_dependencies": [],
  "bits_declarations": [],
  "const_declarations": [],
  "enum_declarations": [],
  "interface_declarations": [],
  "struct_declarations": [],
  "table_declarations": [
    {
      "name": "fidl.test.json/Simple",
      "location": {
        "filename": "json.fidl",
        "line": 4,
        "column": 7
      },
      "members": [
        {
          "ordinal": 1,
          "reserved": false,
          "type": {
            "kind": "primitive",
            "subtype": "uint8"
          },
          "name": "f1",
          "location": {
            "filename": "json.fidl",
            "line": 5,
            "column": 14
          },
          "size": 1,
          "max_out_of_line": 0,
          "alignment": 1,
          "max_handles": 0
        },
        {
          "ordinal": 2,
          "reserved": false,
          "type": {
            "kind": "primitive",
            "subtype": "bool"
          },
          "name": "f2",
          "location": {
            "filename": "json.fidl",
            "line": 6,
            "column": 13
          },
          "size": 1,
          "max_out_of_line": 0,
          "alignment": 1,
          "max_handles": 0
        },
        {
          "ordinal": 3,
          "reserved": true,
          "location": {
            "filename": "json.fidl",
            "line": 7,
            "column": 5
          }
        }
      ],
      "size": 16,
      "max_out_of_line": 48,
      "alignment": 8,
      "max_handles": 0
    }
  ],
  "union_declarations": [],
  "xunion_declarations": [],
  "type_alias_declarations": [],
  "declaration_order": [
    "fidl.test.json/Simple"
  ],
  "declarations": {
    "fidl.test.json/Simple": "table"
  }
}
)JSON"));
  }

  END_TEST;
}

bool json_generator_test_union() {
  BEGIN_TEST;

  for (int i = 0; i < kRepeatTestCount; i++) {
    EXPECT_TRUE(checkJSONGenerator(R"FIDL(
library fidl.test.json;

struct Pizza {
    vector<string:16> toppings;
};

struct Pasta {
    string:16 sauce;
};

union PizzaOrPasta {
    Pizza pizza;
    Pasta pasta;
};

)FIDL",
                                   R"JSON(
{
  "version": "0.0.1",
  "name": "fidl.test.json",
  "library_dependencies": [],
  "bits_declarations": [],
  "const_declarations": [],
  "enum_declarations": [],
  "interface_declarations": [],
  "struct_declarations": [
    {
      "name": "fidl.test.json/Pizza",
      "location": {
        "filename": "json.fidl",
        "line": 4,
        "column": 8
      },
      "anonymous": false,
      "members": [
        {
          "type": {
            "kind": "vector",
            "element_type": {
              "kind": "string",
              "maybe_element_count": 16,
              "nullable": false
            },
            "nullable": false
          },
          "name": "toppings",
          "location": {
            "filename": "json.fidl",
            "line": 5,
            "column": 23
          },
          "size": 16,
          "max_out_of_line": 4294967295,
          "alignment": 8,
          "offset": 0,
          "max_handles": 0
        }
      ],
      "size": 16,
      "max_out_of_line": 4294967295,
      "alignment": 8,
      "max_handles": 0,
      "has_padding": false
    },
    {
      "name": "fidl.test.json/Pasta",
      "location": {
        "filename": "json.fidl",
        "line": 8,
        "column": 8
      },
      "anonymous": false,
      "members": [
        {
          "type": {
            "kind": "string",
            "maybe_element_count": 16,
            "nullable": false
          },
          "name": "sauce",
          "location": {
            "filename": "json.fidl",
            "line": 9,
            "column": 15
          },
          "size": 16,
          "max_out_of_line": 16,
          "alignment": 8,
          "offset": 0,
          "max_handles": 0
        }
      ],
      "size": 16,
      "max_out_of_line": 16,
      "alignment": 8,
      "max_handles": 0,
      "has_padding": false
    }
  ],
  "table_declarations": [],
  "union_declarations": [
    {
      "name": "fidl.test.json/PizzaOrPasta",
      "location": {
        "filename": "json.fidl",
        "line": 12,
        "column": 7
      },
      "members": [
        {
          "type": {
            "kind": "identifier",
            "identifier": "fidl.test.json/Pizza",
            "nullable": false
          },
          "name": "pizza",
          "location": {
            "filename": "json.fidl",
            "line": 13,
            "column": 11
          },
          "size": 16,
          "max_out_of_line": 4294967295,
          "alignment": 8,
          "offset": 8
        },
        {
          "type": {
            "kind": "identifier",
            "identifier": "fidl.test.json/Pasta",
            "nullable": false
          },
          "name": "pasta",
          "location": {
            "filename": "json.fidl",
            "line": 14,
            "column": 11
          },
          "size": 16,
          "max_out_of_line": 16,
          "alignment": 8,
          "offset": 8
        }
      ],
      "size": 24,
      "max_out_of_line": 4294967295,
      "alignment": 8,
      "max_handles": 0
    }
  ],
  "xunion_declarations": [],
  "type_alias_declarations": [],
  "declaration_order": [
    "fidl.test.json/Pizza",
    "fidl.test.json/Pasta",
    "fidl.test.json/PizzaOrPasta"
  ],
  "declarations": {
    "fidl.test.json/Pizza": "struct",
    "fidl.test.json/Pasta": "struct",
    "fidl.test.json/PizzaOrPasta": "union"
  }
}
)JSON"));
  }

  END_TEST;
}

bool json_generator_test_xunion() {
  BEGIN_TEST;

  for (int i = 0; i < kRepeatTestCount; i++) {
    EXPECT_TRUE(checkJSONGenerator(R"FIDL(
library fidl.test.json;

xunion FlexibleFoo {
  string s;
  int32 i;
};

strict xunion StrictFoo {
  string s;
  int32 i;
};

)FIDL",
                                   R"JSON(
{
  "version": "0.0.1",
  "name": "fidl.test.json",
  "library_dependencies": [],
  "bits_declarations": [],
  "const_declarations": [],
  "enum_declarations": [],
  "interface_declarations": [],
  "struct_declarations": [],
  "table_declarations": [],
  "union_declarations": [],
  "xunion_declarations": [
    {
      "name": "fidl.test.json/FlexibleFoo",
      "location": {
        "filename": "json.fidl",
        "line": 4,
        "column": 8
      },
      "members": [
        {
          "ordinal": 1056421836,
          "type": {
            "kind": "string",
            "nullable": false
          },
          "name": "s",
          "location": {
            "filename": "json.fidl",
            "line": 5,
            "column": 10
          },
          "size": 16,
          "max_out_of_line": 4294967295,
          "alignment": 8,
          "offset": 0
        },
        {
          "ordinal": 1911600824,
          "type": {
            "kind": "primitive",
            "subtype": "int32"
          },
          "name": "i",
          "location": {
            "filename": "json.fidl",
            "line": 6,
            "column": 9
          },
          "size": 4,
          "max_out_of_line": 0,
          "alignment": 4,
          "offset": 0
        }
      ],
      "size": 24,
      "max_out_of_line": 4294967295,
      "alignment": 8,
      "max_handles": 0,
      "strict": false
    },
    {
      "name": "fidl.test.json/StrictFoo",
      "location": {
        "filename": "json.fidl",
        "line": 9,
        "column": 15
      },
      "members": [
        {
          "ordinal": 215696753,
          "type": {
            "kind": "string",
            "nullable": false
          },
          "name": "s",
          "location": {
            "filename": "json.fidl",
            "line": 10,
            "column": 10
          },
          "size": 16,
          "max_out_of_line": 4294967295,
          "alignment": 8,
          "offset": 0
        },
        {
          "ordinal": 2063855467,
          "type": {
            "kind": "primitive",
            "subtype": "int32"
          },
          "name": "i",
          "location": {
            "filename": "json.fidl",
            "line": 11,
            "column": 9
          },
          "size": 4,
          "max_out_of_line": 0,
          "alignment": 4,
          "offset": 0
        }
      ],
      "size": 24,
      "max_out_of_line": 4294967295,
      "alignment": 8,
      "max_handles": 0,
      "strict": true
    }
  ],
  "type_alias_declarations": [],
  "declaration_order": [
    "fidl.test.json/StrictFoo",
    "fidl.test.json/FlexibleFoo"
  ],
  "declarations": {
    "fidl.test.json/FlexibleFoo": "xunion",
    "fidl.test.json/StrictFoo": "xunion"
  }
}
)JSON"));
  }

  END_TEST;
}

// This test ensures that inherited methods have the same ordinal / signature /
// etc as the method from which they are inheriting.
bool json_generator_test_inheritance() {
  BEGIN_TEST;

  for (int i = 0; i < kRepeatTestCount; i++) {
    EXPECT_TRUE(checkJSONGenerator(R"FIDL(
library fidl.test.json;

[FragileBase]
protocol super {
   foo(string s) -> (int64 y);
};

protocol sub {
  compose super;
};

)FIDL",
                                   R"JSON({
  "version": "0.0.1",
  "name": "fidl.test.json",
  "library_dependencies": [],
  "bits_declarations": [],
  "const_declarations": [],
  "enum_declarations": [],
  "interface_declarations": [
    {
      "name": "fidl.test.json/super",
      "location": {
        "filename": "json.fidl",
        "line": 5,
        "column": 10
      },
      "maybe_attributes": [
        {
          "name": "FragileBase",
          "value": ""
        }
      ],
      "methods": [
        {
          "ordinal": 790020540,
          "generated_ordinal": 790020540,
          "name": "foo",
          "location": {
            "filename": "json.fidl",
            "line": 6,
            "column": 4
          },
          "has_request": true,
          "maybe_request": [
            {
              "type": {
                "kind": "string",
                "nullable": false
              },
              "name": "s",
              "location": {
                "filename": "json.fidl",
                "line": 6,
                "column": 15
              },
              "size": 16,
              "max_out_of_line": 4294967295,
              "alignment": 8,
              "offset": 16,
              "max_handles": 0
            }
          ],
          "maybe_request_size": 32,
          "maybe_request_alignment": 8,
          "maybe_request_has_padding": false,
          "has_response": true,
          "maybe_response": [
            {
              "type": {
                "kind": "primitive",
                "subtype": "int64"
              },
              "name": "y",
              "location": {
                "filename": "json.fidl",
                "line": 6,
                "column": 28
              },
              "size": 8,
              "max_out_of_line": 0,
              "alignment": 8,
              "offset": 16,
              "max_handles": 0
            }
          ],
          "maybe_response_size": 24,
          "maybe_response_alignment": 8,
          "maybe_response_has_padding": false,
          "is_composed": false
        }
      ]
    },
    {
      "name": "fidl.test.json/sub",
      "location": {
        "filename": "json.fidl",
        "line": 9,
        "column": 10
      },
      "methods": [
        {
          "ordinal": 790020540,
          "generated_ordinal": 790020540,
          "name": "foo",
          "location": {
            "filename": "json.fidl",
            "line": 6,
            "column": 4
          },
          "has_request": true,
          "maybe_request": [
            {
              "type": {
                "kind": "string",
                "nullable": false
              },
              "name": "s",
              "location": {
                "filename": "json.fidl",
                "line": 6,
                "column": 15
              },
              "size": 16,
              "max_out_of_line": 4294967295,
              "alignment": 8,
              "offset": 16,
              "max_handles": 0
            }
          ],
          "maybe_request_size": 32,
          "maybe_request_alignment": 8,
          "maybe_request_has_padding": false,
          "has_response": true,
          "maybe_response": [
            {
              "type": {
                "kind": "primitive",
                "subtype": "int64"
              },
              "name": "y",
              "location": {
                "filename": "json.fidl",
                "line": 6,
                "column": 28
              },
              "size": 8,
              "max_out_of_line": 0,
              "alignment": 8,
              "offset": 16,
              "max_handles": 0
            }
          ],
          "maybe_response_size": 24,
          "maybe_response_alignment": 8,
          "maybe_response_has_padding": false,
          "is_composed": true
        }
      ]
    }
  ],
  "struct_declarations": [],
  "table_declarations": [],
  "union_declarations": [],
  "xunion_declarations": [],
  "type_alias_declarations": [],
  "declaration_order": [
    "fidl.test.json/super",
    "fidl.test.json/sub"
  ],
  "declarations": {
    "fidl.test.json/super": "interface",
    "fidl.test.json/sub": "interface"
  }
})JSON"));
  }

  END_TEST;
}

bool json_generator_test_inheritance_with_recursive_decl() {
  BEGIN_TEST;

  for (int i = 0; i < kRepeatTestCount; i++) {
    EXPECT_TRUE(checkJSONGenerator(R"FIDL(
library fidl.test.json;

[FragileBase]
protocol Parent {
  First(request<Parent> request);
};

protocol Child {
  compose Parent;
  Second(request<Parent> request);
};

)FIDL",
                                   R"JSON({
  "version": "0.0.1",
  "name": "fidl.test.json",
  "library_dependencies": [],
  "bits_declarations": [],
  "const_declarations": [],
  "enum_declarations": [],
  "interface_declarations": [
    {
      "name": "fidl.test.json/Parent",
      "location": {
        "filename": "json.fidl",
        "line": 5,
        "column": 10
      },
      "maybe_attributes": [
        {
          "name": "FragileBase",
          "value": ""
        }
      ],
      "methods": [
        {
          "ordinal": 1722375644,
          "generated_ordinal": 1722375644,
          "name": "First",
          "location": {
            "filename": "json.fidl",
            "line": 6,
            "column": 3
          },
          "has_request": true,
          "maybe_request": [
            {
              "type": {
                "kind": "request",
                "subtype": "fidl.test.json/Parent",
                "nullable": false
              },
              "name": "request",
              "location": {
                "filename": "json.fidl",
                "line": 6,
                "column": 25
              },
              "size": 4,
              "max_out_of_line": 0,
              "alignment": 4,
              "offset": 16,
              "max_handles": 1
            }
          ],
          "maybe_request_size": 24,
          "maybe_request_alignment": 8,
          "maybe_request_has_padding": true,
          "has_response": false,
          "is_composed": false
        }
      ]
    },
    {
      "name": "fidl.test.json/Child",
      "location": {
        "filename": "json.fidl",
        "line": 9,
        "column": 10
      },
      "methods": [
        {
          "ordinal": 1722375644,
          "generated_ordinal": 1722375644,
          "name": "First",
          "location": {
            "filename": "json.fidl",
            "line": 6,
            "column": 3
          },
          "has_request": true,
          "maybe_request": [
            {
              "type": {
                "kind": "request",
                "subtype": "fidl.test.json/Parent",
                "nullable": false
              },
              "name": "request",
              "location": {
                "filename": "json.fidl",
                "line": 6,
                "column": 25
              },
              "size": 4,
              "max_out_of_line": 0,
              "alignment": 4,
              "offset": 16,
              "max_handles": 1
            }
          ],
          "maybe_request_size": 24,
          "maybe_request_alignment": 8,
          "maybe_request_has_padding": true,
          "has_response": false,
          "is_composed": true
        },
        {
          "ordinal": 19139766,
          "generated_ordinal": 19139766,
          "name": "Second",
          "location": {
            "filename": "json.fidl",
            "line": 11,
            "column": 3
          },
          "has_request": true,
          "maybe_request": [
            {
              "type": {
                "kind": "request",
                "subtype": "fidl.test.json/Parent",
                "nullable": false
              },
              "name": "request",
              "location": {
                "filename": "json.fidl",
                "line": 11,
                "column": 26
              },
              "size": 4,
              "max_out_of_line": 0,
              "alignment": 4,
              "offset": 16,
              "max_handles": 1
            }
          ],
          "maybe_request_size": 24,
          "maybe_request_alignment": 8,
          "maybe_request_has_padding": true,
          "has_response": false,
          "is_composed": false
        }
      ]
    }
  ],
  "struct_declarations": [],
  "table_declarations": [],
  "union_declarations": [],
  "xunion_declarations": [],
  "type_alias_declarations": [],
  "declaration_order": [
    "fidl.test.json/Parent",
    "fidl.test.json/Child"
  ],
  "declarations": {
    "fidl.test.json/Parent": "interface",
    "fidl.test.json/Child": "interface"
  }
})JSON"));
  }

  END_TEST;
}

bool json_generator_test_error() {
  BEGIN_TEST;

  for (int i = 0; i < kRepeatTestCount; i++) {
    EXPECT_TRUE(checkJSONGenerator(R"FIDL(
library fidl.test.json;

protocol Example {
   foo(string s) -> (int64 y) error uint32;
};

)FIDL",
                                   R"JSON({
  "version": "0.0.1",
  "name": "fidl.test.json",
  "library_dependencies": [],
  "bits_declarations": [],
  "const_declarations": [],
  "enum_declarations": [],
  "interface_declarations": [
    {
      "name": "fidl.test.json/Example",
      "location": {
        "filename": "json.fidl",
        "line": 4,
        "column": 10
      },
      "methods": [
        {
          "ordinal": 1369693400,
          "generated_ordinal": 1369693400,
          "name": "foo",
          "location": {
            "filename": "json.fidl",
            "line": 5,
            "column": 4
          },
          "has_request": true,
          "maybe_request": [
            {
              "type": {
                "kind": "string",
                "nullable": false
              },
              "name": "s",
              "location": {
                "filename": "json.fidl",
                "line": 5,
                "column": 15
              },
              "size": 16,
              "max_out_of_line": 4294967295,
              "alignment": 8,
              "offset": 16,
              "max_handles": 0
            }
          ],
          "maybe_request_size": 32,
          "maybe_request_alignment": 8,
          "maybe_request_has_padding": false,
          "has_response": true,
          "maybe_response": [
            {
              "type": {
                "kind": "identifier",
                "identifier": "fidl.test.json/Example_foo_Result",
                "nullable": false
              },
              "name": "result",
              "location": {
                "filename": "generated",
                "line": 6,
                "column": 1
              },
              "size": 16,
              "max_out_of_line": 0,
              "alignment": 8,
              "offset": 16,
              "max_handles": 0
            }
          ],
          "maybe_response_size": 32,
          "maybe_response_alignment": 8,
          "maybe_response_has_padding": true,
          "is_composed": false
        }
      ]
    }
  ],
  "struct_declarations": [
    {
      "name": "fidl.test.json/Example_foo_Response",
      "location": {
        "filename": "generated",
        "line": 2,
        "column": 1
      },
      "anonymous": false,
      "members": [
        {
          "type": {
            "kind": "primitive",
            "subtype": "int64"
          },
          "name": "y",
          "location": {
            "filename": "json.fidl",
            "line": 5,
            "column": 28
          },
          "size": 8,
          "max_out_of_line": 0,
          "alignment": 8,
          "offset": 0,
          "max_handles": 0
        }
      ],
      "size": 8,
      "max_out_of_line": 0,
      "alignment": 8,
      "max_handles": 0,
      "has_padding": false
    }
  ],
  "table_declarations": [],
  "union_declarations": [
    {
      "name": "fidl.test.json/Example_foo_Result",
      "location": {
        "filename": "generated",
        "line": 5,
        "column": 1
      },
      "maybe_attributes": [
        {
          "name": "Result",
          "value": ""
        }
      ],
      "members": [
        {
          "type": {
            "kind": "identifier",
            "identifier": "fidl.test.json/Example_foo_Response",
            "nullable": false
          },
          "name": "response",
          "location": {
            "filename": "generated",
            "line": 3,
            "column": 1
          },
          "size": 8,
          "max_out_of_line": 0,
          "alignment": 8,
          "offset": 8
        },
        {
          "type": {
            "kind": "primitive",
            "subtype": "uint32"
          },
          "name": "err",
          "location": {
            "filename": "generated",
            "line": 4,
            "column": 1
          },
          "size": 4,
          "max_out_of_line": 0,
          "alignment": 4,
          "offset": 8
        }
      ],
      "size": 16,
      "max_out_of_line": 0,
      "alignment": 8,
      "max_handles": 0
    }
  ],
  "xunion_declarations": [],
  "type_alias_declarations": [],
  "declaration_order": [
    "fidl.test.json/Example_foo_Response",
    "fidl.test.json/Example_foo_Result",
    "fidl.test.json/Example"
  ],
  "declarations": {
    "fidl.test.json/Example": "interface",
    "fidl.test.json/Example_foo_Response": "struct",
    "fidl.test.json/Example_foo_Result": "union"
  }
})JSON"));
  }

  END_TEST;
}

bool json_generator_test_byte_and_bytes() {
  BEGIN_TEST;

  for (int i = 0; i < kRepeatTestCount; i++) {
    EXPECT_TRUE(checkJSONGenerator(R"FIDL(
library example;

struct ByteAndBytes {
  byte single_byte;
  bytes many_bytes;
  bytes:1024 only_one_k_bytes;
  bytes:1024? opt_only_one_k_bytes;
};

)FIDL",
                                   R"JSON({
  "version": "0.0.1",
  "name": "example",
  "library_dependencies": [],
  "bits_declarations": [],
  "const_declarations": [],
  "enum_declarations": [],
  "interface_declarations": [],
  "struct_declarations": [
    {
      "name": "example/ByteAndBytes",
      "location": {
        "filename": "json.fidl",
        "line": 4,
        "column": 8
      },
      "anonymous": false,
      "members": [
        {
          "type": {
            "kind": "primitive",
            "subtype": "uint8"
          },
          "name": "single_byte",
          "location": {
            "filename": "json.fidl",
            "line": 5,
            "column": 8
          },
          "size": 1,
          "max_out_of_line": 0,
          "alignment": 1,
          "offset": 0,
          "max_handles": 0
        },
        {
          "type": {
            "kind": "vector",
            "element_type": {
              "kind": "primitive",
              "subtype": "uint8"
            },
            "nullable": false
          },
          "name": "many_bytes",
          "location": {
            "filename": "json.fidl",
            "line": 6,
            "column": 9
          },
          "size": 16,
          "max_out_of_line": 4294967295,
          "alignment": 8,
          "offset": 8,
          "max_handles": 0
        },
        {
          "type": {
            "kind": "vector",
            "element_type": {
              "kind": "primitive",
              "subtype": "uint8"
            },
            "maybe_element_count": 1024,
            "nullable": false
          },
          "name": "only_one_k_bytes",
          "location": {
            "filename": "json.fidl",
            "line": 7,
            "column": 14
          },
          "size": 16,
          "max_out_of_line": 1024,
          "alignment": 8,
          "offset": 24,
          "max_handles": 0
        },
        {
          "type": {
            "kind": "vector",
            "element_type": {
              "kind": "primitive",
              "subtype": "uint8"
            },
            "maybe_element_count": 1024,
            "nullable": true
          },
          "name": "opt_only_one_k_bytes",
          "location": {
            "filename": "json.fidl",
            "line": 8,
            "column": 15
          },
          "size": 16,
          "max_out_of_line": 1024,
          "alignment": 8,
          "offset": 40,
          "max_handles": 0
        }
      ],
      "size": 56,
      "max_out_of_line": 4294967295,
      "alignment": 8,
      "max_handles": 0,
      "has_padding": true
    }
  ],
  "table_declarations": [],
  "union_declarations": [],
  "xunion_declarations": [],
  "type_alias_declarations": [],
  "declaration_order": [
    "example/ByteAndBytes"
  ],
  "declarations": {
    "example/ByteAndBytes": "struct"
  }
})JSON"));
  }

  END_TEST;
}

bool json_generator_test_bits() {
  BEGIN_TEST;

  for (int i = 0; i < kRepeatTestCount; i++) {
    EXPECT_TRUE(checkJSONGenerator(R"FIDL(
library fidl.test.json;

bits Bits : uint64 {
    SMALLEST = 1;
    BIGGEST = 0x8000000000000000;
};

)FIDL",
                                   R"JSON(
{
  "version": "0.0.1",
  "name": "fidl.test.json",
  "library_dependencies": [],
  "bits_declarations": [
    {
      "name": "fidl.test.json/Bits",
      "type": {
        "kind": "primitive",
        "subtype": "uint64"
      },
      "mask": "9223372036854775809",
      "members": [
        {
          "name": "SMALLEST",
          "value": {
            "kind": "literal",
            "literal": {
              "kind": "numeric",
              "value": "1",
              "expression": "1"
            }
          }
        },
        {
          "name": "BIGGEST",
          "value": {
            "kind": "literal",
            "literal": {
              "kind": "numeric",
              "value": "9223372036854775808",
              "expression": "0x8000000000000000"
            }
          }
        }
      ]
    }
  ],
  "const_declarations": [],
  "enum_declarations": [],
  "interface_declarations": [],
  "struct_declarations": [],
  "table_declarations": [],
  "union_declarations": [],
  "xunion_declarations": [],
  "type_alias_declarations": [],
  "declaration_order": [
    "fidl.test.json/Bits"
  ],
  "declarations": {
    "fidl.test.json/Bits": "bits"
  }
}
)JSON"));
  }

  END_TEST;
}

bool json_generator_check_escaping() {
  BEGIN_TEST;

  TestLibrary library(R"FIDL(
library escapeme;

/// "pleaseescapethisdocommentproperly"
struct DocCommentWithQuotes {};
)FIDL");
  ASSERT_TRUE(library.Compile());
  auto json = library.GenerateJSON();
  ASSERT_STR_STR(json.c_str(), R"JSON("value": " \"pleaseescapethisdocommentproperly\"\n")JSON");

  END_TEST;
}

bool json_generator_constants() {
  BEGIN_TEST;

  for (int i = 0; i < kRepeatTestCount; i++) {
    EXPECT_TRUE(checkJSONGenerator(R"FIDL(
library values;

const uint8 UINT8 = 0b100;
const uint16 UINT16 = 0b100;
const uint32 UINT32 = 0b100;
const uint64 UINT64 = 0b100;
const int8 INT8 = 0b100;
const int16 INT16 = 0b100;
const int32 INT32 = 0b100;
const int64 INT64 = 0b100;
const float32 FLOAT32 = 3.14159;
const float64 FLOAT64 = 3.14159;
const bool BOOL = true;
const string STRING = "string";

enum Enum {
  E = 0b10101010;
};

bits Bits {
  B = 0x8;
};

struct Struct {
  int64 int64_with_default = 007;
  string string_with_default = "stuff";
  bool bool_with_default = true;
};

)FIDL",
                                   R"JSON(
{
  "version": "0.0.1",
  "name": "values",
  "library_dependencies": [],
  "bits_declarations": [
    {
      "name": "values/Bits",
      "type": {
        "kind": "primitive",
        "subtype": "uint32"
      },
      "mask": "8",
      "members": [
        {
          "name": "B",
          "value": {
            "kind": "literal",
            "literal": {
              "kind": "numeric",
              "value": "8",
              "expression": "0x8"
            }
          }
        }
      ]
    }
  ],
  "const_declarations": [
    {
      "name": "values/UINT8",
      "location": {
        "filename": "json.fidl",
        "line": 4,
        "column": 13
      },
      "type": {
        "kind": "primitive",
        "subtype": "uint8"
      },
      "value": {
        "kind": "literal",
        "literal": {
          "kind": "numeric",
          "value": "4",
          "expression": "0b100"
        }
      }
    },
    {
      "name": "values/UINT16",
      "location": {
        "filename": "json.fidl",
        "line": 5,
        "column": 14
      },
      "type": {
        "kind": "primitive",
        "subtype": "uint16"
      },
      "value": {
        "kind": "literal",
        "literal": {
          "kind": "numeric",
          "value": "4",
          "expression": "0b100"
        }
      }
    },
    {
      "name": "values/UINT32",
      "location": {
        "filename": "json.fidl",
        "line": 6,
        "column": 14
      },
      "type": {
        "kind": "primitive",
        "subtype": "uint32"
      },
      "value": {
        "kind": "literal",
        "literal": {
          "kind": "numeric",
          "value": "4",
          "expression": "0b100"
        }
      }
    },
    {
      "name": "values/UINT64",
      "location": {
        "filename": "json.fidl",
        "line": 7,
        "column": 14
      },
      "type": {
        "kind": "primitive",
        "subtype": "uint64"
      },
      "value": {
        "kind": "literal",
        "literal": {
          "kind": "numeric",
          "value": "4",
          "expression": "0b100"
        }
      }
    },
    {
      "name": "values/INT8",
      "location": {
        "filename": "json.fidl",
        "line": 8,
        "column": 12
      },
      "type": {
        "kind": "primitive",
        "subtype": "int8"
      },
      "value": {
        "kind": "literal",
        "literal": {
          "kind": "numeric",
          "value": "4",
          "expression": "0b100"
        }
      }
    },
    {
      "name": "values/INT16",
      "location": {
        "filename": "json.fidl",
        "line": 9,
        "column": 13
      },
      "type": {
        "kind": "primitive",
        "subtype": "int16"
      },
      "value": {
        "kind": "literal",
        "literal": {
          "kind": "numeric",
          "value": "4",
          "expression": "0b100"
        }
      }
    },
    {
      "name": "values/INT32",
      "location": {
        "filename": "json.fidl",
        "line": 10,
        "column": 13
      },
      "type": {
        "kind": "primitive",
        "subtype": "int32"
      },
      "value": {
        "kind": "literal",
        "literal": {
          "kind": "numeric",
          "value": "4",
          "expression": "0b100"
        }
      }
    },
    {
      "name": "values/INT64",
      "location": {
        "filename": "json.fidl",
        "line": 11,
        "column": 13
      },
      "type": {
        "kind": "primitive",
        "subtype": "int64"
      },
      "value": {
        "kind": "literal",
        "literal": {
          "kind": "numeric",
          "value": "4",
          "expression": "0b100"
        }
      }
    },
    {
      "name": "values/FLOAT32",
      "location": {
        "filename": "json.fidl",
        "line": 12,
        "column": 15
      },
      "type": {
        "kind": "primitive",
        "subtype": "float32"
      },
      "value": {
        "kind": "literal",
        "literal": {
          "kind": "numeric",
          "value": "3.14159",
          "expression": "3.14159"
        }
      }
    },
    {
      "name": "values/FLOAT64",
      "location": {
        "filename": "json.fidl",
        "line": 13,
        "column": 15
      },
      "type": {
        "kind": "primitive",
        "subtype": "float64"
      },
      "value": {
        "kind": "literal",
        "literal": {
          "kind": "numeric",
          "value": "3.14159",
          "expression": "3.14159"
        }
      }
    },
    {
      "name": "values/BOOL",
      "location": {
        "filename": "json.fidl",
        "line": 14,
        "column": 12
      },
      "type": {
        "kind": "primitive",
        "subtype": "bool"
      },
      "value": {
        "kind": "literal",
        "literal": {
          "kind": "true",
          "value": "true",
          "expression": "true"
        }
      }
    },
    {
      "name": "values/STRING",
      "location": {
        "filename": "json.fidl",
        "line": 15,
        "column": 14
      },
      "type": {
        "kind": "string",
        "nullable": false
      },
      "value": {
        "kind": "literal",
        "literal": {
          "kind": "string",
          "value": "string",
          "expression": "\"string\""
        }
      }
    }
  ],
  "enum_declarations": [
    {
      "name": "values/Enum",
      "location": {
        "filename": "json.fidl",
        "line": 17,
        "column": 6
      },
      "type": "uint32",
      "members": [
        {
          "name": "E",
          "location": {
            "filename": "json.fidl",
            "line": 18,
            "column": 3
          },
          "value": {
            "kind": "literal",
            "literal": {
              "kind": "numeric",
              "value": "170",
              "expression": "0b10101010"
            }
          }
        }
      ]
    }
  ],
  "interface_declarations": [],
  "struct_declarations": [
    {
      "name": "values/Struct",
      "location": {
        "filename": "json.fidl",
        "line": 25,
        "column": 8
      },
      "anonymous": false,
      "members": [
        {
          "type": {
            "kind": "primitive",
            "subtype": "int64"
          },
          "name": "int64_with_default",
          "location": {
            "filename": "json.fidl",
            "line": 26,
            "column": 9
          },
          "maybe_default_value": {
            "kind": "literal",
            "literal": {
              "kind": "numeric",
              "value": "007",
              "expression": "007"
            }
          },
          "size": 8,
          "max_out_of_line": 0,
          "alignment": 8,
          "offset": 0,
          "max_handles": 0
        },
        {
          "type": {
            "kind": "string",
            "nullable": false
          },
          "name": "string_with_default",
          "location": {
            "filename": "json.fidl",
            "line": 27,
            "column": 10
          },
          "maybe_default_value": {
            "kind": "literal",
            "literal": {
              "kind": "string",
              "value": "stuff",
              "expression": "\"stuff\""
            }
          },
          "size": 16,
          "max_out_of_line": 4294967295,
          "alignment": 8,
          "offset": 8,
          "max_handles": 0
        },
        {
          "type": {
            "kind": "primitive",
            "subtype": "bool"
          },
          "name": "bool_with_default",
          "location": {
            "filename": "json.fidl",
            "line": 28,
            "column": 8
          },
          "maybe_default_value": {
            "kind": "literal",
            "literal": {
              "kind": "true",
              "value": "true",
              "expression": "true"
            }
          },
          "size": 1,
          "max_out_of_line": 0,
          "alignment": 1,
          "offset": 24,
          "max_handles": 0
        }
      ],
      "size": 32,
      "max_out_of_line": 4294967295,
      "alignment": 8,
      "max_handles": 0,
      "has_padding": true
    }
  ],
  "table_declarations": [],
  "union_declarations": [],
  "xunion_declarations": [],
  "type_alias_declarations": [],
  "declaration_order": [
    "values/UINT8",
    "values/UINT64",
    "values/UINT32",
    "values/UINT16",
    "values/Struct",
    "values/STRING",
    "values/INT8",
    "values/INT64",
    "values/INT32",
    "values/INT16",
    "values/FLOAT64",
    "values/FLOAT32",
    "values/Enum",
    "values/Bits",
    "values/BOOL"
  ],
  "declarations": {
    "values/Bits": "bits",
    "values/UINT8": "const",
    "values/UINT16": "const",
    "values/UINT32": "const",
    "values/UINT64": "const",
    "values/INT8": "const",
    "values/INT16": "const",
    "values/INT32": "const",
    "values/INT64": "const",
    "values/FLOAT32": "const",
    "values/FLOAT64": "const",
    "values/BOOL": "const",
    "values/STRING": "const",
    "values/Enum": "enum",
    "values/Struct": "struct"
  }
}
)JSON"));
  }

  END_TEST;
}

bool json_generator_transitive_dependencies() {
  BEGIN_TEST;

  for (int i = 0; i < kRepeatTestCount; i++) {
    SharedAmongstLibraries shared;
    TestLibrary bottom_dep("bottom.fidl", R"FIDL(
library bottom;

struct Foo {
  int32 a;
};

)FIDL",
                           &shared);
    ASSERT_TRUE(bottom_dep.Compile());
    TestLibrary middle_dep("middle.fidl", R"FIDL(
library middle;

using bottom;

struct Bar {
  bottom.Foo f;
};

)FIDL",
                           &shared);
    ASSERT_TRUE(middle_dep.AddDependentLibrary(std::move(bottom_dep)));
    ASSERT_TRUE(middle_dep.Compile());

    TestLibrary library("top.fidl", R"FIDL(
library top;

using middle;

struct Baz {
  middle.Bar g;
};

)FIDL",
                        &shared);
    ASSERT_TRUE(library.AddDependentLibrary(std::move(middle_dep)));
    EXPECT_TRUE(checkJSONGenerator(std::move(library),
                                   R"JSON(
{
  "version": "0.0.1",
  "name": "top",
  "library_dependencies": [
    {
      "name": "middle",
      "declarations": {
        "middle/Bar": "struct"
      }
    }
  ],
  "bits_declarations": [],
  "const_declarations": [],
  "enum_declarations": [],
  "interface_declarations": [],
  "struct_declarations": [
    {
      "name": "top/Baz",
      "location": {
        "filename": "top.fidl",
        "line": 6,
        "column": 8
      },
      "anonymous": false,
      "members": [
        {
          "type": {
            "kind": "identifier",
            "identifier": "middle/Bar",
            "nullable": false
          },
          "name": "g",
          "location": {
            "filename": "top.fidl",
            "line": 7,
            "column": 14
          },
          "size": 4,
          "max_out_of_line": 0,
          "alignment": 4,
          "offset": 0,
          "max_handles": 0
        }
      ],
      "size": 4,
      "max_out_of_line": 0,
      "alignment": 4,
      "max_handles": 0,
      "has_padding": false
    }
  ],
  "table_declarations": [],
  "union_declarations": [],
  "xunion_declarations": [],
  "type_alias_declarations": [],
  "declaration_order": [
    "top/Baz"
  ],
  "declarations": {
    "top/Baz": "struct"
  }
}
)JSON"));
  }

  END_TEST;
}

bool json_generator_transitive_dependencies_compose() {
  BEGIN_TEST;

  for (int i = 0; i < kRepeatTestCount; i++) {
    SharedAmongstLibraries shared;
    TestLibrary bottom_dep("bottom.fidl", R"FIDL(
library bottom;

struct Foo {
  int32 a;
};

[FragileBase]
protocol Bottom {
  GetFoo() -> (Foo foo);
};

)FIDL",
                           &shared);
    ASSERT_TRUE(bottom_dep.Compile());
    TestLibrary middle_dep("middle.fidl", R"FIDL(
library middle;

using bottom;

[FragileBase]
protocol Middle {
  compose bottom.Bottom;
};

)FIDL",
                           &shared);
    ASSERT_TRUE(middle_dep.AddDependentLibrary(std::move(bottom_dep)));
    ASSERT_TRUE(middle_dep.Compile());

    TestLibrary library("top.fidl", R"FIDL(
library top;

using middle;

protocol Top {
  compose middle.Middle;
};

)FIDL",
                        &shared);
    ASSERT_TRUE(library.AddDependentLibrary(std::move(middle_dep)));
    EXPECT_TRUE(checkJSONGenerator(std::move(library),
                                   R"JSON(
{
  "version": "0.0.1",
  "name": "top",
  "library_dependencies": [
    {
      "name": "bottom",
      "declarations": {
        "bottom/Bottom": "interface",
        "bottom/Foo": "struct"
      }
    },
    {
      "name": "middle",
      "declarations": {
        "middle/Middle": "interface"
      }
    }
  ],
  "bits_declarations": [],
  "const_declarations": [],
  "enum_declarations": [],
  "interface_declarations": [
    {
      "name": "top/Top",
      "location": {
        "filename": "top.fidl",
        "line": 6,
        "column": 10
      },
      "methods": [
        {
          "ordinal": 961142572,
          "generated_ordinal": 961142572,
          "name": "GetFoo",
          "location": {
            "filename": "bottom.fidl",
            "line": 10,
            "column": 3
          },
          "has_request": true,
          "maybe_request": [],
          "maybe_request_size": 16,
          "maybe_request_alignment": 8,
          "maybe_request_has_padding": false,
          "has_response": true,
          "maybe_response": [
            {
              "type": {
                "kind": "identifier",
                "identifier": "bottom/Foo",
                "nullable": false
              },
              "name": "foo",
              "location": {
                "filename": "bottom.fidl",
                "line": 10,
                "column": 20
              },
              "size": 4,
              "max_out_of_line": 0,
              "alignment": 4,
              "offset": 16,
              "max_handles": 0
            }
          ],
          "maybe_response_size": 24,
          "maybe_response_alignment": 8,
          "maybe_response_has_padding": true,
          "is_composed": true
        }
      ]
    }
  ],
  "struct_declarations": [],
  "table_declarations": [],
  "union_declarations": [],
  "xunion_declarations": [],
  "type_alias_declarations": [],
  "declaration_order": [
    "top/Top"
  ],
  "declarations": {
    "top/Top": "interface"
  }
}
)JSON"));
  }

  END_TEST;
}

bool json_generator_placement_of_attributes() {
  BEGIN_TEST;

  for (int i = 0; i < kRepeatTestCount; i++) {
    SharedAmongstLibraries shared;
    TestLibrary dependency("exampleusing.fidl", R"FIDL(
library exampleusing;

struct Empty {};

)FIDL",
                           &shared);
    ASSERT_TRUE(dependency.Compile());

    TestLibrary library("example.fidl", R"FIDL(
[OnLibrary]
library example;

// TODO: Support placement of an attribute on using.
using exampleusing;

[OnBits]
bits ExampleBits {
    [OnBitsMember]
    MEMBER = 1;
};

[OnConst]
const uint32 EXAMPLE_CONST = 0;

[OnEnum]
enum ExampleEnum {
    [OnEnumMember]
    MEMBER = 1;
};

[OnProtocol]
protocol ExampleProtocol {
    [OnMethod]
    Method(exampleusing.Empty arg);
};

[OnStruct]
struct ExampleStruct {
    [OnStructMember]
    uint32 member;
};

[OnTable]
table ExampleTable {
    [OnTableMember]
    1: uint32 member;
};

[OnTypeAlias]
using TypeAlias = uint32;

[OnUnion]
union ExampleUnion {
    [OnUnionMember]
    uint32 variant;
};

[OnXUnion]
xunion ExampleXUnion {
    [OnXUnionMember]
    uint32 variant;
};

)FIDL",
                        &shared);
    ASSERT_TRUE(library.AddDependentLibrary(std::move(dependency)));
    EXPECT_TRUE(checkJSONGenerator(std::move(library),
                                   R"JSON({
  "version": "0.0.1",
  "name": "example",
  "library_dependencies": [
    {
      "name": "exampleusing",
      "declarations": {
        "exampleusing/Empty": "struct"
      }
    }
  ],
  "bits_declarations": [
    {
      "name": "example/ExampleBits",
      "maybe_attributes": [
        {
          "name": "OnBits",
          "value": ""
        }
      ],
      "type": {
        "kind": "primitive",
        "subtype": "uint32"
      },
      "mask": "1",
      "members": [
        {
          "name": "MEMBER",
          "value": {
            "kind": "literal",
            "literal": {
              "kind": "numeric",
              "value": "1",
              "expression": "1"
            }
          },
          "maybe_attributes": [
            {
              "name": "OnBitsMember",
              "value": ""
            }
          ]
        }
      ]
    }
  ],
  "const_declarations": [
    {
      "name": "example/EXAMPLE_CONST",
      "location": {
        "filename": "example.fidl",
        "line": 15,
        "column": 14
      },
      "maybe_attributes": [
        {
          "name": "OnConst",
          "value": ""
        }
      ],
      "type": {
        "kind": "primitive",
        "subtype": "uint32"
      },
      "value": {
        "kind": "literal",
        "literal": {
          "kind": "numeric",
          "value": "0",
          "expression": "0"
        }
      }
    }
  ],
  "enum_declarations": [
    {
      "name": "example/ExampleEnum",
      "location": {
        "filename": "example.fidl",
        "line": 18,
        "column": 6
      },
      "maybe_attributes": [
        {
          "name": "OnEnum",
          "value": ""
        }
      ],
      "type": "uint32",
      "members": [
        {
          "name": "MEMBER",
          "location": {
            "filename": "example.fidl",
            "line": 20,
            "column": 5
          },
          "value": {
            "kind": "literal",
            "literal": {
              "kind": "numeric",
              "value": "1",
              "expression": "1"
            }
          },
          "maybe_attributes": [
            {
              "name": "OnEnumMember",
              "value": ""
            }
          ]
        }
      ]
    }
  ],
  "interface_declarations": [
    {
      "name": "example/ExampleProtocol",
      "location": {
        "filename": "example.fidl",
        "line": 24,
        "column": 10
      },
      "maybe_attributes": [
        {
          "name": "OnProtocol",
          "value": ""
        }
      ],
      "methods": [
        {
          "ordinal": 1123904027,
          "generated_ordinal": 1123904027,
          "name": "Method",
          "location": {
            "filename": "example.fidl",
            "line": 26,
            "column": 5
          },
          "has_request": true,
          "maybe_attributes": [
            {
              "name": "OnMethod",
              "value": ""
            }
          ],
          "maybe_request": [
            {
              "type": {
                "kind": "identifier",
                "identifier": "exampleusing/Empty",
                "nullable": false
              },
              "name": "arg",
              "location": {
                "filename": "example.fidl",
                "line": 26,
                "column": 31
              },
              "size": 1,
              "max_out_of_line": 0,
              "alignment": 1,
              "offset": 16,
              "max_handles": 0
            }
          ],
          "maybe_request_size": 24,
          "maybe_request_alignment": 8,
          "maybe_request_has_padding": true,
          "has_response": false,
          "is_composed": false
        }
      ]
    }
  ],
  "struct_declarations": [
    {
      "name": "example/ExampleStruct",
      "location": {
        "filename": "example.fidl",
        "line": 30,
        "column": 8
      },
      "anonymous": false,
      "maybe_attributes": [
        {
          "name": "OnStruct",
          "value": ""
        }
      ],
      "members": [
        {
          "type": {
            "kind": "primitive",
            "subtype": "uint32"
          },
          "name": "member",
          "location": {
            "filename": "example.fidl",
            "line": 32,
            "column": 12
          },
          "maybe_attributes": [
            {
              "name": "OnStructMember",
              "value": ""
            }
          ],
          "size": 4,
          "max_out_of_line": 0,
          "alignment": 4,
          "offset": 0,
          "max_handles": 0
        }
      ],
      "size": 4,
      "max_out_of_line": 0,
      "alignment": 4,
      "max_handles": 0,
      "has_padding": false
    }
  ],
  "table_declarations": [
    {
      "name": "example/ExampleTable",
      "location": {
        "filename": "example.fidl",
        "line": 36,
        "column": 7
      },
      "maybe_attributes": [
        {
          "name": "OnTable",
          "value": ""
        }
      ],
      "members": [
        {
          "ordinal": 1,
          "reserved": false,
          "type": {
            "kind": "primitive",
            "subtype": "uint32"
          },
          "name": "member",
          "location": {
            "filename": "example.fidl",
            "line": 38,
            "column": 15
          },
          "maybe_attributes": [
            {
              "name": "OnTableMember",
              "value": ""
            }
          ],
          "size": 4,
          "max_out_of_line": 0,
          "alignment": 4,
          "max_handles": 0
        }
      ],
      "size": 16,
      "max_out_of_line": 24,
      "alignment": 8,
      "max_handles": 0
    }
  ],
  "union_declarations": [
    {
      "name": "example/ExampleUnion",
      "location": {
        "filename": "example.fidl",
        "line": 45,
        "column": 7
      },
      "maybe_attributes": [
        {
          "name": "OnUnion",
          "value": ""
        }
      ],
      "members": [
        {
          "type": {
            "kind": "primitive",
            "subtype": "uint32"
          },
          "name": "variant",
          "location": {
            "filename": "example.fidl",
            "line": 47,
            "column": 12
          },
          "maybe_attributes": [
            {
              "name": "OnUnionMember",
              "value": ""
            }
          ],
          "size": 4,
          "max_out_of_line": 0,
          "alignment": 4,
          "offset": 4
        }
      ],
      "size": 8,
      "max_out_of_line": 0,
      "alignment": 4,
      "max_handles": 0
    }
  ],
  "xunion_declarations": [
    {
      "name": "example/ExampleXUnion",
      "location": {
        "filename": "example.fidl",
        "line": 51,
        "column": 8
      },
      "maybe_attributes": [
        {
          "name": "OnXUnion",
          "value": ""
        }
      ],
      "members": [
        {
          "ordinal": 1300389554,
          "type": {
            "kind": "primitive",
            "subtype": "uint32"
          },
          "name": "variant",
          "location": {
            "filename": "example.fidl",
            "line": 53,
            "column": 12
          },
          "maybe_attributes": [
            {
              "name": "OnXUnionMember",
              "value": ""
            }
          ],
          "size": 4,
          "max_out_of_line": 0,
          "alignment": 4,
          "offset": 0
        }
      ],
      "size": 24,
      "max_out_of_line": 8,
      "alignment": 8,
      "max_handles": 0,
      "strict": false
    }
  ],
  "type_alias_declarations": [
    {
      "name": "example/TypeAlias",
      "location": {
        "filename": "example.fidl",
        "line": 42,
        "column": 7
      },
      "maybe_attributes": [
        {
          "name": "OnTypeAlias",
          "value": ""
        }
      ],
      "partial_type_ctor": {
        "name": "uint32",
        "args": [],
        "nullable": false
      }
    }
  ],
  "declaration_order": [
    "example/ExampleProtocol",
    "example/TypeAlias",
    "example/ExampleXUnion",
    "example/ExampleUnion",
    "example/ExampleTable",
    "example/ExampleStruct",
    "example/ExampleEnum",
    "example/ExampleBits",
    "example/EXAMPLE_CONST"
  ],
  "declarations": {
    "example/ExampleBits": "bits",
    "example/EXAMPLE_CONST": "const",
    "example/ExampleEnum": "enum",
    "example/ExampleProtocol": "interface",
    "example/ExampleStruct": "struct",
    "example/ExampleTable": "table",
    "example/ExampleUnion": "union",
    "example/ExampleXUnion": "xunion",
    "example/TypeAlias": "type_alias"
  }
})JSON"));
  }

  END_TEST;
}

bool json_generator_type_aliases() {
  BEGIN_TEST;

  for (int i = 0; i < kRepeatTestCount; i++) {
    EXPECT_TRUE(checkJSONGenerator(R"FIDL(
library example;

using u32 = uint32;
using vec_at_most_five = vector:5;
using vec_of_strings = vector<string>;
using vec_of_strings_at_most_5 = vector<string>:5;
using channel = handle<channel>;

)FIDL",
                                   R"JSON(
{
  "version": "0.0.1",
  "name": "example",
  "library_dependencies": [],
  "bits_declarations": [],
  "const_declarations": [],
  "enum_declarations": [],
  "interface_declarations": [],
  "struct_declarations": [],
  "table_declarations": [],
  "union_declarations": [],
  "xunion_declarations": [],
  "type_alias_declarations": [
    {
      "name": "example/u32",
      "location": {
        "filename": "json.fidl",
        "line": 4,
        "column": 7
      },
      "partial_type_ctor": {
        "name": "uint32",
        "args": [],
        "nullable": false
      }
    },
    {
      "name": "example/vec_at_most_five",
      "location": {
        "filename": "json.fidl",
        "line": 5,
        "column": 7
      },
      "partial_type_ctor": {
        "name": "example/vector",
        "args": [],
        "nullable": false,
        "maybe_size": {
          "kind": "literal",
          "literal": {
            "kind": "numeric",
            "value": "5",
            "expression": "5"
          }
        }
      }
    },
    {
      "name": "example/vec_of_strings",
      "location": {
        "filename": "json.fidl",
        "line": 6,
        "column": 7
      },
      "partial_type_ctor": {
        "name": "vector",
        "args": [
          {
            "name": "string",
            "args": [],
            "nullable": false
          }
        ],
        "nullable": false
      }
    },
    {
      "name": "example/vec_of_strings_at_most_5",
      "location": {
        "filename": "json.fidl",
        "line": 7,
        "column": 7
      },
      "partial_type_ctor": {
        "name": "vector",
        "args": [
          {
            "name": "string",
            "args": [],
            "nullable": false
          }
        ],
        "nullable": false,
        "maybe_size": {
          "kind": "literal",
          "literal": {
            "kind": "numeric",
            "value": "5",
            "expression": "5"
          }
        }
      }
    },
    {
      "name": "example/channel",
      "location": {
        "filename": "json.fidl",
        "line": 8,
        "column": 7
      },
      "partial_type_ctor": {
        "name": "handle",
        "args": [],
        "nullable": false,
        "maybe_handle_subtype": "channel"
      }
    }
  ],
  "declaration_order": [
    "example/vec_of_strings_at_most_5",
    "example/vec_of_strings",
    "example/vec_at_most_five",
    "example/u32",
    "example/channel"
  ],
  "declarations": {
    "example/u32": "type_alias",
    "example/vec_at_most_five": "type_alias",
    "example/vec_of_strings": "type_alias",
    "example/vec_of_strings_at_most_5": "type_alias",
    "example/channel": "type_alias"
  }
}
)JSON"));
  }

  END_TEST;
}

}  // namespace

BEGIN_TEST_CASE(json_generator_tests)
RUN_TEST(json_generator_test_empty_struct)
RUN_TEST(json_generator_test_struct)
RUN_TEST(json_generator_test_table)
RUN_TEST(json_generator_test_union)
RUN_TEST(json_generator_test_xunion)
RUN_TEST(json_generator_test_inheritance)
RUN_TEST(json_generator_test_inheritance_with_recursive_decl)
RUN_TEST(json_generator_test_error)
RUN_TEST(json_generator_test_byte_and_bytes)
RUN_TEST(json_generator_test_bits)
RUN_TEST(json_generator_check_escaping)
RUN_TEST(json_generator_constants)
RUN_TEST(json_generator_transitive_dependencies)
RUN_TEST(json_generator_transitive_dependencies_compose)
RUN_TEST(json_generator_placement_of_attributes)
RUN_TEST(json_generator_type_aliases)
END_TEST_CASE(json_generator_tests)
