/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-json.cpp
 * @brief Minimal JSON value, parser and serializer for the frontend protocol.
 *
 * A small self-contained implementation (no third-party dependency) that is
 * strict about the input it accepts: bounded nesting depth, valid UTF-8 only
 * (invalid bytes become U+FFFD), and escaped control characters on output.
 */

#include "app/engine/dmn-json.hpp"

#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dmn::gui {
namespace {

constexpr int kMaximumDepth{32};
constexpr std::string_view kReplacement{"\xEF\xBF\xBD"};

void appendUtf8(std::string &out, const std::uint32_t codePoint) {
  if (codePoint < 0x80) {
    out.push_back(static_cast<char>(codePoint));
  } else if (codePoint < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
    out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
  } else if (codePoint < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
    out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
    out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
  }
}

// Returns the byte length of the valid UTF-8 sequence at `pos`, or 0.
auto utf8SequenceLength(std::string_view text,
                        const std::size_t pos) -> std::size_t {
  const auto byte { [&](const std::size_t index) {
    return static_cast<unsigned char>(text[index]);
  }};

  const auto lead{byte(pos)};
  std::size_t length{0};
  std::uint32_t minimum{0};
  std::uint32_t codePoint{0};
  if (lead < 0x80) {
    return 1;
  }

  if (lead >= 0xC2 && lead <= 0xDF) {
    length = 2;
    minimum = 0x80;
    codePoint = lead & 0x1F;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    length = 3;
    minimum = 0x800;
    codePoint = lead & 0x0F;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    length = 4;
    minimum = 0x10000;
    codePoint = lead & 0x07;
  } else {
    return 0;
  }

  if (pos + length > text.size()) {
    return 0;
  }

  for (std::size_t i = 1; i < length; ++i) {
    if ((byte(pos + i) & 0xC0) != 0x80) {
      return 0;
    }

    codePoint = (codePoint << 6) | (byte(pos + i) & 0x3F);
  }

  if (codePoint < minimum || codePoint > 0x10FFFF ||
      (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
    return 0;
  }

  return length;
}

void dumpString(std::string &out, std::string_view text) {
  out.push_back('"');
  for (std::size_t pos = 0; pos < text.size();) {
    const auto byte{static_cast<unsigned char>(text[pos])};
    if (byte == '"' || byte == '\\') {
      out.push_back('\\');
      out.push_back(static_cast<char>(byte));
      ++pos;
    } else if (byte == '\n') {
      out += "\\n";
      ++pos;
    } else if (byte == '\r') {
      out += "\\r";
      ++pos;
    } else if (byte == '\t') {
      out += "\\t";
      ++pos;
    } else if (byte < 0x20 || byte == 0x7F) {
      std::array<char, 8> escaped{};
      std::snprintf(escaped.data(), escaped.size(), "\\u%04x", byte);
      out += escaped.data();
      ++pos;
    } else {
      const auto length{utf8SequenceLength(text, pos)};
      if (length == 0) {
        out += kReplacement;
        ++pos;
      } else {
        out.append(text.substr(pos, length));
        pos += length;
      }
    }
  }

  out.push_back('"');
}

void dumpNumber(std::string &out, const double value) {
  if (!std::isfinite(value)) {
    out += "null";

    return;
  }

  std::array<char, 32> buffer{};
  if (value == std::floor(value) && std::fabs(value) < 9.0e15) {
    std::snprintf(buffer.data(), buffer.size(), "%lld",
                  static_cast<long long>(value));
  } else {
    std::snprintf(buffer.data(), buffer.size(), "%.17g", value);
  }

  out += buffer.data();
}

class Parser {
public:
  explicit Parser(std::string_view text) : m_text{text} {}

  auto parseDocument() -> std::optional<Dmn_JsonValue> {
    auto value{parseValue(0)};
    skipSpace();
    if (!value || m_pos != m_text.size()) {
      return std::nullopt;
    }

    return value;
  }

private:
  void skipSpace() {
    while (m_pos < m_text.size() &&
           (m_text[m_pos] == ' ' || m_text[m_pos] == '\t' ||
            m_text[m_pos] == '\n' || m_text[m_pos] == '\r')) {
      ++m_pos;
    }
  }

  auto consume(const char expected) -> bool {
    skipSpace();
    if (m_pos < m_text.size() && m_text[m_pos] == expected) {
      ++m_pos;

      return true;
    }

    return false;
  }

  auto literal(std::string_view word) -> bool {
    if (m_text.substr(m_pos, word.size()) == word) {
      m_pos += word.size();

      return true;
    }

    return false;
  }

  auto parseValue(const int depth) -> std::optional<Dmn_JsonValue> {
    if (depth > kMaximumDepth) {
      return std::nullopt;
    }

    skipSpace();
    if (m_pos >= m_text.size()) {
      return std::nullopt;
    }

    const char current{m_text[m_pos]};
    if (current == '{') {
      return parseObject(depth);
    }

    if (current == '[') {
      return parseArray(depth);
    }

    if (current == '"') {
      auto text{parseString()};
      if (!text) {
        return std::nullopt;
      }

      return Dmn_JsonValue{std::move(*text)};
    }

    if (literal("true")) {
      return Dmn_JsonValue{true};
    }

    if (literal("false")) {
      return Dmn_JsonValue{false};
    }

    if (literal("null")) {
      return Dmn_JsonValue{};
    }

    return parseNumber();
  }

  auto parseNumber() -> std::optional<Dmn_JsonValue> {
    const auto start{m_pos};
    if (m_pos < m_text.size() && m_text[m_pos] == '-') {
      ++m_pos;
    }

    while (m_pos < m_text.size() &&
           (std::isdigit(static_cast<unsigned char>(m_text[m_pos])) != 0 ||
            m_text[m_pos] == '.' || m_text[m_pos] == 'e' ||
            m_text[m_pos] == 'E' || m_text[m_pos] == '+' ||
            m_text[m_pos] == '-')) {
      ++m_pos;
    }

    if (m_pos == start) {
      return std::nullopt;
    }

    const std::string token{m_text.substr(start, m_pos - start)};
    char *end = nullptr;
    const double value{std::strtod(token.c_str(), &end)};
    if (end != token.c_str() + token.size() || !std::isfinite(value)) {
      return std::nullopt;
    }

    return Dmn_JsonValue{value};
  }

  auto parseHex4() -> std::optional<std::uint32_t> {
    if (m_pos + 4 > m_text.size()) {
      return std::nullopt;
    }

    std::uint32_t value{0};
    for (int i = 0; i < 4; ++i) {
      const char digit{m_text[m_pos++]};
      value <<= 4;
      if (digit >= '0' && digit <= '9') {
        value |= static_cast<std::uint32_t>(digit - '0');
      } else if (digit >= 'a' && digit <= 'f') {
        value |= static_cast<std::uint32_t>(digit - 'a' + 10);
      } else if (digit >= 'A' && digit <= 'F') {
        value |= static_cast<std::uint32_t>(digit - 'A' + 10);
      } else {
        return std::nullopt;
      }
    }

    return value;
  }

  auto parseString() -> std::optional<std::string> {
    if (!consume('"')) {
      return std::nullopt;
    }

    std::string out;
    while (m_pos < m_text.size()) {
      const auto byte{static_cast<unsigned char>(m_text[m_pos])};
      if (byte == '"') {
        ++m_pos;

        return out;
      }

      if (byte < 0x20) {
        return std::nullopt;
      }

      if (byte != '\\') {
        const auto length{utf8SequenceLength(m_text, m_pos)};
        if (length == 0) {
          return std::nullopt;
        }

        out.append(m_text.substr(m_pos, length));
        m_pos += length;
        continue;
      }

      ++m_pos;
      if (m_pos >= m_text.size()) {
        return std::nullopt;
      }

      const char escape{m_text[m_pos++]};
      switch (escape) {
      case '"':
      case '\\':
      case '/':
        out.push_back(escape);
        break;
      case 'b':
        out.push_back('\b');
        break;
      case 'f':
        out.push_back('\f');
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      case 'u': {
        auto high{parseHex4()};
        if (!high) {
          return std::nullopt;
        }

        std::uint32_t codePoint{*high};
        if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
          if (!literal("\\u")) {
            return std::nullopt;
          }

          auto low{parseHex4()};
          if (!low || *low < 0xDC00 || *low > 0xDFFF) {
            return std::nullopt;
          }

          codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (*low - 0xDC00);
        } else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) {
          return std::nullopt;
        }

        appendUtf8(out, codePoint);
        break;
      }
      default:
        return std::nullopt;
      }
    }

    return std::nullopt;
  }

