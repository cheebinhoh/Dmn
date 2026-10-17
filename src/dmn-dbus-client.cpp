/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-dbus-client.cpp
 * @brief Optional generic D-Bus client implementation.
 */

#include "dmn-dbus-client.hpp"

#include "dmn-dbus-runtime.hpp"

#include <dbus/dbus.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace dmn {
namespace {

struct ConnectionDeleter {
  void operator()(DBusConnection *connection) const noexcept {
    if (connection != nullptr) {
      if (dbus_connection_get_is_connected(connection)) {
        dbus_connection_close(connection);
      }

      dbus_connection_unref(connection);
    }
  }
};

using ConnectionPtr = std::unique_ptr<DBusConnection, ConnectionDeleter>;

struct MessageDeleter {
  void operator()(DBusMessage *message) const noexcept {
    if (message != nullptr) {
      dbus_message_unref(message);
    }
  }
};

using MessagePtr = std::unique_ptr<DBusMessage, MessageDeleter>;

class ErrorGuard {
public:
  ErrorGuard() { dbus_error_init(&m_error); }

  ~ErrorGuard() { dbus_error_free(&m_error); }

  ErrorGuard(const ErrorGuard &) = delete;

  auto operator=(const ErrorGuard &) -> ErrorGuard & = delete;

  ErrorGuard(ErrorGuard &&) = delete;

  auto operator=(ErrorGuard &&) -> ErrorGuard & = delete;

  auto get() -> DBusError * { return &m_error; }

  auto value() const -> const DBusError & { return m_error; }

private:
  DBusError m_error{};
};

auto makeError(Dmn_DbusErrorKind kind, std::string message,
               std::string remote = {}) -> Dmn_DbusError {
  return Dmn_DbusError{kind, std::move(message), std::move(remote)};
}

auto signatureTypeLength(std::string_view signature) -> std::size_t {
  if (signature.empty()) {
    return 0;
  }

  std::size_t index{};

  if (signature[index] == 'a') {
    return 1 + signatureTypeLength(signature.substr(1));
  }

  if (signature[index] == '(' || signature[index] == '{') {
    const char close = signature[index] == '(' ? ')' : '}';
    ++index;

    while (index < signature.size() && signature[index] != close) {
      const auto length = signatureTypeLength(signature.substr(index));

      if (length == 0) {
        return 0;
      }

      index += length;
    }

    return index < signature.size() ? index + 1 : 0;
  }

  return 1;
}

auto validSignature(std::string_view signature) -> bool {
  if (signature.empty() || signature.size() > DBUS_MAXIMUM_SIGNATURE_LENGTH ||
      signature.find('\0') != std::string_view::npos) {
    return false;
  }

  ErrorGuard error;
  const std::string owned{signature};

  return dbus_signature_validate_single(owned.c_str(), error.get());
}

auto validText(const std::string &text) -> bool {
  if (text.find('\0') != std::string::npos) {
    return false;
  }

  ErrorGuard error;

  return dbus_validate_utf8(text.c_str(), error.get());
}

auto validObjectPath(const std::string &path) -> bool {
  if (path.find('\0') != std::string::npos) {
    return false;
  }

  ErrorGuard error;

  return dbus_validate_path(path.c_str(), error.get());
}

auto containsEmbeddedNul(std::string_view value) -> bool {
  return value.find('\0') != std::string_view::npos;
}

auto validSignatureValue(const std::string &signature) -> bool {
  if (signature.find('\0') != std::string::npos) {
    return false;
  }

  ErrorGuard error;

  return dbus_signature_validate(signature.c_str(), error.get());
}

auto messageSize(DBusMessage *message) -> std::size_t {
  char *bytes{};
  int length{};

  if (!dbus_message_marshal(message, &bytes, &length)) {
    throw std::runtime_error("unable to determine encoded D-Bus message size");
  }

  dbus_free(bytes);

  return static_cast<std::size_t>(length);
}

struct CodecLimits {
  std::size_t max_bytes;
  std::size_t max_depth;
  std::size_t max_values;
};

void encodeValue(DBusMessageIter &iter, const Dmn_DbusValue &value,
                 const CodecLimits &limits, std::size_t depth,
                 std::size_t &count);

void encodeContainer(DBusMessageIter &parent, int type, const char *signature,
                     const std::vector<Dmn_DbusValue> &values,
                     const CodecLimits &limits, std::size_t depth,
                     std::size_t &count) {
  DBusMessageIter child;

  if (!dbus_message_iter_open_container(&parent, type, signature, &child)) {
    throw std::runtime_error("unable to open D-Bus container");
  }

  for (const auto &value : values) {
    encodeValue(child, value, limits, depth + 1, count);
  }

  if (!dbus_message_iter_close_container(&parent, &child)) {
    throw std::runtime_error("unable to close D-Bus container");
  }
}

template <typename T>
void appendBasic(DBusMessageIter &iter, int type, const T &value) {
  if (!dbus_message_iter_append_basic(&iter, type, &value)) {
    throw std::runtime_error("unable to encode D-Bus value");
  }
}

void encodeValue(DBusMessageIter &iter, const Dmn_DbusValue &value,
                 const CodecLimits &limits, std::size_t depth,
                 std::size_t &count) {
  if (depth > limits.max_depth || ++count > limits.max_values) {
    throw std::length_error("D-Bus value resource limit exceeded");
  }

  const auto signature = value.signature();

  if (signature.empty()) {
    throw std::invalid_argument("D-Bus value has no wire type");
  }

  if (signature == "b") {
    const dbus_bool_t data = value.asBool() ? TRUE : FALSE;

    appendBasic(iter, DBUS_TYPE_BOOLEAN, data);
  } else if (signature == "n") {
    const dbus_int16_t data = value.asInt16();

    appendBasic(iter, DBUS_TYPE_INT16, data);
  } else if (signature == "q") {
    const dbus_uint16_t data = value.asUint16();

    appendBasic(iter, DBUS_TYPE_UINT16, data);
  } else if (signature == "i") {
    const dbus_int32_t data = value.asInt32();

    appendBasic(iter, DBUS_TYPE_INT32, data);
  } else if (signature == "u") {
    const dbus_uint32_t data = value.asUint32();

    appendBasic(iter, DBUS_TYPE_UINT32, data);
  } else if (signature == "x") {
    const dbus_int64_t data = value.asInt64();

    appendBasic(iter, DBUS_TYPE_INT64, data);
  } else if (signature == "t") {
    const dbus_uint64_t data = value.asUint64();

    appendBasic(iter, DBUS_TYPE_UINT64, data);
  } else if (signature == "d") {
    const double data = value.asDouble();

    appendBasic(iter, DBUS_TYPE_DOUBLE, data);
  } else if (signature == "y") {
    const unsigned char data = value.asByte();

    appendBasic(iter, DBUS_TYPE_BYTE, data);
  } else if (signature == "s" || signature == "o" || signature == "g") {
    const auto &text = signature == "s"   ? value.asString()
                       : signature == "o" ? value.asObjectPath()
                                          : value.asSignatureValue();

    if ((signature == "s" && !validText(text)) ||
        (signature == "o" && !validObjectPath(text)) ||
        (signature == "g" && !validSignatureValue(text))) {
      throw std::invalid_argument("invalid D-Bus string, path, or signature");
    }

    const char *data = text.c_str();
    const int type = signature == "s"   ? DBUS_TYPE_STRING
                     : signature == "o" ? DBUS_TYPE_OBJECT_PATH
                                        : DBUS_TYPE_SIGNATURE;

    appendBasic(iter, type, data);
  } else if (signature == "ay") {
    const auto &bytes = value.asByteArray();

    if (bytes.size() > limits.max_bytes ||
        bytes.size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      throw std::length_error("D-Bus byte array exceeds configured limit");
    }

    DBusMessageIter array;
    if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
                                          DBUS_TYPE_BYTE_AS_STRING, &array)) {
      throw std::runtime_error("unable to encode D-Bus byte array");
    }

