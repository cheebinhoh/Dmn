/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-dbus-io.cpp
 * @brief Implementation of the optional byte-oriented D-Bus I/O endpoints.
 */

#include "dmn-dbus-io.hpp"
#include "dmn-dbus-runtime.hpp"
#include "dmn-proc.hpp"

#ifdef FIU_ENABLE
#include <fiu.h>
#endif

#include <dbus/dbus.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace dmn {
namespace { // Keep implementation details local to this translation unit.

using namespace std::chrono_literals;

constexpr auto kWorkerWait{50ms};
constexpr auto kShutdownDeadline{1s};
constexpr auto kDiagnosticInterval{1s};

struct DbusConnectionDeleter {
  void operator()(DBusConnection *connection) const noexcept {
    if (connection != nullptr) {
      // Close this endpoint's dedicated connection before releasing its final
      // libdbus reference. The selected bus daemon itself is not owned here.
      dbus_connection_close(connection);
      dbus_connection_unref(connection);
    }
  }
};

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

class DbusErrorGuard {
public:
  DbusErrorGuard() { dbus_error_init(&m_error); }
  ~DbusErrorGuard() { dbus_error_free(&m_error); }

  DbusErrorGuard(const DbusErrorGuard &) = delete;
  auto operator=(const DbusErrorGuard &) -> DbusErrorGuard & = delete;
  DbusErrorGuard(DbusErrorGuard &&) = delete;
  auto operator=(DbusErrorGuard &&) -> DbusErrorGuard & = delete;

  auto get() -> DBusError * { return &m_error; }

private:
  DBusError m_error{};
};

void validateConfig(const Dmn_DbusConfig &config) {
  if (config.max_message_bytes == 0 ||
      config.max_message_bytes >
          static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      config.max_queued_messages == 0 || config.max_queued_bytes == 0 ||
      config.max_queued_bytes < config.max_message_bytes) {
    throw std::invalid_argument("Dmn_Dbus: invalid queue limits");
  }

  DbusErrorGuard error;
  const bool valid{
      dbus_validate_path(config.signal_path.c_str(), error.get()) &&
      dbus_validate_interface(config.signal_interface.c_str(), error.get()) &&
      dbus_validate_member(config.signal_member.c_str(), error.get())};

  if (!valid) {
    throw std::invalid_argument("Dmn_Dbus: invalid signal path, interface, "
                                "or member");
  }
}

auto makeMatchRule(const Dmn_DbusConfig &config) -> std::string {
  return "type='signal',path='" + config.signal_path + "',interface='" +
         config.signal_interface + "',member='" + config.signal_member + "'";
}

auto dbusErrorCode(const DBusError &error) -> std::error_code {
  if (dbus_error_has_name(&error, DBUS_ERROR_DISCONNECTED)) {
    return std::make_error_code(std::errc::connection_reset);
  }

  if (dbus_error_has_name(&error, DBUS_ERROR_NO_MEMORY)) {
    return std::make_error_code(std::errc::not_enough_memory);
  }

  return std::make_error_code(std::errc::io_error);
}

auto connectBus(const Dmn_DbusConfig &config) -> DbusConnectionPtr {
  DbusErrorGuard error;
  DbusConnectionPtr connection{};

  if (config.bus_address.empty()) {
    // The endpoint default is the standard session bus.
    connection.reset(dbus_bus_get_private(DBUS_BUS_SESSION, error.get()));
  } else {
    // An explicit address can identify any bus daemon; registering this
    // connection enables daemon-routed signals and match rules.
    connection.reset(
        dbus_connection_open_private(config.bus_address.c_str(), error.get()));

    if (connection != nullptr &&
        !dbus_bus_register(connection.get(), error.get())) {
      connection.reset();
    }
  }

  if (connection == nullptr) {
    const auto code{dbusErrorCode(*error.get())};
    const std::string message{error.get()->message == nullptr
                                  ? "unable to connect to bus"
                                  : error.get()->message};

    throw std::system_error(code, "Dmn_Dbus: " + message);
  }

  dbus_connection_set_exit_on_disconnect(connection.get(), FALSE);

  return connection;
}

void reportDiagnostic(std::mutex &mutex,
                      std::chrono::steady_clock::time_point &lastDiagnostic,
                      const std::string &message) noexcept {
  try {
    const auto now{std::chrono::steady_clock::now()};

    {
      std::lock_guard lock{mutex};

      if (lastDiagnostic.time_since_epoch().count() != 0 &&
          now - lastDiagnostic < kDiagnosticInterval) {
        return;
      }

      lastDiagnostic = now;
    }

    std::cerr << message << '\n';
  } catch (...) {
    // Callers record failures in status; logging must not escape worker or
    // shutdown paths.
  }
}

} // namespace

