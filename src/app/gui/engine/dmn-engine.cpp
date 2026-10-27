/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-engine.cpp
 * @brief GUI-independent Bluetooth monitor engine driven by JSON messages.
 */

#include "app/gui/engine/dmn-engine.hpp"

#include "app/gui/core/device-monitor-controller.hpp"
#include "app/gui/core/device-selection-model.hpp"
#include "app/gui/engine/dmn-json.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <random>
#include <thread>

namespace dmn::gui {
namespace {

// Retry delays are scaled by a random 80-100% so several phones that fail
// together do not retry in lockstep.
constexpr unsigned int kRetryJitterMinimumPercent = 80;
constexpr unsigned int kRetryJitterMaximumPercent = 100;
constexpr std::string_view kEngineVersion = "1";

// BlueZ reports addresses in upper case but a persisted or hand-written
// identity may not, so every identity comparison is case-insensitive.
auto lower(std::string text) -> std::string {
  std::transform(text.begin(), text.end(), text.begin(), [](const auto ch) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  });

  return text;
}

auto sameIdentity(const Dmn_DeviceIdentity &left,
                  const Dmn_DeviceIdentity &right) -> bool {
  return lower(left.m_adapter_address) == lower(right.m_adapter_address) &&
         lower(left.m_device_address) == lower(right.m_device_address);
}

auto contains(const std::vector<Dmn_DeviceIdentity> &list,
              const Dmn_DeviceIdentity &identity) -> bool {
  return std::any_of(list.begin(), list.end(), [&identity](const auto &entry) {
    return sameIdentity(entry, identity);
  });
}

auto identityKey(const Dmn_DeviceIdentity &identity) -> std::string {
  return lower(identity.m_adapter_address) + "/" +
         lower(identity.m_device_address);
}

auto optionalBool(const std::optional<bool> &value) -> Dmn_JsonValue {
  return value ? Dmn_JsonValue{*value} : Dmn_JsonValue{};
}

auto identityJson(const Dmn_DeviceIdentity &identity) -> Dmn_JsonValue {
  auto json = Dmn_JsonValue::object();
  json.set("adapter", identity.m_adapter_address);
  json.set("device", identity.m_device_address);

  return json;
}

auto nowMilliseconds() -> std::int64_t {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

auto optionalString(const std::optional<std::string> &value) -> Dmn_JsonValue {
  return value ? Dmn_JsonValue{*value} : Dmn_JsonValue{};
}

auto notificationJson(const Dmn_BackendNotification &notification)
    -> Dmn_JsonValue {
  auto json = Dmn_JsonValue::object();
  json.set("kind", notification.m_kind);
  json.set("uid", static_cast<std::uint64_t>(notification.m_uid));
  json.set("category", static_cast<int>(notification.m_category));
  json.set("flags", static_cast<int>(notification.m_flags));
  json.set("app_identifier", optionalString(notification.m_app_identifier));
  json.set("title", optionalString(notification.m_title));
  json.set("subtitle", optionalString(notification.m_subtitle));
  json.set("message", optionalString(notification.m_message));
  json.set("date", optionalString(notification.m_date));

  return json;
}

} // namespace

/**
 * Threading model
 * ---------------
 * All engine state (selection, intents, sessions, flags) is touched only by
 * the single control thread running run(). Other threads interact in two ways:
 * - Frontends call send()/next(), which only use the mutex-guarded inbox and
 *   outbox.
 * - Blocking backend calls (inventory, connect, monitor start/stop) run on
 *   short-lived worker threads created by spawn(); each reports back by
 *   post()ing a job to the control thread instead of mutating state itself.
 *
 * State changes mark dirty flags; flush() coalesces them into at most one
 * `persist` and one `state` message per control-loop pass.
 */
class Dmn_Engine::Impl {
public:
  Impl(std::unique_ptr<Dmn_EngineBackend> backend,
       const Dmn_EngineOptions &options)
      : m_options{options}, m_random{std::random_device{}()},
        m_backend{std::move(backend)} {
    m_backend->setHandlers([this] { post([this] { refreshInventory(); }); },
                           [this](const Dmn_DeviceIdentity &identity,
                                  const Dmn_BackendMonitorEvent &event) {
                             post([this, identity, event] {
                               handleMonitorEvent(identity, event);
                             });
                           });

    auto ready = Dmn_JsonValue::object();
    ready.set("type", "ready");
    ready.set("protocol", kEngineProtocolVersion);
    ready.set("engine", std::string{kEngineVersion});
    ready.set("backend", m_backend->name());
    emit(ready);

    m_thread = std::thread{[this] { run(); }};
  }