    if (!bytes.empty()) {
      const auto *data = bytes.data();
      const int length = static_cast<int>(bytes.size());
      if (!dbus_message_iter_append_fixed_array(&array, DBUS_TYPE_BYTE, &data,
                                                length)) {
        throw std::runtime_error("unable to encode D-Bus byte array");
      }
    }

    if (!dbus_message_iter_close_container(&iter, &array)) {
      throw std::runtime_error("unable to encode D-Bus byte array");
    }
  } else if (signature.front() == 'a' && signature.size() > 2 &&
             signature[1] == '{') {
    const auto &dictionary = value.asDictionary();

    if (!validSignature(dictionary.key_signature) ||
        !validSignature(dictionary.value_signature) ||
        dictionary.key_signature.size() != 1 ||
        std::string_view{"ybnqiuxtdsog"}.find(
            dictionary.key_signature.front()) == std::string_view::npos) {
      throw std::invalid_argument("invalid D-Bus dictionary signature");
    }

    DBusMessageIter array;
    const auto entrySignature =
        "{" + dictionary.key_signature + dictionary.value_signature + "}";

    if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
                                          entrySignature.c_str(), &array)) {
      throw std::runtime_error("unable to encode D-Bus dictionary");
    }

    for (const auto &[key, item] : dictionary.entries) {
      if (key.signature() != dictionary.key_signature ||
          item.signature() != dictionary.value_signature) {
        throw std::invalid_argument("D-Bus dictionary entry type mismatch");
      }

      DBusMessageIter entry;

      if (!dbus_message_iter_open_container(&array, DBUS_TYPE_DICT_ENTRY,
                                            nullptr, &entry)) {
        throw std::runtime_error("unable to encode D-Bus dictionary entry");
      }

      encodeValue(entry, key, limits, depth + 1, count);
      encodeValue(entry, item, limits, depth + 1, count);

      if (!dbus_message_iter_close_container(&array, &entry)) {
        throw std::runtime_error("unable to close D-Bus dictionary entry");
      }
    }

    if (!dbus_message_iter_close_container(&iter, &array)) {
      throw std::runtime_error("unable to close D-Bus dictionary");
    }
  } else if (signature.front() == 'a') {
    const auto &arrayValue = value.asArray();

    if (!validSignature(arrayValue.element_signature)) {
      throw std::invalid_argument("invalid D-Bus array element signature");
    }

    for (const auto &element : arrayValue.values) {
      if (element.signature() != arrayValue.element_signature) {
        throw std::invalid_argument("D-Bus array element type mismatch");
      }
    }

    encodeContainer(iter, DBUS_TYPE_ARRAY, arrayValue.element_signature.c_str(),
                    arrayValue.values, limits, depth, count);
  } else if (signature.front() == '(') {
    const auto &fields = value.asStruct().fields;
    DBusMessageIter structure;

    if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_STRUCT, nullptr,
                                          &structure)) {
      throw std::runtime_error("unable to encode D-Bus struct");
    }

    for (const auto &field : fields) {
      encodeValue(structure, field, limits, depth + 1, count);
    }

    if (!dbus_message_iter_close_container(&iter, &structure)) {
      throw std::runtime_error("unable to close D-Bus struct");
    }
  } else if (signature == "v") {
    const auto &inner = value.asVariant();

    if (!validSignature(inner.signature())) {
      throw std::invalid_argument("invalid D-Bus variant value");
    }

    DBusMessageIter variant;
    const auto contained = inner.signature();

    if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT,
                                          contained.c_str(), &variant)) {
      throw std::runtime_error("unable to encode D-Bus variant");
    }

    encodeValue(variant, inner, limits, depth + 1, count);

    if (!dbus_message_iter_close_container(&iter, &variant)) {
      throw std::runtime_error("unable to close D-Bus variant");
    }
  } else {
    throw std::invalid_argument("unsupported D-Bus value signature");
  }
}