// Each endpoint uses Dmn_Proc for one worker: input dispatches incoming
// signals, while output submits queued signals and pumps its own connection.
// Both loops stop cooperatively; shutdown joins the worker without using
// Dmn_Proc's cancellation-based stopExec() path.
class Dmn_DbusInput::Impl {
public:
  explicit Impl(const Dmn_DbusConfig &config)
      : m_config{config}, m_connection{connectBus(config)} {
    try {
      if (!dbus_connection_add_filter(m_connection.get(), filterMessage, this,
                                      nullptr)) {
        throw std::system_error(
            std::make_error_code(std::errc::not_enough_memory),
            "Dmn_DbusInput: add filter failed");
      }

      m_filter_added = true;

      DbusErrorGuard error;
      const auto matchRule{makeMatchRule(config)};
      dbus_bus_add_match(m_connection.get(), matchRule.c_str(), error.get());

      if (dbus_error_is_set(error.get())) {
        const auto code{dbusErrorCode(*error.get())};
        const std::string message{error.get()->message == nullptr
                                      ? "AddMatch failed"
                                      : error.get()->message};

        throw std::system_error(code, "Dmn_DbusInput: " + message);
      }

      if (!m_worker.exec([this, &workerStop = m_worker_stop] {
            dispatchLoop(workerStop);
          })) {
        throw std::system_error(
            std::make_error_code(std::errc::resource_unavailable_try_again),
            "Dmn_DbusInput: unable to start dispatch worker");
      }
    } catch (...) {
      closeConnection();

      throw;
    }
  }

  ~Impl() noexcept { shutdown(); }

  auto read() -> std::optional<std::string> {
    std::unique_lock lock{m_mutex};

    // The D-Bus filter copies valid payloads here and notifies this condition;
    // readers consume local queued data rather than dispatching the connection.
    m_ready.wait(lock, [this] {
      return !m_queue.empty() || m_stopping || m_status.terminal_error;
    });

    if (!m_queue.empty()) {
      std::string payload{std::move(m_queue.front())};
      m_queue.pop_front();
      m_queued_bytes -= payload.size();
      m_status.pending_input_messages = m_queue.size();
      m_status.pending_input_bytes = m_queued_bytes;

      return payload;
    }

    if (m_status.terminal_error) {
      throw std::system_error(m_status.terminal_error,
                              "Dmn_DbusInput: bus worker failed");
    }

    throw std::system_error(std::make_error_code(std::errc::operation_canceled),
                            "Dmn_DbusInput: input is shut down");
  }

  void shutdown() noexcept {
    std::call_once(m_shutdown_once, [this] {
      {
        std::lock_guard lock{m_mutex};

        m_stopping = true;
      }

      m_worker_stop.store(true, std::memory_order_release);
      m_ready.notify_all();
      m_worker.wait();

      closeConnection();
    });
  }

