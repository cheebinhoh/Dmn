/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-dbus-support.hpp
 * @brief Common helpers for tests that use an isolated D-Bus session.
 */

#ifndef DMN_TEST_DBUS_SUPPORT_HPP_
#define DMN_TEST_DBUS_SUPPORT_HPP_

#include "dmn-dbus-config.hpp"

#include <dbus/dbus.h>
#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include <csignal>
#include <sys/types.h>
#include <unistd.h>

namespace dmn_test_dbus {

using namespace std::chrono_literals;

/** @brief Close and unref a D-Bus connection owned by a unique pointer. */
struct DbusConnectionDeleter {
  void operator()(DBusConnection *connection) const noexcept {
    if (connection != nullptr) {
      dbus_connection_close(connection);
      dbus_connection_unref(connection);
    }
  }
};

/** @brief Unref a D-Bus message owned by a unique pointer. */
struct DbusMessageDeleter {
  void operator()(DBusMessage *message) const noexcept {
    if (message != nullptr) {
      dbus_message_unref(message);
    }
  }
};

using DbusConnectionPtr =
    std::unique_ptr<DBusConnection, DbusConnectionDeleter>;
using DbusMessagePtr = std::unique_ptr<DBusMessage, DbusMessageDeleter>;

/** @brief Initialize and release a libdbus error value with RAII. */
class DbusErrorGuard {
public:
  DbusErrorGuard() { dbus_error_init(&m_error); }
  ~DbusErrorGuard() { dbus_error_free(&m_error); }

  DbusErrorGuard(const DbusErrorGuard &) = delete;
  auto operator=(const DbusErrorGuard &) -> DbusErrorGuard & = delete;

  auto get() -> DBusError * { return &m_error; }

private:
  DBusError m_error{};
};

/**
 * @brief Quote one argument for safe inclusion in a POSIX shell command.
 *
 * Use this when constructing the command that launches the test's private
 * `dbus-daemon`; do not concatenate an unquoted executable path into a shell
 * command.
 */
inline auto shellQuote(std::string_view value) -> std::string {
  std::string quoted{"'"};
  for (const auto character : value) {
    if (character == '\'') {
      quoted += "'\\''";
    } else {
      quoted += character;
    }
  }

  quoted += '\'';

  return quoted;
}

class TempConfigFile {
public:
  TempConfigFile() {
    m_fd = mkstemp(m_path);
    if (m_fd < 0) {
      throw std::system_error(errno, std::generic_category(),
                              "unable to create private D-Bus config file");
    }
  }

  ~TempConfigFile() {
    if (m_fd >= 0) {
      close(m_fd);
    }

    if (m_path[0] != '\0') {
      unlink(m_path);
    }
  }

  TempConfigFile(const TempConfigFile &) = delete;
  auto operator=(const TempConfigFile &) -> TempConfigFile & = delete;

  void write(std::string_view contents) {
    std::size_t written{};
    while (written < contents.size()) {
      const auto result{
          ::write(m_fd, contents.data() + written, contents.size() - written)};
      if (result < 0 && errno == EINTR) {
        continue;
      }

      if (result <= 0) {
        const auto error{result < 0 ? errno : EIO};
        throw std::system_error(error, std::generic_category(),
                                "unable to write private D-Bus config file");
      }

      written += static_cast<std::size_t>(result);
    }

    const auto fd{m_fd};
    m_fd = -1;
    if (close(fd) != 0) {
      const auto error{errno};
      throw std::system_error(error, std::generic_category(),
                              "unable to close private D-Bus config file");
    }
  }

  auto path() const -> const char * { return m_path; }

private:
  char m_path[sizeof("/tmp/dmn-test-dbus-config-XXXXXX")]{
      "/tmp/dmn-test-dbus-config-XXXXXX"};
  int m_fd{-1};
};

/**
 * @brief Own a temporary private session bus for one test process.
 *
 * Construct this before creating D-Bus endpoints, then call
 * `setAsSessionBus()` so libdbus clients in the process connect to this bus.
 * The daemon is stopped when the owner is destroyed.
 */
class PrivateBus {
public:
  /**
   * @brief Start a private session daemon and capture its address and process
   * ID.
   *
   * @param daemonConfig Optional complete dbus-daemon XML configuration.
   */
  explicit PrivateBus(std::string_view daemonConfig = {}) {
    std::unique_ptr<TempConfigFile> configFile;
    if (!daemonConfig.empty()) {
      configFile = std::make_unique<TempConfigFile>();
      configFile->write(daemonConfig);
    }

    using Pipe = std::unique_ptr<FILE, int (*)(FILE *)>;
    std::string command{shellQuote(DMN_DBUS_DAEMON_EXECUTABLE)};
    if (configFile == nullptr) {
      command += " --session";
    } else {
      command += " --config-file=";
      command += shellQuote(configFile->path());
    }

    command += " --fork --print-address=1 --print-pid=1";

    Pipe process{popen(command.c_str(), "r"), pclose};
    if (!process) {
      return;
    }

    char address[4096]{};
    char pid[64]{};
    if (fgets(address, sizeof(address), process.get()) != nullptr &&
        fgets(pid, sizeof(pid), process.get()) != nullptr) {
      m_address = address;
      m_address.erase(m_address.find_last_not_of("\r\n") + 1);
      m_pid = static_cast<pid_t>(std::strtol(pid, nullptr, 10));
    }

    process.reset();
  }