auto decodeValue(DBusMessageIter &iter, const CodecLimits &limits,
                 std::size_t depth, std::size_t &count) -> Dmn_DbusValue {
  if (depth > limits.max_depth || ++count > limits.max_values) {
    throw std::length_error("D-Bus value resource limit exceeded");
  }

  const int type = dbus_message_iter_get_arg_type(&iter);
  switch (type) {
  case DBUS_TYPE_BOOLEAN: {
    dbus_bool_t value{};
    dbus_message_iter_get_basic(&iter, &value);

    return Dmn_DbusValue{value != FALSE};
  }

  case DBUS_TYPE_BYTE: {
    unsigned char value{};
    dbus_message_iter_get_basic(&iter, &value);

    return Dmn_DbusValue{static_cast<std::uint8_t>(value)};
  }

  case DBUS_TYPE_INT16: {
    dbus_int16_t value{};
    dbus_message_iter_get_basic(&iter, &value);

    return Dmn_DbusValue{static_cast<std::int16_t>(value)};
  }

  case DBUS_TYPE_UINT16: {
    dbus_uint16_t value{};
    dbus_message_iter_get_basic(&iter, &value);

    return Dmn_DbusValue{static_cast<std::uint16_t>(value)};
  }

  case DBUS_TYPE_INT32: {
    dbus_int32_t value{};
    dbus_message_iter_get_basic(&iter, &value);

    return Dmn_DbusValue{static_cast<std::int32_t>(value)};
  }

  case DBUS_TYPE_UINT32: {
    dbus_uint32_t value{};
    dbus_message_iter_get_basic(&iter, &value);

    return Dmn_DbusValue{static_cast<std::uint32_t>(value)};
  }

  case DBUS_TYPE_INT64: {
    dbus_int64_t value{};
    dbus_message_iter_get_basic(&iter, &value);

    return Dmn_DbusValue{static_cast<std::int64_t>(value)};
  }

  case DBUS_TYPE_UINT64: {
    dbus_uint64_t value{};
    dbus_message_iter_get_basic(&iter, &value);

    return Dmn_DbusValue{static_cast<std::uint64_t>(value)};
  }

  case DBUS_TYPE_DOUBLE: {
    double value{};
    dbus_message_iter_get_basic(&iter, &value);

    return Dmn_DbusValue{value};
  }

  case DBUS_TYPE_STRING:
  case DBUS_TYPE_OBJECT_PATH:
  case DBUS_TYPE_SIGNATURE: {
    const char *value{};
    dbus_message_iter_get_basic(&iter, &value);

    if (type == DBUS_TYPE_OBJECT_PATH) {
      return Dmn_DbusValue::objectPath(value == nullptr ? "" : value);
    }

    if (type == DBUS_TYPE_SIGNATURE) {
      return Dmn_DbusValue::signatureValue(value == nullptr ? "" : value);
    }

    return Dmn_DbusValue{value == nullptr ? "" : value};
  }

  case DBUS_TYPE_ARRAY: {
    const char *fullSignature = dbus_message_iter_get_signature(&iter);

    if (fullSignature == nullptr) {
      throw std::runtime_error("missing D-Bus array signature");
    }

    const std::string signature{fullSignature};
    dbus_free(const_cast<char *>(fullSignature));

    if (signature == "ay") {
      DBusMessageIter bytesIter;
      dbus_message_iter_recurse(&iter, &bytesIter);
      unsigned char *bytes{};
      int length{};
      dbus_message_iter_get_fixed_array(&bytesIter, &bytes, &length);
      Dmn_DbusValue::ByteArray values;

      if (length > 0) {
        values.assign(bytes, bytes + length);
      }

      return Dmn_DbusValue::byteArray(std::move(values));
    }

    DBusMessageIter contents;
    dbus_message_iter_recurse(&iter, &contents);
    const auto elementSignature = signature.substr(1);

    if (elementSignature.size() >= 2 && elementSignature.front() == '{' &&
        elementSignature.back() == '}') {
      const auto inner = std::string_view{elementSignature}.substr(
          1, elementSignature.size() - 2);
      const auto keyLength = signatureTypeLength(inner);

      if (keyLength == 0 || keyLength >= inner.size()) {
        throw std::runtime_error("invalid D-Bus dictionary signature");
      }

      std::vector<std::pair<Dmn_DbusValue, Dmn_DbusValue>> entries;

      while (dbus_message_iter_get_arg_type(&contents) != DBUS_TYPE_INVALID) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&contents, &entry);
        auto key = decodeValue(entry, limits, depth + 1, count);

        if (!dbus_message_iter_next(&entry)) {
          throw std::runtime_error("incomplete D-Bus dictionary entry");
        }

        auto value = decodeValue(entry, limits, depth + 1, count);
        entries.emplace_back(std::move(key), std::move(value));

        if (!dbus_message_iter_next(&contents)) {
          break;
        }
      }

      return Dmn_DbusValue::dictionary(std::string{inner.substr(0, keyLength)},
                                       std::string{inner.substr(keyLength)},
                                       std::move(entries));
    }

    std::vector<Dmn_DbusValue> values;
    while (dbus_message_iter_get_arg_type(&contents) != DBUS_TYPE_INVALID) {
      values.push_back(decodeValue(contents, limits, depth + 1, count));

      if (!dbus_message_iter_next(&contents)) {
        break;
      }
    }

    return Dmn_DbusValue::array(elementSignature, std::move(values));
  }

  case DBUS_TYPE_STRUCT: {
    DBusMessageIter fieldsIter;
    dbus_message_iter_recurse(&iter, &fieldsIter);
    std::vector<Dmn_DbusValue> fields;

    while (dbus_message_iter_get_arg_type(&fieldsIter) != DBUS_TYPE_INVALID) {
      fields.push_back(decodeValue(fieldsIter, limits, depth + 1, count));

      if (!dbus_message_iter_next(&fieldsIter)) {
        break;
      }
    }

    return Dmn_DbusValue::structure(std::move(fields));
  }

  case DBUS_TYPE_VARIANT: {
    DBusMessageIter contained;
    dbus_message_iter_recurse(&iter, &contained);

    if (dbus_message_iter_get_arg_type(&contained) == DBUS_TYPE_INVALID) {
      throw std::runtime_error("empty D-Bus variant");
    }

    return Dmn_DbusValue::variant(
        decodeValue(contained, limits, depth + 1, count));
  }

  default:
    throw std::runtime_error("unsupported D-Bus wire type");
  }
}

auto encodeArguments(DBusMessage *message,
                     const Dmn_DbusClient::Arguments &arguments,
                     const CodecLimits &limits) -> bool {
  DBusMessageIter iter;
  dbus_message_iter_init_append(message, &iter);
  std::size_t count{};

  try {
    for (const auto &value : arguments) {
      encodeValue(iter, value, limits, 0, count);

      if (messageSize(message) > limits.max_bytes) {
        return false;
      }
    }
  } catch (const std::length_error &) {
    return false;
  }

  return messageSize(message) <= limits.max_bytes;
}

auto decodeArguments(DBusMessage *message, const CodecLimits &limits)
    -> Dmn_DbusResult<Dmn_DbusClient::Arguments> {
  if (messageSize(message) > limits.max_bytes) {
    return std::unexpected(makeError(Dmn_DbusErrorKind::ResourceLimit,
                                     "D-Bus reply exceeds configured limit"));
  }

  Dmn_DbusClient::Arguments values;
  DBusMessageIter iter;

  if (!dbus_message_iter_init(message, &iter)) {
    return values;
  }

  std::size_t count{};

  try {
    do {
      values.push_back(decodeValue(iter, limits, 0, count));
    } while (dbus_message_iter_next(&iter));
  } catch (const std::length_error &error) {
    return std::unexpected(
        makeError(Dmn_DbusErrorKind::ResourceLimit, error.what()));
  } catch (const std::exception &error) {
    return std::unexpected(makeError(Dmn_DbusErrorKind::Codec, error.what()));
  }

  return values;
}

} // namespace

class Dmn_DbusClient::Impl : public std::enable_shared_from_this<Impl> {
public:
  struct CallRequest {
    std::promise<Dmn_DbusResult<Arguments>> promise;
    std::shared_ptr<Impl> keep_alive;
    DBusPendingCall *pending{};
    CodecLimits limits{};
    std::chrono::steady_clock::time_point deadline;
    std::atomic_bool completed{};
  };

  explicit Impl(const Dmn_DbusClientConfig &config) : m_config{config} {
    validateConfig();
    detail::initializeDbusRuntime();
    connect();

    if (!dbus_connection_add_filter(m_connection.get(), filter, this,
                                    nullptr)) {
      throw std::runtime_error(
          "Dmn_DbusClient: unable to install message filter");
    }

    if (!m_dispatch.exec([this] { dispatchLoop(); })) {
      dbus_connection_remove_filter(m_connection.get(), filter, this);

      throw std::runtime_error("unable to start D-Bus dispatch worker");
    }

    if (!m_callbacks.exec([this] { callbackLoop(); })) {
      m_stopping = true;
      m_callbackCondition.notify_all();

      try {
        (void)m_dispatch.wait();
      } catch (...) {
        dbus_connection_remove_filter(m_connection.get(), filter, this);

        throw;
      }

      dbus_connection_remove_filter(m_connection.get(), filter, this);

      throw std::runtime_error("unable to start D-Bus callback worker");
    }

    std::unique_lock lock{m_mutex};
    m_dispatchCondition.wait(lock, [this] { return m_dispatchReady; });
  }

  ~Impl() { shutdown(); }

