/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-dbus-client.cpp
 * @brief Private-bus tests for the generic D-Bus client.
 */

#include "dmn-dbus-client.hpp"
#include "dmn-dbus-io.hpp"
#include "dmn-test-dbus-support.hpp"

#include <dbus/dbus.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

static_assert(std::is_copy_constructible_v<dmn::Dmn_DbusValue>);
static_assert(std::is_copy_assignable_v<dmn::Dmn_DbusValue>);
static_assert(std::is_nothrow_move_constructible_v<dmn::Dmn_DbusValue>);
static_assert(std::is_nothrow_move_assignable_v<dmn::Dmn_DbusValue>);

static_assert(!std::is_copy_constructible_v<dmn::Dmn_DbusSubscription>);
static_assert(!std::is_copy_assignable_v<dmn::Dmn_DbusSubscription>);
static_assert(std::is_nothrow_move_constructible_v<dmn::Dmn_DbusSubscription>);
static_assert(std::is_nothrow_move_assignable_v<dmn::Dmn_DbusSubscription>);
static_assert(!std::is_copy_constructible_v<dmn::Dmn_DbusCall>);
static_assert(!std::is_copy_assignable_v<dmn::Dmn_DbusCall>);
static_assert(std::is_nothrow_move_constructible_v<dmn::Dmn_DbusCall>);
static_assert(std::is_nothrow_move_assignable_v<dmn::Dmn_DbusCall>);
static_assert(!std::is_copy_constructible_v<dmn::Dmn_DbusClient>);
static_assert(!std::is_copy_assignable_v<dmn::Dmn_DbusClient>);
static_assert(!std::is_move_constructible_v<dmn::Dmn_DbusClient>);
static_assert(!std::is_move_assignable_v<dmn::Dmn_DbusClient>);

using namespace std::chrono_literals;
using dmn_test_dbus::DbusConnectionPtr;
using dmn_test_dbus::DbusErrorGuard;
using dmn_test_dbus::DbusMessagePtr;
using dmn_test_dbus::PrivateBus;

constexpr char kServiceName[] = "org.dmn.TestService";
constexpr char kObjectPath[] = "/org/dmn/Test";
constexpr char kInterface[] = "org.dmn.Test";
constexpr char kPropertyInterface[] = "org.freedesktop.DBus.Properties";

auto encodedEchoCallSize(const std::string &argument) -> std::size_t {
  DbusMessagePtr message{dbus_message_new_method_call(kServiceName, kObjectPath,
                                                      kInterface, "Echo")};
  const char *value = argument.c_str();
  if (message == nullptr ||
      !dbus_message_append_args(message.get(), DBUS_TYPE_STRING, &value,
                                DBUS_TYPE_INVALID)) {
    throw std::runtime_error("unable to build encoded-size test message");
  }

  char *bytes{};
  int length{};
  if (!dbus_message_marshal(message.get(), &bytes, &length)) {
    throw std::runtime_error("unable to marshal encoded-size test message");
  }

  dbus_free(bytes);

  return static_cast<std::size_t>(length);
}

TEST(DbusClientHandleTest,
     SubscriptionMoveAssignmentAndCancellationAreIdempotent) {
  int firstCancellationCount{};
  int secondCancellationCount{};
  {
    dmn::Dmn_DbusSubscription first{[&] { ++firstCancellationCount; }};
    dmn::Dmn_DbusSubscription second{[&] { ++secondCancellationCount; }};
    second = std::move(first);
    EXPECT_EQ(firstCancellationCount, 0);
    EXPECT_EQ(secondCancellationCount, 1);

    second.cancel();
    second.cancel();
    EXPECT_EQ(firstCancellationCount, 1);
  }

  EXPECT_EQ(firstCancellationCount, 1);
  EXPECT_EQ(secondCancellationCount, 1);
}

TEST(DbusClientHandleTest, SubscriptionMoveConstructorTransfersCancellation) {
  int cancellationCount{};
  dmn::Dmn_DbusSubscription source{[&] { ++cancellationCount; }};
  dmn::Dmn_DbusSubscription moved{std::move(source)};
  source.cancel();
  EXPECT_EQ(cancellationCount, 0);
  moved.cancel();
  EXPECT_EQ(cancellationCount, 1);
}

TEST(DbusClientHandleTest, DestroyingActiveSubscriptionCancelsExactlyOnce) {
  int cancellationCount{};
  {
    dmn::Dmn_DbusSubscription subscription{[&] { ++cancellationCount; }};
  }

  EXPECT_EQ(cancellationCount, 1);
}

TEST(DbusClientHandleTest, CallMoveWaitGetAndCancelOwnOneResult) {
  std::promise<dmn::Dmn_DbusCall::Result> promise;
  int cancellationCount{};
  dmn::Dmn_DbusCall original{promise.get_future(),
                             [&] { ++cancellationCount; }};
  EXPECT_EQ(original.waitFor(0ms), std::future_status::timeout);

  dmn::Dmn_DbusCall moved{std::move(original)};
  moved.cancel();
  moved.cancel();
  EXPECT_EQ(cancellationCount, 1);

  promise.set_value(
      dmn::Dmn_DbusCall::Arguments{dmn::Dmn_DbusValue{std::uint32_t{42}}});
  ASSERT_EQ(moved.waitFor(0ms), std::future_status::ready);
  auto result = moved.get();
  ASSERT_TRUE(result);
  ASSERT_EQ(result->size(), 1U);
  EXPECT_EQ(result->front().asUint32(), 42U);
}

TEST(DbusClientHandleTest, CallMoveAssignmentTransfersResultAndCancellation) {
  std::promise<dmn::Dmn_DbusCall::Result> sourcePromise;
  std::promise<dmn::Dmn_DbusCall::Result> replacedPromise;
  int sourceCancellationCount{};
  int replacedCancellationCount{};
  dmn::Dmn_DbusCall source{sourcePromise.get_future(),
                           [&] { ++sourceCancellationCount; }};
  dmn::Dmn_DbusCall destination{replacedPromise.get_future(),
                                [&] { ++replacedCancellationCount; }};

  destination = std::move(source);
  EXPECT_EQ(sourceCancellationCount, 0);
  EXPECT_EQ(replacedCancellationCount, 0);
  destination.cancel();
  EXPECT_EQ(sourceCancellationCount, 1);
  EXPECT_EQ(replacedCancellationCount, 0);
  sourcePromise.set_value(dmn::Dmn_DbusCall::Arguments{});
  ASSERT_TRUE(destination.get());
}

TEST(DbusClientHandleTest, DestroyingCallHandleDoesNotCancelOperation) {
  std::promise<dmn::Dmn_DbusCall::Result> promise;
  int cancellationCount{};
  {
    dmn::Dmn_DbusCall call{promise.get_future(), [&] { ++cancellationCount; }};
  }

  EXPECT_EQ(cancellationCount, 0);
  promise.set_value(dmn::Dmn_DbusCall::Arguments{});
}

TEST(DbusClientHandleTest, HandleCancellationSuppressesCallbackExceptions) {
  dmn::Dmn_DbusSubscription subscription{
      [] { throw std::runtime_error("subscription cancellation failure"); }};
  EXPECT_NO_THROW(subscription.cancel());

  std::promise<dmn::Dmn_DbusCall::Result> promise;
  dmn::Dmn_DbusCall call{
      promise.get_future(),
      [] { throw std::runtime_error("call cancellation failure"); }};
  EXPECT_NO_THROW(call.cancel());
  promise.set_value(dmn::Dmn_DbusCall::Arguments{});
}

class FakeDbusService {
public:
  explicit FakeDbusService(const std::string &address) {
    DbusErrorGuard error;
    m_connection.reset(
        dbus_connection_open_private(address.c_str(), error.get()));
    if (m_connection == nullptr ||
        !dbus_bus_register(m_connection.get(), error.get())) {
      throw std::runtime_error("unable to connect fake D-Bus service");
    }

    dbus_connection_set_exit_on_disconnect(m_connection.get(), FALSE);
    if (dbus_bus_request_name(m_connection.get(), kServiceName,
                              DBUS_NAME_FLAG_DO_NOT_QUEUE, error.get()) !=
            DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER ||
        !dbus_connection_add_filter(m_connection.get(), filter, this,
                                    nullptr)) {
      throw std::runtime_error("unable to initialize fake D-Bus service");
    }

    m_worker = std::thread{[this] { dispatchLoop(); }};
  }

  ~FakeDbusService() {
    m_stopping = true;
    if (m_worker.joinable()) {
      m_worker.join();
    }
  }

