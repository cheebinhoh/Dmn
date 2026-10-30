/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-bluetooth-cli.cpp
 * @brief Command-line application for the selected-iPhone ANCS monitor.
 *
 * The CLI is a frontend of the shared Dmn_Engine (src/app/engine), like the GTK
 * and COSMIC apps. It sends one "monitor_direct" command to the engine's BlueZ
 * backend (direct mode), prints the engine's notification events as text, and
 * maps the engine's failure codes to exit statuses. Device resolution,
 * discovery, pairing, connection and ANCS startup all happen in the backend.
 */

#include "app/engine/dmn-engine-backend.hpp"
#include "app/engine/dmn-engine.hpp"
#include "app/engine/dmn-json.hpp"
#include "dmn-pipe.hpp"
#include "dmn-runtime.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <unicode/uchar.h>

namespace {

using namespace std::chrono_literals;

enum class ExitStatus : int {
  Success = 0,
  InvalidCommandLine = 2,
  BluezUnavailable = 3,
  DeviceNotFound = 4,
  PairOrConnectFailure = 5,
  AncsUnavailable = 6,
  AncsStartupFailure = 7,
  RuntimeFailure = 8
};

constexpr auto exitCode(ExitStatus status) -> int {
  return static_cast<int>(status);
}

#if defined(DMN_BLUETOOTH_TEST_OUTPUT_QUEUE_LIMIT)
constexpr std::size_t kMaximumQueuedOutputEvents{
    DMN_BLUETOOTH_TEST_OUTPUT_QUEUE_LIMIT};
#else
constexpr std::size_t kMaximumQueuedOutputEvents{256};
#endif

struct Options {
  bool help{};
  std::string device;
  std::optional<std::string> adapter;
  std::chrono::seconds discoveryTimeout{30};
  std::chrono::seconds pairTimeout{120};
  std::chrono::seconds connectTimeout{30};
  std::chrono::seconds servicesTimeout{30};
  std::chrono::system_clock::time_point applicationStartedAt{};
  bool unlimitedNotifications{};
  bool ignoreExistingNotifications{};
};

void usage(std::ostream &out) {
  out << "Command-line ANCS notification monitor for a selected iPhone.\n"
         "Usage: dmn-bluetooth-cli monitor DEVICE [OPTIONS]\n"
         "       dmn-bluetooth-cli --help\n"
         "       dmn-bluetooth-cli monitor --help\n"
         "\n"
         "Monitor notification details from one selected iPhone over ANCS.\n"
         "DEVICE is a Bluetooth address or exact BlueZ display name.\n"
         "Options:\n"
         "  --adapter ADAPTER                 Adapter address or object path\n"
         "  --discovery-timeout SECONDS       Default: 30\n"
         "  --pair-timeout SECONDS            Default: 120\n"
         "  --connect-timeout SECONDS         Default: 30\n"
         "  --services-timeout SECONDS        Default: 30\n"
         "  --unlimited                       Do not cap queued notifications\n"
         "  --ignore-existing                 Suppress notifications dated "
         "before startup\n"
         "  --help                            Show this help\n";
}

auto parseSeconds(std::string_view text, std::chrono::seconds &value) -> bool {
  unsigned int seconds{};
  const auto parsed{
      std::from_chars(text.data(), text.data() + text.size(), seconds)};
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      seconds == 0 || seconds > 600) {
    return false;
  }

  value = std::chrono::seconds{seconds};

  return true;
}