  ~PrivateBus() { stop(); }

  /** @brief Return the address printed by the private session daemon. */
  auto address() const -> const std::string & { return m_address; }

  /** @brief Report whether daemon startup produced a usable address and PID. */
  auto valid() const -> bool { return !m_address.empty() && m_pid > 0; }

  /**
   * @brief Set this private bus as the process's D-Bus session bus.
   *
   * @return `true` on success; otherwise report the setup error and return
   *         `false`.
   */
  auto setAsSessionBus() const -> bool {
    if (!valid()) {
      std::fprintf(stderr, "unable to start private D-Bus session daemon\n");

      return false;
    }

    if (setenv("DBUS_SESSION_BUS_ADDRESS", m_address.c_str(), 1) != 0) {
      std::perror("unable to set private D-Bus session address");

      return false;
    }

    return true;
  }

  /** @brief Stop the owned daemon; safe to call more than once. */
  void stop() noexcept {
    if (m_pid > 0) {
      kill(m_pid, SIGTERM);
      m_pid = -1;
    }
  }

private:
  std::string m_address;
  pid_t m_pid{-1};
};

/** @brief Return the default endpoint configuration used by D-Bus tests. */
inline auto makeConfig() -> dmn::Dmn_DbusConfig { return {}; }

/**
 * @brief Poll a condition until it succeeds or the timeout expires.
 *
 * Prefer this over fixed sleeps when tests wait for asynchronous endpoint
 * activity.
 *
 * @param predicate A callable that returns whether the expected condition
 *                  holds.
 * @param timeout Maximum time to wait; defaults to three seconds.
 * @return `true` if the predicate holds before timeout, otherwise `false`.
 */
template <typename Predicate>
inline auto waitUntil(Predicate predicate,
                      std::chrono::milliseconds timeout = 3s) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }

    std::this_thread::sleep_for(5ms);
  }

  return predicate();
}

/**
 * @brief Count names registered on a connected private D-Bus bus.
 *
 * @throws std::runtime_error if the bus returns an invalid reply or the
 *         `ListNames` call fails.
 */
inline auto connectionNameCount(DBusConnection *connection) -> std::size_t {
  DbusErrorGuard error;
  DbusMessagePtr request{dbus_message_new_method_call(
      "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
      "ListNames")};
  if (request == nullptr) {
    throw std::runtime_error("unable to create private bus ListNames request");
  }

  DbusMessagePtr reply{dbus_connection_send_with_reply_and_block(
      connection, request.get(), 3000, error.get())};
  if (reply == nullptr) {
    throw std::runtime_error("unable to list private bus connection names");
  }

  DBusMessageIter iterator;
  if (!dbus_message_iter_init(reply.get(), &iterator) ||
      dbus_message_iter_get_arg_type(&iterator) != DBUS_TYPE_ARRAY) {
    throw std::runtime_error("private bus ListNames returned an invalid reply");
  }

  DBusMessageIter names;
  dbus_message_iter_recurse(&iterator, &names);
  std::size_t count{};
  while (dbus_message_iter_get_arg_type(&names) == DBUS_TYPE_STRING) {
    ++count;

    if (!dbus_message_iter_next(&names)) {
      break;
    }
  }

  return count;
}

/**
 * @brief Count names registered with the private bus at @p address.
 *
 * @throws std::runtime_error if the bus cannot be queried or returns an
 *         invalid reply.
 */
inline auto busNameCount(std::string_view address) -> std::size_t {
  DbusErrorGuard error;
  DbusConnectionPtr connection{
      dbus_connection_open_private(std::string{address}.c_str(), error.get())};
  if (connection == nullptr ||
      !dbus_bus_register(connection.get(), error.get())) {
    throw std::runtime_error(
        error.get()->message == nullptr
            ? "unable to connect to private bus for name count"
            : error.get()->message);
  }

  return connectionNameCount(connection.get());
}

/**
 * @brief Count names currently registered with the private session bus.
 *
 * @throws std::runtime_error if the bus cannot be queried or returns an
 *         invalid reply.
 */
inline auto sessionBusNameCount() -> std::size_t {
  DbusErrorGuard error;
  DbusConnectionPtr connection{
      dbus_bus_get_private(DBUS_BUS_SESSION, error.get())};
  if (connection == nullptr) {
    throw std::runtime_error("unable to connect to private bus for name count");
  }

  return connectionNameCount(connection.get());
}

} // namespace dmn_test_dbus

#endif // DMN_TEST_DBUS_SUPPORT_HPP_