  FakeDbusService(const FakeDbusService &) = delete;
  auto operator=(const FakeDbusService &) -> FakeDbusService & = delete;

private:
  static auto filter(DBusConnection *connection, DBusMessage *message,
                     void *data) -> DBusHandlerResult {
    auto *self = static_cast<FakeDbusService *>(data);
    if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_METHOD_CALL) {
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    DBusMessage *reply{};
    if (dbus_message_is_method_call(message, kInterface, "Echo")) {
      reply = echo(connection, message);
    } else if (dbus_message_is_method_call(message, kInterface, "Fail")) {
      reply = dbus_message_new_error(message, "org.dmn.Test.Failed",
                                     "intentional remote failure");
    } else if (dbus_message_is_method_call(message, kInterface, "Delay")) {
      std::this_thread::sleep_for(150ms);
      reply = dbus_message_new_method_return(message);
    } else if (dbus_message_is_method_call(message, kInterface, "EmitSignal")) {
      self->emitSignal(connection);
      reply = dbus_message_new_method_return(message);
    } else if (dbus_message_is_method_call(message, kInterface,
                                           "EmitForeignSignal")) {
      self->emitSignalAt(connection, "/org/dmn/Other", kInterface);
      reply = dbus_message_new_method_return(message);
    } else if (dbus_message_is_method_call(message, kInterface,
                                           "EmitSiblingSignal")) {
      self->emitSignalAt(connection, "/org/dmnish/Other", kInterface);
      reply = dbus_message_new_method_return(message);
    } else if (dbus_message_is_method_call(message, kInterface,
                                           "EmitSignalBurst")) {
      self->emitSignalBurst(connection, 4);
      reply = dbus_message_new_method_return(message);
    } else if (dbus_message_is_method_call(message, kInterface,
                                           "AcceptObjectManagerShape")) {
      reply = self->acceptObjectManagerShape(message);
    } else if (dbus_message_is_method_call(message, kInterface,
                                           "AcceptNestedArray")) {
      const char *signature = dbus_message_get_signature(message);
      reply = signature != nullptr && std::string_view{signature} == "aas"
                  ? dbus_message_new_method_return(message)
                  : dbus_message_new_error(message, DBUS_ERROR_INVALID_ARGS,
                                           "unexpected nested array signature");
    } else if (dbus_message_is_method_call(message, kInterface,
                                           "AcceptStructVariant")) {
      reply = self->acceptStructVariant(message);
    } else if (dbus_message_is_method_call(message, kPropertyInterface,
                                           "Get")) {
      reply = self->getProperty(message);
    } else if (dbus_message_is_method_call(message, kPropertyInterface,
                                           "Set")) {
      reply = self->setProperty(message);
    } else if (dbus_message_is_method_call(message,
                                           "org.freedesktop.DBus.ObjectManager",
                                           "GetManagedObjects")) {
      reply = self->getManagedObjects(message);
    } else {
      reply = dbus_message_new_error(message, DBUS_ERROR_UNKNOWN_METHOD,
                                     "unknown test method");
    }

    if (reply != nullptr) {
      dbus_connection_send(connection, reply, nullptr);
      dbus_message_unref(reply);
      dbus_connection_flush(connection);
    }

    return DBUS_HANDLER_RESULT_HANDLED;
  }

  static auto echo(DBusConnection *, DBusMessage *request) -> DBusMessage * {
    DBusMessage *reply = dbus_message_new_method_return(request);
    if (reply == nullptr) {
      return nullptr;
    }

    DBusMessageIter in;
    DBusMessageIter out;
    dbus_message_iter_init(request, &in);
    dbus_message_iter_init_append(reply, &out);
    while (dbus_message_iter_get_arg_type(&in) != DBUS_TYPE_INVALID) {
      const int type = dbus_message_iter_get_arg_type(&in);
      if (type == DBUS_TYPE_STRING || type == DBUS_TYPE_OBJECT_PATH ||
          type == DBUS_TYPE_SIGNATURE) {
        const char *value{};
        dbus_message_iter_get_basic(&in, &value);
        if (!dbus_message_iter_append_basic(&out, type, &value)) {
          dbus_message_unref(reply);

          return nullptr;
        }
      } else if (type == DBUS_TYPE_UINT32) {
        dbus_uint32_t value{};
        dbus_message_iter_get_basic(&in, &value);
        if (!dbus_message_iter_append_basic(&out, type, &value)) {
          dbus_message_unref(reply);

          return nullptr;
        }
      } else if (type == DBUS_TYPE_BYTE) {
        unsigned char value{};
        dbus_message_iter_get_basic(&in, &value);
        if (!dbus_message_iter_append_basic(&out, type, &value)) {
          dbus_message_unref(reply);

          return nullptr;
        }
      } else if (type == DBUS_TYPE_BOOLEAN) {
        dbus_bool_t value{};
        dbus_message_iter_get_basic(&in, &value);
        if (!dbus_message_iter_append_basic(&out, type, &value)) {
          dbus_message_unref(reply);

          return nullptr;
        }
      } else if (type == DBUS_TYPE_INT16) {
        dbus_int16_t value{};
        dbus_message_iter_get_basic(&in, &value);
        if (!dbus_message_iter_append_basic(&out, type, &value)) {
          dbus_message_unref(reply);

          return nullptr;
        }
      } else if (type == DBUS_TYPE_UINT16) {
        dbus_uint16_t value{};
        dbus_message_iter_get_basic(&in, &value);
        if (!dbus_message_iter_append_basic(&out, type, &value)) {
          dbus_message_unref(reply);

          return nullptr;
        }
      } else if (type == DBUS_TYPE_INT32) {
        dbus_int32_t value{};
        dbus_message_iter_get_basic(&in, &value);
        if (!dbus_message_iter_append_basic(&out, type, &value)) {
          dbus_message_unref(reply);

          return nullptr;
        }
      } else if (type == DBUS_TYPE_INT64) {
        dbus_int64_t value{};
        dbus_message_iter_get_basic(&in, &value);
        if (!dbus_message_iter_append_basic(&out, type, &value)) {
          dbus_message_unref(reply);

          return nullptr;
        }
      } else if (type == DBUS_TYPE_UINT64) {
        dbus_uint64_t value{};
        dbus_message_iter_get_basic(&in, &value);
        if (!dbus_message_iter_append_basic(&out, type, &value)) {
          dbus_message_unref(reply);

          return nullptr;
        }
      } else if (type == DBUS_TYPE_DOUBLE) {
        double value{};
        dbus_message_iter_get_basic(&in, &value);
        if (!dbus_message_iter_append_basic(&out, type, &value)) {
          dbus_message_unref(reply);

          return nullptr;
        }
      } else if (type == DBUS_TYPE_ARRAY &&
                 dbus_message_iter_get_element_type(&in) == DBUS_TYPE_BYTE) {
        DBusMessageIter arrayIn;
        dbus_message_iter_recurse(&in, &arrayIn);
        unsigned char *bytes{};
        int length{};
        dbus_message_iter_get_fixed_array(&arrayIn, &bytes, &length);
        DBusMessageIter arrayOut;
        if (!dbus_message_iter_open_container(
                &out, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE_AS_STRING, &arrayOut) ||
            (length > 0 &&
             [&] {
               const unsigned char *constBytes = bytes;

               return !dbus_message_iter_append_fixed_array(
                   &arrayOut, DBUS_TYPE_BYTE, &constBytes, length);
             }()) ||
            !dbus_message_iter_close_container(&out, &arrayOut)) {
          dbus_message_unref(reply);

          return nullptr;
        }
      } else {
        dbus_message_unref(reply);

        return nullptr;
      }

      if (!dbus_message_iter_next(&in)) {
        break;
      }
    }

    return reply;
  }

  static auto acceptObjectManagerShape(DBusMessage *request) -> DBusMessage * {
    const char *signature = dbus_message_get_signature(request);
    if (signature == nullptr ||
        std::string_view{signature} != "a{oa{sa{sv}}}") {
      return dbus_message_new_error(request, DBUS_ERROR_INVALID_ARGS,
                                    "unexpected ObjectManager signature");
    }

    DBusMessage *reply = dbus_message_new_method_return(request);
    DBusMessageIter out;
    dbus_message_iter_init_append(reply, &out);
    const dbus_bool_t accepted = TRUE;
    if (!dbus_message_iter_append_basic(&out, DBUS_TYPE_BOOLEAN, &accepted)) {
      dbus_message_unref(reply);

      return nullptr;
    }

    return reply;
  }

  static auto acceptStructVariant(DBusMessage *request) -> DBusMessage * {
    const char *signature = dbus_message_get_signature(request);
    if (signature == nullptr || std::string_view{signature} != "(is)v") {
      return dbus_message_new_error(request, DBUS_ERROR_INVALID_ARGS,
                                    "unexpected struct/variant signature");
    }

    DBusMessage *reply = dbus_message_new_method_return(request);
    DBusMessageIter out;
    DBusMessageIter structure;
    DBusMessageIter variant;
    dbus_int32_t number = 17;
    const char *text = "struct-field";
    dbus_uint32_t variantValue = 42;
    dbus_message_iter_init_append(reply, &out);
    if (!dbus_message_iter_open_container(&out, DBUS_TYPE_STRUCT, nullptr,
                                          &structure) ||
        !dbus_message_iter_append_basic(&structure, DBUS_TYPE_INT32, &number) ||
        !dbus_message_iter_append_basic(&structure, DBUS_TYPE_STRING, &text) ||
        !dbus_message_iter_close_container(&out, &structure) ||
        !dbus_message_iter_open_container(
            &out, DBUS_TYPE_VARIANT, DBUS_TYPE_UINT32_AS_STRING, &variant) ||
        !dbus_message_iter_append_basic(&variant, DBUS_TYPE_UINT32,
                                        &variantValue) ||
        !dbus_message_iter_close_container(&out, &variant)) {
      dbus_message_unref(reply);

      return nullptr;
    }

    return reply;
  }