auto parseOptions(int argc, char **argv, Options &options) -> bool {
  if (argc == 2 && std::string_view{argv[1]} == "--help") {
    options.help = true;

    return true;
  }

  if (argc == 3 && std::string_view{argv[1]} == "monitor" &&
      std::string_view{argv[2]} == "--help") {
    options.help = true;

    return true;
  }

  if (argc < 3 || std::string_view{argv[1]} != "monitor" ||
      std::string_view{argv[2]} == "--help" ||
      std::string_view{argv[2]}.starts_with("--")) {
    return false;
  }

  options.device = argv[2];
  bool adapterSeen{};
  bool unlimitedSeen{};
  bool ignoreExistingSeen{};
  std::map<std::string_view, bool> timeoutSeen;
  for (int index = 3; index < argc; ++index) {
    const std::string_view option{argv[index]};
    if (option == "--unlimited") {
      if (unlimitedSeen) {
        return false;
      }

      unlimitedSeen = true;
      options.unlimitedNotifications = true;
      continue;
    }

    if (option == "--ignore-existing") {
      if (ignoreExistingSeen) {
        return false;
      }

      ignoreExistingSeen = true;
      options.ignoreExistingNotifications = true;
      continue;
    }

    if (option == "--adapter") {
      if (adapterSeen || index + 1 >= argc ||
          std::string_view{argv[index + 1]}.starts_with("--")) {
        return false;
      }

      adapterSeen = true;
      options.adapter = argv[++index];
      continue;
    }

    std::chrono::seconds *target{};
    if (option == "--discovery-timeout") {
      target = &options.discoveryTimeout;
    } else if (option == "--pair-timeout") {
      target = &options.pairTimeout;
    } else if (option == "--connect-timeout") {
      target = &options.connectTimeout;
    } else if (option == "--services-timeout") {
      target = &options.servicesTimeout;
    } else {
      return false;
    }

    if (timeoutSeen[option] || index + 1 >= argc ||
        std::string_view{argv[index + 1]}.starts_with("--") ||
        !parseSeconds(argv[++index], *target)) {
      return false;
    }

    timeoutSeen[option] = true;
  }

  return true;
}

auto encodeHex(std::uint32_t value, int width) -> std::string {
  std::ostringstream stream;
  stream << std::hex << std::nouppercase << std::setfill('0')
         << std::setw(width) << value;

  return stream.str();
}

auto appendEscapedCodePoint(std::string &output, std::uint32_t codePoint,
                            std::string_view bytes) -> void {
  if (codePoint == '\\') {
    output += "\\\\";
  } else if (codePoint == '\n') {
    output += "\\n";
  } else if (codePoint == '\r') {
    output += "\\r";
  } else if (codePoint == '\t') {
    output += "\\t";
  } else if (codePoint < 0x20 || codePoint == 0x7f) {
    output += "\\x";
    output += encodeHex(codePoint, 2);
  } else {
    const auto characterType{u_charType(static_cast<UChar32>(codePoint))};
    if (characterType == U_CONTROL_CHAR || characterType == U_FORMAT_CHAR ||
        characterType == U_LINE_SEPARATOR ||
        characterType == U_PARAGRAPH_SEPARATOR) {
      output += "\\u{";
      std::ostringstream stream;
      stream << std::uppercase << std::hex << codePoint;
      output += stream.str();
      output += '}';
    } else {
      output.append(bytes);
    }
  }
}

auto escapedText(std::string_view value) -> std::string {
  std::string output;
  std::size_t index{};
  while (index < value.size()) {
    const auto first{static_cast<unsigned char>(value[index])};
    std::size_t width{1};
    std::uint32_t codePoint{first};
    bool valid{true};
    if (first >= 0xc2 && first <= 0xdf) {
      width = 2;
      codePoint = first & 0x1f;
    } else if (first >= 0xe0 && first <= 0xef) {
      width = 3;
      codePoint = first & 0x0f;
    } else if (first >= 0xf0 && first <= 0xf4) {
      width = 4;
      codePoint = first & 0x07;
    } else if (first >= 0x80) {
      valid = false;
    }

    if (valid && width > 1) {
      if (width > value.size() - index) {
        valid = false;
      } else {
        for (std::size_t offset = 1; offset < width; ++offset) {
          const auto continuation{
              static_cast<unsigned char>(value[index + offset])};
          if ((continuation & 0xc0) != 0x80) {
            valid = false;
            break;
          }

          codePoint = (codePoint << 6) | (continuation & 0x3f);
        }

        if ((width == 2 && codePoint < 0x80) ||
            (width == 3 && codePoint < 0x800) ||
            (width == 4 && codePoint < 0x10000) ||
            (codePoint >= 0xd800 && codePoint <= 0xdfff) ||
            codePoint > 0x10ffff) {
          valid = false;
        }
      }
    }

    if (!valid) {
      output += "\\x";
      output += encodeHex(first, 2);
      ++index;
      continue;
    }

    appendEscapedCodePoint(output, codePoint, value.substr(index, width));
    index += width;
  }

  return output;
}