  auto parseArray(const int depth) -> std::optional<Dmn_JsonValue> {
    consume('[');
    auto array{Dmn_JsonValue::array()};
    if (consume(']')) {
      return array;
    }

    do {
      auto item{parseValue(depth + 1)};
      if (!item) {
        return std::nullopt;
      }

      array.push(std::move(*item));
    } while (consume(','));

    if (!consume(']')) {
      return std::nullopt;
    }

    return array;
  }

  auto parseObject(const int depth) -> std::optional<Dmn_JsonValue> {
    consume('{');
    auto object{Dmn_JsonValue::object()};
    if (consume('}')) {
      return object;
    }

    do {
      skipSpace();
      auto key{parseString()};
      if (!key || !consume(':')) {
        return std::nullopt;
      }

      auto value{parseValue(depth + 1)};
      if (!value) {
        return std::nullopt;
      }

      object.set(std::move(*key), std::move(*value));
    } while (consume(','));

    if (!consume('}')) {
      return std::nullopt;
    }

    return object;
  }

  std::string_view m_text;
  std::size_t m_pos{};
};

} // namespace

auto Dmn_JsonValue::parse(std::string_view text)
    -> std::optional<Dmn_JsonValue> {
  return Parser{text}.parseDocument();
}

auto Dmn_JsonValue::dump() const -> std::string {
  std::string out;
  if (const auto *flag = std::get_if<bool>(&m_value); flag != nullptr) {
    out = *flag ? "true" : "false";
  } else if (const auto *number = std::get_if<double>(&m_value);
             number != nullptr) {
    dumpNumber(out, *number);
  } else if (const auto *text = std::get_if<std::string>(&m_value);
             text != nullptr) {
    dumpString(out, *text);
  } else if (const auto *array = std::get_if<Array>(&m_value);
             array != nullptr) {
    out.push_back('[');
    for (std::size_t i = 0; i < array->size(); ++i) {
      out += (i == 0 ? "" : ",");
      out += (*array)[i].dump();
    }

    out.push_back(']');
  } else if (const auto *object = std::get_if<Object>(&m_value);
             object != nullptr) {
    out.push_back('{');
    bool first{true};
    for (const auto &[key, member] : *object) {
      out += (first ? "" : ",");
      first = false;
      dumpString(out, key);
      out.push_back(':');
      out += member.dump();
    }

    out.push_back('}');
  } else {
    out = "null";
  }

  return out;
}