  auto submit(const std::string &destination, const std::string &path,
              const std::string &interface, const std::string &member,
              const Arguments &arguments,
              std::chrono::milliseconds timeout) -> Dmn_DbusCall {
    auto request = std::make_shared<CallRequest>();
    auto future = request->promise.get_future();
    const auto validation = validateCall(destination, path, interface, member);

    if (validation) {
      request->promise.set_value(std::unexpected(*validation));

      return Dmn_DbusCall{std::move(future), {}};
    }

    if (timeout == std::chrono::milliseconds::zero()) {
      timeout = m_config.limits.method_timeout;
    }

    if (timeout.count() <= 0 ||
        timeout.count() > std::numeric_limits<int>::max()) {
      request->promise.set_value(
          std::unexpected(makeError(Dmn_DbusErrorKind::InvalidArgument,
                                    "method timeout is outside valid range")));

      return Dmn_DbusCall{std::move(future), {}};
    }

    bool atCapacity{};

    {
      std::lock_guard lock{m_mutex};

      if (!m_accepting.load()) {
        request->promise.set_value(std::unexpected(makeError(
            Dmn_DbusErrorKind::Shutdown, "D-Bus client is shut down")));

        return Dmn_DbusCall{std::move(future), {}};
      }

      if (!dbus_connection_get_is_connected(m_connection.get())) {
        request->promise.set_value(
            std::unexpected(makeError(Dmn_DbusErrorKind::NotConnected,
                                      "D-Bus connection is disconnected")));

        return Dmn_DbusCall{std::move(future), {}};
      }

      const auto active = m_in_flight.fetch_add(1);
      atCapacity = active >= std::min(m_config.limits.max_in_flight_calls,
                                      m_config.limits.max_queued_commands);

      if (atCapacity) {
        m_in_flight.fetch_sub(1);
      }
    }

    if (atCapacity) {
      request->promise.set_value(
          std::unexpected(makeError(Dmn_DbusErrorKind::ResourceLimit,
                                    "in-flight D-Bus call limit reached")));

      return Dmn_DbusCall{std::move(future), {}};
    }

    MessagePtr message{dbus_message_new_method_call(
        destination.c_str(), path.c_str(), interface.c_str(), member.c_str())};

    if (message == nullptr) {
      m_in_flight.fetch_sub(1);
      this->finish(request, std::unexpected(makeError(
                                Dmn_DbusErrorKind::Transport,
                                "unable to allocate D-Bus method call")));

      return Dmn_DbusCall{std::move(future), {}};
    }

    const CodecLimits limits{m_config.limits.max_message_bytes,
                             m_config.limits.max_container_depth,
                             m_config.limits.max_values};

    try {
      if (!encodeArguments(message.get(), arguments, limits)) {
        m_in_flight.fetch_sub(1);
        this->finish(request,
                     std::unexpected(makeError(
                         Dmn_DbusErrorKind::ResourceLimit,
                         "encoded D-Bus call exceeds configured limits")));

        return Dmn_DbusCall{std::move(future), {}};
      }
    } catch (const std::exception &error) {
      m_in_flight.fetch_sub(1);
      this->finish(request,
                   std::unexpected(makeError(Dmn_DbusErrorKind::InvalidArgument,
                                             error.what())));

      return Dmn_DbusCall{std::move(future), {}};
    }

    request->limits = limits;
    request->deadline = std::chrono::steady_clock::now() + timeout;
    request->keep_alive = shared_from_this();
    DBusPendingCall *pending{};
    bool rejectedByShutdown{};
    bool queued{};

    {
      std::lock_guard lock{m_mutex};

      rejectedByShutdown = !m_accepting.load();
      if (!rejectedByShutdown &&
          dbus_connection_get_is_connected(m_connection.get())) {
        queued = dbus_connection_send_with_reply(
                     m_connection.get(), message.get(), &pending,
                     static_cast<int>(timeout.count())) &&
                 pending != nullptr;

        if (queued) {
          request->pending = pending;
          m_pending.emplace(request.get(), Pending{request, pending});
        }
      }
    }

    if (rejectedByShutdown) {
      m_in_flight.fetch_sub(1);
      this->finish(request,
                   std::unexpected(makeError(Dmn_DbusErrorKind::Shutdown,
                                             "D-Bus client is shutting down")));

      return Dmn_DbusCall{std::move(future), {}};
    }

    if (!queued) {
      m_in_flight.fetch_sub(1);
      this->finish(request, std::unexpected(makeError(
                                Dmn_DbusErrorKind::Transport,
                                "unable to queue D-Bus method call")));

      return Dmn_DbusCall{std::move(future), {}};
    }

    auto *context = new std::shared_ptr<CallRequest>{request};
    if (!dbus_pending_call_set_notify(pending, pendingComplete, context,
                                      deletePendingContext)) {
      delete context;
      DBusPendingCall *ownedPending{};

      {
        std::lock_guard lock{m_mutex};

        m_pending.erase(request.get());
        ownedPending = request->pending;
        request->pending = nullptr;
      }

      dbus_pending_call_cancel(ownedPending);
      dbus_pending_call_unref(ownedPending);
      m_in_flight.fetch_sub(1);
      request->promise.set_value(std::unexpected(
          makeError(Dmn_DbusErrorKind::Transport,
                    "unable to register D-Bus pending-call completion")));
      request->keep_alive.reset();

      return Dmn_DbusCall{std::move(future), {}};
    }

    std::weak_ptr<Impl> weakImpl = shared_from_this();
    std::weak_ptr<CallRequest> weakRequest = request;

    return Dmn_DbusCall{std::move(future), [weakImpl, weakRequest] {
                          const auto impl = weakImpl.lock();
                          const auto pending = weakRequest.lock();
                          if (impl != nullptr && pending != nullptr) {
                            impl->cancelCall(pending);
                          }
                        }};
  }

  void cancelCall(const std::shared_ptr<CallRequest> &request) noexcept {
    if (request->completed.exchange(true)) {
      return;
    }

    m_in_flight.fetch_sub(1);

    try {
      request->promise.set_value(std::unexpected(makeError(
          Dmn_DbusErrorKind::Cancelled, "D-Bus method call was cancelled")));
    } catch (...) {
    }

    cancelPending(request);
  }

  void cancelPending(const std::shared_ptr<CallRequest> &request) noexcept {
    DBusPendingCall *pending{};
    {
      std::lock_guard lock{m_mutex};

      if (request->pending != nullptr) {
        pending = request->pending;
        dbus_pending_call_ref(pending);
        request->pending = nullptr;
        m_pending.erase(request.get());
        m_pendingCondition.notify_all();
      }
    }

    if (pending != nullptr) {
      dbus_pending_call_cancel(pending);
      dbus_pending_call_unref(pending);
      dbus_pending_call_unref(pending);
    }
  }

