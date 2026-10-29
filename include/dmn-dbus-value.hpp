/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-dbus-value.hpp
 * @brief Owned, type-preserving values for the D-Bus client.
 */

#ifndef DMN_DBUS_VALUE_HPP_
#define DMN_DBUS_VALUE_HPP_

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace dmn {

/**
 * @brief An owned D-Bus value with its wire type preserved.
 *
 * Container factories require their element/key/value signatures explicitly,
 * which preserves the type of empty arrays and dictionaries. Container access
 * is const-only; copying a value is safe and shares immutable container
 * storage.
 *
 * This is the data model at the generic D-Bus client boundary: client methods
 * accept and return these values, and the private client codec translates
 * them to and from libdbus messages. Higher-level clients such as
 * Dmn_BluezClient interpret the typed values as service-specific properties
 * and events. This class is transport-independent; it does not connect to a
 * bus, encode messages, or assign BlueZ meaning to a value.
 */
class Dmn_DbusValue final {
public:
  /**
   * @brief Owned storage for a D-Bus byte array (`ay`).
   */
  using ByteArray = std::vector<std::uint8_t>;

  struct ObjectPath {
    /**
     * @brief The validated D-Bus object-path text.
     */
    std::string value;
  };

  struct SignatureValue {
    /**
     * @brief The D-Bus type-signature text.
     */
    std::string value;
  };

  struct ArrayData {
    /**
     * @brief Signature of each element, retained for empty arrays.
     */
    std::string element_signature;

    /**
     * @brief Array elements in their original order.
     */
    std::vector<Dmn_DbusValue> values;
  };

  struct DictionaryData {
    /**
     * @brief Signature of each dictionary key.
     */
    std::string key_signature;

    /**
     * @brief Signature of each dictionary value.
     */
    std::string value_signature;

    /**
     * @brief Key/value entries in their original order.
     */
    std::vector<std::pair<Dmn_DbusValue, Dmn_DbusValue>> entries;
  };

  struct StructData {
    /**
     * @brief Structure fields in their original order.
     */
    std::vector<Dmn_DbusValue> fields;
  };

  struct VariantData {
    /**
     * @brief The one value wrapped by this D-Bus variant.
     */
    std::shared_ptr<const Dmn_DbusValue> value;
  };

  using Storage = std::variant<
      std::monostate, bool, std::int16_t, std::uint16_t, std::int32_t,
      std::uint32_t, std::int64_t, std::uint64_t, double, std::uint8_t,
      std::string, ObjectPath, SignatureValue, ByteArray,
      std::shared_ptr<const ArrayData>, std::shared_ptr<const DictionaryData>,
      std::shared_ptr<const StructData>, std::shared_ptr<const VariantData>>;

  /**
   * @brief Construct an empty sentinel; it is not a wire-encodable value.
   */
  Dmn_DbusValue() = default;

  /**
   * @brief Copy a value; immutable container storage may be shared.
   */
  Dmn_DbusValue(const Dmn_DbusValue &) = default;

  /**
   * @brief Copy-assign a value; immutable container storage may be shared.
   */
  auto operator=(const Dmn_DbusValue &) -> Dmn_DbusValue & = default;

  /**
   * @brief Move a value and its owned storage.
   */
  Dmn_DbusValue(Dmn_DbusValue &&) noexcept = default;

  /**
   * @brief Move-assign a value and its owned storage.
   */
  auto operator=(Dmn_DbusValue &&) noexcept -> Dmn_DbusValue & = default;

  /**
   * @brief Destroy the value and release its owned/shared storage.
   */
  virtual ~Dmn_DbusValue() = default;

  /**
   * @brief Construct a D-Bus boolean value.
   */
  Dmn_DbusValue(bool value);

  /**
   * @brief Construct a signed 16-bit D-Bus integer.
   */
  Dmn_DbusValue(std::int16_t value);

  /**
   * @brief Construct an unsigned 16-bit D-Bus integer.
   */
  Dmn_DbusValue(std::uint16_t value);

  /**
   * @brief Construct a signed 32-bit D-Bus integer.
   */
  Dmn_DbusValue(std::int32_t value);

  /**
   * @brief Construct an unsigned 32-bit D-Bus integer.
   */
  Dmn_DbusValue(std::uint32_t value);

  /**
   * @brief Construct a signed 64-bit D-Bus integer.
   */
  Dmn_DbusValue(std::int64_t value);

  /**
   * @brief Construct an unsigned 64-bit D-Bus integer.
   */
  Dmn_DbusValue(std::uint64_t value);

  /**
   * @brief Construct a D-Bus double value.
   */
  Dmn_DbusValue(double value);

  /**
   * @brief Construct a D-Bus byte value.
   */
  Dmn_DbusValue(std::uint8_t value);

  /**
   * @brief Construct an owned D-Bus string value.
   */
  Dmn_DbusValue(std::string value);

  /**
   * @brief Construct an owned D-Bus string value from a C string.
   */
  Dmn_DbusValue(const char *value);

  /**
   * @brief Construct a value with the D-Bus object-path type (`o`).
   *
   * @param value Object path text.
   * @return The typed object-path value.
   */
  [[nodiscard]] static auto objectPath(std::string value) -> Dmn_DbusValue;

  /**
   * @brief Construct a value with the D-Bus signature type (`g`).
   *
   * @param value Signature text.
   * @return The typed signature value.
   */
  [[nodiscard]] static auto signatureValue(std::string value) -> Dmn_DbusValue;

  /**
   * @brief Construct a D-Bus byte array (`ay`).
   *
   * @param value Owned bytes.
   * @return The byte-array value.
   */
  [[nodiscard]] static auto byteArray(ByteArray value) -> Dmn_DbusValue;

  /**
   * @brief Construct an array with an explicit element signature.
   *
   * @param elementSignature D-Bus signature of every element.
   * @param values Array contents; may be empty.
   * @return The typed array value.
   */
  [[nodiscard]] static auto
  array(std::string elementSignature,
        std::vector<Dmn_DbusValue> values) -> Dmn_DbusValue;

  /**
   * @brief Construct a dictionary with explicit key and value signatures.
   *
   * @param keySignature D-Bus basic type of each key.
   * @param valueSignature D-Bus signature of each value.
   * @param entries Ordered dictionary entries; may be empty.
   * @return The typed dictionary value.
   */
  [[nodiscard]] static auto
  dictionary(std::string keySignature, std::string valueSignature,
             std::vector<std::pair<Dmn_DbusValue, Dmn_DbusValue>> entries)
      -> Dmn_DbusValue;

  /**
   * @brief Construct a D-Bus structure with ordered fields.
   *
   * @param fields Structure contents.
   * @return The structure value.
   */
  [[nodiscard]] static auto
  structure(std::vector<Dmn_DbusValue> fields) -> Dmn_DbusValue;

  /**
   * @brief Wrap one value in a D-Bus variant (`v`).
   *
   * @param value Owned value to wrap.
   * @return The variant value.
   */
  [[nodiscard]] static auto variant(Dmn_DbusValue value) -> Dmn_DbusValue;

  /**
   * @brief Return this value's complete D-Bus wire signature.
   */
  [[nodiscard]] auto signature() const -> std::string;

  /**
   * @brief Read the value as a boolean.
   * @throws std::bad_variant_access If this value is not a boolean.
   */
  [[nodiscard]] auto asBool() const -> bool;

  /**
   * @brief Read a signed 16-bit value; throws on a type mismatch.
   */
  [[nodiscard]] auto asInt16() const -> std::int16_t;

  /**
   * @brief Read an unsigned 16-bit value; throws on a type mismatch.
   */
  [[nodiscard]] auto asUint16() const -> std::uint16_t;

  /**
   * @brief Read a signed 32-bit value; throws on a type mismatch.
   */
  [[nodiscard]] auto asInt32() const -> std::int32_t;

  /**
   * @brief Read an unsigned 32-bit value; throws on a type mismatch.
   */
  [[nodiscard]] auto asUint32() const -> std::uint32_t;

  /**
   * @brief Read a signed 64-bit value; throws on a type mismatch.
   */
  [[nodiscard]] auto asInt64() const -> std::int64_t;

  /**
   * @brief Read an unsigned 64-bit value; throws on a type mismatch.
   */
  [[nodiscard]] auto asUint64() const -> std::uint64_t;

  /**
   * @brief Read a double value; throws on a type mismatch.
   */
  [[nodiscard]] auto asDouble() const -> double;

  /**
   * @brief Read a byte value; throws on a type mismatch.
   */
  [[nodiscard]] auto asByte() const -> std::uint8_t;

  /**
   * @brief Read a string by reference; throws on a type mismatch.
   */
  [[nodiscard]] auto asString() const -> const std::string &;

  /**
   * @brief Read object-path text by reference; throws on a type mismatch.
   */
  [[nodiscard]] auto asObjectPath() const -> const std::string &;

  /**
   * @brief Read signature text by reference; throws on a type mismatch.
   */
  [[nodiscard]] auto asSignatureValue() const -> const std::string &;

  /**
   * @brief Read bytes by reference; throws on a type mismatch.
   */
  [[nodiscard]] auto asByteArray() const -> const ByteArray &;

  /**
   * @brief Read array contents by reference; throws on a type mismatch.
   */
  [[nodiscard]] auto asArray() const -> const ArrayData &;

  /**
   * @brief Read dictionary contents by reference; throws on a type mismatch.
   */
  [[nodiscard]] auto asDictionary() const -> const DictionaryData &;

  /**
   * @brief Read structure fields by reference; throws on a type mismatch.
   */
  [[nodiscard]] auto asStruct() const -> const StructData &;

  /**
   * @brief Read the value contained by a variant.
   *
   * @return Reference to the contained value, valid while this value lives.
   * @throws std::bad_variant_access If this value is not a variant.
   */
  [[nodiscard]] auto asVariant() const -> const Dmn_DbusValue &;

private:
  explicit Dmn_DbusValue(Storage storage);

  Storage m_storage{};
};

} // namespace dmn

#endif // DMN_DBUS_VALUE_HPP_