  auto getProperty(DBusMessage *request) -> DBusMessage * {
    const char *interface{};
    const char *property{};
    if (!dbus_message_get_args(request, nullptr, DBUS_TYPE_STRING, &interface,
                               DBUS_TYPE_STRING, &property,
                               DBUS_TYPE_INVALID) ||
        std::string_view{interface} != kInterface) {
      return dbus_message_new_error(request, DBUS_ERROR_UNKNOWN_PROPERTY,
                                    "unknown property");
    }

    if (std::string_view{property} == "EmptyReply") {
      return dbus_message_new_method_return(request);
    }

    if (std::string_view{property} == "WrongShape") {
      DBusMessage *reply = dbus_message_new_method_return(request);
      DBusMessageIter out;
      dbus_message_iter_init_append(reply, &out);
      const char *wrongType = "not a variant";
      if (!dbus_message_iter_append_basic(&out, DBUS_TYPE_STRING, &wrongType)) {
        dbus_message_unref(reply);

        return nullptr;
      }

      return reply;
    }

    if (std::string_view{property} != "Level") {
      return dbus_message_new_error(request, DBUS_ERROR_UNKNOWN_PROPERTY,
                                    "unknown property");
    }

    DBusMessage *reply = dbus_message_new_method_return(request);
    DBusMessageIter out;
    dbus_message_iter_init_append(reply, &out);
    DBusMessageIter variant;
    dbus_uint32_t value = m_level;
    if (!dbus_message_iter_open_container(
            &out, DBUS_TYPE_VARIANT, DBUS_TYPE_UINT32_AS_STRING, &variant) ||
        !dbus_message_iter_append_basic(&variant, DBUS_TYPE_UINT32, &value) ||
        !dbus_message_iter_close_container(&out, &variant)) {
      dbus_message_unref(reply);

      return nullptr;
    }

    return reply;
  }

  auto setProperty(DBusMessage *request) -> DBusMessage * {
    DBusMessageIter in;
    if (!dbus_message_iter_init(request, &in) ||
        dbus_message_iter_get_arg_type(&in) != DBUS_TYPE_STRING) {
      return dbus_message_new_error(request, DBUS_ERROR_INVALID_ARGS,
                                    "invalid property arguments");
    }

    const char *interface{};
    dbus_message_iter_get_basic(&in, &interface);
    if (!dbus_message_iter_next(&in) ||
        dbus_message_iter_get_arg_type(&in) != DBUS_TYPE_STRING) {
      return dbus_message_new_error(request, DBUS_ERROR_INVALID_ARGS,
                                    "missing property name");
    }

    const char *property{};
    dbus_message_iter_get_basic(&in, &property);
    if (!dbus_message_iter_next(&in) ||
        dbus_message_iter_get_arg_type(&in) != DBUS_TYPE_VARIANT ||
        std::string_view{interface} != kInterface) {
      return dbus_message_new_error(request, DBUS_ERROR_UNKNOWN_PROPERTY,
                                    "unknown property");
    }

    if (std::string_view{property} == "MalformedSet") {
      DBusMessage *reply = dbus_message_new_method_return(request);
      DBusMessageIter out;
      dbus_message_iter_init_append(reply, &out);
      const dbus_bool_t unexpectedValue = TRUE;
      if (!dbus_message_iter_append_basic(&out, DBUS_TYPE_BOOLEAN,
                                          &unexpectedValue)) {
        dbus_message_unref(reply);

        return nullptr;
      }

      return reply;
    }

    if (std::string_view{property} != "Level") {
      return dbus_message_new_error(request, DBUS_ERROR_UNKNOWN_PROPERTY,
                                    "unknown property");
    }

    DBusMessageIter variant;
    dbus_message_iter_recurse(&in, &variant);
    if (dbus_message_iter_get_arg_type(&variant) != DBUS_TYPE_UINT32) {
      return dbus_message_new_error(request, DBUS_ERROR_INVALID_ARGS,
                                    "Level must be uint32");
    }

    dbus_message_iter_get_basic(&variant, &m_level);

    return dbus_message_new_method_return(request);
  }

  auto getManagedObjects(DBusMessage *request) -> DBusMessage * {
    DBusMessage *reply = dbus_message_new_method_return(request);
    DBusMessageIter out;
    DBusMessageIter objects;
    DBusMessageIter object;
    DBusMessageIter interfaces;
    DBusMessageIter interfaceEntry;
    DBusMessageIter properties;
    DBusMessageIter property;
    DBusMessageIter variant;
    dbus_uint32_t level = m_level;
    const char *path = "/org/dmn/Test/device0";
    const char *interface = "org.bluez.Device1";
    const char *propertyName = "Connected";
    dbus_bool_t connected = TRUE;
    dbus_message_iter_init_append(reply, &out);
    if (!dbus_message_iter_open_container(&out, DBUS_TYPE_ARRAY, "{oa{sa{sv}}}",
                                          &objects) ||
        !dbus_message_iter_open_container(&objects, DBUS_TYPE_DICT_ENTRY,
                                          nullptr, &object) ||
        !dbus_message_iter_append_basic(&object, DBUS_TYPE_OBJECT_PATH,
                                        &path) ||
        !dbus_message_iter_open_container(&object, DBUS_TYPE_ARRAY, "{sa{sv}}",
                                          &interfaces) ||
        !dbus_message_iter_open_container(&interfaces, DBUS_TYPE_DICT_ENTRY,
                                          nullptr, &interfaceEntry) ||
        !dbus_message_iter_append_basic(&interfaceEntry, DBUS_TYPE_STRING,
                                        &interface) ||
        !dbus_message_iter_open_container(&interfaceEntry, DBUS_TYPE_ARRAY,
                                          "{sv}", &properties) ||
        !dbus_message_iter_open_container(&properties, DBUS_TYPE_DICT_ENTRY,
                                          nullptr, &property) ||
        !dbus_message_iter_append_basic(&property, DBUS_TYPE_STRING,
                                        &propertyName) ||
        !dbus_message_iter_open_container(&property, DBUS_TYPE_VARIANT,
                                          DBUS_TYPE_BOOLEAN_AS_STRING,
                                          &variant) ||
        !dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN,
                                        &connected) ||
        !dbus_message_iter_close_container(&property, &variant) ||
        !dbus_message_iter_close_container(&properties, &property) ||
        !dbus_message_iter_close_container(&interfaceEntry, &properties) ||
        !dbus_message_iter_close_container(&interfaces, &interfaceEntry) ||
        !dbus_message_iter_close_container(&object, &interfaces) ||
        !dbus_message_iter_close_container(&objects, &object) ||
        !dbus_message_iter_close_container(&out, &objects)) {
      dbus_message_unref(reply);

      return nullptr;
    }

    (void)level;

    return reply;
  }

  void emitSignal(DBusConnection *connection) {
    emitSignalAt(connection, kObjectPath, kInterface);
  }

  void emitSignalAt(DBusConnection *connection, const char *path,
                    const char *interface) {
    DBusMessage *signal = dbus_message_new_signal(path, interface, "Changed");
    if (signal == nullptr) {
      return;
    }

    const char *value = "ready";
    dbus_message_append_args(signal, DBUS_TYPE_STRING, &value,
                             DBUS_TYPE_INVALID);
    dbus_connection_send(connection, signal, nullptr);
    dbus_message_unref(signal);
  }

  void emitSignalBurst(DBusConnection *connection, std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
      emitIndexedSignal(connection, static_cast<dbus_uint32_t>(index));
    }
  }

  void emitIndexedSignal(DBusConnection *connection, dbus_uint32_t index) {
    DbusMessagePtr signal{
        dbus_message_new_signal(kObjectPath, kInterface, "Changed")};
    if (signal == nullptr ||
        !dbus_message_append_args(signal.get(), DBUS_TYPE_UINT32, &index,
                                  DBUS_TYPE_INVALID)) {
      return;
    }

    dbus_connection_send(connection, signal.get(), nullptr);
    dbus_connection_flush(connection);
  }

  void dispatchLoop() {
    while (!m_stopping.load()) {
      if (!dbus_connection_read_write_dispatch(m_connection.get(), 25)) {
        break;
      }
    }
  }

  DbusConnectionPtr m_connection;
  std::atomic_bool m_stopping{};
  std::thread m_worker;
  dbus_uint32_t m_level{7};
};

class DbusClientTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(m_bus.valid());
    ASSERT_TRUE(m_bus.setAsSessionBus());
    m_service = std::make_unique<FakeDbusService>(m_bus.address());
    dmn::Dmn_DbusClientConfig config;
    config.bus_kind = dmn::Dmn_DbusBusKind::Address;
    config.bus_address = m_bus.address();
    config.limits.method_timeout = 500ms;
    m_client = std::make_unique<dmn::Dmn_DbusClient>(config);
  }

  void TearDown() override {
    m_client.reset();
    m_service.reset();
  }

  PrivateBus m_bus;
  std::unique_ptr<FakeDbusService> m_service;
  std::unique_ptr<dmn::Dmn_DbusClient> m_client;
};