auto categoryName(std::uint8_t category) -> std::string {
  constexpr std::array<std::string_view, 12> kCategories{
      "other",          "incoming_call",    "missed_call", "voicemail",
      "social",         "schedule",         "email",       "news",
      "health_fitness", "business_finance", "location",    "entertainment"};
  if (category < kCategories.size()) {
    return std::string{kCategories[category]};
  }

  return "unknown(" + std::to_string(category) + ")";
}

struct Notification {
  std::string event;
  std::string kind;
  std::uint32_t uid{};
  std::uint8_t category{};
  std::uint8_t categoryCount{};
  std::uint8_t flags{};
  std::optional<std::string> appIdentifier;
  std::optional<std::string> title;
  std::optional<std::string> subtitle;
  std::optional<std::string> message;
  std::optional<std::string> date;
};

auto optionalField(const dmn::gui::Dmn_JsonValue &object,
                   std::string_view key) -> std::optional<std::string> {
  const auto *value = object.find(key);
  if (value == nullptr || value->isNull()) {
    return std::nullopt;
  }

  return value->stringOr();
}

auto parseNotification(const dmn::gui::Dmn_JsonValue &json) -> Notification {
  Notification notification;
  notification.event = json.getString("event");
  notification.kind = json.getString("kind");
  notification.uid = static_cast<std::uint32_t>(json.getNumber("uid", 0));
  notification.category =
      static_cast<std::uint8_t>(json.getNumber("category", 0));
  notification.categoryCount =
      static_cast<std::uint8_t>(json.getNumber("category_count", 0));
  notification.flags = static_cast<std::uint8_t>(json.getNumber("flags", 0));
  notification.appIdentifier = optionalField(json, "app_identifier");
  notification.title = optionalField(json, "title");
  notification.subtitle = optionalField(json, "subtitle");
  notification.message = optionalField(json, "message");
  notification.date = optionalField(json, "date");

  return notification;
}

auto printNotification(const Notification &notification) -> bool {
  std::string header;
  if (notification.event == "added") {
    header = "notification added";
  } else if (notification.event == "updated") {
    header = "notification updated";
  } else if (notification.event == "removed") {
    header = "notification removed";
  } else if (notification.event == "attributes_unavailable") {
    header = "notification attributes unavailable event=";
    header += notification.kind == "attributes_unavailable"
                  ? std::string{"attributes unavailable"}
                  : notification.kind;
  } else {
    return true;
  }

  std::cout << header << " uid=0x" << encodeHex(notification.uid, 8)
            << " category=" << categoryName(notification.category) << " count="
            << static_cast<unsigned int>(notification.categoryCount)
            << " flags=0x" << encodeHex(notification.flags, 2) << '\n';
  if (notification.event != "removed" &&
      notification.event != "attributes_unavailable") {
    const auto printField{
        [](std::string_view label, const std::optional<std::string> &value) {
          if (value && !value->empty()) {
            std::cout << label << ": " << escapedText(*value) << '\n';
          }
        }};
    printField("app_identifier", notification.appIdentifier);
    printField("title", notification.title);
    printField("subtitle", notification.subtitle);
    printField("message", notification.message);
    printField("date", notification.date);
  }

  std::cout << '\n';
  std::cout.flush();

  return static_cast<bool>(std::cout);
}

struct FailureState {
  void set(ExitStatus status, std::string message) {
    std::lock_guard lock{mutex};

    if (exitStatus == exitCode(ExitStatus::Success)) {
      diagnostic = std::move(message);
      exitStatus = exitCode(status);
    }
  }

