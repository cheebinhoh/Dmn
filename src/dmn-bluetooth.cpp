/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-bluetooth.cpp
 * @brief Terminal monitor for one selected iPhone ANCS session.
 */

#include "dmn-ancs-bluez-monitor.hpp"
#include "dmn-pipe.hpp"
#include "dmn-runtime.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
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

constexpr std::size_t kMaximumQueuedOutputEvents{256};

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
  out << "Usage: dmn-bluetooth monitor DEVICE [OPTIONS]\n"
         "       dmn-bluetooth --help\n"
         "       dmn-bluetooth monitor --help\n"
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
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), seconds);
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
    const auto characterType = u_charType(static_cast<UChar32>(codePoint));
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
    const auto first = static_cast<unsigned char>(value[index]);
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
          const auto continuation =
              static_cast<unsigned char>(value[index + offset]);
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

auto eventKindName(dmn::Dmn_AncsNotificationKind kind) -> std::string_view {
  switch (kind) {
  case dmn::Dmn_AncsNotificationKind::Added:
    return "added";
  case dmn::Dmn_AncsNotificationKind::Updated:
    return "updated";
  case dmn::Dmn_AncsNotificationKind::Removed:
    return "removed";
  case dmn::Dmn_AncsNotificationKind::AttributesUnavailable:
    return "attributes unavailable";
  }

  return "unknown";
}