TEST(DbusClientInitializationTest, ConcurrentClientsAndLegacyEndpoints) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  ASSERT_TRUE(bus.setAsSessionBus());
  std::mutex failuresMutex;
  std::vector<std::string> failures;
  std::vector<std::thread> constructors;
  for (std::size_t index = 0; index < 4; ++index) {
    constructors.emplace_back([&bus, &failures, &failuresMutex] {
      try {
        dmn::Dmn_DbusClientConfig clientConfig;
        clientConfig.bus_kind = dmn::Dmn_DbusBusKind::Address;
        clientConfig.bus_address = bus.address();
        dmn::Dmn_DbusClient client{clientConfig};

        dmn::Dmn_DbusConfig endpointConfig;
        endpointConfig.bus_address = bus.address();
        dmn::Dmn_DbusInput input{endpointConfig};
        dmn::Dmn_DbusOutput output{endpointConfig};
      } catch (const std::exception &error) {
        std::lock_guard lock{failuresMutex};
        failures.emplace_back(error.what());
      } catch (...) {
        std::lock_guard lock{failuresMutex};
        failures.emplace_back("unknown construction failure");
      }
    });
  }

  for (auto &constructor : constructors) {
    constructor.join();
  }

  for (const auto &failure : failures) {
    ADD_FAILURE() << failure;
  }
}

TEST(DbusClientInitializationTest,
     SystemBusSelectionUsesPrivateDaemonOverride) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  const char *previousAddress = std::getenv("DBUS_SYSTEM_BUS_ADDRESS");
  const bool hadPreviousAddress = previousAddress != nullptr;
  const std::string savedAddress =
      previousAddress == nullptr ? std::string{} : previousAddress;
  ASSERT_EQ(setenv("DBUS_SYSTEM_BUS_ADDRESS", bus.address().c_str(), 1), 0);
  struct RestoreSystemBusAddress {
    bool hadPrevious;
    std::string previous;
    ~RestoreSystemBusAddress() {
      if (hadPrevious) {
        (void)setenv("DBUS_SYSTEM_BUS_ADDRESS", previous.c_str(), 1);
      } else {
        (void)unsetenv("DBUS_SYSTEM_BUS_ADDRESS");
      }
    }
  } restore{hadPreviousAddress, savedAddress};

  dmn::Dmn_DbusClientConfig config;
  config.bus_kind = dmn::Dmn_DbusBusKind::System;
  dmn::Dmn_DbusClient client{config};
  ASSERT_TRUE(client.status().connected);
  const auto id = client.call("org.freedesktop.DBus", "/org/freedesktop/DBus",
                              "org.freedesktop.DBus", "GetId");
  ASSERT_TRUE(id) << id.error().message;
  ASSERT_EQ(id->size(), 1U);
  EXPECT_EQ(id->front().signature(), "s");
  EXPECT_FALSE(id->front().asString().empty());
}

TEST_F(DbusClientTest, CallsMethodsAndPreservesPrimitiveTypesAndBytes) {
  const std::vector<dmn::Dmn_DbusValue> args{
      dmn::Dmn_DbusValue{true},
      dmn::Dmn_DbusValue{std::uint8_t{255}},
      dmn::Dmn_DbusValue{std::int16_t{-123}},
      dmn::Dmn_DbusValue{std::uint16_t{65000}},
      dmn::Dmn_DbusValue{std::int32_t{-123456}},
      dmn::Dmn_DbusValue{std::string{"hello"}},
      dmn::Dmn_DbusValue{std::uint32_t{42}},
      dmn::Dmn_DbusValue{std::int64_t{-1234567890}},
      dmn::Dmn_DbusValue{std::uint64_t{1234567890}},
      dmn::Dmn_DbusValue{1.25},
      dmn::Dmn_DbusValue::objectPath("/org/dmn/Test/device0"),
      dmn::Dmn_DbusValue::signatureValue("a{sv}"),
      dmn::Dmn_DbusValue::byteArray({0, 1, 127, 128, 255})};
  auto result =
      m_client->call(kServiceName, kObjectPath, kInterface, "Echo", args);
  ASSERT_TRUE(result.has_value()) << result.error().message;
  ASSERT_EQ(result->size(), args.size());
  EXPECT_TRUE((*result)[0].asBool());
  EXPECT_EQ((*result)[1].asByte(), 255U);
  EXPECT_EQ((*result)[2].asInt16(), -123);
  EXPECT_EQ((*result)[3].asUint16(), 65000U);
  EXPECT_EQ((*result)[4].asInt32(), -123456);
  EXPECT_EQ((*result)[5].asString(), "hello");
  EXPECT_EQ((*result)[6].asUint32(), 42U);
  EXPECT_EQ((*result)[7].asInt64(), -1234567890);
  EXPECT_EQ((*result)[8].asUint64(), 1234567890U);
  EXPECT_DOUBLE_EQ((*result)[9].asDouble(), 1.25);
  EXPECT_EQ((*result)[10].asObjectPath(), "/org/dmn/Test/device0");
  EXPECT_EQ((*result)[11].asSignatureValue(), "a{sv}");
  EXPECT_EQ((*result)[12].asByteArray(),
            (dmn::Dmn_DbusValue::ByteArray{0, 1, 127, 128, 255}));
  EXPECT_EQ((*result)[0].signature(), "b");
  EXPECT_EQ((*result)[1].signature(), "y");
  EXPECT_EQ((*result)[5].signature(), "s");
  EXPECT_EQ((*result)[10].signature(), "o");
  EXPECT_EQ((*result)[11].signature(), "g");
  EXPECT_EQ((*result)[12].signature(), "ay");
}

TEST_F(DbusClientTest, PreservesRemoteErrorNameAndMessage) {
  auto result = m_client->call(kServiceName, kObjectPath, kInterface, "Fail");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().kind, dmn::Dmn_DbusErrorKind::Remote);
  EXPECT_EQ(result.error().remote_name, "org.dmn.Test.Failed");
  EXPECT_EQ(result.error().message, "intentional remote failure");
}

TEST_F(DbusClientTest, MethodTimeoutIsFiniteAndDistinctFromRemoteFailure) {
  auto result =
      m_client->call(kServiceName, kObjectPath, kInterface, "Delay", {}, 25ms);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().kind, dmn::Dmn_DbusErrorKind::Timeout);

  const auto start = std::chrono::steady_clock::now();
  m_client->shutdown();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_LT(elapsed, 200ms);
}

TEST_F(DbusClientTest, AsyncCallValidatesTimeoutAndCallNames) {
  auto invalidTimeout =
      m_client
          ->callAsync(kServiceName, kObjectPath, kInterface, "Echo", {}, -1ms)
          .get();
  ASSERT_FALSE(invalidTimeout);
  EXPECT_EQ(invalidTimeout.error().kind,
            dmn::Dmn_DbusErrorKind::InvalidArgument);

  auto invalidName =
      m_client->callAsync(kServiceName, "not/a/path", kInterface, "Echo");
  ASSERT_EQ(invalidName.waitFor(0ms), std::future_status::ready);
  auto invalidResult = invalidName.get();
  ASSERT_FALSE(invalidResult);
  EXPECT_EQ(invalidResult.error().kind,
            dmn::Dmn_DbusErrorKind::InvalidArgument);

  auto defaultTimeout =
      m_client->callAsync(kServiceName, kObjectPath, kInterface, "Delay");
  ASSERT_TRUE(defaultTimeout.get());
}

TEST_F(DbusClientTest, GetsAndSetsTypedPropertiesThroughPropertiesInterface) {
  auto initial =
      m_client->getProperty(kServiceName, kObjectPath, kInterface, "Level");
  ASSERT_TRUE(initial.has_value()) << initial.error().message;
  EXPECT_EQ(initial->asUint32(), 7U);
  auto set =
      m_client->setProperty(kServiceName, kObjectPath, kInterface, "Level",
                            dmn::Dmn_DbusValue{std::uint32_t{19}});
  ASSERT_TRUE(set.has_value()) << set.error().message;
  auto changed =
      m_client->getProperty(kServiceName, kObjectPath, kInterface, "Level");
  ASSERT_TRUE(changed.has_value()) << changed.error().message;
  EXPECT_EQ(changed->asUint32(), 19U);

  auto missing =
      m_client->getProperty(kServiceName, kObjectPath, kInterface, "Missing");
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().kind, dmn::Dmn_DbusErrorKind::Remote);
  EXPECT_EQ(missing.error().remote_name, DBUS_ERROR_UNKNOWN_PROPERTY);

  auto wrongType =
      m_client->setProperty(kServiceName, kObjectPath, kInterface, "Level",
                            dmn::Dmn_DbusValue{"not uint32"});
  ASSERT_FALSE(wrongType);
  EXPECT_EQ(wrongType.error().kind, dmn::Dmn_DbusErrorKind::Remote);
  EXPECT_EQ(wrongType.error().remote_name, DBUS_ERROR_INVALID_ARGS);

  for (const auto *property : {"EmptyReply", "WrongShape"}) {
    auto malformed =
        m_client->getProperty(kServiceName, kObjectPath, kInterface, property);
    ASSERT_FALSE(malformed);
    EXPECT_EQ(malformed.error().kind, dmn::Dmn_DbusErrorKind::Codec);
  }

  auto malformedSet =
      m_client->setProperty(kServiceName, kObjectPath, kInterface,
                            "MalformedSet", dmn::Dmn_DbusValue{true});
  ASSERT_FALSE(malformedSet);
  EXPECT_EQ(malformedSet.error().kind, dmn::Dmn_DbusErrorKind::Codec);
}