  auto get() -> std::pair<int, std::string> {
    std::lock_guard lock{mutex};

    return {exitStatus.load(), diagnostic};
  }

  std::mutex mutex;
  std::atomic<int> exitStatus{exitCode(ExitStatus::Success)};
  std::string diagnostic;
};

struct CountRelease {
  explicit CountRelease(std::atomic<std::size_t> &count) : count{count} {}
  ~CountRelease() { count.fetch_sub(1); }
  std::atomic<std::size_t> &count;
};

struct OutputRecord {
  Notification notification;
};

// Outcome of the engine's direct monitor, filled by the reader thread.
struct DirectState {
  enum class Phase { Starting, Ready, Failed };

  std::mutex mutex;
  std::condition_variable changed;
  Phase phase{Phase::Starting};
  bool stopped{};
  std::string stopError;
  std::string code;
  std::string detail;
  std::string remoteName;
  bool cancelled{};
};

auto progressMessage(std::string_view stage,
                     std::string_view device) -> std::string {
  constexpr std::string_view kRemainingProfiles{"remaining_profiles:"};
  if (stage.starts_with(kRemainingProfiles)) {
    return "requesting BlueZ to connect the GATT-capable bearer and "
           "remaining profiles for " +
           escapedText(stage.substr(kRemainingProfiles.size()));
  }

  std::string_view message;
  if (stage == "inventory") {
    message = "checking BlueZ inventory for ";
  } else if (stage == "discovery") {
    message = "discovering ";
  } else if (stage == "pairing") {
    message = "pairing ";
  } else if (stage == "connection") {
    message = "connecting ";
  } else if (stage == "ancs_startup") {
    message = "starting ANCS session for ";
  } else {
    return escapedText(stage);
  }

  return std::string{message} + escapedText(device);
}

struct StartFailure {
  std::string_view prefix;
  ExitStatus status;
};

auto startFailure(std::string_view code) -> StartFailure {
  if (code == "invalid_argument") {
    return {"invalid monitor configuration", ExitStatus::InvalidCommandLine};
  }

  if (code == "not_ready") {
    return {"monitor startup failed", ExitStatus::RuntimeFailure};
  }

  if (code == "inventory") {
    return {"BlueZ inventory unavailable", ExitStatus::BluezUnavailable};
  }

  if (code == "discovery") {
    return {"Bluetooth discovery failed", ExitStatus::BluezUnavailable};
  }

  if (code == "device_not_found") {
    return {"device was not found", ExitStatus::DeviceNotFound};
  }

  if (code == "pairing") {
    return {"device pairing failed", ExitStatus::PairOrConnectFailure};
  }

  if (code == "connection") {
    return {"device connection failed", ExitStatus::PairOrConnectFailure};
  }

  if (code == "ancs_unsupported") {
    return {"ANCS startup failed", ExitStatus::AncsUnavailable};
  }

  if (code == "ancs_startup") {
    return {"ANCS startup failed", ExitStatus::AncsStartupFailure};
  }

  if (code == "cancelled") {
    return {"monitor startup canceled", ExitStatus::RuntimeFailure};
  }

  if (code == "cleanup") {
    return {"ANCS cleanup failed", ExitStatus::RuntimeFailure};
  }

  return {"BlueZ unavailable", ExitStatus::BluezUnavailable};
}