  ~Impl() {
    // Joining the control thread runs shutdown(), which cancels monitor starts
    // and waits for every worker, so no backend call outlives the engine.
    post([this] { m_shutdown_requested = true; });
    m_thread.join();
    m_backend->setHandlers({}, {});
  }

  Impl(const Impl &) = delete;
  auto operator=(const Impl &) -> Impl & = delete;
  Impl(Impl &&) = delete;
  auto operator=(Impl &&) -> Impl & = delete;

  // Returns false only for input that can never be a command (bad JSON, no
  // "type") or after shutdown. Unknown command types are accepted and ignored
  // so a newer frontend can talk to an older engine.
  auto send(std::string_view commandJson) -> bool {
    auto command = Dmn_JsonValue::parse(commandJson);
    if (!command || !command->isObject() || command->find("type") == nullptr ||
        m_closed.load(std::memory_order_acquire)) {
      return false;
    }

    post([this, command = std::move(*command)] { handleCommand(command); });

    return true;
  }

  auto
  next(const std::chrono::milliseconds timeout) -> std::optional<std::string> {
    std::unique_lock lock{m_out_mutex};
    m_out_cv.wait_for(lock, timeout,
                      [this] { return !m_outbox.empty() || m_closed; });
    if (m_outbox.empty()) {
      return std::nullopt;
    }

    auto message = std::move(m_outbox.front());
    m_outbox.pop_front();

    return message;
  }

private:
  // Lifecycle of one device's ANCS monitor.
  enum class Phase { kIdle, kStarting, kActive, kStopping };

  // Per-device monitor state. m_cancel is shared with the worker running
  // startMonitor() so a disconnect or shutdown can abort a start in flight.
  // m_disconnect_after_stop defers a user disconnect until the monitor has
  // stopped, so BlueZ is never asked to drop a link mid-subscription.
  struct Session {
    Phase m_phase{Phase::kIdle};
    std::shared_ptr<std::atomic<bool>> m_cancel;
    std::string m_detail{"Not monitoring"};
    bool m_disconnect_after_stop{};
  };

  struct Task {
    std::thread m_thread;
    std::atomic<bool> m_done{false};
  };

  void post(std::function<void()> job) {
    {
      std::lock_guard lock{m_in_mutex};
      m_inbox.push_back(std::move(job));
    }

    m_in_cv.notify_one();
  }

  void emit(const Dmn_JsonValue &message) {
    {
      std::lock_guard lock{m_out_mutex};
      m_outbox.push_back(message.dump());
    }

    m_out_cv.notify_all();
  }

  void spawn(std::function<void()> work) {
    auto task = std::make_unique<Task>();
    auto *raw = task.get();
    task->m_thread = std::thread{[raw, work = std::move(work)] {
      work();
      raw->m_done.store(true, std::memory_order_release);
    }};
    m_tasks.push_back(std::move(task));
  }

  // Joins workers that have finished so threads do not accumulate. Called
  // from the control loop; a worker only sets m_done after its last access to
  // engine state, which makes the join here non-blocking.
  void reapTasks() {
    std::erase_if(m_tasks, [](auto &task) {
      if (!task->m_done.load(std::memory_order_acquire)) {
        return false;
      }

      task->m_thread.join();

      return true;
    });
  }

  // Control loop: drain posted jobs, reap workers, refresh the inventory on a
  // timer, reconcile monitors against intent, then flush coalesced output.
  void run() {
    refreshInventory();
    auto nextRefresh = Dmn_DeviceMonitorController::Clock::now() +
                       m_options.m_inventory_refresh_interval;

    while (!m_shutdown_requested) {
      std::deque<std::function<void()>> batch;
      {
        std::unique_lock lock{m_in_mutex};
        m_in_cv.wait_for(lock, m_options.m_tick_interval,
                         [this] { return !m_inbox.empty(); });
        batch.swap(m_inbox);
      }

      for (auto &job : batch) {
        job();
        if (m_shutdown_requested) {
          break;
        }
      }

      if (m_shutdown_requested) {
        break;
      }

      reapTasks();
      const auto now = Dmn_DeviceMonitorController::Clock::now();
      if (now >= nextRefresh) {
        refreshInventory();
        nextRefresh = now + m_options.m_inventory_refresh_interval;
      }

      reconcile(now);
      flush();
    }

    shutdown();
  }