  auto subscribe(
      const Dmn_DbusSignalMatch &match, SignalCallbackWithPath callback,
      std::chrono::milliseconds timeout) -> Dmn_DbusResult<std::uint64_t> {
    if (match.interface.empty() || match.member.empty() || !callback ||
        (!match.path.empty() && !match.path_namespace.empty()) ||
        containsEmbeddedNul(match.sender) ||
        containsEmbeddedNul(match.interface) ||
        containsEmbeddedNul(match.member) || containsEmbeddedNul(match.path) ||
        containsEmbeddedNul(match.path_namespace)) {
      return std::unexpected(makeError(Dmn_DbusErrorKind::InvalidArgument,
                                       "invalid D-Bus signal match"));
    }

    ErrorGuard error;
    const bool valid =
        dbus_validate_interface(match.interface.c_str(), error.get()) &&
        dbus_validate_member(match.member.c_str(), error.get()) &&
        (match.sender.empty() ||
         dbus_validate_bus_name(match.sender.c_str(), error.get())) &&
        (match.path.empty() ||
         dbus_validate_path(match.path.c_str(), error.get())) &&
        (match.path_namespace.empty() ||
         dbus_validate_path(match.path_namespace.c_str(), error.get()));

    if (!valid) {
      return std::unexpected(makeError(Dmn_DbusErrorKind::InvalidArgument,
                                       "invalid D-Bus signal match names"));
    }

    const auto rule = makeMatchRule(match);
    std::uint64_t id{};

    {
      std::lock_guard lock{m_mutex};

      if (m_subscriptions.size() >= m_config.limits.max_queued_commands) {
        return std::unexpected(
            makeError(Dmn_DbusErrorKind::ResourceLimit,
                      "D-Bus signal subscription limit reached"));
      }

      id = ++m_next_subscription;
      m_subscriptions.emplace(id,
                              Subscription{match, rule, std::move(callback)});
    }

    auto added = submit(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS,
                        "AddMatch", {Dmn_DbusValue{rule}}, timeout)
                     .get();
    if (!added) {
      {
        std::lock_guard lock{m_mutex};

        m_subscriptions.erase(id);
      }

      (void)sendMatchCommand("RemoveMatch", rule);

      return std::unexpected(added.error());
    }

    if (!added->empty()) {
      {
        std::lock_guard lock{m_mutex};

        m_subscriptions.erase(id);
      }

      (void)sendMatchCommand("RemoveMatch", rule);

      return std::unexpected(makeError(Dmn_DbusErrorKind::Codec,
                                       "malformed D-Bus AddMatch reply"));
    }

    return id;
  }

  void cancel(std::uint64_t id) noexcept {
    std::string rule;
    {
      std::lock_guard lock{m_mutex};

      const auto found = m_subscriptions.find(id);

      if (found == m_subscriptions.end()) {
        return;
      }

      rule = found->second.rule;
      m_subscriptions.erase(found);
    }

    (void)sendMatchCommand("RemoveMatch", rule);
  }

  auto dispatchThreadId() const -> Dmn_Proc::ThreadId {
    return m_dispatch.getThreadId();
  }

  auto status() const -> Dmn_DbusClientStatus {
    Dmn_DbusClientStatus result;
    result.shutting_down = !m_accepting.load();
    result.connected = !result.shutting_down && m_connection != nullptr &&
                       dbus_connection_get_is_connected(m_connection.get());
    result.pending_calls = m_in_flight.load();
    result.dropped_signal_messages = m_droppedSignalMessages.load();
    result.dropped_signal_bytes = m_droppedSignalBytes.load();
    result.shutdown_discarded_signal_messages =
        m_shutdownDiscardedSignalMessages.load();
    result.shutdown_discarded_signal_bytes =
        m_shutdownDiscardedSignalBytes.load();

    {
      std::lock_guard lock{m_callbackMutex};

      result.queued_signal_messages = queuedSignalCountLocked();
      result.queued_signal_bytes = m_queuedSignalBytes;
    }

    {
      std::lock_guard lock{m_mutex};

      result.terminal_error = m_terminalError;
    }

    return result;
  }

  auto drainSignalCallbacks() -> Dmn_DbusResult<void> {
    {
      std::lock_guard lock{m_callbackMutex};

      if (std::this_thread::get_id() == m_callbacks.getThreadId()) {
        return std::unexpected(
            makeError(Dmn_DbusErrorKind::InvalidArgument,
                      "cannot drain D-Bus callbacks from the callback worker"));
      }
    }

    std::lock_guard drainLock{m_callbackDrainMutex};
    auto completion = std::make_shared<std::promise<void>>();
    auto completed = completion->get_future();

    {
      std::unique_lock lock{m_callbackMutex};

      m_callbackCondition.wait(lock, [this] {
        const auto queued = queuedSignalCountLocked();

        return m_stopping.load() ||
               queued < m_config.limits.max_signal_messages ||
               (queued == 0 && m_config.limits.max_signal_messages == 0);
      });

      if (m_stopping.load()) {
        return std::unexpected(makeError(Dmn_DbusErrorKind::Shutdown,
                                         "D-Bus client is shutting down"));
      }

      m_signalQueue.push_back(QueuedSignal{{}, {}, {}, 0, completion});
    }

    m_callbackCondition.notify_one();
    completed.wait();

    if (m_stopping.load()) {
      return std::unexpected(makeError(Dmn_DbusErrorKind::Shutdown,
                                       "D-Bus client is shutting down"));
    }

    return {};
  }

  void shutdown() noexcept {
    if (m_shutdownStarted.exchange(true)) {
      return;
    }

    std::vector<std::shared_ptr<CallRequest>> pendingRequests;

    {
      std::lock_guard lock{m_mutex};

      m_accepting = false;

      for (const auto &[raw, pending] : m_pending) {
        (void)raw;
        pendingRequests.push_back(pending.request);
      }
    }

    for (const auto &request : pendingRequests) {
      if (!request->completed.exchange(true)) {
        m_in_flight.fetch_sub(1);

        try {
          request->promise.set_value(std::unexpected(makeError(
              Dmn_DbusErrorKind::Shutdown, "D-Bus client is shutting down")));
        } catch (...) {
        }
      }

      cancelPending(request);
    }

    {
      std::unique_lock lock{m_mutex};

      m_pendingCondition.wait_for(lock, std::chrono::milliseconds{250},
                                  [this] { return m_pending.empty(); });
    }

    m_stopping = true;
    m_callbackCondition.notify_all();

    (void)m_dispatch.wait();

    (void)m_callbacks.wait();

    if (m_connection != nullptr) {
      dbus_connection_remove_filter(m_connection.get(), filter, this);

      {
        std::lock_guard lock{m_mutex};

        m_subscriptions.clear();

        for (auto &[raw, pending] : m_pending) {
          (void)raw;

          if (pending.call != nullptr) {
            dbus_pending_call_unref(pending.call);
            pending.call = nullptr;
          }

          pending.request->pending = nullptr;
          pending.request->keep_alive.reset();
        }

        m_pending.clear();
      }

      dbus_connection_close(m_connection.get());
    }
  }

private:
  static void deletePendingContext(void *data) {
    delete static_cast<std::shared_ptr<CallRequest> *>(data);
  }