auto runMonitor(const Options &options) -> int {
  auto runtime{dmn::Dmn_Runtime_Manager<>::createInstance()};
  std::atomic_bool signalSeen{};
  runtime->registerSignalHandlerHook(SIGINT,
                                     [&signalSeen](int) { signalSeen = true; });
  runtime->registerSignalHandlerHook(SIGTERM,
                                     [&signalSeen](int) { signalSeen = true; });

  struct sigaction ignorePipe {};
  ignorePipe.sa_handler = SIG_IGN;
  sigemptyset(&ignorePipe.sa_mask);
  (void)sigaction(SIGPIPE, &ignorePipe, nullptr);

  dmn::gui::Dmn_BluezBackendOptions backendOptions;
  backendOptions.m_direct_monitor = true;
  backendOptions.m_discovery_timeout = options.discoveryTimeout;
  backendOptions.m_pairing_timeout = options.pairTimeout;
  backendOptions.m_connection_timeout = options.connectTimeout;
  backendOptions.m_services_timeout = options.servicesTimeout;
  backendOptions.m_unlimited_notifications = options.unlimitedNotifications;
  if (options.ignoreExistingNotifications) {
    backendOptions.m_notification_cutoff = options.applicationStartedAt;
  }

#if defined(DMN_BLUETOOTH_TESTING)
  const char *testBusAddress = std::getenv("DMN_BLUETOOTH_TEST_BUS_ADDRESS");
  if (testBusAddress == nullptr || *testBusAddress == '\0') {
    std::cerr << "test BlueZ bus address is not configured\n";

    return exitCode(ExitStatus::BluezUnavailable);
  }

  backendOptions.m_test_bus_address = testBusAddress;
#endif

  dmn::gui::Dmn_EngineOptions engineOptions;
  engineOptions.m_inventory_enabled = false;
  dmn::gui::Dmn_Engine engine{dmn::gui::makeBluezBackend(backendOptions),
                              engineOptions};

  FailureState failure;
  DirectState direct;
  std::atomic_bool outputFailed{};
  std::atomic_bool mainLoopActive{};
  std::atomic<std::size_t> queuedOutputEvents{};
#if defined(DMN_BLUETOOTH_TEST_BACKPRESSURE_GATE)
  const bool testBackpressureGate{
      std::getenv("DMN_BLUETOOTH_TEST_ENABLE_BACKPRESSURE_GATE") != nullptr};
  std::mutex testOutputMutex;
  std::condition_variable testOutputCondition;
  bool releaseTestOutput{};
#endif

  dmn::Dmn_Pipe<OutputRecord> outputPipe{
      "dmn-bluetooth-output", [&](OutputRecord &&record) {
        CountRelease countRelease{queuedOutputEvents};
#if defined(DMN_BLUETOOTH_TEST_BACKPRESSURE_GATE)
        if (testBackpressureGate) {
          std::cerr << "DMN_TEST_OUTPUT_GATE_REACHED\n" << std::flush;
          std::unique_lock lock{testOutputMutex};

          testOutputCondition.wait(lock, [&] { return releaseTestOutput; });
        }
#endif
        if (outputFailed) {
          return;
        }

        if (!printNotification(record.notification)) {
          outputFailed = true;
          failure.set(ExitStatus::RuntimeFailure,
                      "failed to write notification output");
          if (mainLoopActive) {
            runtime->exitMainLoop();
          }
        }
      }};

  // Engine events arrive on the reader thread; the bounded queue and the
  // runtime job hop keep printing off that thread and detect a stalled stdout.
  const auto handleEvent{[&](const dmn::gui::Dmn_JsonValue &message) {
    const auto kind{message.getString("kind")};
    const auto count{queuedOutputEvents.fetch_add(1) + 1};
    if (count > kMaximumQueuedOutputEvents) {
      queuedOutputEvents.fetch_sub(1);
      failure.set(ExitStatus::RuntimeFailure,
                  "monitor output queue exceeded its " +
                      std::to_string(kMaximumQueuedOutputEvents) +
                      "-event limit");
#if defined(DMN_BLUETOOTH_TEST_BACKPRESSURE_GATE)
      if (testBackpressureGate) {
        {
          std::lock_guard lock{testOutputMutex};

          releaseTestOutput = true;
        }

        testOutputCondition.notify_all();
        std::cerr << "DMN_TEST_QUEUE_OVERFLOW\n" << std::flush;
      }
#endif
      if (mainLoopActive) {
        runtime->exitMainLoop();
      }

      return;
    }

    // State events carry no notification; they still pass through the bounded
    // output queue so backpressure accounting covers every engine event.
    Notification notification;
    if (kind == "notification") {
      if (const auto *json = message.find("notification"); json != nullptr) {
        notification = parseNotification(*json);
      }
    }

    const auto errorText{message.getString("error")};
    const auto fatal{message.getBool("terminal", false)};
    try {
      runtime->addJob([&, notification = std::move(notification), kind,
                       errorText, fatal](const dmn::Dmn_Runtime_Job &) {
        if (outputFailed) {
          queuedOutputEvents.fetch_sub(1);

          return;
        }

        if (kind == "error") {
          std::cerr << "ANCS: " << escapedText(errorText) << '\n';
          if (fatal) {
            failure.set(ExitStatus::RuntimeFailure, errorText);
            if (mainLoopActive) {
              runtime->exitMainLoop();
            }
          }

          queuedOutputEvents.fetch_sub(1);

          return;
        }

        try {
          outputPipe.write(OutputRecord{notification});
        } catch (const std::exception &error) {
          queuedOutputEvents.fetch_sub(1);
          outputFailed = true;
          failure.set(ExitStatus::RuntimeFailure, error.what());
          if (mainLoopActive) {
            runtime->exitMainLoop();
          }
        }
      });
    } catch (const std::exception &error) {
      queuedOutputEvents.fetch_sub(1);
      failure.set(ExitStatus::RuntimeFailure, error.what());
      if (mainLoopActive) {
        runtime->exitMainLoop();
      }
    }
  }};

  const auto handleDirect{[&](const dmn::gui::Dmn_JsonValue &message) {
    const auto state{message.getString("state")};
    if (state == "progress") {
      std::cerr << progressMessage(message.getString("stage"), options.device)
                << '\n';

      return;
    }

    {
      std::lock_guard lock{direct.mutex};
      if (state == "ready") {
        direct.phase = DirectState::Phase::Ready;
      } else if (state == "failed") {
        direct.phase = DirectState::Phase::Failed;
        direct.code = message.getString("code");
        direct.detail = message.getString("detail");
        direct.remoteName = message.getString("remote_name");
        direct.cancelled = message.getBool("cancelled", false);
      } else if (state == "stopped") {
        direct.stopped = true;
        direct.stopError = message.getString("message");
      }
    }

    direct.changed.notify_all();
  }};

  // nextMessage() returns nothing on timeout, so the reader runs until the
  // engine's final "bye" message.
  std::thread reader{[&] {
    for (;;) {
      const auto text{engine.nextMessage(100ms)};
      if (!text) {
        continue;
      }

      const auto message{dmn::gui::Dmn_JsonValue::parse(*text)};
      if (!message || !message->isObject()) {
        continue;
      }

      const auto type{message->getString("type")};
      if (type == "event") {
        handleEvent(*message);
      } else if (type == "direct_monitor") {
        handleDirect(*message);
      } else if (type == "bye") {
        return;
      }
    }
  }};

  const auto stopMonitor{[&]() -> std::string {
    (void)engine.send(R"({"type":"stop_direct"})");
    std::unique_lock lock{direct.mutex};
    direct.changed.wait(lock, [&] { return direct.stopped; });

    return direct.stopError;
  }};

  const auto finish{[&] {
    (void)engine.send(R"({"type":"shutdown"})");
    reader.join();
  }};

  auto command{dmn::gui::Dmn_JsonValue::object()};
  command.set("type", "monitor_direct");
  command.set("device", options.device);
  if (options.adapter) {
    command.set("adapter", *options.adapter);
  }

  if (!engine.send(command.dump())) {
    finish();
    std::cerr << "unable to configure ANCS event delivery\n";

    return exitCode(ExitStatus::AncsStartupFailure);
  }

  bool cancelSent{};
  {
    std::unique_lock lock{direct.mutex};
    while (direct.phase == DirectState::Phase::Starting) {
      direct.changed.wait_for(lock, 50ms);
      if (!cancelSent && signalSeen.load(std::memory_order_acquire)) {
        cancelSent = true;
        (void)engine.send(R"({"type":"stop_direct"})");
      }
    }
  }

  if (direct.phase == DirectState::Phase::Failed) {
    const auto interrupted{direct.cancelled &&
                           signalSeen.load(std::memory_order_acquire)};
    finish();
    if (interrupted) {
      return exitCode(ExitStatus::Success);
    }

    const auto reported{startFailure(direct.code)};
    std::cerr << reported.prefix << ": " << escapedText(direct.detail);
    if (!direct.remoteName.empty()) {
      std::cerr << " (" << escapedText(direct.remoteName) << ")";
    }

    std::cerr << '\n';

    return exitCode(reported.status);
  }

  auto stopBeforeMainLoop{[&] {
    const auto stopError{stopMonitor()};
    finish();
    if (!stopError.empty()) {
      std::cerr << "ANCS subscription cleanup failed: "
                << escapedText(stopError) << '\n';

      return exitCode(ExitStatus::RuntimeFailure);
    }

    return exitCode(ExitStatus::Success);
  }};

  sigset_t pendingSignals{};
  if (sigpending(&pendingSignals) != 0) {
    const auto error{errno};
    std::cerr << "unable to check for pending shutdown signals: "
              << std::strerror(error) << '\n';
    (void)stopBeforeMainLoop();

    return exitCode(ExitStatus::RuntimeFailure);
  }

  const auto interruptPending{sigismember(&pendingSignals, SIGINT)};
  const auto terminatePending{sigismember(&pendingSignals, SIGTERM)};
  if (interruptPending < 0 || terminatePending < 0) {
    const auto error{errno};
    std::cerr << "unable to inspect pending shutdown signals: "
              << std::strerror(error) << '\n';
    (void)stopBeforeMainLoop();

    return exitCode(ExitStatus::RuntimeFailure);
  }

  if (signalSeen.load(std::memory_order_acquire) || interruptPending != 0 ||
      terminatePending != 0) {
    return stopBeforeMainLoop();
  }

  std::cerr << "monitor ready; press Ctrl-C to stop\n";
  mainLoopActive = true;
  try {
    runtime->enterMainLoop();
  } catch (const std::exception &error) {
    failure.set(ExitStatus::RuntimeFailure, error.what());
  }

  mainLoopActive = false;

  const auto stopError{stopMonitor()};
  if (!stopError.empty()) {
    failure.set(ExitStatus::RuntimeFailure,
                "ANCS subscription cleanup failed: " + stopError);
  }

  finish();

  const auto [currentStatus, currentDiagnostic] = failure.get();
  if (currentStatus != exitCode(ExitStatus::Success) &&
      !currentDiagnostic.empty()) {
    std::cerr << "monitor failed: " << escapedText(currentDiagnostic) << '\n';
  }

  auto drainOutputJobs{[&] {
    auto barrierComplete{std::make_shared<std::atomic_bool>()};
    runtime->addJob([runtime, barrierComplete](const dmn::Dmn_Runtime_Job &) {
      *barrierComplete = true;
      runtime->exitMainLoop();
    });

    while (!*barrierComplete) {
      mainLoopActive = true;
      runtime->enterMainLoop();
      mainLoopActive = false;
    }
  }};

  try {
    drainOutputJobs();
    (void)outputPipe.waitForEmpty();
  } catch (const std::exception &error) {
    failure.set(ExitStatus::RuntimeFailure,
                "monitor output drain failed: " + std::string{error.what()});
  }

  return failure.get().first;
}

} // namespace

auto main(int argc, char **argv) -> int {
  if (argc == 2 && std::string_view{argv[1]} == "--version") {
    std::cout << "dmn-bluetooth-cli " << DMN_VERSION << '\n';

    return exitCode(ExitStatus::Success);
  }

  const auto applicationStartedAt{std::chrono::system_clock::now()};
  Options options;
  options.applicationStartedAt = applicationStartedAt;
  if (!parseOptions(argc, argv, options)) {
    usage(std::cerr);
    std::cerr << "invalid or duplicate command-line option\n";

    return exitCode(ExitStatus::InvalidCommandLine);
  }

  if (options.help) {
    usage(std::cout);

    return exitCode(ExitStatus::Success);
  }

  return runMonitor(options);
}
