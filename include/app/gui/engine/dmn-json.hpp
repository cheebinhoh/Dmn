/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-json.hpp
 * @brief Minimal JSON value, parser and serializer for the frontend protocol.
 *
 * Overview
 * --------
 * The engine exchanges one UTF-8 JSON object per message with its frontend.
 * This value type covers exactly that need: null, booleans, numbers, strings,
 * arrays and insertion-ordered objects. Parsing is bounded in nesting depth and
 * serialization replaces invalid UTF-8 so untrusted notification text can never
 * produce an invalid document.
 */

#ifndef DMN_BLUETOOTH_GUI_ENGINE_JSON_HPP_
#define DMN_BLUETOOTH_GUI_ENGINE_JSON_HPP_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace dmn::gui {

/**
 * @brief A JSON document node.
 */
class Dmn_JsonValue {
public:
  using Array = std::vector<Dmn_JsonValue>;
  using Member = std::pair<std::string, Dmn_JsonValue>;
  using Object = std::vector<Member>;

  Dmn_JsonValue() = default;
  Dmn_JsonValue(std::nullptr_t) {}
  Dmn_JsonValue(const bool value) : m_value{value} {}
  Dmn_JsonValue(const int value) : m_value{static_cast<double>(value)} {}
  Dmn_JsonValue(const std::int64_t value)
      : m_value{static_cast<double>(value)} {}
  Dmn_JsonValue(const std::uint64_t value)
      : m_value{static_cast<double>(value)} {}
  Dmn_JsonValue(const double value) : m_value{value} {}
  Dmn_JsonValue(std::string value) : m_value{std::move(value)} {}
  Dmn_JsonValue(const char *value) : m_value{std::string{value}} {}
  Dmn_JsonValue(Array value) : m_value{std::move(value)} {}
  Dmn_JsonValue(Object value) : m_value{std::move(value)} {}

  [[nodiscard]] static auto object() -> Dmn_JsonValue {
    return Dmn_JsonValue{Object{}};
  }

  [[nodiscard]] static auto array() -> Dmn_JsonValue {
    return Dmn_JsonValue{Array{}};
  }

  /**
   * @brief Parses a complete document; returns nullopt on any syntax error,
   *        trailing data, or nesting deeper than the supported limit.
   */
  [[nodiscard]] static auto
  parse(std::string_view text) -> std::optional<Dmn_JsonValue>;

  /**
   * @brief Serializes compactly as valid UTF-8 JSON.
   */
  [[nodiscard]] auto dump() const -> std::string;

  [[nodiscard]] auto isObject() const -> bool {
    return std::holds_alternative<Object>(m_value);
  }

  [[nodiscard]] auto isArray() const -> bool {
    return std::holds_alternative<Array>(m_value);
  }

  [[nodiscard]] auto isNull() const -> bool {
    return std::holds_alternative<std::monostate>(m_value);
  }

  /**
   * @brief Appends or replaces an object member; no-op for non-objects.
   */
  auto set(std::string key, Dmn_JsonValue value) -> Dmn_JsonValue &;

  /**
   * @brief Appends to an array; no-op for non-arrays.
   */
  auto push(Dmn_JsonValue value) -> Dmn_JsonValue &;

  [[nodiscard]] auto find(std::string_view key) const -> const Dmn_JsonValue *;

  [[nodiscard]] auto items() const -> const Array &;

  [[nodiscard]] auto stringOr(std::string fallback = {}) const -> std::string;

  [[nodiscard]] auto boolOr(bool fallback) const -> bool;

  [[nodiscard]] auto numberOr(double fallback) const -> double;

  [[nodiscard]] auto getString(std::string_view key,
                               std::string fallback = {}) const -> std::string;

  [[nodiscard]] auto getBool(std::string_view key, bool fallback) const -> bool;

  [[nodiscard]] auto getNumber(std::string_view key,
                               double fallback) const -> double;

private:
  std::variant<std::monostate, bool, double, std::string, Array, Object>
      m_value;
};

} // namespace dmn::gui

#endif // DMN_BLUETOOTH_GUI_ENGINE_JSON_HPP_