  static void pendingComplete(DBusPendingCall *pending, void *data) {
    auto request = *static_cast<std::shared_ptr<CallRequest> *>(data);
    auto impl = request->keep_alive;

    if (impl == nullptr) {
      return;
    }

    Dmn_DbusResult<Arguments> result = std::unexpected(makeError(
        Dmn_DbusErrorKind::Transport, "D-Bus call completed without a reply"));

    try {
      MessagePtr reply{dbus_pending_call_steal_reply(pending)};

      if (reply != nullptr &&
          dbus_message_get_type(reply.get()) == DBUS_MESSAGE_TYPE_ERROR) {
        const char *name = dbus_message_get_error_name(reply.get());
        const char *text{};
        DBusMessageIter iter;

        if (dbus_message_iter_init(reply.get(), &iter) &&
            dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
          dbus_message_iter_get_basic(&iter, &text);
        }

        const std::string remoteName = name == nullptr ? "" : name;
        const std::string message =
            text == nullptr ? "remote D-Bus method failed" : text;
        const auto kind = remoteName == DBUS_ERROR_NO_REPLY
                              ? Dmn_DbusErrorKind::Timeout
                          : remoteName == DBUS_ERROR_DISCONNECTED
                              ? Dmn_DbusErrorKind::Transport
                              : Dmn_DbusErrorKind::Remote;
        result = std::unexpected(makeError(kind, message, remoteName));
      } else if (reply != nullptr) {
        result = decodeArguments(reply.get(), request->limits);
      }
    } catch (const std::exception &error) {
      result =
          std::unexpected(makeError(Dmn_DbusErrorKind::Codec, error.what()));
    } catch (...) {
      result = std::unexpected(makeError(
          Dmn_DbusErrorKind::Codec, "unknown D-Bus reply decoding failure"));
    }

    {
      std::lock_guard lock{impl->m_mutex};
      impl->m_pending.erase(request.get());
      impl->m_pendingCondition.notify_all();

      if (request->pending != nullptr) {
        dbus_pending_call_unref(request->pending);
        request->pending = nullptr;
      }
    }

    if (!request->completed.exchange(true)) {
      impl->m_in_flight.fetch_sub(1);

      try {
        request->promise.set_value(std::move(result));
      } catch (...) {
      }
    }

    request->keep_alive.reset();
  }

  void finish(const std::shared_ptr<CallRequest> &request,
              Dmn_DbusResult<Arguments> result) noexcept {
    if (!request->completed.exchange(true)) {
      try {
        request->promise.set_value(std::move(result));
      } catch (...) {
      }

      request->keep_alive.reset();
    }
  }

  struct Subscription {
    Dmn_DbusSignalMatch match;
    std::string rule;
    SignalCallbackWithPath callback;
  };

  struct QueuedSignal {
    SignalCallbackWithPath callback;
    std::string path;
    Arguments arguments;
    std::size_t bytes{};
    std::shared_ptr<std::promise<void>> completion;
  };

  struct Pending {
    std::shared_ptr<CallRequest> request;
    DBusPendingCall *call{};
  };

  void validateConfig() {
    const auto &limits = m_config.limits;
    const bool validBusKind = m_config.bus_kind == Dmn_DbusBusKind::Session ||
                              m_config.bus_kind == Dmn_DbusBusKind::System ||
                              m_config.bus_kind == Dmn_DbusBusKind::Address;

    if (!validBusKind ||
        (m_config.bus_kind == Dmn_DbusBusKind::Address &&
         (m_config.bus_address.empty() ||
          containsEmbeddedNul(m_config.bus_address))) ||
        (m_config.bus_kind != Dmn_DbusBusKind::Address &&
         !m_config.bus_address.empty()) ||
        limits.method_timeout.count() <= 0 ||
        limits.method_timeout.count() > std::numeric_limits<int>::max() ||
        limits.max_in_flight_calls == 0 || limits.max_queued_commands == 0 ||
        limits.max_signal_messages == 0 || limits.max_signal_bytes == 0 ||
        limits.max_message_bytes == 0 ||
        limits.max_message_bytes >
            static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        limits.max_container_depth == 0 || limits.max_values == 0) {
      throw std::invalid_argument("Dmn_DbusClient: invalid configuration");
    }
  }

  void connect() {
    ErrorGuard error;

    if (m_config.bus_kind == Dmn_DbusBusKind::Address) {
      // Explicit addresses still connect to a bus daemon, not directly to a
      // service. Registration enables named destinations and daemon-managed
      // signal match rules.
      if (m_config.bus_address.find(':') == std::string::npos) {
        throw std::system_error(
            std::make_error_code(std::errc::invalid_argument),
            "Dmn_DbusClient: malformed D-Bus address");
      }

      DBusAddressEntry **entries{};
      int count{};

      if (!dbus_parse_address(m_config.bus_address.c_str(), &entries, &count,
                              error.get()) ||
          count == 0) {
        if (entries != nullptr) {
          dbus_address_entries_free(entries);
        }

        throw std::system_error(
            std::make_error_code(std::errc::invalid_argument),
            "Dmn_DbusClient: invalid D-Bus address");
      }

      for (int index = 0; index < count; ++index) {
        const char *method = dbus_address_entry_get_method(entries[index]);

        if (method == nullptr || (std::string_view{method} != "unix" &&
                                  std::string_view{method} != "tcp" &&
                                  std::string_view{method} != "nonce-tcp" &&
                                  std::string_view{method} != "launchd" &&
                                  std::string_view{method} != "autolaunch")) {
          dbus_address_entries_free(entries);

          throw std::system_error(
              std::make_error_code(std::errc::invalid_argument),
              "Dmn_DbusClient: unsupported D-Bus address transport");
        }
      }

      dbus_address_entries_free(entries);
      m_connection.reset(dbus_connection_open_private(
          m_config.bus_address.c_str(), error.get()));

      if (m_connection != nullptr &&
          !dbus_bus_register(m_connection.get(), error.get())) {
        m_connection.reset();
      }
    } else {
      // Session and System choose the standard daemon by kind while libdbus
      // still gives this client its own dedicated connection.
      const auto bus = m_config.bus_kind == Dmn_DbusBusKind::System
                           ? DBUS_BUS_SYSTEM
                           : DBUS_BUS_SESSION;
      m_connection.reset(dbus_bus_get_private(bus, error.get()));
    }

    if (m_connection == nullptr) {
      const auto message = error.value().message == nullptr
                               ? "unable to connect to D-Bus"
                               : error.value().message;

      throw std::system_error(std::make_error_code(std::errc::not_connected),
                              message);
    }

    dbus_connection_set_exit_on_disconnect(m_connection.get(), FALSE);
  }

  auto validateCall(const std::string &destination, const std::string &path,
                    const std::string &interface,
                    const std::string &member) -> std::optional<Dmn_DbusError> {
    ErrorGuard error;
    if (containsEmbeddedNul(destination) || containsEmbeddedNul(path) ||
        containsEmbeddedNul(interface) || containsEmbeddedNul(member) ||
        !dbus_validate_bus_name(destination.c_str(), error.get()) ||
        !dbus_validate_path(path.c_str(), error.get()) ||
        !dbus_validate_interface(interface.c_str(), error.get()) ||
        !dbus_validate_member(member.c_str(), error.get())) {
      return makeError(Dmn_DbusErrorKind::InvalidArgument,
                       "invalid D-Bus destination, path, interface, or member");
    }

    return std::nullopt;
  }

  static auto makeMatchRule(const Dmn_DbusSignalMatch &match) -> std::string {
    std::string rule{"type='signal',interface='" + match.interface +
                     "',member='" + match.member + "'"};

    if (!match.sender.empty()) {
      rule += ",sender='" + match.sender + "'";
    }

    if (!match.path.empty()) {
      rule += ",path='" + match.path + "'";
    }

    if (!match.path_namespace.empty()) {
      rule += ",path_namespace='" + match.path_namespace + "'";
    }

    return rule;
  }

  auto sendMatchCommand(const char *member, const std::string &rule) -> bool {
    if (!m_accepting.load() ||
        !dbus_connection_get_is_connected(m_connection.get())) {
      return false;
    }

    MessagePtr request{dbus_message_new_method_call(
        DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, member)};

    if (request == nullptr) {
      return false;
    }

    const char *ruleText = rule.c_str();

    if (!dbus_message_append_args(request.get(), DBUS_TYPE_STRING, &ruleText,
                                  DBUS_TYPE_INVALID) ||
        !dbus_connection_send(m_connection.get(), request.get(), nullptr)) {
      return false;
    }

    return true;
  }