auto printNotification(const dmn::Dmn_AncsEvent &event) -> bool {
  if (!event.notification) {
    return true;
  }

  const auto &notification = *event.notification;
  std::string header;
  switch (event.kind) {
  case dmn::Dmn_AncsEvent::Kind::NotificationAdded:
    header = "notification added";
    break;
  case dmn::Dmn_AncsEvent::Kind::NotificationUpdated:
    header = "notification updated";
    break;
  case dmn::Dmn_AncsEvent::Kind::NotificationRemoved:
    header = "notification removed";
    break;
  case dmn::Dmn_AncsEvent::Kind::AttributesUnavailable:
    header = "notification attributes unavailable event=";
    header += eventKindName(notification.kind);
    break;
  default:
    return true;
  }

  std::cout << header << " uid=0x" << encodeHex(notification.uid, 8)
            << " category=" << categoryName(notification.category) << " count="
            << static_cast<unsigned int>(notification.category_count)
            << " flags=0x" << encodeHex(notification.flags, 2) << '\n';
  if (event.kind != dmn::Dmn_AncsEvent::Kind::NotificationRemoved &&
      event.kind != dmn::Dmn_AncsEvent::Kind::AttributesUnavailable) {
    const auto printField = [](std::string_view label,
                               const std::optional<std::string> &value) {
      if (value && !value->empty()) {
        std::cout << label << ": " << escapedText(*value) << '\n';
      }
    };
    printField("app_identifier", notification.app_identifier);
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
  dmn::Dmn_AncsEvent event;
};

auto runMonitor(const Options &options) -> int {
  auto runtime = dmn::Dmn_Runtime_Manager<>::createInstance();
  std::atomic_bool signalSeen{};
  runtime->registerSignalHandlerHook(SIGINT,
                                     [&signalSeen](int) { signalSeen = true; });
  runtime->registerSignalHandlerHook(SIGTERM,
                                     [&signalSeen](int) { signalSeen = true; });

  struct sigaction ignorePipe {};
  ignorePipe.sa_handler = SIG_IGN;
  sigemptyset(&ignorePipe.sa_mask);
  (void)sigaction(SIGPIPE, &ignorePipe, nullptr);

  dmn::Dmn_BluezClientConfig bluezConfig;
#if defined(DMN_BLUETOOTH_TESTING)
  const char *testBusAddress = std::getenv("DMN_BLUETOOTH_TEST_BUS_ADDRESS");
  if (testBusAddress == nullptr || *testBusAddress == '\0') {
    std::cerr << "test BlueZ bus address is not configured\n";

    return exitCode(ExitStatus::BluezUnavailable);
  }

  bluezConfig.test_bus_address = testBusAddress;
#endif

  std::unique_ptr<dmn::Dmn_AncsBluezMonitor> monitor;
  try {
    monitor = std::make_unique<dmn::Dmn_AncsBluezMonitor>(bluezConfig);
  } catch (const std::exception &error) {
    std::cerr << "BlueZ unavailable: " << escapedText(error.what()) << '\n';

    return exitCode(ExitStatus::BluezUnavailable);
  }

  FailureState failure;
  std::atomic_bool outputFailed{};
  std::atomic_bool mainLoopActive{};
  std::atomic<std::size_t> queuedOutputEvents{};

  dmn::Dmn_Pipe<OutputRecord> outputPipe{
      "dmn-bluetooth-output", [&](OutputRecord &&record) {
        CountRelease countRelease{queuedOutputEvents};
        if (outputFailed) {
          return;
        }

        if (!printNotification(record.event)) {
          outputFailed = true;
          failure.set(ExitStatus::RuntimeFailure,
                      "failed to write notification output");
          if (mainLoopActive) {
            runtime->exitMainLoop();
          }
        }
      }};

  auto handlerSet =
      monitor->setEventHandler([&](const dmn::Dmn_AncsEvent &event) {
        const auto count = queuedOutputEvents.fetch_add(1) + 1;
        if (count > kMaximumQueuedOutputEvents) {
          queuedOutputEvents.fetch_sub(1);
          failure.set(ExitStatus::RuntimeFailure,
                      "monitor output queue exceeded its 256-event limit");
          if (mainLoopActive) {
            runtime->exitMainLoop();
          }

          return;
        }

        try {
          runtime->addJob([&, event](const dmn::Dmn_Runtime_Job &) {
            if (outputFailed) {
              queuedOutputEvents.fetch_sub(1);

              return;
            }

            if (event.kind == dmn::Dmn_AncsEvent::Kind::Error && event.error) {
              std::cerr << "ANCS: " << escapedText(event.error->message)
                        << '\n';
              if (event.error->fatal) {
                failure.set(ExitStatus::RuntimeFailure, event.error->message);
                if (mainLoopActive) {
                  runtime->exitMainLoop();
                }
              }

              queuedOutputEvents.fetch_sub(1);

              return;
            }

            try {
              outputPipe.write(OutputRecord{event});
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
      });
  if (!handlerSet) {
    std::cerr << "unable to configure ANCS event delivery\n";

    return exitCode(ExitStatus::AncsStartupFailure);
  }

  dmn::Dmn_AncsMonitorConfig monitorConfig;
  monitorConfig.adapter_selector = options.adapter;
  monitorConfig.discovery_timeout = options.discoveryTimeout;
  monitorConfig.pairing_timeout = options.pairTimeout;
  monitorConfig.connection_timeout = options.connectTimeout;
  monitorConfig.ancs.service_resolution_timeout = options.servicesTimeout;
  if (options.unlimitedNotifications) {
    monitorConfig.ancs.maximum_pending_notifications = 0;
  }

  if (options.ignoreExistingNotifications) {
    monitorConfig.ancs.notification_date_cutoff = options.applicationStartedAt;
  }

  monitorConfig.progress_handler = [&options](const auto progress) {
    std::string_view message;
    switch (progress) {
    case dmn::Dmn_AncsMonitorProgress::Inventory:
      message = "checking BlueZ inventory for ";
      break;
    case dmn::Dmn_AncsMonitorProgress::Discovery:
      message = "discovering ";
      break;
    case dmn::Dmn_AncsMonitorProgress::Pairing:
      message = "pairing ";
      break;
    case dmn::Dmn_AncsMonitorProgress::Connection:
      message = "connecting ";
      break;
    case dmn::Dmn_AncsMonitorProgress::AncsStartup:
      message = "starting ANCS session for ";
      break;
    }

    std::cerr << message << escapedText(options.device) << '\n';
  };
  auto started = monitor->start(options.device, monitorConfig);
  if (!started) {
    const auto &error = started.error();
    std::string_view prefix;
    ExitStatus status{ExitStatus::BluezUnavailable};
    switch (error.stage) {
    case dmn::Dmn_AncsMonitorStage::InvalidArgument:
      prefix = "invalid monitor configuration";
      status = ExitStatus::InvalidCommandLine;
      break;
    case dmn::Dmn_AncsMonitorStage::NotReady:
      prefix = "monitor startup failed";
      status = ExitStatus::RuntimeFailure;
      break;
    case dmn::Dmn_AncsMonitorStage::Inventory:
      prefix = "BlueZ inventory unavailable";
      status = ExitStatus::BluezUnavailable;
      break;
    case dmn::Dmn_AncsMonitorStage::Discovery:
      prefix = "Bluetooth discovery failed";
      status = ExitStatus::BluezUnavailable;
      break;
    case dmn::Dmn_AncsMonitorStage::DeviceNotFound:
      prefix = "device was not found";
      status = ExitStatus::DeviceNotFound;
      break;
    case dmn::Dmn_AncsMonitorStage::Pairing:
      prefix = "device pairing failed";
      status = ExitStatus::PairOrConnectFailure;
      break;
    case dmn::Dmn_AncsMonitorStage::Connection:
      prefix = "device connection failed";
      status = ExitStatus::PairOrConnectFailure;
      break;
    case dmn::Dmn_AncsMonitorStage::AncsStartup:
      prefix = "ANCS startup failed";
      status = error.ancs_error_kind == dmn::Dmn_AncsErrorKind::Unsupported
                   ? ExitStatus::AncsUnavailable
                   : ExitStatus::AncsStartupFailure;
      break;
    case dmn::Dmn_AncsMonitorStage::Cleanup:
      prefix = "ANCS cleanup failed";
      status = ExitStatus::RuntimeFailure;
      break;
    }

    std::cerr << prefix << ": " << escapedText(error.message);
    if (!error.remote_name.empty()) {
      std::cerr << " (" << escapedText(error.remote_name) << ")";
    }

    std::cerr << '\n';

    return exitCode(status);
  }

  if (started->requested_remaining_profiles) {
    std::cerr << "requesting BlueZ to connect the GATT-capable bearer and "
                 "remaining profiles for "
              << escapedText(started->device.name) << '\n';
  }

  std::cerr << "monitor ready; press Ctrl-C to stop\n";
  mainLoopActive = true;
  try {
    runtime->enterMainLoop();
  } catch (const std::exception &error) {
    failure.set(ExitStatus::RuntimeFailure, error.what());
  }

  mainLoopActive = false;

  auto stopped = monitor->stop();
  if (!stopped) {
    failure.set(ExitStatus::RuntimeFailure,
                "ANCS subscription cleanup failed: " + stopped.error().message);
  }

  const auto [currentStatus, currentDiagnostic] = failure.get();
  if (currentStatus != exitCode(ExitStatus::Success) &&
      !currentDiagnostic.empty()) {
    std::cerr << "monitor failed: " << escapedText(currentDiagnostic) << '\n';
  }

  auto drainOutputJobs = [&] {
    auto barrierComplete = std::make_shared<std::atomic_bool>();
    runtime->addJob([runtime, barrierComplete](const dmn::Dmn_Runtime_Job &) {
      *barrierComplete = true;
      runtime->exitMainLoop();
    });

    while (!*barrierComplete) {
      mainLoopActive = true;
      runtime->enterMainLoop();
      mainLoopActive = false;
    }
  };

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
  const auto applicationStartedAt = std::chrono::system_clock::now();
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