TEST_F(DbusClientTest, DeliversOwnedSignalsOffDispatchThreadAndCancels) {
  std::promise<std::vector<dmn::Dmn_DbusValue>> delivered;
  auto deliveredFuture = delivered.get_future();
  const auto dispatchThread = m_client->dispatchThreadId();
  dmn::Dmn_Proc::ThreadId callbackThread;
  std::atomic_size_t callbackCount{};
  dmn::Dmn_DbusSignalMatch match;
  match.sender = kServiceName;
  match.interface = kInterface;
  match.member = "Changed";
  match.path = kObjectPath;
  auto subscription =
      m_client->subscribe(match, [&delivered, &callbackThread, &callbackCount](
                                     std::vector<dmn::Dmn_DbusValue> values) {
        callbackThread = std::this_thread::get_id();
        if (callbackCount.fetch_add(1) == 0) {
          delivered.set_value(std::move(values));
        }
      });
  ASSERT_TRUE(subscription.has_value()) << subscription.error().message;
  auto emitted =
      m_client->call(kServiceName, kObjectPath, kInterface, "EmitSignal");
  ASSERT_TRUE(emitted.has_value()) << emitted.error().message;
  ASSERT_EQ(deliveredFuture.wait_for(1s), std::future_status::ready);
  const auto values = deliveredFuture.get();
  ASSERT_EQ(values.size(), 1U);
  EXPECT_EQ(values[0].asString(), "ready");
  EXPECT_NE(callbackThread, dispatchThread);
  auto foreign = m_client->call(kServiceName, kObjectPath, kInterface,
                                "EmitForeignSignal");
  ASSERT_TRUE(foreign.has_value()) << foreign.error().message;
  std::this_thread::sleep_for(20ms);
  EXPECT_EQ(callbackCount.load(), 1U);
  subscription->cancel();
  auto emittedAgain =
      m_client->call(kServiceName, kObjectPath, kInterface, "EmitSignal");
  ASSERT_TRUE(emittedAgain.has_value()) << emittedAgain.error().message;
  std::this_thread::sleep_for(20ms);
  EXPECT_EQ(callbackCount.load(), 1U);
}

TEST_F(DbusClientTest, RejectsIncompleteAndConflictingSignalMatches) {
  const auto expectInvalid = [this](const dmn::Dmn_DbusSignalMatch &match) {
    auto result =
        m_client->subscribe(match, [](std::vector<dmn::Dmn_DbusValue>) {});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, dmn::Dmn_DbusErrorKind::InvalidArgument);
  };

  dmn::Dmn_DbusSignalMatch match;
  match.interface = kInterface;
  expectInvalid(match);
  match.interface.clear();
  match.member = "Changed";
  expectInvalid(match);

  match.interface = kInterface;
  match.path = kObjectPath;
  match.path_namespace = "/org/dmn";
  expectInvalid(match);

  auto noCallback = m_client->subscribe(match, {});
  ASSERT_FALSE(noCallback);
  EXPECT_EQ(noCallback.error().kind, dmn::Dmn_DbusErrorKind::InvalidArgument);

  auto noPathCallback = m_client->subscribeWithPath(match, {});
  ASSERT_FALSE(noPathCallback);
  EXPECT_EQ(noPathCallback.error().kind,
            dmn::Dmn_DbusErrorKind::InvalidArgument);
}

TEST_F(DbusClientTest, DrainsSignalCallbacksQueuedBeforeTheBarrier) {
  std::atomic_bool callbackCompleted{};
  dmn::Dmn_DbusSignalMatch match;
  match.sender = kServiceName;
  match.interface = kInterface;
  match.member = "Changed";
  auto subscription = m_client->subscribe(
      match, [&callbackCompleted](std::vector<dmn::Dmn_DbusValue>) {
        callbackCompleted = true;
      });
  ASSERT_TRUE(subscription) << subscription.error().message;

  const auto emitted =
      m_client->call(kServiceName, kObjectPath, kInterface, "EmitSignal");
  ASSERT_TRUE(emitted) << emitted.error().message;
  const auto drained = m_client->drainSignalCallbacks();

  ASSERT_TRUE(drained) << drained.error().message;
  EXPECT_TRUE(callbackCompleted.load());
}

TEST_F(DbusClientTest, DrainFromCallbackWorkerReturnsInvalidArgument) {
  std::promise<dmn::Dmn_DbusResult<void>> resultPromise;
  auto resultFuture = resultPromise.get_future();
  dmn::Dmn_DbusSignalMatch match;
  match.interface = kInterface;
  match.member = "Changed";
  auto subscription = m_client->subscribe(
      match, [this, &resultPromise](std::vector<dmn::Dmn_DbusValue>) {
        resultPromise.set_value(m_client->drainSignalCallbacks());
      });
  ASSERT_TRUE(subscription) << subscription.error().message;

  auto emitted =
      m_client->call(kServiceName, kObjectPath, kInterface, "EmitSignal");
  ASSERT_TRUE(emitted) << emitted.error().message;
  ASSERT_EQ(resultFuture.wait_for(1s), std::future_status::ready);
  auto result = resultFuture.get();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, dmn::Dmn_DbusErrorKind::InvalidArgument);
}

TEST_F(DbusClientTest,
       SubscribeWithPathRetainsPathAndAppliesNamespaceBoundary) {
  using Signal = std::pair<std::string, std::vector<dmn::Dmn_DbusValue>>;
  std::promise<Signal> delivered;
  auto deliveredFuture = delivered.get_future();
  std::atomic_size_t callbackCount{};

  dmn::Dmn_DbusSignalMatch match;
  match.sender = kServiceName;
  match.interface = kInterface;
  match.member = "Changed";
  match.path_namespace = "/org/dmn";
  auto subscription = m_client->subscribeWithPath(
      match, [&delivered, &callbackCount](
                 std::string path, std::vector<dmn::Dmn_DbusValue> args) {
        if (callbackCount.fetch_add(1) == 0) {
          delivered.set_value({std::move(path), std::move(args)});
        }
      });
  ASSERT_TRUE(subscription) << subscription.error().message;

  auto sibling = m_client->call(kServiceName, kObjectPath, kInterface,
                                "EmitSiblingSignal");
  ASSERT_TRUE(sibling) << sibling.error().message;
  ASSERT_TRUE(m_client->drainSignalCallbacks());
  EXPECT_EQ(callbackCount.load(), 0U);

  auto emitted = m_client->call(kServiceName, kObjectPath, kInterface,
                                "EmitForeignSignal");
  ASSERT_TRUE(emitted) << emitted.error().message;
  ASSERT_EQ(deliveredFuture.wait_for(1s), std::future_status::ready);
  const auto signal = deliveredFuture.get();
  EXPECT_EQ(signal.first, "/org/dmn/Other");
  ASSERT_EQ(signal.second.size(), 1U);
  EXPECT_EQ(signal.second.front().asString(), "ready");
}

TEST_F(DbusClientTest, OverlappingSubscriptionsPreservePerSubscriptionOrder) {
  std::mutex mutex;
  std::vector<std::uint32_t> firstValues;
  std::vector<std::uint32_t> secondValues;
  dmn::Dmn_DbusSignalMatch match;
  match.sender = kServiceName;
  match.interface = kInterface;
  match.member = "Changed";
  auto first = m_client->subscribe(
      match, [&mutex, &firstValues](std::vector<dmn::Dmn_DbusValue> args) {
        std::lock_guard lock{mutex};
        firstValues.push_back(args.front().asUint32());
      });
  auto second = m_client->subscribe(
      match, [&mutex, &secondValues](std::vector<dmn::Dmn_DbusValue> args) {
        std::lock_guard lock{mutex};
        secondValues.push_back(args.front().asUint32());
      });
  ASSERT_TRUE(first) << first.error().message;
  ASSERT_TRUE(second) << second.error().message;

  auto emitted =
      m_client->call(kServiceName, kObjectPath, kInterface, "EmitSignalBurst");
  ASSERT_TRUE(emitted) << emitted.error().message;
  ASSERT_TRUE(m_client->drainSignalCallbacks());
  EXPECT_EQ(firstValues, (std::vector<std::uint32_t>{0, 1, 2, 3}));
  EXPECT_EQ(secondValues, (std::vector<std::uint32_t>{0, 1, 2, 3}));
}

