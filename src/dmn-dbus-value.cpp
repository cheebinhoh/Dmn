/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-dbus-value.cpp
 * @brief Value construction and signature support for the D-Bus client.
 */

#include "dmn-dbus-value.hpp"

#include <stdexcept>
#include <type_traits>

namespace dmn {

Dmn_DbusValue::Dmn_DbusValue(Storage storage) : m_storage{std::move(storage)} {}

Dmn_DbusValue::Dmn_DbusValue(bool value) : m_storage{value} {}

Dmn_DbusValue::Dmn_DbusValue(std::int16_t value) : m_storage{value} {}

Dmn_DbusValue::Dmn_DbusValue(std::uint16_t value) : m_storage{value} {}

Dmn_DbusValue::Dmn_DbusValue(std::int32_t value) : m_storage{value} {}

Dmn_DbusValue::Dmn_DbusValue(std::uint32_t value) : m_storage{value} {}

Dmn_DbusValue::Dmn_DbusValue(std::int64_t value) : m_storage{value} {}

Dmn_DbusValue::Dmn_DbusValue(std::uint64_t value) : m_storage{value} {}

Dmn_DbusValue::Dmn_DbusValue(double value) : m_storage{value} {}

Dmn_DbusValue::Dmn_DbusValue(std::uint8_t value) : m_storage{value} {}

Dmn_DbusValue::Dmn_DbusValue(std::string value) : m_storage{std::move(value)} {}

Dmn_DbusValue::Dmn_DbusValue(const char *value) {
  if (value == nullptr) {
    throw std::invalid_argument("Dmn_DbusValue: string must not be null");
  }

  m_storage = std::string{value};
}

auto Dmn_DbusValue::objectPath(std::string value) -> Dmn_DbusValue {
  return Dmn_DbusValue{ObjectPath{std::move(value)}};
}

auto Dmn_DbusValue::signatureValue(std::string value) -> Dmn_DbusValue {
  return Dmn_DbusValue{SignatureValue{std::move(value)}};
}

auto Dmn_DbusValue::byteArray(ByteArray value) -> Dmn_DbusValue {
  return Dmn_DbusValue{std::move(value)};
}

auto Dmn_DbusValue::array(std::string elementSignature,
                          std::vector<Dmn_DbusValue> values) -> Dmn_DbusValue {
  return Dmn_DbusValue{std::make_shared<const ArrayData>(
      ArrayData{std::move(elementSignature), std::move(values)})};
}

auto Dmn_DbusValue::dictionary(
    std::string keySignature, std::string valueSignature,
    std::vector<std::pair<Dmn_DbusValue, Dmn_DbusValue>> entries)
    -> Dmn_DbusValue {
  return Dmn_DbusValue{std::make_shared<const DictionaryData>(DictionaryData{
      std::move(keySignature), std::move(valueSignature), std::move(entries)})};
}

auto Dmn_DbusValue::structure(std::vector<Dmn_DbusValue> fields)
    -> Dmn_DbusValue {
  return Dmn_DbusValue{
      std::make_shared<const StructData>(StructData{std::move(fields)})};
}

auto Dmn_DbusValue::variant(Dmn_DbusValue value) -> Dmn_DbusValue {
  return Dmn_DbusValue{std::make_shared<const VariantData>(
      VariantData{std::make_shared<const Dmn_DbusValue>(std::move(value))})};
}

auto Dmn_DbusValue::signature() const -> std::string {
  return std::visit(
      [](const auto &value) -> std::string {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return {};
        } else if constexpr (std::is_same_v<T, bool>) {
          return "b";
        } else if constexpr (std::is_same_v<T, std::int16_t>) {
          return "n";
        } else if constexpr (std::is_same_v<T, std::uint16_t>) {
          return "q";
        } else if constexpr (std::is_same_v<T, std::int32_t>) {
          return "i";
        } else if constexpr (std::is_same_v<T, std::uint32_t>) {
          return "u";
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
          return "x";
        } else if constexpr (std::is_same_v<T, std::uint64_t>) {
          return "t";
        } else if constexpr (std::is_same_v<T, double>) {
          return "d";
        } else if constexpr (std::is_same_v<T, std::uint8_t>) {
          return "y";
        } else if constexpr (std::is_same_v<T, std::string>) {
          return "s";
        } else if constexpr (std::is_same_v<T, ObjectPath>) {
          return "o";
        } else if constexpr (std::is_same_v<T, SignatureValue>) {
          return "g";
        } else if constexpr (std::is_same_v<T, ByteArray>) {
          return "ay";
        } else if constexpr (std::is_same_v<T,
                                            std::shared_ptr<const ArrayData>>) {
          return value == nullptr ? std::string{}
                                  : "a" + value->element_signature;
        } else if constexpr (std::is_same_v<
                                 T, std::shared_ptr<const DictionaryData>>) {
          return value == nullptr ? std::string{}
                                  : "a{" + value->key_signature +
                                        value->value_signature + "}";
        } else if constexpr (std::is_same_v<
                                 T, std::shared_ptr<const StructData>>) {
          if (value == nullptr) {
            return {};
          }

          std::string signature{"("};
          for (const auto &field : value->fields) {
            signature += field.signature();
          }

          signature += ')';
          return signature;
        } else {
          return "v";
        }
      },
      m_storage);
}

auto Dmn_DbusValue::asBool() const -> bool { return std::get<bool>(m_storage); }

auto Dmn_DbusValue::asInt16() const -> std::int16_t {
  return std::get<std::int16_t>(m_storage);
}

auto Dmn_DbusValue::asUint16() const -> std::uint16_t {
  return std::get<std::uint16_t>(m_storage);
}

auto Dmn_DbusValue::asInt32() const -> std::int32_t {
  return std::get<std::int32_t>(m_storage);
}

auto Dmn_DbusValue::asUint32() const -> std::uint32_t {
  return std::get<std::uint32_t>(m_storage);
}

auto Dmn_DbusValue::asInt64() const -> std::int64_t {
  return std::get<std::int64_t>(m_storage);
}

auto Dmn_DbusValue::asUint64() const -> std::uint64_t {
  return std::get<std::uint64_t>(m_storage);
}

auto Dmn_DbusValue::asDouble() const -> double {
  return std::get<double>(m_storage);
}

auto Dmn_DbusValue::asByte() const -> std::uint8_t {
  return std::get<std::uint8_t>(m_storage);
}

auto Dmn_DbusValue::asString() const -> const std::string & {
  return std::get<std::string>(m_storage);
}

auto Dmn_DbusValue::asObjectPath() const -> const std::string & {
  return std::get<ObjectPath>(m_storage).value;
}

auto Dmn_DbusValue::asSignatureValue() const -> const std::string & {
  return std::get<SignatureValue>(m_storage).value;
}

auto Dmn_DbusValue::asByteArray() const -> const ByteArray & {
  return std::get<ByteArray>(m_storage);
}

auto Dmn_DbusValue::asArray() const -> const ArrayData & {
  return *std::get<std::shared_ptr<const ArrayData>>(m_storage);
}

auto Dmn_DbusValue::asDictionary() const -> const DictionaryData & {
  return *std::get<std::shared_ptr<const DictionaryData>>(m_storage);
}

auto Dmn_DbusValue::asStruct() const -> const StructData & {
  return *std::get<std::shared_ptr<const StructData>>(m_storage);
}

auto Dmn_DbusValue::asVariant() const -> const Dmn_DbusValue & {
  return *std::get<std::shared_ptr<const VariantData>>(m_storage)->value;
}

} // namespace dmn