auto Dmn_JsonValue::set(std::string key,
                        Dmn_JsonValue value) -> Dmn_JsonValue & {
  if (auto *object = std::get_if<Object>(&m_value); object != nullptr) {
    for (auto &member : *object) {
      if (member.first == key) {
        member.second = std::move(value);

        return *this;
      }
    }

    object->emplace_back(std::move(key), std::move(value));
  }

  return *this;
}

auto Dmn_JsonValue::push(Dmn_JsonValue value) -> Dmn_JsonValue & {
  if (auto *array = std::get_if<Array>(&m_value); array != nullptr) {
    array->push_back(std::move(value));
  }

  return *this;
}

auto Dmn_JsonValue::find(std::string_view key) const -> const Dmn_JsonValue * {
  if (const auto *object = std::get_if<Object>(&m_value); object != nullptr) {
    for (const auto &member : *object) {
      if (member.first == key) {
        return &member.second;
      }
    }
  }

  return nullptr;
}

auto Dmn_JsonValue::items() const -> const Array & {
  static const Array kEmpty;
  if (const auto *array = std::get_if<Array>(&m_value); array != nullptr) {
    return *array;
  }

  return kEmpty;
}

auto Dmn_JsonValue::stringOr(std::string fallback) const -> std::string {
  if (const auto *text = std::get_if<std::string>(&m_value); text != nullptr) {
    return *text;
  }

  return fallback;
}

auto Dmn_JsonValue::boolOr(const bool fallback) const -> bool {
  if (const auto *flag = std::get_if<bool>(&m_value); flag != nullptr) {
    return *flag;
  }

  return fallback;
}

auto Dmn_JsonValue::numberOr(const double fallback) const -> double {
  if (const auto *number = std::get_if<double>(&m_value); number != nullptr) {
    return *number;
  }

  return fallback;
}

auto Dmn_JsonValue::getString(std::string_view key,
                              std::string fallback) const -> std::string {
  const auto *member = find(key);

  return member == nullptr ? fallback : member->stringOr(std::move(fallback));
}

auto Dmn_JsonValue::getBool(std::string_view key,
                            const bool fallback) const -> bool {
  const auto *member = find(key);

  return member == nullptr ? fallback : member->boolOr(fallback);
}

auto Dmn_JsonValue::getNumber(std::string_view key,
                              const double fallback) const -> double {
  const auto *member = find(key);

  return member == nullptr ? fallback : member->numberOr(fallback);
}

} // namespace dmn::gui