TEST_F(DbusClientTest, DecodesObjectManagerShapedNestedValues) {
  auto result =
      m_client->call(kServiceName, kObjectPath,
                     "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
  ASSERT_TRUE(result.has_value()) << result.error().message;
  ASSERT_EQ(result->size(), 1U);
  EXPECT_EQ((*result)[0].signature(), "a{oa{sa{sv}}}");
  const auto &objects = (*result)[0].asDictionary();
  ASSERT_EQ(objects.entries.size(), 1U);
  EXPECT_EQ(objects.entries[0].first.asObjectPath(), "/org/dmn/Test/device0");
  const auto &interfaces = objects.entries[0].second.asDictionary();
  ASSERT_EQ(interfaces.entries.size(), 1U);
  EXPECT_EQ(interfaces.entries[0].first.asString(), "org.bluez.Device1");
  const auto &properties = interfaces.entries[0].second.asDictionary();
  ASSERT_EQ(properties.entries.size(), 1U);
  EXPECT_EQ(properties.entries[0].first.asString(), "Connected");
  EXPECT_TRUE(properties.entries[0].second.asVariant().asBool());
}

TEST_F(DbusClientTest, EncodesNestedObjectManagerValuesForBluezCalls) {
  using Value = dmn::Dmn_DbusValue;
  auto properties = Value::dictionary(
      "s", "v",
      {{Value{"Connected"}, Value::variant(Value{true})},
       {Value{"UUIDs"},
        Value::variant(Value::array("s", {Value{"180f"}, Value{"180a"}}))}});
  auto interfaces = Value::dictionary(
      "s", "a{sv}", {{Value{"org.bluez.Device1"}, std::move(properties)}});
  auto objects = Value::dictionary(
      "o", "a{sa{sv}}",
      {{Value::objectPath("/org/bluez/hci0/dev_00_11_22_33_44_55"),
        std::move(interfaces)}});
  ASSERT_EQ(objects.signature(), "a{oa{sa{sv}}}");
  auto result = m_client->call(kServiceName, kObjectPath, kInterface,
                               "AcceptObjectManagerShape", {objects});
  ASSERT_TRUE(result.has_value()) << result.error().message;
  ASSERT_EQ(result->size(), 1U);
  EXPECT_TRUE((*result)[0].asBool());
}

TEST_F(DbusClientTest, EncodesAndDecodesStructAndVariantValues) {
  using Value = dmn::Dmn_DbusValue;
  const auto structure =
      Value::structure({Value{std::int32_t{17}}, Value{"struct-field"}});
  const auto variant = Value::variant(Value{std::uint32_t{42}});
  EXPECT_EQ(structure.signature(), "(is)");
  EXPECT_EQ(variant.signature(), "v");

  auto result = m_client->call(kServiceName, kObjectPath, kInterface,
                               "AcceptStructVariant", {structure, variant});
  ASSERT_TRUE(result) << result.error().message;
  ASSERT_EQ(result->size(), 2U);
  const auto &fields = (*result)[0].asStruct().fields;
  ASSERT_EQ(fields.size(), 2U);
  EXPECT_EQ(fields[0].asInt32(), 17);
  EXPECT_EQ(fields[1].asString(), "struct-field");
  EXPECT_EQ((*result)[1].asVariant().asUint32(), 42U);
}

TEST_F(DbusClientTest, AsyncCallsAreBoundedAndCompleteFromDispatchOwner) {
  auto config = dmn::Dmn_DbusClientConfig{};
  config.bus_kind = dmn::Dmn_DbusBusKind::Address;
  config.bus_address = m_bus.address();
  config.limits.max_in_flight_calls = 1;
  config.limits.method_timeout = 500ms;
  dmn::Dmn_DbusClient client{config};
  auto pending =
      client.callAsync(kServiceName, kObjectPath, kInterface, "Delay");
  EXPECT_EQ(client.status().pending_calls, 1U);
  auto saturated =
      client.callAsync(kServiceName, kObjectPath, kInterface, "Echo");
  ASSERT_TRUE(saturated.waitFor(100ms) == std::future_status::ready);
  auto rejected = saturated.get();
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(rejected.error().kind, dmn::Dmn_DbusErrorKind::ResourceLimit);
  auto completed = pending.get();
  ASSERT_TRUE(completed.has_value()) << completed.error().message;
  EXPECT_EQ(client.status().pending_calls, 0U);
}

TEST_F(DbusClientTest, QueuedCommandLimitBoundsCallsAndSubscriptions) {
  auto config = dmn::Dmn_DbusClientConfig{};
  config.bus_kind = dmn::Dmn_DbusBusKind::Address;
  config.bus_address = m_bus.address();
  config.limits.max_in_flight_calls = 8;
  config.limits.max_queued_commands = 1;
  config.limits.method_timeout = 500ms;
  dmn::Dmn_DbusClient client{config};

  auto pending =
      client.callAsync(kServiceName, kObjectPath, kInterface, "Delay");
  auto rejected =
      client.callAsync(kServiceName, kObjectPath, kInterface, "Echo");
  ASSERT_EQ(rejected.waitFor(0ms), std::future_status::ready);
  auto rejectedResult = rejected.get();
  ASSERT_FALSE(rejectedResult);
  EXPECT_EQ(rejectedResult.error().kind, dmn::Dmn_DbusErrorKind::ResourceLimit);
  ASSERT_TRUE(pending.get());

  dmn::Dmn_DbusSignalMatch match;
  match.interface = kInterface;
  match.member = "Changed";
  auto subscription =
      client.subscribe(match, [](std::vector<dmn::Dmn_DbusValue>) {});
  ASSERT_TRUE(subscription) << subscription.error().message;
  auto overLimitSubscription =
      client.subscribe(match, [](std::vector<dmn::Dmn_DbusValue>) {});
  ASSERT_FALSE(overLimitSubscription);
  EXPECT_EQ(overLimitSubscription.error().kind,
            dmn::Dmn_DbusErrorKind::ResourceLimit);
}

TEST_F(DbusClientTest, EnforcesExactAndExceededValueAndMessageLimits) {
  dmn::Dmn_DbusClientConfig exactConfig;
  exactConfig.bus_kind = dmn::Dmn_DbusBusKind::Address;
  exactConfig.bus_address = m_bus.address();
  exactConfig.limits.method_timeout = 500ms;
  exactConfig.limits.max_values = 1;
  dmn::Dmn_DbusClient exactValueClient{exactConfig};
  auto atValueLimit =
      exactValueClient.call(kServiceName, kObjectPath, kInterface, "Echo",
                            {dmn::Dmn_DbusValue{"one value"}});
  ASSERT_TRUE(atValueLimit) << atValueLimit.error().message;

  auto overValueLimit = exactValueClient.call(
      kServiceName, kObjectPath, kInterface, "Echo",
      {dmn::Dmn_DbusValue{"one"}, dmn::Dmn_DbusValue{"two"}});
  ASSERT_FALSE(overValueLimit);
  EXPECT_EQ(overValueLimit.error().kind, dmn::Dmn_DbusErrorKind::ResourceLimit);

  exactConfig.limits.max_values = 2;
  dmn::Dmn_DbusClient limitedReplyClient{exactConfig};
  auto overReplyValueLimit = limitedReplyClient.call(
      kServiceName, kObjectPath, "org.freedesktop.DBus.ObjectManager",
      "GetManagedObjects");
  ASSERT_FALSE(overReplyValueLimit);
  EXPECT_EQ(overReplyValueLimit.error().kind,
            dmn::Dmn_DbusErrorKind::ResourceLimit);

  exactConfig.limits.max_values = 16;
  const std::string payload{"x"};
  const auto exactMessageSize = encodedEchoCallSize(payload);
  exactConfig.limits.max_values = 16;
  exactConfig.limits.max_message_bytes = exactMessageSize;
  dmn::Dmn_DbusClient exactMessageClient{exactConfig};
  auto atMessageLimit =
      exactMessageClient.call(kServiceName, kObjectPath, kInterface, "Echo",
                              {dmn::Dmn_DbusValue{payload}});
  ASSERT_TRUE(atMessageLimit) << atMessageLimit.error().message;

  exactConfig.limits.max_message_bytes = exactMessageSize - 1;
  dmn::Dmn_DbusClient smallMessageClient{exactConfig};
  auto overMessageLimit =
      smallMessageClient.call(kServiceName, kObjectPath, kInterface, "Echo",
                              {dmn::Dmn_DbusValue{payload}});
  ASSERT_FALSE(overMessageLimit);
  EXPECT_EQ(overMessageLimit.error().kind,
            dmn::Dmn_DbusErrorKind::ResourceLimit);
}

TEST_F(DbusClientTest, ShutdownCompletesOutstandingCallsWithShutdownError) {
  dmn::Dmn_DbusClientConfig config;
  config.bus_kind = dmn::Dmn_DbusBusKind::Address;
  config.bus_address = m_bus.address();
  config.limits.method_timeout = 2s;
  auto client = std::make_unique<dmn::Dmn_DbusClient>(config);
  auto pending =
      client->callAsync(kServiceName, kObjectPath, kInterface, "Delay");
  client->shutdown();
  EXPECT_TRUE(client->status().shutting_down);
  EXPECT_FALSE(client->status().connected);
  EXPECT_EQ(client->status().pending_calls, 0U);
  client.reset();
  ASSERT_EQ(pending.waitFor(1s), std::future_status::ready);
  auto result = pending.get();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().kind, dmn::Dmn_DbusErrorKind::Shutdown);
}

TEST_F(DbusClientTest, ShutdownIsIdempotentAndRejectsNewOperations) {
  const auto dispatchThread = m_client->dispatchThreadId();
  EXPECT_NE(dispatchThread, dmn::Dmn_Proc::ThreadId{});
  EXPECT_TRUE(m_client->status().connected);

  m_client->shutdown();
  EXPECT_EQ(m_client->dispatchThreadId(), dispatchThread);
  m_client->shutdown();
  const auto status = m_client->status();
  EXPECT_TRUE(status.shutting_down);
  EXPECT_FALSE(status.connected);
  EXPECT_EQ(status.pending_calls, 0U);

  auto afterShutdown =
      m_client->callAsync(kServiceName, kObjectPath, kInterface, "Echo");
  ASSERT_EQ(afterShutdown.waitFor(0ms), std::future_status::ready);
  auto result = afterShutdown.get();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, dmn::Dmn_DbusErrorKind::Shutdown);
  EXPECT_EQ(m_client->drainSignalCallbacks().error().kind,
            dmn::Dmn_DbusErrorKind::Shutdown);
}