  // Ordered teardown: cancel starts, stop scanning, wait for workers (a
  // cancelled start returns promptly), stop monitors that are still live, and
  // only then publish `bye` so a frontend sees it after all other output.
  void shutdown() {
    m_shutting_down = true;
    for (auto &[key, session] : m_sessions) {
      if (session.m_cancel) {
        session.m_cancel->store(true, std::memory_order_release);
      }
    }

    if (m_scanning) {
      (void)m_backend->setScanning(false, {});
    }

    for (auto &task : m_tasks) {
      task->m_thread.join();
    }

    m_tasks.clear();

    for (const auto &entry : m_selection.devices()) {
      const auto found = m_sessions.find(identityKey(entry.m_identity));
      if (found != m_sessions.end() && found->second.m_phase != Phase::kIdle) {
        (void)m_backend->stopMonitor(entry.m_identity);
      }
    }

    auto bye = Dmn_JsonValue::object();
    bye.set("type", "bye");
    emit(bye);
    m_closed.store(true, std::memory_order_release);
    m_out_cv.notify_all();
  }

  void markDirty() { m_state_dirty = true; }

  void setStatus(std::string status) {
    m_status = std::move(status);
    markDirty();
  }

  // `persist` goes before `state` so a frontend stores the new intents before
  // it renders them.
  void flush() {
    if (m_persist_dirty) {
      m_persist_dirty = false;
      emitPersist();
    }

    if (m_state_dirty) {
      m_state_dirty = false;
      emitState();
    }
  }

  void emitPersist() {
    auto message = Dmn_JsonValue::object();
    message.set("type", "persist");
    auto saved = Dmn_JsonValue::array();
    for (const auto &identity : m_selection.savedSelection()) {
      saved.push(identityJson(identity));
    }

    auto monitoring = Dmn_JsonValue::array();
    for (const auto &identity : m_intents) {
      monitoring.push(identityJson(identity));
    }

    message.set("saved", std::move(saved));
    message.set("monitoring", std::move(monitoring));
    emit(message);
  }

  auto sessionFor(const Dmn_DeviceIdentity &identity) -> Session & {
    return m_sessions[identityKey(identity)];
  }

  auto findEntry(const Dmn_DeviceIdentity &identity) const
      -> const Dmn_DeviceEntry * {
    for (const auto &entry : m_selection.devices()) {
      if (sameIdentity(entry.m_identity, identity)) {
        return &entry;
      }
    }

    return nullptr;
  }

  // Adapts engine state to the shared Dmn_DeviceActionContext so button
  // availability uses exactly the rules of the GTK frontend.
  auto actionContext(const Dmn_DeviceEntry &entry) -> Dmn_DeviceActionContext {
    const auto &session = sessionFor(entry.m_identity);
    Dmn_DeviceActionContext context;
    context.m_saved = contains(m_selection.savedSelection(), entry.m_identity);
    context.m_selection_dirty = m_selection.hasUnsavedChanges();
    context.m_monitor_active = session.m_phase == Phase::kActive;
    context.m_monitor_requested = contains(m_intents, entry.m_identity);
    context.m_monitor_busy = session.m_phase == Phase::kStarting ||
                             session.m_phase == Phase::kStopping;
    context.m_monitor_start_pending = session.m_phase == Phase::kStarting;
    context.m_monitor_cancel_requested =
        session.m_cancel && session.m_cancel->load();
    context.m_connection_pending =
        m_pending_connect && sameIdentity(*m_pending_connect, entry.m_identity);
    context.m_disconnect_pending =
        m_pending_disconnect &&
        sameIdentity(*m_pending_disconnect, entry.m_identity);
    context.m_connect_after_monitor_stop = false;
    context.m_operation_busy = m_connection_busy;
    context.m_close_requested = m_shutting_down;

    return context;
  }