  auto status() const -> Dmn_DbusIoStatus {
    std::lock_guard lock{m_mutex};

    return m_status;
  }

private:
  static auto filterMessage(DBusConnection *, DBusMessage *message,
                            void *userData) -> DBusHandlerResult {
    auto *self{static_cast<Impl *>(userData)};

    try {
      self->handleMessage(message);
    } catch (const std::bad_alloc &) {
      self->recordAllocationError();
    } catch (...) {
      self->setTerminalError(std::make_error_code(std::errc::io_error),
                             "Dmn_DbusInput: callback failed");
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }

  void handleMessage(DBusMessage *message) {
    // The bus match narrows delivery, and this local check enforces the full
    // configured tuple before interpreting the body as this transport's data.
    if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_SIGNAL ||
        !dbus_message_has_path(message, m_config.signal_path.c_str()) ||
        !dbus_message_has_interface(message,
                                    m_config.signal_interface.c_str()) ||
        !dbus_message_has_member(message, m_config.signal_member.c_str())) {
      return;
    }

    const char *signature{dbus_message_get_signature(message)};

    if (signature == nullptr || std::strcmp(signature, "ay") != 0) {
      recordMalformedSignal();

      return;
    }

    DBusMessageIter iterator;
    if (!dbus_message_iter_init(message, &iterator) ||
        dbus_message_iter_get_arg_type(&iterator) != DBUS_TYPE_ARRAY) {
      recordMalformedSignal();

      return;
    }

    DBusMessageIter array;
    dbus_message_iter_recurse(&iterator, &array);
    unsigned char *bytes{};
    int length{};

    dbus_message_iter_get_fixed_array(&array, &bytes, &length);
    if (length < 0) {
      recordMalformedSignal();

      return;
    }

    const auto size{static_cast<std::size_t>(length)};
    if (size > m_config.max_message_bytes) {
      {
        std::lock_guard lock{m_mutex};

        ++m_status.oversized_received;
      }

      reportDiagnostic(m_mutex, m_last_diagnostic,
                       "Dmn_DbusInput: discarded oversized signal");

      return;
    }

    // Admission is bounded by both item count and total payload bytes. Copying
    // here gives public readers owned storage independent of DBusMessage life.
    bool queueFull{};

    {
      std::lock_guard lock{m_mutex};

      if (m_stopping) {
        return;
      }

      if (m_queue.size() >= m_config.max_queued_messages ||
          size > m_config.max_queued_bytes - m_queued_bytes) {
        ++m_status.input_queue_drops;
        queueFull = true;
      }
    }

    if (queueFull) {
      reportDiagnostic(m_mutex, m_last_diagnostic,
                       "Dmn_DbusInput: input queue full; dropped signal");

      return;
    }

#ifdef FIU_ENABLE
    // Exercise the callback's allocation-failure handling deterministically.
    if (fiu_fail("dmn/dbus/input/payload_allocation") != 0) {
      throw std::bad_alloc{};
    }
#endif

    std::string payload;
    if (size != 0) {
      payload.assign(reinterpret_cast<const char *>(bytes), size);
    }

    {
      std::lock_guard lock{m_mutex};

      if (m_stopping) {
        return;
      }

      // Keep queue accounting unchanged if deque insertion throws.
      m_queue.push_back(std::move(payload));
      m_queued_bytes += size;
      ++m_status.messages_received;
      m_status.pending_input_messages = m_queue.size();
      m_status.pending_input_bytes = m_queued_bytes;
    }

    m_ready.notify_one();
  }

  void dispatchLoop(const std::atomic_bool &workerStop) noexcept {
    while (true) {
      if (workerStop.load(std::memory_order_acquire)) {
        break;
      }

      // Reading and dispatching delivers bus-routed signals to filterMessage;
      // that callback validates and queues payloads for public read() calls.
      if (!dbus_connection_read_write_dispatch(
              m_connection.get(), static_cast<int>(kWorkerWait.count()))) {
        setTerminalError(std::make_error_code(std::errc::connection_reset),
                         "Dmn_DbusInput: bus disconnected");
        break;
      }
    }
  }

  void setTerminalError(const std::error_code &error,
                        const std::string &message) noexcept {
    {
      std::lock_guard lock{m_mutex};

      if (!m_status.terminal_error) {
        m_status.terminal_error = error;
      }
    }

    m_ready.notify_all();
    reportDiagnostic(m_mutex, m_last_diagnostic, message);
  }

  void recordAllocationError() noexcept {
    {
      std::lock_guard lock{m_mutex};

      ++m_status.input_allocation_errors;
    }

    reportDiagnostic(m_mutex, m_last_diagnostic,
                     "Dmn_DbusInput: unable to queue signal payload");
  }

  void recordMalformedSignal() noexcept {
    {
      std::lock_guard lock{m_mutex};

      ++m_status.malformed_received;
    }

    reportDiagnostic(m_mutex, m_last_diagnostic,
                     "Dmn_DbusInput: discarded malformed signal");
  }

  void closeConnection() noexcept {
    if (m_connection == nullptr) {
      return;
    }

    if (m_filter_added) {
      dbus_connection_remove_filter(m_connection.get(), filterMessage, this);
      m_filter_added = false;
    }

    m_connection.reset();
  }

  Dmn_DbusConfig m_config;
  DbusConnectionPtr m_connection;
  bool m_filter_added{};
  mutable std::mutex m_mutex;
  std::condition_variable m_ready;
  std::deque<std::string> m_queue;
  std::size_t m_queued_bytes{};
  bool m_stopping{};
  Dmn_DbusIoStatus m_status{};
  std::chrono::steady_clock::time_point m_last_diagnostic{};
  std::once_flag m_shutdown_once;
  std::atomic_bool m_worker_stop{}; ///< Captured state outlives the worker.
  Dmn_Proc m_worker{"Dmn_DbusInput"};
};