TEST_F(DbusClientTest, CancelsOutstandingCallAndDiscardsLateReply) {
  auto pending =
      m_client->callAsync(kServiceName, kObjectPath, kInterface, "Delay");
  pending.cancel();
  auto cancelled = pending.get();
  ASSERT_FALSE(cancelled.has_value());
  EXPECT_EQ(cancelled.error().kind, dmn::Dmn_DbusErrorKind::Cancelled);

  std::this_thread::sleep_for(200ms);
  auto following = m_client->call(kServiceName, kObjectPath, kInterface, "Echo",
                                  {dmn::Dmn_DbusValue{"after-cancel"}});
  ASSERT_TRUE(following.has_value()) << following.error().message;
  ASSERT_EQ(following->size(), 1U);
  EXPECT_EQ((*following)[0].asString(), "after-cancel");
}

TEST_F(DbusClientTest, CancelledCallsAreRemovedFromPendingRegistry) {
  dmn::Dmn_DbusClientConfig config;
  config.bus_kind = dmn::Dmn_DbusBusKind::Address;
  config.bus_address = m_bus.address();
  auto client = std::make_unique<dmn::Dmn_DbusClient>(config);

  auto pending =
      client->callAsync(kServiceName, kObjectPath, kInterface, "Delay");
  pending.cancel();
  ASSERT_FALSE(pending.get().has_value());

  const auto start = std::chrono::steady_clock::now();
  client->shutdown();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_LT(elapsed, 200ms);
}

TEST_F(DbusClientTest, ReportsBusDisconnectForPendingCallAndClientStatus) {
  auto pending =
      m_client->callAsync(kServiceName, kObjectPath, kInterface, "Delay");
  m_bus.stop();
  ASSERT_EQ(pending.waitFor(1s), std::future_status::ready);
  auto result = pending.get();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().kind, dmn::Dmn_DbusErrorKind::Transport);
  ASSERT_TRUE(dmn_test_dbus::waitUntil(
      [this] { return !m_client->status().connected; }, 1s));
  EXPECT_FALSE(m_client->status().terminal_error.empty());
}

TEST_F(DbusClientTest, SignalQueueOverflowIsVisibleInStatus) {
  auto config = dmn::Dmn_DbusClientConfig{};
  config.bus_kind = dmn::Dmn_DbusBusKind::Address;
  config.bus_address = m_bus.address();
  config.limits.max_signal_messages = 1;
  config.limits.max_signal_bytes = 1024;
  auto client = std::make_unique<dmn::Dmn_DbusClient>(config);
  std::promise<void> callbackStarted;
  auto started = callbackStarted.get_future();
  std::promise<void> releaseCallback;
  auto release = releaseCallback.get_future().share();
  std::atomic_size_t callbacks{};
  dmn::Dmn_DbusSignalMatch match;
  match.sender = kServiceName;
  match.interface = kInterface;
  match.member = "Changed";
  auto subscription =
      client->subscribe(match, [&callbackStarted, &callbacks,
                                release](std::vector<dmn::Dmn_DbusValue>) {
        if (callbacks.fetch_add(1) == 0) {
          callbackStarted.set_value();
        }

        release.wait_for(2s);
      });
  ASSERT_TRUE(subscription.has_value()) << subscription.error().message;
  auto emitted =
      client->call(kServiceName, kObjectPath, kInterface, "EmitSignalBurst");
  ASSERT_TRUE(emitted.has_value()) << emitted.error().message;
  ASSERT_EQ(started.wait_for(1s), std::future_status::ready);
  const bool overflow = dmn_test_dbus::waitUntil(
      [&client] { return client->status().dropped_signal_messages > 0; }, 1s);
  const auto status = client->status();
  releaseCallback.set_value();
  ASSERT_TRUE(client->drainSignalCallbacks());
  EXPECT_TRUE(overflow);
  EXPECT_GT(status.dropped_signal_messages, 0U);
  EXPECT_GT(status.dropped_signal_bytes, 0U);
  EXPECT_LE(status.queued_signal_messages, 1U);
  EXPECT_LE(callbacks.load(), 2U);
}

TEST_F(DbusClientTest, SignalQueueEnforcesExactAndExceededByteLimits) {
  const auto runSignalPair = [this](std::size_t maxBytes)
      -> std::tuple<std::size_t, std::size_t, std::size_t, std::size_t> {
    auto config = dmn::Dmn_DbusClientConfig{};
    config.bus_kind = dmn::Dmn_DbusBusKind::Address;
    config.bus_address = m_bus.address();
    config.limits.max_signal_messages = 2;
    config.limits.max_signal_bytes = maxBytes;
    auto client = std::make_unique<dmn::Dmn_DbusClient>(config);

    std::promise<void> callbackStartedPromise;
    auto callbackStarted = callbackStartedPromise.get_future();
    std::promise<void> releasePromise;
    auto release = releasePromise.get_future().share();
    std::atomic_size_t callbackCount{};
    dmn::Dmn_DbusSignalMatch match;
    match.interface = kInterface;
    match.member = "Changed";
    auto subscription =
        client->subscribe(match, [&callbackStartedPromise, &callbackCount,
                                  release](std::vector<dmn::Dmn_DbusValue>) {
          if (callbackCount.fetch_add(1) == 0) {
            callbackStartedPromise.set_value();
          }

          release.wait();
        });
    EXPECT_TRUE(subscription) << subscription.error().message;
    if (!subscription) {
      return {0, 0, 0, 0};
    }

    auto firstSignal =
        client->call(kServiceName, kObjectPath, kInterface, "EmitSignal");
    EXPECT_TRUE(firstSignal) << firstSignal.error().message;
    if (callbackStarted.wait_for(1s) != std::future_status::ready) {
      const auto status = client->status();
      releasePromise.set_value();
      if (status.dropped_signal_messages > 0) {
        return {status.queued_signal_bytes, status.queued_signal_messages,
                status.dropped_signal_messages, callbackCount.load()};
      }

      ADD_FAILURE() << "signal callback did not start";

      return {0, 0, 0, 0};
    }

    auto secondSignal =
        client->call(kServiceName, kObjectPath, kInterface, "EmitSignal");
    EXPECT_TRUE(secondSignal) << secondSignal.error().message;
    if (!dmn_test_dbus::waitUntil(
            [&client] {
              return client->status().queued_signal_messages == 1 ||
                     client->status().dropped_signal_messages > 0;
            },
            1s)) {
      ADD_FAILURE() << "second signal was neither queued nor dropped";
      releasePromise.set_value();

      return {0, 0, 0, 0};
    }

    const auto signalStatus = client->status();
    if (signalStatus.queued_signal_messages > 1) {
      ADD_FAILURE() << "expected queued signals, found "
                    << signalStatus.queued_signal_messages << " with "
                    << signalStatus.queued_signal_bytes << " queued bytes and "
                    << signalStatus.dropped_signal_messages << " drops";
    }

    const auto queuedBytes = signalStatus.queued_signal_bytes;
    const auto queuedMessages = signalStatus.queued_signal_messages;
    const auto droppedMessages = client->status().dropped_signal_messages;
    releasePromise.set_value();
    EXPECT_TRUE(client->drainSignalCallbacks());

    return {queuedBytes, queuedMessages, droppedMessages, callbackCount.load()};
  };

  const auto [calibrationBytes, calibrationMessages, calibrationDrops,
              calibrationCallbacks] = runSignalPair(1024 * 1024);
  ASSERT_EQ(calibrationMessages, 1U);
  const auto signalBytes = calibrationBytes;
  EXPECT_GT(signalBytes, 0U);
  EXPECT_EQ(calibrationDrops, 0U);
  EXPECT_EQ(calibrationCallbacks, 2U);

  const auto [atLimitBytes, atLimitMessages, atLimitDrops, atLimitCallbacks] =
      runSignalPair(signalBytes);
  EXPECT_EQ(atLimitBytes, signalBytes);
  EXPECT_EQ(atLimitMessages, 1U);
  EXPECT_EQ(atLimitDrops, 0U);
  EXPECT_EQ(atLimitCallbacks, 2U);

  const auto [overLimitBytes, overLimitMessages, overLimitDrops,
              overLimitCallbacks] = runSignalPair(signalBytes - 1);
  EXPECT_EQ(overLimitBytes, 0U);
  EXPECT_EQ(overLimitMessages, 0U);
  EXPECT_EQ(overLimitDrops, 1U);
  EXPECT_EQ(overLimitCallbacks, 0U);
}