  static auto phaseName(const Session &session, const bool retrying) -> const
      char * {
    switch (session.m_phase) {
    case Phase::kActive:
      return "active";
    case Phase::kStarting:
      return "starting";
    case Phase::kStopping:
      return "stopping";
    case Phase::kIdle:
      break;
    }

    return retrying ? "retrying" : "idle";
  }

  void emitState() {
    auto message = Dmn_JsonValue::object();
    message.set("type", "state");

    auto adapters = Dmn_JsonValue::array();
    for (const auto &adapter : m_adapters) {
      auto json = Dmn_JsonValue::object();
      json.set("address", adapter.m_address);
      json.set("name", adapter.m_name);
      json.set("powered", adapter.m_powered);
      adapters.push(std::move(json));
    }

    message.set("adapters", std::move(adapters));
    message.set("scanning", m_scanning);
    message.set("busy", m_connection_busy || m_scan_busy);
    message.set("status", m_status);
    message.set("unsaved_changes", m_selection.hasUnsavedChanges());

    auto sort = Dmn_JsonValue::object();
    sort.set("column", m_selection.sortColumn() == Dmn_DeviceSortColumn::kName
                           ? "name"
                           : "status");
    sort.set("ascending", m_selection.sortAscending());
    message.set("sort", std::move(sort));

    auto devices = Dmn_JsonValue::array();
    for (const auto &entry : m_selection.devices()) {
      const auto context = actionContext(entry);
      const auto availability = deviceActionAvailability(entry, context);
      const auto &session = sessionFor(entry.m_identity);
      auto json = identityJson(entry.m_identity);
      json.set("name", deviceDisplayName(entry));
      json.set("status", deviceDisplayStatus(entry));
      json.set("paired", optionalBool(entry.m_paired));
      json.set("connected", optionalBool(entry.m_connected));
      json.set("present", entry.m_present);
      json.set("saved", context.m_saved);
      json.set("selected",
               contains(m_selection.draftSelection(), entry.m_identity));
      json.set("connecting", context.m_connection_pending);
      json.set("disconnecting", context.m_disconnect_pending);
      json.set("monitoring_requested", context.m_monitor_requested);
      // "retrying" = wanted and idle after a failure, awaiting the next
      // attempt.
      json.set("monitor_state",
               phaseName(session, context.m_monitor_requested &&
                                      session.m_phase == Phase::kIdle &&
                                      session.m_detail != "Not monitoring"));
      json.set("monitor_detail", session.m_detail);
      json.set("can_connect", availability.m_can_connect);
      json.set("can_disconnect", availability.m_can_disconnect);
      json.set("can_start_monitoring", availability.m_can_start_monitoring);
      json.set("can_stop_monitoring", availability.m_can_stop_monitoring);
      devices.push(std::move(json));
    }

    message.set("devices", std::move(devices));
    emit(message);
  }

  // At most one snapshot runs at a time; requests arriving meanwhile are
  // dropped because the running snapshot is at least as fresh.
  void refreshInventory() {
    if (m_inventory_busy || m_shutting_down) {
      return;
    }

    m_inventory_busy = true;
    spawn([this] {
      auto inventory = m_backend->snapshot();
      post([this, inventory = std::move(inventory)]() mutable {
        m_inventory_busy = false;
        if (!inventory) {
          setStatus("Inventory refresh failed: " + inventory.error().m_message);

          return;
        }

        m_adapters = std::move(inventory->m_adapters);
        m_selection.replaceInventory(std::move(inventory->m_devices));
        markDirty();
      });
    });
  }