  static auto filter(DBusConnection *, DBusMessage *message,
                     void *data) -> DBusHandlerResult {
    auto *self = static_cast<Impl *>(data);

    try {
      return filterMessage(message, self);
    } catch (const std::exception &error) {
      self->recordTerminalError(std::string{"D-Bus signal decode failed: "} +
                                error.what());
    } catch (...) {
      self->recordTerminalError(
          "D-Bus signal decode failed with an unknown exception");
    }

    return DBUS_HANDLER_RESULT_HANDLED;
  }

  static auto filterMessage(DBusMessage *message,
                            Impl *self) -> DBusHandlerResult {
    if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_SIGNAL) {
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    const char *interface = dbus_message_get_interface(message);
    const char *member = dbus_message_get_member(message);
    const char *sender = dbus_message_get_sender(message);
    const char *path = dbus_message_get_path(message);
    std::vector<SignalCallbackWithPath> callbacks;
    {
      std::lock_guard lock{self->m_mutex};

      for (const auto &[id, subscription] : self->m_subscriptions) {
        (void)id;

        if (subscription.match.interface !=
                (interface == nullptr ? "" : interface) ||
            subscription.match.member != (member == nullptr ? "" : member) ||
            (!subscription.match.sender.empty() &&
             subscription.match.sender.front() == ':' &&
             subscription.match.sender != (sender == nullptr ? "" : sender)) ||
            (!subscription.match.path.empty() &&
             subscription.match.path != (path == nullptr ? "" : path)) ||
            (!subscription.match.path_namespace.empty() &&
             (path == nullptr ||
              std::string_view{path}.substr(
                  0, subscription.match.path_namespace.size()) !=
                  subscription.match.path_namespace ||
              (std::string_view{path}.size() >
                   subscription.match.path_namespace.size() &&
               subscription.match.path_namespace != "/" &&
               path[subscription.match.path_namespace.size()] != '/')))) {

          continue;
        }

        callbacks.push_back(subscription.callback);
      }
    }

    if (callbacks.empty()) {
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    const CodecLimits limits{self->m_config.limits.max_message_bytes,
                             self->m_config.limits.max_container_depth,
                             self->m_config.limits.max_values};
    const auto decoded = decodeArguments(message, limits);

    if (!decoded) {
      self->recordTerminalError(decoded.error().message);

      return DBUS_HANDLER_RESULT_HANDLED;
    }

    const auto bytes = messageSize(message);
    std::lock_guard lock{self->m_callbackMutex};
    const auto queuedSignalCount = self->queuedSignalCountLocked();

    if (callbacks.size() >
            self->m_config.limits.max_signal_messages -
                std::min(queuedSignalCount,
                         self->m_config.limits.max_signal_messages) ||
        bytes > (self->m_config.limits.max_signal_bytes -
                 std::min(self->m_queuedSignalBytes,
                          self->m_config.limits.max_signal_bytes)) /
                    callbacks.size()) {
      self->m_droppedSignalMessages.fetch_add(1);
      self->m_droppedSignalBytes.fetch_add(bytes);

      return DBUS_HANDLER_RESULT_HANDLED;
    }

    for (auto &callback : callbacks) {
      self->m_queuedSignalBytes += bytes;
      self->m_signalQueue.push_back(QueuedSignal{
          callback, path == nullptr ? "" : path, *decoded, bytes, {}});
    }

    self->m_callbackCondition.notify_one();

    return DBUS_HANDLER_RESULT_HANDLED;
  }

  void recordTerminalError(std::string error) {
    std::lock_guard lock{m_mutex};

    m_terminalError = std::move(error);
  }

  auto queuedSignalCountLocked() const -> std::size_t {
    return static_cast<std::size_t>(
        std::count_if(m_signalQueue.begin(), m_signalQueue.end(),
                      [](const auto &signal) { return !signal.completion; }));
  }

  void dispatchLoop() {
    {
      std::lock_guard lock{m_mutex};

      m_dispatchReady = true;
    }

    m_dispatchCondition.notify_all();

    while (!m_stopping.load()) {
      const bool dispatched =
          dbus_connection_read_write_dispatch(m_connection.get(), 25);
      expireCalls();

      if (!dispatched) {
        if (!dbus_connection_get_is_connected(m_connection.get())) {
          {
            std::lock_guard lock{m_mutex};

            m_terminalError = "D-Bus connection disconnected";
          }

          failPendingOnDisconnect();

          break;
        }
      }
    }
  }

  void expireCalls() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::shared_ptr<CallRequest>> expired;

    {
      std::lock_guard lock{m_mutex};

      for (const auto &[raw, pending] : m_pending) {
        (void)raw;

        if (pending.request->deadline <= now) {
          expired.push_back(pending.request);
        }
      }
    }

    for (const auto &request : expired) {
      if (!request->completed.exchange(true)) {
        m_in_flight.fetch_sub(1);

        try {
          request->promise.set_value(std::unexpected(makeError(
              Dmn_DbusErrorKind::Timeout, "D-Bus method call timed out")));
        } catch (...) {
        }
      }

      cancelPending(request);
    }
  }

  void failPendingOnDisconnect() {
    std::vector<std::shared_ptr<CallRequest>> pendingRequests;
    {
      std::lock_guard lock{m_mutex};

      for (const auto &[raw, pending] : m_pending) {
        (void)raw;
        pendingRequests.push_back(pending.request);
      }
    }

    for (const auto &request : pendingRequests) {
      if (!request->completed.exchange(true)) {
        m_in_flight.fetch_sub(1);

        try {
          request->promise.set_value(std::unexpected(makeError(
              Dmn_DbusErrorKind::Transport, "D-Bus connection disconnected")));
        } catch (...) {
        }
      }

      cancelPending(request);
    }
  }

  void callbackLoop() {
    while (true) {
      QueuedSignal signal;
      {
        std::unique_lock lock{m_callbackMutex};
        m_callbackCondition.wait(lock, [this] {
          return m_stopping.load() || !m_signalQueue.empty();
        });

        if (m_stopping.load()) {
          m_shutdownDiscardedSignalMessages.fetch_add(
              queuedSignalCountLocked());
          m_shutdownDiscardedSignalBytes.fetch_add(m_queuedSignalBytes);
          m_queuedSignalBytes = 0;

          for (auto &queuedSignal : m_signalQueue) {
            if (queuedSignal.completion) {
              queuedSignal.completion->set_value();
            }
          }

          m_signalQueue.clear();

          return;
        }

        signal = std::move(m_signalQueue.front());
        m_signalQueue.pop_front();

        if (!signal.completion) {
          m_queuedSignalBytes -= signal.bytes;
        }

        m_callbackCondition.notify_all();
      }

      if (signal.completion) {
        signal.completion->set_value();

        continue;
      }

      try {
        signal.callback(std::move(signal.path), std::move(signal.arguments));
      } catch (const std::exception &error) {
        recordTerminalError(std::string{"signal callback failed: "} +
                            error.what());
      } catch (...) {
        recordTerminalError("signal callback failed with an unknown exception");
      }
    }
  }