TEST_F(DbusClientTest, ShutdownReportsQueuedSignalsItDiscards) {
  auto config = dmn::Dmn_DbusClientConfig{};
  config.bus_kind = dmn::Dmn_DbusBusKind::Address;
  config.bus_address = m_bus.address();
  config.limits.max_signal_messages = 8;
  auto client = std::make_unique<dmn::Dmn_DbusClient>(config);

  std::promise<void> callbackStartedPromise;
  auto callbackStarted = callbackStartedPromise.get_future();
  std::promise<void> releasePromise;
  auto release = releasePromise.get_future().share();
  dmn::Dmn_DbusSignalMatch match;
  match.interface = kInterface;
  match.member = "Changed";
  auto subscription =
      client->subscribe(match, [&callbackStartedPromise,
                                release](std::vector<dmn::Dmn_DbusValue>) {
        callbackStartedPromise.set_value();
        release.wait();
      });
  ASSERT_TRUE(subscription) << subscription.error().message;

  auto emitted =
      client->call(kServiceName, kObjectPath, kInterface, "EmitSignalBurst");
  ASSERT_TRUE(emitted) << emitted.error().message;
  if (callbackStarted.wait_for(1s) != std::future_status::ready) {
    releasePromise.set_value();
    FAIL() << "signal callback did not start";
  }

  if (!dmn_test_dbus::waitUntil(
          [&client] { return client->status().queued_signal_messages > 0; },
          1s)) {
    releasePromise.set_value();
    FAIL() << "additional signals were not queued before shutdown";
  }

  std::thread releaseThread{[&releasePromise] {
    std::this_thread::sleep_for(100ms);
    releasePromise.set_value();
  }};
  client->shutdown();
  releaseThread.join();

  const auto status = client->status();
  EXPECT_TRUE(status.shutting_down);
  EXPECT_GT(status.shutdown_discarded_signal_messages, 0U);
  EXPECT_GT(status.shutdown_discarded_signal_bytes, 0U);
}

TEST_F(DbusClientTest, RejectsArrayElementTypeMismatchAndContainerDepthLimit) {
  auto mismatched =
      dmn::Dmn_DbusValue::array("s", {dmn::Dmn_DbusValue{std::uint32_t{1}}});
  auto invalid = m_client->call(kServiceName, kObjectPath, kInterface, "Echo",
                                {mismatched});
  ASSERT_FALSE(invalid.has_value());
  EXPECT_EQ(invalid.error().kind, dmn::Dmn_DbusErrorKind::InvalidArgument);

  auto config = dmn::Dmn_DbusClientConfig{};
  config.bus_kind = dmn::Dmn_DbusBusKind::Address;
  config.bus_address = m_bus.address();
  config.limits.max_container_depth = 1;
  config.limits.method_timeout = 500ms;
  dmn::Dmn_DbusClient shallowClient{config};
  auto nested = dmn::Dmn_DbusValue::array(
      "as", {dmn::Dmn_DbusValue::array("s", {dmn::Dmn_DbusValue{"too deep"}})});
  auto tooDeep = shallowClient.call(kServiceName, kObjectPath, kInterface,
                                    "Echo", {nested});
  ASSERT_FALSE(tooDeep.has_value());
  EXPECT_EQ(tooDeep.error().kind, dmn::Dmn_DbusErrorKind::ResourceLimit);

  config.limits.max_container_depth = 2;
  dmn::Dmn_DbusClient exactDepthClient{config};
  auto atDepthLimit = exactDepthClient.call(
      kServiceName, kObjectPath, kInterface, "AcceptNestedArray", {nested});
  ASSERT_TRUE(atDepthLimit) << atDepthLimit.error().message;

  config.limits.max_container_depth = 2;
  dmn::Dmn_DbusClient shallowReplyClient{config};
  auto deepReply = shallowReplyClient.call(kServiceName, kObjectPath,
                                           "org.freedesktop.DBus.ObjectManager",
                                           "GetManagedObjects");
  ASSERT_FALSE(deepReply);
  EXPECT_EQ(deepReply.error().kind, dmn::Dmn_DbusErrorKind::ResourceLimit);
}

TEST_F(DbusClientTest, RejectsInvalidCallNamesAndOversizedInput) {
  for (const auto &[destination, path, interface, member] :
       {std::tuple{kServiceName, "not/a/path", kInterface, "Echo"},
        std::tuple{"org.dmn..Invalid", kObjectPath, kInterface, "Echo"},
        std::tuple{kServiceName, kObjectPath, "invalid-interface", "Echo"},
        std::tuple{kServiceName, kObjectPath, kInterface, "invalid-member"}}) {
    auto invalid = m_client->call(destination, path, interface, member);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().kind, dmn::Dmn_DbusErrorKind::InvalidArgument);
  }

  auto config = dmn::Dmn_DbusClientConfig{};
  config.bus_kind = dmn::Dmn_DbusBusKind::Address;
  config.bus_address = m_bus.address();
  config.limits.max_message_bytes = 128;
  config.limits.method_timeout = 500ms;
  dmn::Dmn_DbusClient smallClient{config};
  auto oversized =
      smallClient.call(kServiceName, kObjectPath, kInterface, "Echo",
                       {dmn::Dmn_DbusValue{std::string(256, 'x')}});
  ASSERT_FALSE(oversized.has_value());
  EXPECT_EQ(oversized.error().kind, dmn::Dmn_DbusErrorKind::ResourceLimit);
}

TEST_F(DbusClientTest, RejectsEmbeddedNulInDbusIdentifiers) {
  const auto embeddedNul = [](const std::string &prefix) {
    return prefix + '\0' + "suffix";
  };

  for (const auto &[destination, path, interface, member] : std::vector<
           std::tuple<std::string, std::string, std::string, std::string>>{
           {embeddedNul(kServiceName), kObjectPath, kInterface, "Echo"},
           {kServiceName, embeddedNul(kObjectPath), kInterface, "Echo"},
           {kServiceName, kObjectPath, embeddedNul(kInterface), "Echo"},
           {kServiceName, kObjectPath, kInterface, embeddedNul("Echo")}}) {
    auto invalid = m_client->call(destination, path, interface, member);
    ASSERT_FALSE(invalid.has_value());
    EXPECT_EQ(invalid.error().kind, dmn::Dmn_DbusErrorKind::InvalidArgument);
  }

  const auto expectInvalidMatch = [this](dmn::Dmn_DbusSignalMatch match) {
    auto result =
        m_client->subscribe(match, [](std::vector<dmn::Dmn_DbusValue>) {});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, dmn::Dmn_DbusErrorKind::InvalidArgument);
  };
  dmn::Dmn_DbusSignalMatch match;
  match.interface = kInterface;
  match.member = "Changed";
  match.sender = embeddedNul(kServiceName);
  expectInvalidMatch(match);
  match.sender.clear();
  match.interface = embeddedNul(kInterface);
  expectInvalidMatch(match);
  match.interface = kInterface;
  match.member = embeddedNul("Changed");
  expectInvalidMatch(match);
  match.member = "Changed";
  match.path = embeddedNul(kObjectPath);
  expectInvalidMatch(match);
  match.path.clear();
  match.path_namespace = embeddedNul(kObjectPath);
  expectInvalidMatch(match);

  auto config = dmn::Dmn_DbusClientConfig{};
  config.bus_kind = dmn::Dmn_DbusBusKind::Address;
  config.bus_address = embeddedNul(m_bus.address());
  EXPECT_THROW((dmn::Dmn_DbusClient{config}), std::invalid_argument);
}

TEST_F(DbusClientTest, RejectsInvalidConnectionConfigurationWithoutFallback) {
  auto config = dmn::Dmn_DbusClientConfig{};
  config.bus_kind = dmn::Dmn_DbusBusKind::Address;
  config.bus_address = "unix:path=/dev/null";
  EXPECT_THROW((dmn::Dmn_DbusClient{config}), std::system_error);

  config.bus_kind = static_cast<dmn::Dmn_DbusBusKind>(255);
  config.bus_address.clear();
  EXPECT_THROW((dmn::Dmn_DbusClient{config}), std::invalid_argument);
}

TEST_F(DbusClientTest, RejectsEveryZeroOrOutOfRangeResourceLimit) {
  const auto expectInvalid = [this](dmn::Dmn_DbusClientLimits limits) {
    dmn::Dmn_DbusClientConfig config;
    config.bus_kind = dmn::Dmn_DbusBusKind::Address;
    config.bus_address = m_bus.address();
    config.limits = limits;
    EXPECT_THROW((dmn::Dmn_DbusClient{config}), std::invalid_argument);
  };

  auto limits = dmn::Dmn_DbusClientLimits{};
  limits.method_timeout = 0ms;
  expectInvalid(limits);
  limits.method_timeout =
      std::chrono::milliseconds{std::numeric_limits<int>::max()} + 1ms;
  expectInvalid(limits);
  limits = {};
  limits.max_in_flight_calls = 0;
  expectInvalid(limits);
  limits = {};
  limits.max_queued_commands = 0;
  expectInvalid(limits);
  limits = {};
  limits.max_signal_messages = 0;
  expectInvalid(limits);
  limits = {};
  limits.max_signal_bytes = 0;
  expectInvalid(limits);
  limits = {};
  limits.max_message_bytes = 0;
  expectInvalid(limits);
  limits = {};
  limits.max_message_bytes =
      static_cast<std::size_t>(std::numeric_limits<int>::max()) + 1U;
  expectInvalid(limits);
  limits = {};
  limits.max_container_depth = 0;
  expectInvalid(limits);
  limits = {};
  limits.max_values = 0;
  expectInvalid(limits);
}

} // namespace