  // Feeds the shared controller the current facts per device; it decides
  // which monitors to start or stop (honoring retry back-off), and the engine
  // only executes those commands.
  void reconcile(const Dmn_DeviceMonitorController::Clock::time_point now) {
    std::vector<Dmn_MonitorControllerDeviceState> states;
    for (const auto &entry : m_selection.devices()) {
      const auto &session = sessionFor(entry.m_identity);
      Dmn_MonitorControllerDeviceState state;
      state.m_identity = entry.m_identity;
      state.m_saved = contains(m_selection.savedSelection(), entry.m_identity);
      state.m_monitoring_requested = contains(m_intents, entry.m_identity);
      state.m_present = entry.m_present;
      state.m_paired = entry.m_paired.value_or(false);
      state.m_connected = entry.m_connected.value_or(false);
      state.m_monitor_active = session.m_phase == Phase::kActive;
      state.m_monitor_busy = session.m_phase == Phase::kStarting ||
                             session.m_phase == Phase::kStopping;
      states.push_back(std::move(state));
    }

    for (const auto &command :
         m_controller.reconcile(states, !m_shutting_down, now)) {
      if (command.m_action == Dmn_MonitorControllerCommand::Action::kStart) {
        startMonitor(command.m_identity);
      } else {
        stopMonitor(command.m_identity);
      }
    }
  }

  void startMonitor(const Dmn_DeviceIdentity &identity) {
    auto &session = sessionFor(identity);
    session.m_phase = Phase::kStarting;
    session.m_cancel = std::make_shared<std::atomic<bool>>(false);
    session.m_detail = "Starting";
    markDirty();

    spawn([this, identity, cancel = session.m_cancel] {
      auto result = m_backend->startMonitor(
          identity, *cancel, [this, identity](const std::string &progress) {
            post([this, identity, progress] {
              auto &current = sessionFor(identity);
              if (current.m_phase == Phase::kStarting) {
                current.m_detail = progress;
                markDirty();
              }
            });
          });
      post([this, identity, result = std::move(result)] {
        finishStart(identity, result);
      });
    });
  }

  // A cancelled start is not a failure: it must not trigger retry back-off.
  void finishStart(const Dmn_DeviceIdentity &identity,
                   const Dmn_BackendResult &result) {
    auto &session = sessionFor(identity);
    markDirty();
    if (result) {
      session.m_phase = Phase::kActive;
      session.m_detail = "Monitoring";
      m_controller.recordStartSuccess(identity);
    } else {
      session.m_phase = Phase::kIdle;
      if (result.error().m_cancelled) {
        session.m_detail = "Not monitoring";
      } else {
        session.m_detail = "Start failed: " + result.error().m_message;
        if (contains(m_intents, identity)) {
          recordFailure(identity);
        }

        emitEvent(identity, "monitor_error", {}, result.error().m_message,
                  nullptr);
      }
    }

    resumeDisconnect(identity);
  }

  void stopMonitor(const Dmn_DeviceIdentity &identity) {
    auto &session = sessionFor(identity);
    session.m_phase = Phase::kStopping;
    session.m_detail = "Stopping";
    markDirty();
    spawn([this, identity] {
      (void)m_backend->stopMonitor(identity);
      post([this, identity] {
        auto &current = sessionFor(identity);
        current.m_phase = Phase::kIdle;
        current.m_detail = "Not monitoring";
        markDirty();
        resumeDisconnect(identity);
      });
    });
  }

  void recordFailure(const Dmn_DeviceIdentity &identity) {
    std::uniform_int_distribution<unsigned int> jitter{
        kRetryJitterMinimumPercent, kRetryJitterMaximumPercent};
    (void)m_controller.recordStartFailure(
        identity, Dmn_DeviceMonitorController::Clock::now(), jitter(m_random));
  }

  // Continues a disconnect that was waiting for the monitor to become idle.
  void resumeDisconnect(const Dmn_DeviceIdentity &identity) {
    auto &session = sessionFor(identity);
    if (!session.m_disconnect_after_stop ||
        session.m_phase == Phase::kStarting ||
        session.m_phase == Phase::kStopping) {
      return;
    }

    session.m_disconnect_after_stop = false;
    runDisconnect(identity, session.m_phase == Phase::kActive);
  }

  void handleMonitorEvent(const Dmn_DeviceIdentity &identity,
                          const Dmn_BackendMonitorEvent &event) {
    const auto *notification =
        event.m_notification ? &*event.m_notification : nullptr;
    switch (event.m_kind) {
    case Dmn_BackendMonitorEvent::Kind::kNotification:
      emitEvent(identity, "notification", {}, {}, notification);
      break;
    case Dmn_BackendMonitorEvent::Kind::kState:
      emitEvent(identity, "state", event.m_state, {}, nullptr);
      break;
    case Dmn_BackendMonitorEvent::Kind::kError:
      emitEvent(identity, "error", {}, event.m_error, nullptr);
      break;
    }

    // A terminal event means the backend ended the monitor on its own (for
    // example the phone left range); treat it like a failed start so an
    // intended monitor retries with back-off.
    auto &session = sessionFor(identity);
    if (event.m_terminal && session.m_phase == Phase::kActive) {
      session.m_phase = Phase::kIdle;
      session.m_detail =
          "Monitor stopped: " +
          (event.m_error.empty() ? event.m_state : event.m_error);
      if (contains(m_intents, identity)) {
        recordFailure(identity);
      }

      markDirty();
    }
  }

