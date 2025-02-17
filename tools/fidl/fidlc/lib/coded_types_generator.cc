// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fidl/coded_types_generator.h"

#include "fidl/coded_ast.h"
#include "fidl/flat/values.h"
#include "fidl/names.h"
#include "fidl/types.h"

namespace fidl {

coded::MemcpyCompatibility ComputeMemcpyCompatibility(const flat::Type* type) {
  auto typeshape = type->typeshape(fidl::WireFormat::kV1NoEe);
  if (typeshape.MaxOutOfLine() == 0 && typeshape.MaxHandles() == 0 &&
      !typeshape.HasFlexibleEnvelope() && !typeshape.HasPadding()) {
    return coded::MemcpyCompatibility::kCanMemcpy;
  }
  return coded::MemcpyCompatibility::kCannotMemcpy;
}

CodedTypesGenerator::FlattenedStructMember::FlattenedStructMember(const flat::StructMember& member)
    : type(flat::GetType(member.type_ctor)),
      name(member.name),
      inline_size_v1(member.typeshape(WireFormat::kV1NoEe).InlineSize()),
      inline_size_v2(member.typeshape(WireFormat::kV2).InlineSize()),
      offset_v1(member.fieldshape(WireFormat::kV1NoEe).Offset()),
      offset_v2(member.fieldshape(WireFormat::kV2).Offset()),
      padding(member.fieldshape(WireFormat::kV1NoEe).Padding()) {
  assert(padding == member.fieldshape(WireFormat::kV2).Padding());
}

std::vector<CodedTypesGenerator::FlattenedStructMember> CodedTypesGenerator::FlattenedStructMembers(
    const flat::Struct& input) {
  auto get_struct_decl = [](const flat::StructMember& member) -> const flat::Struct* {
    if (flat::GetType(member.type_ctor)->nullability == types::Nullability::kNullable) {
      return nullptr;
    }
    const flat::Type* type = flat::GetType(member.type_ctor);
    if (type->kind != flat::Type::Kind::kIdentifier) {
      return nullptr;
    }
    auto identifier_type = static_cast<const flat::IdentifierType*>(type);
    if (identifier_type->type_decl->kind != flat::Decl::Kind::kStruct) {
      return nullptr;
    }
    return static_cast<const flat::Struct*>(identifier_type->type_decl);
  };

  std::vector<FlattenedStructMember> result;
  for (const auto& member : input.members) {
    auto flattened_member = FlattenedStructMember(member);
    auto struct_decl = get_struct_decl(member);
    if (!struct_decl) {
      result.push_back(flattened_member);
      continue;
    }
    if (struct_decl->members.size() == 0) {
      result.push_back(flattened_member);
      continue;
    }
    auto flattened_members = FlattenedStructMembers(*struct_decl);
    for (size_t i = 0; i < flattened_members.size(); i++) {
      auto inner_member = flattened_members[i];
      if (i == flattened_members.size() - 1) {
        inner_member.padding += flattened_member.padding;
      }
      inner_member.offset_v1 += flattened_member.offset_v1;
      inner_member.offset_v2 += flattened_member.offset_v2;
      result.push_back(inner_member);
    }
  }
  return result;
}

std::vector<const coded::Type*> CodedTypesGenerator::AllCodedTypes() const {
  std::vector<const coded::Type*> coded_types;
  coded_types.reserve(coded_types_.size() + named_coded_types_.size());

  for (const auto& coded_type : coded_types_) {
    assert(coded_type.get());
    if (!coded_type->is_coding_needed)
      continue;

    coded_types.push_back(coded_type.get());
  }

  for (const auto& [_, coded_type] : named_coded_types_) {
    assert(coded_type.get());

    coded_types.push_back(coded_type.get());
  }

  return coded_types;
}

const coded::Type* CodedTypesGenerator::CompileType(const flat::Type* type,
                                                    coded::CodingContext context) {
  switch (type->kind) {
    case flat::Type::Kind::kArray: {
      auto array_type = static_cast<const flat::ArrayType*>(type);
      auto coded_element_type =
          CompileType(array_type->element_type, coded::CodingContext::kOutsideEnvelope);

      auto iter = array_type_map_.find(array_type);
      if (iter != array_type_map_.end())
        return iter->second;

      uint32_t array_size_v1 = array_type->typeshape(WireFormat::kV1NoEe).InlineSize();
      uint32_t array_size_v2 = array_type->typeshape(WireFormat::kV2).InlineSize();
      uint32_t element_size_v1 =
          array_type->element_type->typeshape(WireFormat::kV1NoEe).InlineSize();
      uint32_t element_size_v2 = array_type->element_type->typeshape(WireFormat::kV2).InlineSize();
      auto name = NameCodedArray(coded_element_type->coded_name, array_size_v1);
      auto coded_array_type = std::make_unique<coded::ArrayType>(
          std::move(name), coded_element_type, array_size_v1, array_size_v2, element_size_v1,
          element_size_v2, context);
      array_type_map_[array_type] = coded_array_type.get();
      coded_types_.push_back(std::move(coded_array_type));
      return coded_types_.back().get();
    }
    case flat::Type::Kind::kVector: {
      auto vector_type = static_cast<const flat::VectorType*>(type);
      auto iter = vector_type_map_.find(vector_type);
      if (iter != vector_type_map_.end())
        return iter->second;
      auto coded_element_type =
          CompileType(vector_type->element_type, coded::CodingContext::kOutsideEnvelope);
      uint32_t max_count = vector_type->element_count->value;
      uint32_t element_size_v1 = coded_element_type->size_v1;
      uint32_t element_size_v2 = coded_element_type->size_v2;
      std::string_view element_name = coded_element_type->coded_name;
      auto name = NameCodedVector(element_name, max_count, vector_type->nullability);
      auto coded_vector_type = std::make_unique<coded::VectorType>(
          std::move(name), coded_element_type, max_count, element_size_v1, element_size_v2,
          vector_type->nullability, ComputeMemcpyCompatibility(vector_type->element_type));
      vector_type_map_[vector_type] = coded_vector_type.get();
      coded_types_.push_back(std::move(coded_vector_type));
      return coded_types_.back().get();
    }
    case flat::Type::Kind::kString: {
      auto string_type = static_cast<const flat::StringType*>(type);
      auto iter = string_type_map_.find(string_type);
      if (iter != string_type_map_.end())
        return iter->second;
      uint32_t max_size = string_type->max_size->value;
      auto name = NameCodedString(max_size, string_type->nullability);
      auto coded_string_type =
          std::make_unique<coded::StringType>(std::move(name), max_size, string_type->nullability);
      string_type_map_[string_type] = coded_string_type.get();
      coded_types_.push_back(std::move(coded_string_type));
      return coded_types_.back().get();
    }
    case flat::Type::Kind::kHandle: {
      auto handle_type = static_cast<const flat::HandleType*>(type);
      auto iter = handle_type_map_.find(handle_type);
      if (iter != handle_type_map_.end())
        return iter->second;
      types::RightsWrappedType rights =
          *static_cast<const flat::HandleRights*>(handle_type->rights);
      auto name = NameCodedHandle(handle_type->subtype, rights, handle_type->nullability);
      auto coded_handle_type = std::make_unique<coded::HandleType>(
          std::move(name), handle_type->subtype, rights, handle_type->nullability);
      handle_type_map_[handle_type] = coded_handle_type.get();
      coded_types_.push_back(std::move(coded_handle_type));
      return coded_types_.back().get();
    }
    case flat::Type::Kind::kRequestHandle: {
      auto request_type = static_cast<const flat::RequestHandleType*>(type);
      auto iter = request_type_map_.find(request_type);
      if (iter != request_type_map_.end())
        return iter->second;
      auto name = NameCodedRequestHandle(NameCodedName(request_type->protocol_type->name),
                                         request_type->nullability);
      auto coded_request_type =
          std::make_unique<coded::RequestHandleType>(std::move(name), request_type->nullability);
      request_type_map_[request_type] = coded_request_type.get();
      coded_types_.push_back(std::move(coded_request_type));
      return coded_types_.back().get();
    }
    case flat::Type::Kind::kTransportSide: {
      auto channel_end = static_cast<const flat::TransportSideType*>(type);
      auto iter = channel_end_map_.find(channel_end);
      if (iter != channel_end_map_.end())
        return iter->second;
      // TODO(fxbug.dev/70186): This is where the separate handling of the old
      // vs new syntax channel ends, ends (i.e. in the coded types backend they
      // are represented using the same types). As we clean up the old
      // representation in fidlc, we'll want to switch the coded AST to better
      // match the new representation (client/server end rather than protocol/request).
      if (channel_end->end == flat::TransportSide::kClient) {
        // In the old syntax this would be represented as an identifier type of a
        // protocol decl, so the code in this if statement is copied from the
        // kIdentifier > kProtocol code path below in order to maintain the same
        // behavior.
        auto name = NameCodedProtocolHandle(NameCodedName(channel_end->protocol_decl->name),
                                            channel_end->nullability);
        auto coded_protocol_type =
            std::make_unique<coded::ProtocolHandleType>(std::move(name), channel_end->nullability);
        channel_end_map_[channel_end] = coded_protocol_type.get();
        coded_types_.push_back(std::move(coded_protocol_type));
        return coded_types_.back().get();
      } else {
        // In the old syntax this would be represented as a RequestType,
        // so the code in this if statement is copied from the
        // kRequestHandle code path below in order to maintain the same
        // behavior.
        auto name = NameCodedRequestHandle(NameCodedName(channel_end->protocol_decl->name),
                                           channel_end->nullability);
        auto coded_request_type =
            std::make_unique<coded::RequestHandleType>(std::move(name), channel_end->nullability);
        channel_end_map_[channel_end] = coded_request_type.get();
        coded_types_.push_back(std::move(coded_request_type));
        return coded_types_.back().get();
      }
    }
    case flat::Type::Kind::kPrimitive: {
      auto primitive_type = static_cast<const flat::PrimitiveType*>(type);
      auto iter = primitive_type_map_.find(primitive_type);
      if (iter != primitive_type_map_.end())
        return iter->second;
      auto name = NameFlatName(primitive_type->name);
      auto coded_primitive_type = std::make_unique<coded::PrimitiveType>(
          std::move(name), primitive_type->subtype,
          primitive_type->typeshape(WireFormat::kV1NoEe).InlineSize(), context);
      primitive_type_map_[primitive_type] = coded_primitive_type.get();
      coded_types_.push_back(std::move(coded_primitive_type));
      return coded_types_.back().get();
    }
    case flat::Type::Kind::kIdentifier: {
      auto identifier_type = static_cast<const flat::IdentifierType*>(type);
      auto iter = named_coded_types_.find(identifier_type->name);
      if (iter == named_coded_types_.end()) {
        assert(false && "unknown type in named type map!");
      }
      // We may need to set the emit-pointer bit on structs, unions, and xunions now.
      auto coded_type = iter->second.get();
      switch (coded_type->kind) {
        case coded::Type::Kind::kStruct: {
          // Structs were compiled as part of decl compilation,
          // but we may now need to generate the StructPointer.
          if (identifier_type->nullability != types::Nullability::kNullable)
            return coded_type;
          auto iter = struct_type_map_.find(identifier_type);
          if (iter != struct_type_map_.end()) {
            return iter->second;
          }
          auto coded_struct_type = static_cast<coded::StructType*>(coded_type);
          auto struct_pointer_type = std::make_unique<coded::StructPointerType>(
              NamePointer(coded_struct_type->coded_name), coded_struct_type);
          coded_struct_type->maybe_reference_type = struct_pointer_type.get();
          struct_type_map_[identifier_type] = struct_pointer_type.get();
          coded_types_.push_back(std::move(struct_pointer_type));
          return coded_types_.back().get();
        }
        case coded::Type::Kind::kTable: {
          // Tables cannot be nullable.
          assert(identifier_type->nullability != types::Nullability::kNullable);
          return coded_type;
        }
        case coded::Type::Kind::kXUnion: {
          if (identifier_type->nullability != types::Nullability::kNullable) {
            return coded_type;
          }
          auto coded_xunion_type = static_cast<coded::XUnionType*>(coded_type);
          return coded_xunion_type->maybe_reference_type;
        }
        case coded::Type::Kind::kProtocol: {
          auto iter = protocol_type_map_.find(identifier_type);
          if (iter != protocol_type_map_.end())
            return iter->second;
          auto name = NameCodedProtocolHandle(NameCodedName(identifier_type->name),
                                              identifier_type->nullability);
          auto coded_protocol_type = std::make_unique<coded::ProtocolHandleType>(
              std::move(name), identifier_type->nullability);
          protocol_type_map_[identifier_type] = coded_protocol_type.get();
          coded_types_.push_back(std::move(coded_protocol_type));
          return coded_types_.back().get();
        }
        case coded::Type::Kind::kEnum:
        case coded::Type::Kind::kBits:
          return coded_type;
        case coded::Type::Kind::kPrimitive:
        case coded::Type::Kind::kProtocolHandle:
        case coded::Type::Kind::kStructPointer:
        case coded::Type::Kind::kMessage:
        case coded::Type::Kind::kRequestHandle:
        case coded::Type::Kind::kHandle:
        case coded::Type::Kind::kArray:
        case coded::Type::Kind::kVector:
        case coded::Type::Kind::kString:
          assert(false && "anonymous type in named type map!");
          break;
      }
      __builtin_unreachable();
    }
    case flat::Type::Kind::kBox:
      // this defers to the code path for a nullable struct identifier type.
      return CompileType(static_cast<const flat::BoxType*>(type)->boxed_type, context);
  }
}

void CodedTypesGenerator::CompileFields(const flat::Decl* decl) {
  switch (decl->kind) {
    case flat::Decl::Kind::kProtocol: {
      auto protocol_decl = static_cast<const flat::Protocol*>(decl);
      coded::ProtocolType* coded_protocol =
          static_cast<coded::ProtocolType*>(named_coded_types_[decl->name].get());
      size_t i = 0;
      for (const auto& method_with_info : protocol_decl->all_methods) {
        assert(method_with_info.method != nullptr);
        const auto& method = *method_with_info.method;
        auto CompileMessage = [&](const flat::Struct* message) -> void {
          std::unique_ptr<coded::MessageType>& coded_message =
              coded_protocol->messages_during_compile[i++];
          std::vector<coded::StructElement>& request_elements = coded_message->elements;
          uint32_t field_num = 0;
          bool is_noop = true;

          // TODO(fxbug.dev/76316): remove this assert once this generator is able to
          //  properly handle empty structs as payloads.
          assert((!message || (message && !message->members.empty())) &&
                 "cannot process empty message payloads");

          if (message) {
            for (const auto& parameter : FlattenedStructMembers(*message)) {
              auto coded_parameter_type =
                  CompileType(parameter.type, coded::CodingContext::kOutsideEnvelope);
              if (!coded_parameter_type->is_noop) {
                request_elements.push_back(
                    coded::StructField(parameter.type->Resourceness(), parameter.offset_v1,
                                       parameter.offset_v2, coded_parameter_type));
                is_noop = false;
              }
              if (parameter.padding != 0) {
                request_elements.push_back(coded::StructPadding::FromLength(
                    parameter.inline_size_v1 + parameter.offset_v1,
                    parameter.inline_size_v2 + parameter.offset_v2, parameter.padding));
                is_noop = false;
              }
              field_num++;
            }
          }

          coded_message->is_noop = is_noop;
          // We move the coded_message to coded_types_ so that we'll generate tables for the
          // message in the proper order.
          coded_types_.push_back(std::move(coded_message));
          // We also keep back pointers to reference to these messages via the
          // coded_protocol.
          coded_protocol->messages_after_compile.push_back(
              static_cast<const coded::MessageType*>(coded_types_.back().get()));
        };
        if (method.has_request) {
          CompileMessage(method.maybe_request_payload);
        }
        if (method.has_response) {
          CompileMessage(method.maybe_response_payload);
        }
      }
      break;
    }
    case flat::Decl::Kind::kStruct: {
      auto struct_decl = static_cast<const flat::Struct*>(decl);
      if (struct_decl->is_request_or_response)
        break;
      coded::StructType* coded_struct =
          static_cast<coded::StructType*>(named_coded_types_[decl->name].get());
      std::vector<coded::StructElement>& struct_elements = coded_struct->elements;
      uint32_t field_num = 0;
      bool is_noop = true;
      for (const auto& member : FlattenedStructMembers(*struct_decl)) {
        std::string member_name = coded_struct->coded_name + "_" + std::string(member.name.data());
        auto coded_member_type = CompileType(member.type, coded::CodingContext::kOutsideEnvelope);
        if (!coded_member_type->is_noop) {
          struct_elements.push_back(coded::StructField(
              member.type->Resourceness(), member.offset_v1, member.offset_v2, coded_member_type));
          is_noop = false;
        }
        if (member.padding != 0) {
          struct_elements.push_back(coded::StructPadding::FromLength(
              member.inline_size_v1 + member.offset_v1, member.inline_size_v2 + member.offset_v2,
              member.padding));
          is_noop = false;
        }
        field_num++;
      }
      coded_struct->is_noop = is_noop;
      break;
    }
    case flat::Decl::Kind::kUnion: {
      auto union_decl = static_cast<const flat::Union*>(decl);
      auto type = named_coded_types_[decl->name].get();
      coded::XUnionType* coded_xunion = static_cast<coded::XUnionType*>(type);
      coded::XUnionType* nullable_coded_xunion = coded_xunion->maybe_reference_type;
      assert(nullable_coded_xunion != nullptr && "Named coded xunion must have a reference type!");
      assert(coded_xunion->fields.empty() && "The coded xunion fields are being compiled twice!");

      std::set<uint32_t> members;
      for (const auto& member_ref : union_decl->MembersSortedByXUnionOrdinal()) {
        const auto& member = member_ref.get();
        if (!members.emplace(member.ordinal->value).second) {
          assert(false && "Duplicate ordinal found in table generation");
        }
        if (member.maybe_used) {
          const auto* coded_member_type = CompileType(flat::GetType(member.maybe_used->type_ctor),
                                                      coded::CodingContext::kInsideEnvelope);
          coded_xunion->fields.emplace_back(coded_member_type);
          nullable_coded_xunion->fields.emplace_back(coded_member_type);
        } else {
          coded_xunion->fields.emplace_back(nullptr);
          nullable_coded_xunion->fields.emplace_back(nullptr);
        }
      }
      break;
    }
    case flat::Decl::Kind::kTable: {
      auto table_decl = static_cast<const flat::Table*>(decl);
      coded::TableType* coded_table =
          static_cast<coded::TableType*>(named_coded_types_[decl->name].get());
      std::vector<coded::TableField>& table_fields = coded_table->fields;
      std::map<uint32_t, const flat::Table::Member*> members;
      for (const auto& member : table_decl->members) {
        if (!members.emplace(member.ordinal->value, &member).second) {
          assert(false && "Duplicate ordinal found in table generation");
        }
      }
      for (const auto& member_pair : members) {
        const auto& member = *member_pair.second;
        if (!member.maybe_used)
          continue;
        std::string member_name =
            coded_table->coded_name + "_" + std::string(member.maybe_used->name.data());
        auto coded_member_type = CompileType(flat::GetType(member.maybe_used->type_ctor),
                                             coded::CodingContext::kInsideEnvelope);
        table_fields.emplace_back(coded_member_type, member.ordinal->value);
      }
      break;
    }
    default: {
      break;
    }
  }
}

void CodedTypesGenerator::CompileDecl(const flat::Decl* decl) {
  switch (decl->kind) {
    case flat::Decl::Kind::kBits: {
      auto bits_decl = static_cast<const flat::Bits*>(decl);
      std::string bits_name = NameCodedName(bits_decl->name);
      auto primitive_type =
          static_cast<const flat::PrimitiveType*>(flat::GetType(bits_decl->subtype_ctor));
      named_coded_types_.emplace(
          bits_decl->name,
          std::make_unique<coded::BitsType>(
              std::move(bits_name), primitive_type->subtype,
              primitive_type->typeshape(WireFormat::kV1NoEe).InlineSize(), bits_decl->mask,
              NameFlatName(bits_decl->name), bits_decl->strictness));
      break;
    }
    case flat::Decl::Kind::kEnum: {
      auto enum_decl = static_cast<const flat::Enum*>(decl);
      std::string enum_name = NameCodedName(enum_decl->name);
      std::vector<uint64_t> members;
      for (const auto& member : enum_decl->members) {
        std::unique_ptr<flat::ConstantValue> value;
        uint64_t uint64 = 0;
        bool ok = member.value->Value().Convert(flat::ConstantValue::Kind::kUint64, &value);
        if (ok) {
          uint64 = static_cast<flat::NumericConstantValue<uint64_t>*>(value.get())->value;
        } else {
          ok = member.value->Value().Convert(flat::ConstantValue::Kind::kInt64, &value);
          if (ok) {
            // Note: casting int64_t to uint64_t is well-defined.
            uint64 = static_cast<uint64_t>(
                static_cast<flat::NumericConstantValue<int64_t>*>(value.get())->value);
          } else {
            assert(false && "Failed to convert enum member to uint64 or int64");
          }
        }
        members.push_back(uint64);
      }
      named_coded_types_.emplace(
          enum_decl->name,
          std::make_unique<coded::EnumType>(
              std::move(enum_name), enum_decl->type->subtype,
              enum_decl->type->typeshape(WireFormat::kV1NoEe).InlineSize(), std::move(members),
              NameFlatName(enum_decl->name), enum_decl->strictness));
      break;
    }
    case flat::Decl::Kind::kProtocol: {
      auto protocol_decl = static_cast<const flat::Protocol*>(decl);
      std::string protocol_name = NameCodedName(protocol_decl->name);
      std::string protocol_qname = NameFlatName(protocol_decl->name);
      std::vector<std::unique_ptr<coded::MessageType>> protocol_messages;
      for (const auto& method_with_info : protocol_decl->all_methods) {
        assert(method_with_info.method != nullptr);
        const auto& method = *method_with_info.method;
        std::string method_name = NameMethod(protocol_name, method);
        std::string method_qname = NameMethod(protocol_qname, method);
        auto CreateMessage = [&](const flat::Struct* message, types::MessageKind kind) -> void {
          std::string message_name = NameMessage(method_name, kind);
          std::string message_qname = NameMessage(method_qname, kind);

          // TODO(fxbug.dev/76316): remove this assert once this generator is
          //  able to properly handle empty structs as payloads.
          assert((!message || (message && !message->members.empty())) &&
                 "cannot process empty message payloads");

          auto typeshape_v1 = message != nullptr ? message->typeshape(WireFormat::kV1NoEe)
                                                 : TypeShape::ForEmptyPayload();
          auto typeshape_v2 = message != nullptr ? message->typeshape(WireFormat::kV2)
                                                 : TypeShape::ForEmptyPayload();
          protocol_messages.push_back(std::make_unique<coded::MessageType>(
              std::move(message_name), std::vector<coded::StructElement>(),
              typeshape_v1.InlineSize(), typeshape_v2.InlineSize(), std::move(message_qname)));
        };
        if (method.has_request) {
          CreateMessage(method.maybe_request_payload, types::MessageKind::kRequest);
        }
        if (method.has_response) {
          auto kind =
              method.has_request ? types::MessageKind::kResponse : types::MessageKind::kEvent;
          CreateMessage(method.maybe_response_payload, kind);
        }
      }
      named_coded_types_.emplace(
          decl->name, std::make_unique<coded::ProtocolType>(std::move(protocol_messages)));
      break;
    }
    case flat::Decl::Kind::kTable: {
      auto table_decl = static_cast<const flat::Table*>(decl);
      std::string table_name = NameCodedName(table_decl->name);
      named_coded_types_.emplace(decl->name,
                                 std::make_unique<coded::TableType>(
                                     std::move(table_name), std::vector<coded::TableField>(),
                                     NameFlatName(table_decl->name), table_decl->resourceness));
      break;
    }
    case flat::Decl::Kind::kStruct: {
      auto struct_decl = static_cast<const flat::Struct*>(decl);
      if (struct_decl->is_request_or_response)
        break;
      std::string struct_name = NameCodedName(struct_decl->name);
      named_coded_types_.emplace(decl->name,
                                 std::make_unique<coded::StructType>(
                                     std::move(struct_name), std::vector<coded::StructElement>(),
                                     struct_decl->typeshape(WireFormat::kV1NoEe).InlineSize(),
                                     struct_decl->typeshape(WireFormat::kV2).InlineSize(),
                                     NameFlatName(struct_decl->name)));
      break;
    }
    case flat::Decl::Kind::kUnion: {
      auto union_decl = static_cast<const flat::Union*>(decl);
      std::string union_name = NameCodedName(union_decl->name);
      std::string nullable_xunion_name = NameCodedNullableName(union_decl->name);

      // Always create the reference type
      auto nullable_xunion_type = std::make_unique<coded::XUnionType>(
          std::move(nullable_xunion_name), std::vector<coded::XUnionField>(),
          NameFlatName(union_decl->name), types::Nullability::kNullable, union_decl->strictness,
          union_decl->resourceness.value());
      coded::XUnionType* nullable_xunion_ptr = nullable_xunion_type.get();
      coded_types_.push_back(std::move(nullable_xunion_type));

      auto xunion_type = std::make_unique<coded::XUnionType>(
          std::move(union_name), std::vector<coded::XUnionField>(), NameFlatName(union_decl->name),
          types::Nullability::kNonnullable, union_decl->strictness,
          union_decl->resourceness.value());
      xunion_type->maybe_reference_type = nullable_xunion_ptr;
      named_coded_types_.emplace(decl->name, std::move(xunion_type));
      break;
    }
    case flat::Decl::Kind::kConst:
    case flat::Decl::Kind::kResource:
    case flat::Decl::Kind::kService:
    case flat::Decl::Kind::kTypeAlias:
      // Nothing to do.
      break;
  }
}

void CodedTypesGenerator::CompileCodedTypes() {
  for (const auto& decl : library_->declaration_order_) {
    CompileDecl(decl);
  }

  for (const auto& decl : library_->declaration_order_) {
    if (decl->name.library() != library_)
      continue;
    CompileFields(decl);
  }
}

}  // namespace fidl