  Dmn_DbusClientConfig m_config;
  ConnectionPtr m_connection;
  std::atomic_bool m_stopping{};
  std::atomic_bool m_accepting{true};
  std::atomic_bool m_shutdownStarted{};
  std::atomic_size_t m_in_flight{};
  Dmn_Proc m_dispatch{"dmn-dbus-dispatch"};
  Dmn_Proc m_callbacks{"dmn-dbus-callback"};
  mutable std::mutex m_mutex;
  std::condition_variable m_pendingCondition;
  std::condition_variable m_dispatchCondition;
  std::unordered_map<std::uint64_t, Subscription> m_subscriptions;
  std::unordered_map<CallRequest *, Pending> m_pending;
  std::uint64_t m_next_subscription{};
  bool m_dispatchReady{};
  mutable std::mutex m_callbackMutex;
  std::mutex m_callbackDrainMutex;
  std::condition_variable m_callbackCondition;
  std::deque<QueuedSignal> m_signalQueue;
  std::size_t m_queuedSignalBytes{};
  std::atomic_size_t m_droppedSignalMessages{};
  std::atomic_size_t m_droppedSignalBytes{};
  std::atomic_size_t m_shutdownDiscardedSignalMessages{};
  std::atomic_size_t m_shutdownDiscardedSignalBytes{};
  std::string m_terminalError;
};

Dmn_DbusSubscription::Dmn_DbusSubscription(std::function<void()> cancel)
    : m_cancel{std::move(cancel)} {}

Dmn_DbusSubscription::~Dmn_DbusSubscription() { cancel(); }

Dmn_DbusSubscription::Dmn_DbusSubscription(
    Dmn_DbusSubscription &&other) noexcept
    : m_cancel{std::move(other.m_cancel)} {
  other.m_cancel = {};
}

auto Dmn_DbusSubscription::operator=(Dmn_DbusSubscription &&other) noexcept
    -> Dmn_DbusSubscription & {
  if (this != &other) {
    cancel();
    m_cancel = std::move(other.m_cancel);
    other.m_cancel = {};
  }

  return *this;
}

void Dmn_DbusSubscription::cancel() noexcept {
  if (m_cancel) {
    auto callback = std::move(m_cancel);

    try {
      callback();
    } catch (...) {
    }
  }
}

Dmn_DbusCall::Dmn_DbusCall(std::future<Result> future,
                           std::function<void()> cancel)
    : m_future{std::move(future)}, m_cancel{std::move(cancel)} {}

auto Dmn_DbusCall::get() -> Result { return m_future.get(); }

void Dmn_DbusCall::cancel() noexcept {
  if (m_cancel) {
    auto callback = std::move(m_cancel);

    try {
      callback();
    } catch (...) {
    }
  }
}

Dmn_DbusClient::Dmn_DbusClient(const Dmn_DbusClientConfig &config)
    : m_impl{std::make_shared<Impl>(config)} {}

Dmn_DbusClient::~Dmn_DbusClient() {
  if (m_impl != nullptr) {
    m_impl->shutdown();
  }
}

void Dmn_DbusClient::shutdown() noexcept {
  if (m_impl != nullptr) {
    m_impl->shutdown();
  }
}

auto Dmn_DbusClient::callAsync(
    const std::string &destination, const std::string &path,
    const std::string &interface, const std::string &member,
    Arguments arguments, std::chrono::milliseconds timeout) -> Dmn_DbusCall {
  return m_impl->submit(destination, path, interface, member, arguments,
                        timeout);
}

auto Dmn_DbusClient::call(const std::string &destination,
                          const std::string &path, const std::string &interface,
                          const std::string &member, const Arguments &arguments,
                          std::chrono::milliseconds timeout)
    -> Dmn_DbusResult<Arguments> {
  return callAsync(destination, path, interface, member, arguments, timeout)
      .get();
}

auto Dmn_DbusClient::getProperty(
    const std::string &destination, const std::string &path,
    const std::string &interface,
    const std::string &property) -> Dmn_DbusResult<Dmn_DbusValue> {
  auto result =
      call(destination, path, "org.freedesktop.DBus.Properties", "Get",
           {Dmn_DbusValue{interface}, Dmn_DbusValue{property}});

  if (!result) {
    return std::unexpected(result.error());
  }

  if (result->size() != 1 || (*result)[0].signature() != "v") {
    return std::unexpected(
        makeError(Dmn_DbusErrorKind::Codec, "malformed D-Bus property reply"));
  }

  return (*result)[0].asVariant();
}

auto Dmn_DbusClient::setProperty(const std::string &destination,
                                 const std::string &path,
                                 const std::string &interface,
                                 const std::string &property,
                                 Dmn_DbusValue value) -> Dmn_DbusResult<void> {
  auto result =
      call(destination, path, "org.freedesktop.DBus.Properties", "Set",
           {Dmn_DbusValue{interface}, Dmn_DbusValue{property},
            Dmn_DbusValue::variant(std::move(value))});

  if (!result) {
    return std::unexpected(result.error());
  }

  if (!result->empty()) {
    return std::unexpected(makeError(Dmn_DbusErrorKind::Codec,
                                     "malformed D-Bus property set reply"));
  }

  return {};
}

auto Dmn_DbusClient::subscribe(const Dmn_DbusSignalMatch &match,
                               SignalCallback callback)
    -> Dmn_DbusResult<Dmn_DbusSubscription> {
  return subscribe(match, std::move(callback),
                   std::chrono::milliseconds::zero());
}

auto Dmn_DbusClient::subscribe(
    const Dmn_DbusSignalMatch &match, SignalCallback callback,
    std::chrono::milliseconds timeout) -> Dmn_DbusResult<Dmn_DbusSubscription> {
  if (!callback) {
    return std::unexpected(makeError(Dmn_DbusErrorKind::InvalidArgument,
                                     "invalid D-Bus signal match"));
  }

  return subscribeWithPath(
      match,
      [callback = std::move(callback)](std::string,
                                       Arguments arguments) mutable {
        callback(std::move(arguments));
      },
      timeout);
}

auto Dmn_DbusClient::subscribeWithPath(const Dmn_DbusSignalMatch &match,
                                       SignalCallbackWithPath callback)
    -> Dmn_DbusResult<Dmn_DbusSubscription> {
  return subscribeWithPath(match, std::move(callback),
                           std::chrono::milliseconds::zero());
}

auto Dmn_DbusClient::subscribeWithPath(
    const Dmn_DbusSignalMatch &match, SignalCallbackWithPath callback,
    std::chrono::milliseconds timeout) -> Dmn_DbusResult<Dmn_DbusSubscription> {
  auto subscribed = m_impl->subscribe(match, std::move(callback), timeout);

  if (!subscribed) {
    return std::unexpected(subscribed.error());
  }

  std::weak_ptr<Impl> weak = m_impl;
  const auto id = *subscribed;

  return Dmn_DbusSubscription{[weak, id] {
    if (const auto impl = weak.lock()) {
      impl->cancel(id);
    }
  }};
}

auto Dmn_DbusClient::drainSignalCallbacks() -> Dmn_DbusResult<void> {
  return m_impl->drainSignalCallbacks();
}

auto Dmn_DbusClient::dispatchThreadId() const -> Dmn_Proc::ThreadId {
  return m_impl->dispatchThreadId();
}

auto Dmn_DbusClient::status() const -> Dmn_DbusClientStatus {
  return m_impl->status();
}

} // namespace dmn