  void emitEvent(const Dmn_DeviceIdentity &identity, const std::string &kind,
                 const std::string &state, const std::string &error,
                 const Dmn_BackendNotification *notification) {
    auto message = Dmn_JsonValue::object();
    message.set("type", "event");
    message.set("sequence", m_sequence++);
    message.set("received_ms", nowMilliseconds());
    message.set("adapter", identity.m_adapter_address);
    message.set("device", identity.m_device_address);
    const auto *entry = findEntry(identity);
    message.set("name", entry != nullptr ? deviceDisplayName(*entry)
                                         : identity.m_device_address);
    message.set("kind", kind);
    if (!state.empty()) {
      message.set("state", state);
    }

    if (!error.empty()) {
      message.set("error", error);
    }

    if (notification != nullptr) {
      message.set("notification", notificationJson(*notification));
    }

    emit(message);
  }

  // Dispatch for protocol-v1 commands. Missing fields take safe defaults
  // (empty address, selected=false, enabled=false) rather than failing, and a
  // device command naming an unknown device is ignored by its handler.
  void handleCommand(const Dmn_JsonValue &command) {
    const auto type = command.getString("type");
    const Dmn_DeviceIdentity requested{command.getString("adapter"),
                                       command.getString("device")};
    if (type == "hello") {
      auto ready = Dmn_JsonValue::object();
      ready.set("type", "ready");
      ready.set("protocol", kEngineProtocolVersion);
      ready.set("engine", std::string{kEngineVersion});
      ready.set("backend", m_backend->name());
      emit(ready);
      markDirty();
    } else if (type == "configure") {
      handleConfigure(command);
    } else if (type == "refresh") {
      refreshInventory();
    } else if (type == "scan") {
      handleScan(command);
    } else if (type == "select") {
      handleSelect(requested, command.getBool("selected", false));
    } else if (type == "save_selection") {
      handleSave();
    } else if (type == "discard_selection") {
      m_selection.discardChanges();
      markDirty();
    } else if (type == "sort") {
      m_selection.sortDevices(command.getString("column", "name") == "status"
                                  ? Dmn_DeviceSortColumn::kStatus
                                  : Dmn_DeviceSortColumn::kName,
                              command.getBool("ascending", true));
      markDirty();
    } else if (type == "connect") {
      handleConnect(requested);
    } else if (type == "disconnect") {
      handleDisconnect(requested);
    } else if (type == "monitor") {
      handleMonitor(requested, command.getBool("enabled", false));
    } else if (type == "shutdown") {
      m_shutdown_requested = true;
    }
  }

  auto parseIdentities(const Dmn_JsonValue *list)
      -> std::vector<Dmn_DeviceIdentity> {
    std::vector<Dmn_DeviceIdentity> identities;
    if (list == nullptr) {
      return identities;
    }

    for (const auto &item : list->items()) {
      identities.push_back(
          {item.getString("adapter"), item.getString("device")});
    }

    return identities;
  }

  // Restores persisted state. A monitoring intent is honored only for a device
  // that is also in the saved selection, matching the GTK app's invariant.
  void handleConfigure(const Dmn_JsonValue &command) {
    m_selection.loadSavedSelection(parseIdentities(command.find("saved")));
    m_intents.clear();
    for (const auto &identity : parseIdentities(command.find("monitoring"))) {
      for (const auto &saved : m_selection.savedSelection()) {
        if (sameIdentity(saved, identity) && !contains(m_intents, saved)) {
          m_intents.push_back(saved);
        }
      }
    }

    if (const auto *sort = command.find("sort"); sort != nullptr) {
      m_selection.sortDevices(sort->getString("column", "name") == "status"
                                  ? Dmn_DeviceSortColumn::kStatus
                                  : Dmn_DeviceSortColumn::kName,
                              sort->getBool("ascending", true));
    }

    markDirty();
  }