class Dmn_DbusOutput::Impl {
public:
  explicit Impl(const Dmn_DbusConfig &config)
      : m_config{config}, m_connection{connectBus(config)} {
#ifdef FIU_ENABLE
    if (fiu_fail("dmn/dbus/output/worker_start") != 0 ||
        !m_worker.exec(
            [this, &workerStop = m_worker_stop] { sendLoop(workerStop); })) {
#else
    if (!m_worker.exec(
            [this, &workerStop = m_worker_stop] { sendLoop(workerStop); })) {
#endif
      throw std::system_error(
          std::make_error_code(std::errc::resource_unavailable_try_again),
          "Dmn_DbusOutput: unable to start send worker");
    }
  }

  ~Impl() noexcept { shutdown(); }

  void write(const std::string &payload) { write(std::string{payload}); }

  void write(std::string &&payload) {
    // Config validation guarantees max_queued_bytes >= max_message_bytes, so
    // an accepted payload always fits an empty queue and cannot wait forever.
    if (payload.size() > m_config.max_message_bytes) {
      throw std::system_error(std::make_error_code(std::errc::message_size),
                              "Dmn_DbusOutput: payload exceeds limit");
    }

    // Blocks until the bounded local queue admits the payload, the endpoint
    // shuts down, or the worker fails. The worker owns all libdbus sends, so
    // transport progress or peer receipt is not implied.
    std::unique_lock lock{m_mutex};

    const auto hasSpace{[this, size = payload.size()] {
      return m_queue.size() < m_config.max_queued_messages &&
             size <= m_config.max_queued_bytes - m_pending_bytes;
    }};

    // Fast path: with space and no earlier blocked writer, admit immediately;
    // tickets are irrelevant and the writer never waits. Only when the queue
    // is full (or earlier writers are already blocked) does the writer take a
    // ticket and wait. Blocked writers are admitted strictly in ticket order
    // so a small later payload cannot overtake a larger waiting one.
    // Shutdown or failure wakes every blocked writer regardless of ticket.
    if (!m_stopping && !m_status.terminal_error &&
        (m_next_ticket != m_serving_ticket || !hasSpace())) {
      const auto ticket{m_next_ticket++};
      ++m_status.output_write_waits;

      m_space.wait(lock, [this, ticket, &hasSpace] {
        return m_stopping || m_status.terminal_error ||
               (ticket == m_serving_ticket && hasSpace());
      });

      // Admitted or aborted, this ticket is done; let the next waiter proceed.
      ++m_serving_ticket;
      m_space.notify_all();
    }

    if (m_status.terminal_error) {
      throw std::system_error(m_status.terminal_error,
                              "Dmn_DbusOutput: bus worker failed");
    }

    if (m_stopping) {
      throw std::system_error(
          std::make_error_code(std::errc::operation_canceled),
          "Dmn_DbusOutput: output is shut down");
    }

    m_pending_bytes += payload.size();
    m_queue.push_back(std::move(payload));
    ++m_status.messages_written;
    m_status.pending_output_messages = m_queue.size();
    m_status.pending_output_bytes = m_pending_bytes;
    m_ready.notify_one();
  }

  void shutdown() noexcept {
    std::call_once(m_shutdown_once, [this] {
      {
        std::lock_guard lock{m_mutex};

        // Stop new writes but let sendLoop submit accepted items until its
        // deadline; afterward it records whatever remains before close.
        m_stopping = true;
        m_shutdown_deadline =
            std::chrono::steady_clock::now() + kShutdownDeadline;
      }

      m_worker_stop.store(true, std::memory_order_release);
      m_ready.notify_all();
      m_space.notify_all();
      m_worker.wait();

      if (m_connection != nullptr) {
        const auto outgoingBytes{static_cast<std::size_t>(std::max(
            dbus_connection_get_outgoing_size(m_connection.get()), 0L))};
        bool unsentOutput{};

        {
          std::lock_guard lock{m_mutex};

          m_status.libdbus_outgoing_bytes = outgoingBytes;
          m_status.shutdown_unsent_messages = m_queue.size();
          m_status.shutdown_unsent_bytes = m_pending_bytes;
          m_status.pending_output_messages = m_queue.size();
          m_status.pending_output_bytes = m_pending_bytes;
          m_status.libdbus_bytes_discarded_on_shutdown = outgoingBytes;
          unsentOutput = !m_queue.empty() || outgoingBytes != 0;
        }

        if (unsentOutput) {
          reportDiagnostic(m_mutex, m_last_diagnostic,
                           "Dmn_DbusOutput: shutdown discarded pending output");
        }

        m_connection.reset();
      }
    });
  }

  auto status() const -> Dmn_DbusIoStatus {
    std::lock_guard lock{m_mutex};

    return m_status;
  }

private:
  auto makeSignal(const std::string &payload) -> DbusMessagePtr {
    DbusMessagePtr message{dbus_message_new_signal(
        m_config.signal_path.c_str(), m_config.signal_interface.c_str(),
        m_config.signal_member.c_str())};

    if (message == nullptr) {
      throw std::system_error(
          std::make_error_code(std::errc::not_enough_memory),
          "Dmn_DbusOutput: signal allocation failed");
    }

    DBusMessageIter iterator;
    dbus_message_iter_init_append(message.get(), &iterator);
    DBusMessageIter array;

    if (!dbus_message_iter_open_container(&iterator, DBUS_TYPE_ARRAY,
                                          DBUS_TYPE_BYTE_AS_STRING, &array)) {
      throw std::system_error(
          std::make_error_code(std::errc::not_enough_memory),
          "Dmn_DbusOutput: array allocation failed");
    }

    const auto size{static_cast<int>(payload.size())};
    const auto *bytes{reinterpret_cast<const unsigned char *>(payload.data())};

    if (size != 0 && !dbus_message_iter_append_fixed_array(
                         &array, DBUS_TYPE_BYTE, &bytes, size)) {
      dbus_message_iter_abandon_container(&iterator, &array);

      throw std::system_error(
          std::make_error_code(std::errc::not_enough_memory),
          "Dmn_DbusOutput: payload allocation failed");
    }

    if (!dbus_message_iter_close_container(&iterator, &array)) {
      throw std::system_error(
          std::make_error_code(std::errc::not_enough_memory),
          "Dmn_DbusOutput: message finalization failed");
    }

    return message;
  }

  void sendLoop(const std::atomic_bool &workerStop) noexcept {
    try {
      while (true) {
        std::string payload;
        bool havePayload{};
        bool stallOutput{};
#ifdef FIU_ENABLE
        // Keep queued data pending while still allowing bounded bus dispatch.
        stallOutput = fiu_fail("dmn/dbus/output/send_stall") != 0;
#endif
        const auto outgoingBytes{
            dbus_connection_get_outgoing_size(m_connection.get())};

        {
          std::unique_lock lock{m_mutex};

          // Wake promptly for new work, but also revisit the connection on a
          // finite cadence while idle to progress pending libdbus I/O and
          // detect a disconnected bus.
          m_ready.wait_for(lock, kWorkerWait, [this, &workerStop] {
            return workerStop.load(std::memory_order_acquire) ||
                   !m_queue.empty();
          });

          if (workerStop.load(std::memory_order_acquire) &&
              (m_queue.empty() ||
               std::chrono::steady_clock::now() >= m_shutdown_deadline)) {
            m_status.shutdown_unsent_messages = m_queue.size();
            m_status.shutdown_unsent_bytes = m_pending_bytes;
            m_status.pending_output_messages = m_queue.size();
            m_status.pending_output_bytes = m_pending_bytes;

            break;
          }

          if (!stallOutput && !m_queue.empty() &&
              static_cast<std::size_t>(std::max(outgoingBytes, 0L)) <
                  m_config.max_queued_bytes) {
            // Keep the payload in the application queue until libdbus accepts
            // the encoded signal; this bounds memory across both queues.
            payload = m_queue.front();
            havePayload = true;
          }
        }

        if (havePayload) {
          auto message{makeSignal(payload)};

          // send() only admits the signal to libdbus's outgoing queue. The
          // subsequent read/write/dispatch call drives transport I/O; neither
          // step acknowledges routing or receipt by a signal subscriber.
          if (!dbus_connection_send(m_connection.get(), message.get(),
                                    nullptr)) {
            throw std::system_error(
                std::make_error_code(std::errc::not_enough_memory),
                "Dmn_DbusOutput: libdbus rejected signal");
          }

          {
            std::lock_guard lock{m_mutex};

            m_pending_bytes -= m_queue.front().size();
            m_queue.pop_front();
            ++m_status.messages_queued_to_libdbus;
            m_status.pending_output_messages = m_queue.size();
            m_status.pending_output_bytes = m_pending_bytes;
          }

          m_space.notify_all();
        }

        // Pump both directions even if there was no payload to submit: this
        // advances pending output and observes connection failure.
        if (!dbus_connection_read_write_dispatch(
                m_connection.get(), static_cast<int>(kWorkerWait.count()))) {
          throw std::system_error(
              std::make_error_code(std::errc::connection_reset),
              "Dmn_DbusOutput: bus disconnected");
        }

        const auto pendingBytes{static_cast<std::size_t>(std::max(
            dbus_connection_get_outgoing_size(m_connection.get()), 0L))};

        {
          std::lock_guard lock{m_mutex};

          m_status.libdbus_outgoing_bytes = pendingBytes;
        }
      }
    } catch (const std::system_error &error) {
      setWorkerError(error.code(), error.what());
    } catch (const std::bad_alloc &) {
      setWorkerError(std::make_error_code(std::errc::not_enough_memory),
                     "Dmn_DbusOutput: worker allocation failed");
    } catch (...) {
      setWorkerError(std::make_error_code(std::errc::io_error),
                     "Dmn_DbusOutput: unexpected worker failure");
    }
  }

  void setWorkerError(const std::error_code &error,
                      const std::string &message) noexcept {
    const auto pendingBytes{static_cast<std::size_t>(
        std::max(dbus_connection_get_outgoing_size(m_connection.get()), 0L))};

    {
      std::lock_guard lock{m_mutex};

      ++m_status.output_worker_errors;

      if (!m_status.terminal_error) {
        m_status.terminal_error = error;
      }

      m_status.libdbus_outgoing_bytes = pendingBytes;
    }

    m_space.notify_all();
    reportDiagnostic(m_mutex, m_last_diagnostic, message);
  }

  Dmn_DbusConfig m_config;
  DbusConnectionPtr m_connection;
  mutable std::mutex m_mutex;
  std::condition_variable m_ready;
  std::condition_variable m_space;
  std::uint64_t m_next_ticket{};
  std::uint64_t m_serving_ticket{};
  std::deque<std::string> m_queue;
  std::size_t m_pending_bytes{};
  bool m_stopping{};
  std::chrono::steady_clock::time_point m_shutdown_deadline{};
  Dmn_DbusIoStatus m_status{};
  std::chrono::steady_clock::time_point m_last_diagnostic{};
  std::once_flag m_shutdown_once;
  std::atomic_bool m_worker_stop{}; ///< Captured state outlives the worker.
  Dmn_Proc m_worker{"Dmn_DbusOutput"};
};

Dmn_DbusInput::Dmn_DbusInput(const Dmn_DbusConfig &config) {
  // Initialize libdbus threading before any other libdbus API is called.
  detail::initializeDbusRuntime();
  validateConfig(config);

  m_impl = std::make_unique<Impl>(config);
}

Dmn_DbusInput::~Dmn_DbusInput() noexcept { shutdown(); }

auto Dmn_DbusInput::read() -> std::optional<std::string> {
  return m_impl->read();
}

void Dmn_DbusInput::write([[maybe_unused]] const std::string &item) {
  throw std::system_error(
      std::make_error_code(std::errc::operation_not_supported),
      "Dmn_DbusInput: write is not supported");
}

void Dmn_DbusInput::write([[maybe_unused]] std::string &&item) {
  throw std::system_error(
      std::make_error_code(std::errc::operation_not_supported),
      "Dmn_DbusInput: write is not supported");
}

void Dmn_DbusInput::shutdown() noexcept {
  if (m_impl) {
    m_impl->shutdown();
  }
}

auto Dmn_DbusInput::status() const -> Dmn_DbusIoStatus {
  return m_impl->status();
}

Dmn_DbusOutput::Dmn_DbusOutput(const Dmn_DbusConfig &config) {
  // Initialize libdbus threading before any other libdbus API is called.
  detail::initializeDbusRuntime();
  validateConfig(config);

  m_impl = std::make_unique<Impl>(config);
}

Dmn_DbusOutput::~Dmn_DbusOutput() noexcept { shutdown(); }

auto Dmn_DbusOutput::read() -> std::optional<std::string> {
  throw std::system_error(
      std::make_error_code(std::errc::operation_not_supported),
      "Dmn_DbusOutput: read is not supported");
}

void Dmn_DbusOutput::write(const std::string &item) { m_impl->write(item); }

void Dmn_DbusOutput::write(std::string &&item) {
  m_impl->write(std::move(item));
}

void Dmn_DbusOutput::shutdown() noexcept {
  if (m_impl) {
    m_impl->shutdown();
  }
}

auto Dmn_DbusOutput::status() const -> Dmn_DbusIoStatus {
  return m_impl->status();
}

} // namespace dmn
