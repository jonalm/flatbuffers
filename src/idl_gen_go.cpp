/*
 * Copyright 2014 Google Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// independent from idl_parser, since this code is not needed for most clients

#include <sstream>
#include <string>
#include <iostream>

#include "flatbuffers/code_generators.h"
#include "flatbuffers/flatbuffers.h"
#include "flatbuffers/idl.h"
#include "flatbuffers/util.h"

#ifdef _WIN32
#  include <direct.h>
#  define PATH_SEPARATOR "\\"
#  define mkdir(n, m) _mkdir(n)
#else
#  include <sys/stat.h>
#  define PATH_SEPARATOR "/"
#endif

namespace flatbuffers {

namespace go {

// see https://docs.julialang.org/en/v1/base/base/#Keywords
static const char *const g_golang_keywords[] = {
  "baremodule", "begin", "break", "catch", "const", "continue", "do", "else",
  "elseif", "end", "export", "false", "finally", "for", "function", "global",
  "if", "import", "let", "local", "macro", "module", "quote", "return", 
  "struct", "true", "try", "using", "while"
};

static std::string GoIdentity(const std::string &name) {
  for (size_t i = 0;
       i < sizeof(g_golang_keywords) / sizeof(g_golang_keywords[0]); i++) {
    if (name == g_golang_keywords[i]) { return MakeCamel(name + "_", false); }
  }

  return MakeCamel(name, false);
}

class GoGenerator : public BaseGenerator {
 public:
  GoGenerator(const Parser &parser, const std::string &path,
              const std::string &file_name, const std::string &go_namespace)
      : BaseGenerator(parser, path, file_name, "" /* not used*/,
                      "" /* not used */, "go"),
        cur_name_space_(nullptr) {
    std::istringstream iss(go_namespace);
    std::string component;
    while (std::getline(iss, component, '.')) {
      go_namespace_.components.push_back(component);
    }
  }

  bool generate() {
    std::string one_file_code;
    bool needs_imports = false;

    for (auto it = parser_.enums_.vec.begin(); it != parser_.enums_.vec.end();
         ++it) {
      tracked_imported_namespaces_.clear();
      needs_imports = false;
      std::string enumcode;
      GenEnum(**it, &enumcode);
      if (parser_.opts.one_file) {
        one_file_code += enumcode;
      } else {
        if (!SaveType(**it, enumcode, needs_imports, true)) return false;
      }
    }


    for (auto it = parser_.structs_.vec.begin();
         it != parser_.structs_.vec.end(); ++it) {
      tracked_imported_namespaces_.clear();
      std::string declcode;
      GenStruct(**it, &declcode);
      if (parser_.opts.one_file) {
        one_file_code += declcode;
      } else {
        if (!SaveType(**it, declcode, true, false)) return false;
      }
    }

    if (parser_.opts.one_file) {
      std::string code = "";
      const bool is_enum = !parser_.enums_.vec.empty();
      BeginFile(LastNamespacePart(go_namespace_), true, is_enum, &code);
      code += one_file_code;
      const std::string filename =
          GeneratedFileName(path_, file_name_, parser_.opts);
      return SaveFile(filename.c_str(), code, false);
    }

    return true;
  }

 private:
  Namespace go_namespace_;
  Namespace *cur_name_space_;

  struct NamespacePtrLess {
    bool operator()(const Namespace *a, const Namespace *b) const {
      return *a < *b;
    }
  };
  std::set<const Namespace *, NamespacePtrLess> tracked_imported_namespaces_;

  // Most field accessors need to retrieve and test the field offset first,
  // this is the prefix code for that.
  std::string OffsetPrefix(const FieldDef &field) {
    return "{\n\to := flatbuffers.UOffsetT(rcv._tab.Offset(" +
           NumToString(field.value.offset) + "))\n\tif o != 0 {\n";
  }

  // Begin a class declaration.
  void BeginClass(const StructDef &struct_def, std::string *code_ptr) {
    std::string &code = *code_ptr;

    code += "struct "+ struct_def.name + " <: ";
    code += struct_def.fixed ? "FlatBuffers.Struct\n" : "FlatBuffers.Table\n";
    code += "\tbytes::Vector{UInt8}\n";
    code += "\tpos::Base.Int\n";
    code += "end\n\n";

  }

  // Construct the name of the type for this enum.
  std::string GetEnumTypeName(const EnumDef &enum_def) {
    return WrapInNameSpaceAndTrack(enum_def.defined_namespace,
                                   GoIdentity(enum_def.name));
  }

  // Begin enum code with a class declaration.
  void BeginEnum(const EnumDef &enum_def, std::string *code_ptr) {
    std::string &code = *code_ptr;
    code += "FlatBuffers.@scopedenum " + enum_def.name + "::" + GenTypeBasic(enum_def.underlying_type)  + " ";
  }

  // A single enum member.
  void EnumMember(const EnumDef &enum_def, const EnumVal &ev,
                  size_t max_name_length, std::string *code_ptr) {
    std::string &code = *code_ptr;
    code += ev.name + "=" + enum_def.ToString(ev) + " ";
  }

  // End enum code.
  void EndEnum(std::string *code_ptr) {
    std::string &code = *code_ptr;
    code += "\n\n";
  }

  // Get the value of a struct's scalar.
  void GetScalarFieldOfStruct(const StructDef &struct_def,
                              const FieldDef &field, std::string *code_ptr) {
    std::string &code = *code_ptr;
    code += "\t\treturn FlatBuffers.get(x, FlatBuffers.pos(x) + " + NumToString(field.value.offset) + ", " + TypeName(field) +  ")\n";
  }

  // Get the value of a table's scalar.
  void GetScalarFieldOfTable(const StructDef &struct_def, const FieldDef &field,
                             std::string *code_ptr) {
    std::string &code = *code_ptr;
    code += "\t\to = FlatBuffers.offset(x, " + NumToString(field.value.offset) +")\n";
    code += "\t\to != 0 && return FlatBuffers.get(x, o + FlatBuffers.pos(x), " + TypeName(field) + ")\n";
    code += "\t\treturn " + GenTypeGet(field.value.type) + "(" + GenConstant(field) + ")\n";
  }

  // Get a struct by initializing an existing struct.
  // Specific to Struct.
  //void GetStructFieldOfStruct(const StructDef &struct_def,
  //                            const FieldDef &field, std::string *code_ptr) {
  //  std::string &code = *code_ptr;
  //  GenReceiver(struct_def, code_ptr);
  //  code += " " + MakeCamel(field.name);
  //  code += "(obj *" + TypeName(field);
  //  code += ") *" + TypeName(field);
  //  code += " {\n";
  //  code += "\tif obj == nil {\n";
  //  code += "\t\tobj = new(" + TypeName(field) + ")\n";
  //  code += "\t}\n";
  //  code += "\tobj.Init(rcv._tab.Bytes, rcv._tab.Pos+";
  //  code += NumToString(field.value.offset) + ")";
  //  code += "\n\treturn obj\n";
  //  code += "}\n";
  //}

  // Get a struct by initializing an existing struct.
  // Specific to Table.
  void GetStructFieldOfTable(const StructDef &struct_def, const FieldDef &field,
                             std::string *code_ptr) {
    std::string &code = *code_ptr;

    // todo chech branch on struct_def->fixed in commented code below
    code += "\t\to = FlatBuffers.offset(x, " + NumToString(field.value.offset) + ")\n";
    code += "\t\tif o != 0\n";
    code += "\t\t\ty = FlatBuffers.indirect(x, o + FlatBuffers.pos(x))\n";
    code += "\t\t\treturn FlatBuffers.init(" + TypeName(field) + ", FlatBuffers.bytes(x), y)\n";
    code += "\t\tend\n";

    //if (field.value.type.struct_def->fixed) {
    //  code += "\t\tx := o + rcv._tab.Pos\n";
    //} else {
    //  code += "\t\tx := rcv._tab.Indirect(o + rcv._tab.Pos)\n";
    //}
  }

  // Get the value of a string.
  void GetStringField(const StructDef &struct_def, const FieldDef &field,
                      std::string *code_ptr) {
    std::string &code = *code_ptr;
    code += "\t\to = FlatBuffers.offset(x, " + NumToString(field.value.offset) +")\n";
    code += "\t\to != 0 && return String(x, o + FlatBuffers.pos(x))\n";
    code += "\t\treturn string(" + GenConstant(field) + ")\n";
  }

  // Get the value of a union from an object.
  //void GetUnionField(const StructDef &struct_def, const FieldDef &field,
  //                   std::string *code_ptr) {
  //  std::string &code = *code_ptr;
  //  GenReceiver(struct_def, code_ptr);
  //  code += " " + MakeCamel(field.name) + "(";
  //  code += "obj " + GenTypePointer(field.value.type) + ") bool ";
  //  code += OffsetPrefix(field);
  //  code += "\t\t" + GenGetter(field.value.type);
  //  code += "(obj, o)\n\t\treturn true\n\t}\n";
  //  code += "\treturn false\n";
  //  code += "}\n\n";
  //}

  // Get the value of a vector's struct member.
  void GetMemberOfVector(const StructDef &struct_def,
                                 const FieldDef &field, std::string *code_ptr) {
    std::string &code = *code_ptr;
    code += "\t\to = FlatBuffers.offset(x, " + NumToString(field.value.offset) +")\n";
    code += "\t\to != 0 && return FlatBuffers.Array{" + TypeName(field) + "}(x, o)\n";
  }

  // Begin the creator function signature.
  void BeginBuilderArgs(const StructDef &struct_def, std::string *code_ptr) {
    std::string &code = *code_ptr;
    code += "function create" + struct_def.name;
    code += "(b::FlatBuffers.Builder";
  }

  // Recursively generate arguments for a constructor, to deal with nested
  // structs.
  void StructBuilderArgs(const StructDef &struct_def, const char *nameprefix,
                         std::string *code_ptr) {
    for (auto it = struct_def.fields.vec.begin();
         it != struct_def.fields.vec.end(); ++it) {
      auto &field = **it;
      if (IsStruct(field.value.type)) {
        // Generate arguments for a struct inside a struct. To ensure names
        // don't clash, and to make it obvious these arguments are constructing
        // a nested struct, prefix the name with the field name.
        StructBuilderArgs(*field.value.type.struct_def,
                          (nameprefix + (field.name + "_")).c_str(), code_ptr);
      } else {
        std::string &code = *code_ptr;
        code += std::string(", ") + nameprefix;
        code += GoIdentity(field.name);
        code += "::" + TypeName(field);
      }
    }
  }

  // End the creator function signature.
  void EndBuilderArgs(std::string *code_ptr) {
    std::string &code = *code_ptr;
    code += ")\n";
  }

  // Recursively generate struct construction statements and instert manual
  // padding.
  void StructBuilderBody(const StructDef &struct_def, const char *nameprefix,
                         std::string *code_ptr) {
    std::string &code = *code_ptr;
    code += "\tFlatBuffers.prep!(b, " + NumToString(struct_def.minalign) + ", ";
    code += NumToString(struct_def.bytesize) + ")\n";
    for (auto it = struct_def.fields.vec.rbegin();
         it != struct_def.fields.vec.rend(); ++it) {
      auto &field = **it;
      if (field.padding)
        code += "\tFlatBuffers.pad!(b, " + NumToString(field.padding) + ")\n";
      if (IsStruct(field.value.type)) {
        // TODO: investigate this branch
        StructBuilderBody(*field.value.type.struct_def,
                          (nameprefix + (field.name + "_")).c_str(), code_ptr);
      } else {
        code += "\tFlatBuffers.prepend!(b, ";
        code += CastToBaseType(field.value.type,
                               nameprefix + GoIdentity(field.name)) +
                ")\n";
      }
    }
  }

  void EndBuilderBody(std::string *code_ptr) {
    std::string &code = *code_ptr;
    code += "\treturn FlatBuffers.offset(b)\n";
    code += "end\n";
  }

  // Get the value of a table's starting offset.
  void GetStartOfTable(const StructDef &struct_def, std::string *code_ptr) {
    std::string &code = *code_ptr;
    code += struct_def.name + "Start";
    code += "(b::FlatBuffers.Builder) = FlatBuffers.startobject!(b, ";
    code += NumToString(struct_def.fields.vec.size());
    code += ")\n";
  }

  // Set the value of a table's field.
  void BuildFieldOfTable(const StructDef &struct_def, const FieldDef &field,
                         const size_t offset, std::string *code_ptr) {
    std::string &code = *code_ptr;
    code += struct_def.name + "Add" + MakeCamel(field.name);
    code += "(b::FlatBuffers.Builder, ";
    code += GoIdentity(field.name);

    if (!IsScalar(field.value.type.base_type) && (!struct_def.fixed)) {
      code += "::FlatBuffers.UOffsetT) = ";
      code += "FlatBuffers.prependoffsetslot!(b, ";
    } else {
      code += "::" + TypeName(field) + ") = ";
      code += "FlatBuffers.prependslot!(b, ";
    }
    code += NumToString(offset) + ", ";
    code += GoIdentity(field.name) + ", ";
    code += GenConstant(field);
    code += ")\n";
    
  }

  // Set the value of one of the members of a table's vector.
  void BuildVectorOfTable(const StructDef &struct_def, const FieldDef &field,
                          std::string *code_ptr) {
    std::string &code = *code_ptr;
    auto vector_type = field.value.type.VectorType();
    auto alignment = InlineAlignment(vector_type);
    auto elem_size = InlineSize(vector_type);
    code += struct_def.name + "Start";
    code += MakeCamel(field.name);
    code += "Vector(b::FlatBuffers.Builder, numelems::Integer) = ";
    code += "FlatBuffers.startvector!(";
    code += "b, ";
    code +=  NumToString(elem_size);
    code += ", numelems, ";
    code += NumToString(alignment);
    code += + ")\n";
  }

  // Get the offset of the end of a table.
  void GetEndOffsetOnTable(const StructDef &struct_def, std::string *code_ptr) {
    std::string &code = *code_ptr;
    code += struct_def.name + "End";
    code += "(b::FlatBuffers.Builder) = FlatBuffers.endobject!(b)";
  }

  // Generate the receiver for function signatures.
  //void GenReceiver(const StructDef &struct_def, std::string *code_ptr) {
  //  std::string &code = *code_ptr;
  //  code += "func (rcv *" + struct_def.name + ")";
  //}

  void GenPropertyNames(const StructDef &struct_def, std::string *code_ptr){
      std::string &code = *code_ptr;
      code += "Base.propertynames(::" + struct_def.name + ") = (\n";
      for (auto it = struct_def.fields.vec.begin();
         it != struct_def.fields.vec.end(); ++it) {
          auto &field = **it;
          if (field.deprecated) continue;
          code += "\t:" + GoIdentity(field.name) + ",\n";
      }
      code += ")\n\n";
  }

    void GenPropertyNamesAsStruct(const StructDef &struct_def, std::string *code_ptr){
      std::string &code = *code_ptr;
      code += "module " + struct_def.name + "Properties\n";
      code += "abstract type AbstractProperty end\n";

      for (auto it = struct_def.fields.vec.begin();
         it != struct_def.fields.vec.end(); ++it) {
          auto &field = **it;
          if (field.deprecated) continue;
          code += "struct " + GoIdentity(field.name) + " <: AbstractProperty end\n";
      }
      code += "end\n\n";

  }


  void GenGetProperty(const StructDef &struct_def, std::string *code_ptr){
      std::string &code = *code_ptr;
      bool first = true;

      //bool x = true;
      code += "function Base.getproperty(x::" + struct_def.name + ", field::Symbol)\n";
      for (auto it = struct_def.fields.vec.begin();
         it != struct_def.fields.vec.end(); ++it) {
          auto &field = **it;
          if (field.deprecated) continue;

          if (first){
            code += "\tif field === ";
            first = false;
          } else {
            code += "\telseif field === ";
          }
          code += ":" + GoIdentity(field.name) + "\n";

          //GenComment(field.doc_comment, code_ptr, nullptr, "");
          if (IsScalar(field.value.type.base_type)) {
            if (struct_def.fixed) {
              code += "\t\t#GetScalarFieldOfStruct\n";
              GetScalarFieldOfStruct(struct_def, field, code_ptr);
            } else {
              code += "\t\t#GetScalarFieldOfTable\n";
              GetScalarFieldOfTable(struct_def, field, code_ptr);
            }
          } 
          else {
            switch (field.value.type.base_type) {
              case BASE_TYPE_STRUCT:
                if (struct_def.fixed) {
                  code += "\t\t#GetStructFieldOfStruct\n";
                  //GetStructFieldOfStruct(struct_def, field, code_ptr);
                } else {
                  code += "\t\t#GetStructFieldOfTable\n";
                  GetStructFieldOfTable(struct_def, field, code_ptr);
                }
                break;
              case BASE_TYPE_STRING:
                code += "\t\t#GetStringField\n";
                GetStringField(struct_def, field, code_ptr);
                break;
              case BASE_TYPE_VECTOR: {
                auto vectortype = field.value.type.VectorType();
                if (vectortype.base_type == BASE_TYPE_STRUCT) {
                  code += "\t\t#GetMemberOfVectorOfStruct\n";
                } else {
                  code += "\t\t#GetMemberOfVectorOfNonStruct\n";
                }
                GetMemberOfVector(struct_def, field, code_ptr);
                break;
              }
              case BASE_TYPE_UNION: {
                code += "\t\t#GetUnionField\n";
                //GetUnionField(struct_def, field, code_ptr);
                break;
              }
              default: {
                code += "\t\t#FLATBUFFERS_ASSERT\n";
                //FLATBUFFERS_ASSERT(0);
              }
              code += "\n";
            }
          }
      }
      code += "\tend\n";
      code += "\treturn nothing\n";
      code += "end\n\n";
  }

    void GenGetPropertyByNameStruct(const StructDef &struct_def, std::string *code_ptr){
      std::string &code = *code_ptr;

      for (auto it = struct_def.fields.vec.begin();
         it != struct_def.fields.vec.end(); ++it) {
          auto &field = **it;
          if (field.deprecated) continue;

          code += "function Base.getindex(x::" + struct_def.name + ", ::Type{" +struct_def.name + "Properties." +  GoIdentity(field.name) + "})\n";
          
          //GenComment(field.doc_comment, code_ptr, nullptr, "");
          if (IsScalar(field.value.type.base_type)) {
            if (struct_def.fixed) {
              code += "\t\t#GetScalarFieldOfStruct\n";
              GetScalarFieldOfStruct(struct_def, field, code_ptr);
            } else {
              code += "\t\t#GetScalarFieldOfTable\n";
              GetScalarFieldOfTable(struct_def, field, code_ptr);
            }
          } 
          else {
            switch (field.value.type.base_type) {
              case BASE_TYPE_STRUCT:
                if (struct_def.fixed) {
                  code += "\t\t#GetStructFieldOfStruct\n";
                  //GetStructFieldOfStruct(struct_def, field, code_ptr);
                } else {
                  code += "\t\t#GetStructFieldOfTable\n";
                  GetStructFieldOfTable(struct_def, field, code_ptr);
                }
                break;
              case BASE_TYPE_STRING:
                code += "\t\t#GetStringField\n";
                GetStringField(struct_def, field, code_ptr);
                break;
              case BASE_TYPE_VECTOR: {
                auto vectortype = field.value.type.VectorType();
                if (vectortype.base_type == BASE_TYPE_STRUCT) {
                  code += "\t\t#GetMemberOfVectorOfStruct\n";
                } else {
                  code += "\t\t#GetMemberOfVectorOfNonStruct\n";
                }
                GetMemberOfVector(struct_def, field, code_ptr);
                break;
              }
              case BASE_TYPE_UNION: {
                code += "\t\t#GetUnionField\n";
                //GetUnionField(struct_def, field, code_ptr);
                break;
              }
              default: {
                code += "\t\t#FLATBUFFERS_ASSERT\n";
                //FLATBUFFERS_ASSERT(0);
              }
            }
            code += "\t\treturn nothing\n";
          }
          code += "end\n\n";

      }
      //code += "\tend\n";
      //code += "\treturn nothing\n";
      code += "\n";

  }



  // Generate table constructors, conditioned on its members' types.
  void GenTableBuilders(const StructDef &struct_def, std::string *code_ptr) {
    GetStartOfTable(struct_def, code_ptr);

    for (auto it = struct_def.fields.vec.begin();
         it != struct_def.fields.vec.end(); ++it) {
      auto &field = **it;
      if (field.deprecated) continue;

      auto offset = it - struct_def.fields.vec.begin();
      BuildFieldOfTable(struct_def, field, offset, code_ptr);
      if (IsVector(field.value.type)) {
        BuildVectorOfTable(struct_def, field, code_ptr);
      }
    }

    GetEndOffsetOnTable(struct_def, code_ptr);
  }

  // Generate struct or table methods.
  void GenStruct(const StructDef &struct_def, std::string *code_ptr) {
    if (struct_def.generated) return;

    cur_name_space_ = struct_def.defined_namespace;

    GenComment(struct_def.doc_comment, code_ptr, nullptr);
    BeginClass(struct_def, code_ptr);

    GenPropertyNames(struct_def, code_ptr);
    GenGetProperty(struct_def, code_ptr);

    GenPropertyNamesAsStruct(struct_def, code_ptr);
    GenGetPropertyByNameStruct(struct_def, code_ptr);

    // Generate builders
    if (struct_def.fixed) {
      // create a struct constructor function
      GenStructBuilder(struct_def, code_ptr);
    } else {
      // Create a set of functions that allow table construction.
      GenTableBuilders(struct_def, code_ptr);
    }
  }


  // Generate enum declarations.
  void GenEnum(const EnumDef &enum_def, std::string *code_ptr) {
    if (enum_def.generated) return;

    auto max_name_length = MaxNameLength(enum_def);
    cur_name_space_ = enum_def.defined_namespace;

    // GenComment(enum_def.doc_comment, code_ptr, nullptr);
    BeginEnum(enum_def, code_ptr);
    for (auto it = enum_def.Vals().begin(); it != enum_def.Vals().end(); ++it) {
      const EnumVal &ev = **it;
      EnumMember(enum_def, ev, max_name_length, code_ptr);
    }
    EndEnum(code_ptr);
  }

  std::string GenTypeBasic(const Type &type) {
    // clang-format off
    static const char *ctypename[] = {
      #define FLATBUFFERS_TD(ENUM, IDLTYPE, CTYPE, JTYPE, GTYPE, ...) \
        #GTYPE,
        FLATBUFFERS_GEN_TYPES(FLATBUFFERS_TD)
      #undef FLATBUFFERS_TD
    };
    // clang-format on
    return ctypename[type.base_type];
  }

  std::string GenTypePointer(const Type &type) {
    switch (type.base_type) {
      case BASE_TYPE_STRING: return "[]byte";
      case BASE_TYPE_VECTOR: return GenTypeGet(type.VectorType());
      case BASE_TYPE_STRUCT: return WrapInNameSpaceAndTrack(*type.struct_def);
      case BASE_TYPE_UNION:
        // fall through
      default: return "*flatbuffers.Table";
    }
  }

  std::string GenTypeGet(const Type &type) {
    if (type.enum_def != nullptr) { return GetEnumTypeName(*type.enum_def); }
    return IsScalar(type.base_type) ? GenTypeBasic(type) : GenTypePointer(type);
  }

  std::string TypeName(const FieldDef &field) {
    return GenTypeGet(field.value.type);
  }

  // If type is an enum, returns value with a cast to the enum type, otherwise
  // returns value as-is.
  std::string CastToEnum(const Type &type, std::string value) {
    if (type.enum_def == nullptr) {
      return value;
    } else {
      return GenTypeGet(type) + "(" + value + ")";
    }
  }

  // If type is an enum, returns value with a cast to the enum base type,
  // otherwise returns value as-is.
  std::string CastToBaseType(const Type &type, std::string value) {
    if (type.enum_def == nullptr) {
      return value;
    } else {
      return GenTypeBasic(type) + "(" + value + ")";
    }
  }

  std::string GenConstant(const FieldDef &field) {
    switch (field.value.type.base_type) {
      case BASE_TYPE_BOOL:
        return field.value.constant == "0" ? "false" : "true";
      default: return field.value.constant;
    }
  }

  // Create a struct with a builder and the struct's arguments.
  void GenStructBuilder(const StructDef &struct_def, std::string *code_ptr) {
    BeginBuilderArgs(struct_def, code_ptr);
    StructBuilderArgs(struct_def, "", code_ptr);
    EndBuilderArgs(code_ptr);
    StructBuilderBody(struct_def, "", code_ptr);
    EndBuilderBody(code_ptr);
  }
  // Begin by declaring namespace and imports.
  void BeginFile(const std::string &name_space_name, const bool needs_imports,
                 const bool is_enum, std::string *code_ptr) {
    std::string &code = *code_ptr;
    code += "# Code generated by the FlatBuffers compiler. DO NOT EDIT.\n\n";
    code += "using Arrow: FlatBuffers\n\n";
  }

  // Save out the generated code for a Go Table type.
  bool SaveType(const Definition &def, const std::string &classcode,
                const bool needs_imports, const bool is_enum) {
    if (!classcode.length()) return true;

    Namespace &ns = go_namespace_.components.empty() ? *def.defined_namespace
                                                     : go_namespace_;
    std::string code = "";
    BeginFile(LastNamespacePart(ns), needs_imports, is_enum, &code);
    code += classcode;
    // Strip extra newlines at end of file to make it gofmt-clean.
    while (code.length() > 2 && code.substr(code.length() - 2) == "\n\n") {
      code.pop_back();
    }
    std::string filename = NamespaceDir(ns) + def.name + ".jl";
    return SaveFile(filename.c_str(), code, false);
  }

  // Create the full name of the imported namespace (format: A__B__C).
  std::string NamespaceImportName(const Namespace *ns) {
    std::string s = "";
    for (auto it = ns->components.begin(); it != ns->components.end(); ++it) {
      if (s.size() == 0) {
        s += *it;
      } else {
        s += "__" + *it;
      }
    }
    return s;
  }

  // Create the full path for the imported namespace (format: A/B/C).
  std::string NamespaceImportPath(const Namespace *ns) {
    std::string s = "";
    for (auto it = ns->components.begin(); it != ns->components.end(); ++it) {
      if (s.size() == 0) {
        s += *it;
      } else {
        s += "/" + *it;
      }
    }
    return s;
  }

  // Ensure that a type is prefixed with its go package import name if it is
  // used outside of its namespace.
  std::string WrapInNameSpaceAndTrack(const Namespace *ns,
                                      const std::string &name) {
    if (CurrentNameSpace() == ns) return name;

    tracked_imported_namespaces_.insert(ns);

    std::string import_name = NamespaceImportName(ns);
    return import_name + "." + name;
  }

  std::string WrapInNameSpaceAndTrack(const Definition &def) {
    return WrapInNameSpaceAndTrack(def.defined_namespace, def.name);
  }

  const Namespace *CurrentNameSpace() const { return cur_name_space_; }

  static size_t MaxNameLength(const EnumDef &enum_def) {
    size_t max = 0;
    for (auto it = enum_def.Vals().begin(); it != enum_def.Vals().end(); ++it) {
      max = std::max((*it)->name.length(), max);
    }
    return max;
  }
};
}  // namespace go

bool GenerateGo(const Parser &parser, const std::string &path,
                const std::string &file_name) {
  go::GoGenerator generator(parser, path, file_name, parser.opts.go_namespace);
  return generator.generate();
}

}  // namespace flatbuffers