  void handleScan(const Dmn_JsonValue &command) {
    const auto enabled = command.getBool("enabled", false);
    if (m_scan_busy || m_scanning == enabled) {
      return;
    }

    const auto selector = lower(command.getString("adapter"));
    std::string path;
    for (const auto &adapter : m_adapters) {
      if (selector.empty() || lower(adapter.m_address) == selector) {
        path = adapter.m_path;

        break;
      }
    }

    if (enabled && path.empty()) {
      setStatus("No Bluetooth adapter is available for scanning.");

      return;
    }

    m_scan_busy = true;
    markDirty();
    spawn([this, enabled, path] {
      auto result = m_backend->setScanning(enabled, path);
      post([this, enabled, result = std::move(result)] {
        m_scan_busy = false;
        if (result) {
          m_scanning = enabled;
          setStatus(enabled ? "Scanning for devices…" : "Scan stopped.");
        } else {
          setStatus("Scan failed: " + result.error().m_message);
        }

        refreshInventory();
      });
    });
  }

  void handleSelect(const Dmn_DeviceIdentity &requested, const bool selected) {
    const auto *entry = findEntry(requested);
    if (entry == nullptr) {
      return;
    }

    if (!m_selection.setDraftSelected(entry->m_identity, selected) &&
        selected) {
      setStatus("The selection limit has been reached.");
    }

    markDirty();
  }

  void handleSave() {
    m_selection.saveSelection();
    std::erase_if(m_intents, [this](const auto &identity) {
      const bool keep = contains(m_selection.savedSelection(), identity);
      if (!keep) {
        m_controller.resetRetries(identity);
      }

      return !keep;
    });
    m_persist_dirty = true;
    setStatus("Selection saved.");
  }

  void handleConnect(const Dmn_DeviceIdentity &requested) {
    const auto *entry = findEntry(requested);
    if (entry == nullptr) {
      return;
    }

    const auto identity = entry->m_identity;
    if (m_connection_busy) {
      setStatus("Wait for the current connection operation to finish.");

      return;
    }

    if (!deviceActionAvailability(*entry, actionContext(*entry))
             .m_can_connect) {
      setStatus("Save and pair this device before connecting it.");

      return;
    }

    m_connection_busy = true;
    m_pending_connect = identity;
    setStatus("Connecting…");
    spawn([this, identity] {
      auto result = m_backend->connect(identity);
      post([this, result = std::move(result)] {
        m_connection_busy = false;
        m_pending_connect.reset();
        if (result) {
          setStatus("Connected.");
        } else if (result.error().m_cancelled) {
          setStatus("Connection canceled.");
        } else {
          setStatus("Connect failed: " + result.error().m_message);
        }

        refreshInventory();
      });
    });
  }

  void handleDisconnect(const Dmn_DeviceIdentity &requested) {
    const auto *entry = findEntry(requested);
    if (entry == nullptr) {
      return;
    }

    const auto identity = entry->m_identity;
    if (m_pending_connect && sameIdentity(*m_pending_connect, identity)) {
      setStatus("Canceling connection…");
      spawn([this, identity] { m_backend->cancelConnect(identity); });

      return;
    }

    if (m_connection_busy) {
      setStatus("Wait for the current connection operation to finish.");

      return;
    }

    if (!deviceActionAvailability(*entry, actionContext(*entry))
             .m_can_disconnect) {
      return;
    }

    // Disconnecting ends the user's monitoring intent, not just the session.
    if (std::erase_if(m_intents, [&identity](const auto &item) {
          return sameIdentity(item, identity);
        }) > 0) {
      m_persist_dirty = true;
    }

    m_controller.resetRetries(identity);
    auto &session = sessionFor(identity);
    switch (session.m_phase) {
    case Phase::kStarting:
      session.m_disconnect_after_stop = true;
      session.m_cancel->store(true, std::memory_order_release);
      setStatus("Canceling monitor startup before disconnecting…");
      break;
    case Phase::kStopping:
      session.m_disconnect_after_stop = true;
      setStatus("Stopping the monitor before disconnecting…");
      break;
    case Phase::kActive:
      runDisconnect(identity, true);
      break;
    case Phase::kIdle:
      runDisconnect(identity, false);
      break;
    }
  }

  void runDisconnect(const Dmn_DeviceIdentity &identity, const bool stopFirst) {
    if (m_connection_busy) {
      setStatus("Wait for the current connection operation to finish.");

      return;
    }

    m_connection_busy = true;
    m_pending_disconnect = identity;
    if (stopFirst) {
      sessionFor(identity).m_phase = Phase::kStopping;
      sessionFor(identity).m_detail = "Stopping";
    }

    setStatus("Disconnecting…");
    spawn([this, identity, stopFirst] {
      if (stopFirst) {
        (void)m_backend->stopMonitor(identity);
      }

      auto result = m_backend->disconnect(identity);
      post([this, identity, stopFirst, result = std::move(result)] {
        m_connection_busy = false;
        m_pending_disconnect.reset();
        if (stopFirst) {
          auto &session = sessionFor(identity);
          session.m_phase = Phase::kIdle;
          session.m_detail = "Not monitoring";
        }

        setStatus(result ? "Disconnected."
                         : "Disconnect failed: " + result.error().m_message);
        refreshInventory();
      });
    });
  }

  void handleMonitor(const Dmn_DeviceIdentity &requested, const bool enabled) {
    const auto *entry = findEntry(requested);
    if (entry == nullptr) {
      return;
    }

    const auto identity = entry->m_identity;
    if (enabled) {
      if (contains(m_intents, identity)) {
        return;
      }

      if (!deviceActionAvailability(*entry, actionContext(*entry))
               .m_can_start_monitoring) {
        setStatus("Save the selection and connect this device before "
                  "monitoring it.");

        return;
      }

      m_intents.push_back(identity);
      m_controller.resetRetries(identity);
    } else {
      std::erase_if(m_intents, [&identity](const auto &item) {
        return sameIdentity(item, identity);
      });
      m_controller.resetRetries(identity);
      auto &session = sessionFor(identity);
      if (session.m_phase == Phase::kStarting && session.m_cancel) {
        session.m_cancel->store(true, std::memory_order_release);
      }
    }

    m_persist_dirty = true;
    markDirty();
  }

  Dmn_EngineOptions m_options;

  std::mutex m_in_mutex;
  std::condition_variable m_in_cv;
  std::deque<std::function<void()>> m_inbox;

  std::mutex m_out_mutex;
  std::condition_variable m_out_cv;
  std::deque<std::string> m_outbox;
  std::atomic<bool> m_closed{false};

  // Everything below is touched only by the control thread after construction.
  Dmn_DeviceSelectionModel m_selection;
  Dmn_DeviceMonitorController m_controller;
  std::vector<Dmn_DeviceIdentity> m_intents;
  std::map<std::string, Session> m_sessions;
  std::vector<Dmn_BackendAdapter> m_adapters;
  std::list<std::unique_ptr<Task>> m_tasks;
  std::optional<Dmn_DeviceIdentity> m_pending_connect;
  std::optional<Dmn_DeviceIdentity> m_pending_disconnect;
  std::string m_status;
  std::mt19937 m_random;
  std::uint64_t m_sequence{1};
  bool m_scanning{};
  bool m_scan_busy{};
  bool m_inventory_busy{};
  bool m_connection_busy{};
  bool m_state_dirty{true};
  bool m_persist_dirty{};
  bool m_shutdown_requested{};
  bool m_shutting_down{};

  std::unique_ptr<Dmn_EngineBackend> m_backend;
  std::thread m_thread;
};

Dmn_Engine::Dmn_Engine(std::unique_ptr<Dmn_EngineBackend> backend,
                       const Dmn_EngineOptions &options)
    : m_impl{std::make_unique<Impl>(std::move(backend), options)} {}

Dmn_Engine::~Dmn_Engine() = default;

auto Dmn_Engine::send(std::string_view commandJson) -> bool {
  return m_impl->send(commandJson);
}

auto Dmn_Engine::nextMessage(const std::chrono::milliseconds timeout)
    -> std::optional<std::string> {
  return m_impl->next(timeout);
}

} // namespace dmn::gui
