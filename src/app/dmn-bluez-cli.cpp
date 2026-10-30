/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-bluez-cli.cpp
 * @brief Command-line application for BlueZ inventory and device actions.
 */

#include "dmn-bluez-client.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <exception>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

namespace {

enum class ExitStatus : int {
  Success = 0,
  InvalidCommandLine = 2,
  NoPoweredAdapter = 3,
  BluezUnavailable = 4,
  DiscoveryFailure = 5,
  AmbiguousAdapter = 6,
  AdapterNotFound = 7,
  RuntimeError = 8,
  DeviceSelectionOrArgumentError = 9,
  DeviceActionRejected = 10,
  RequestedStateNotReached = 11
};

constexpr auto exitCode(ExitStatus status) -> int {
  return static_cast<int>(status);
}

volatile std::sig_atomic_t stopRequested{};

void requestStop(int) { stopRequested = 1; }

struct Options {
  bool discovery{};
  bool help{};
  std::optional<std::string> adapter;
  std::optional<std::string> connect;
  std::optional<std::string> disconnect;
  std::optional<std::string> pair;
};

void usage(std::ostream &out) {
  out << "BlueZ command-line utility for device inventory and operations.\n"
         "Usage: dmn-bluez-cli [--discovery] "
         "[--adapter <address-or-object-path>] [--help]\n"
         "       dmn-bluez-cli [--adapter <address-or-object-path>] "
         "--connect <device-address-or-path> [--discovery]\n"
         "       dmn-bluez-cli [--adapter <address-or-object-path>] "
         "--pair <device-address-or-path>\n"
         "       dmn-bluez-cli [--adapter <address-or-object-path>] "
         "--disconnect <device-address-or-path>\n"
         "Prints an RFC 4180 CSV inventory to stdout; diagnostics go to "
         "stderr.\n";
}

auto parseOptions(int argc, char **argv, Options &options) -> bool {
  bool adapterSpecified{};

  for (int index = 1; index < argc; ++index) {
    const std::string argument{argv[index]};
    if (argument == "--help") {
      if (options.help) {
        return false;
      }

      options.help = true;
    } else if (argument == "--discovery") {
      if (options.discovery) {
        return false;
      }

      options.discovery = true;
    } else if (argument == "--adapter") {
      if (adapterSpecified || index + 1 >= argc || argv[index + 1][0] == '-') {
        return false;
      }

      adapterSpecified = true;
      options.adapter = argv[++index];
    } else if (argument == "--connect" || argument == "--disconnect" ||
               argument == "--pair") {
      if (index + 1 >= argc || argv[index + 1][0] == '-') {
        return false;
      }

      auto &action = argument == "--connect"      ? options.connect
                     : argument == "--disconnect" ? options.disconnect
                                                  : options.pair;
      if (action) {
        return false;
      }

      action = argv[++index];
    } else {
      return false;
    }
  }

  if (options.help) {
    return argc == 2;
  }

  const auto actionCount{options.connect.has_value() +
                         options.disconnect.has_value() +
                         options.pair.has_value()};

  return actionCount <= 1 && !(options.disconnect && options.discovery) &&
         !(options.pair && options.discovery);
}

auto normalizedAddress(std::string_view address) -> std::string {
  std::string normalized{address};
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::toupper(character));
                 });

  return normalized;
}

auto stateText(const std::optional<bool> &state) -> std::string {
  if (!state) {
    return {};
  }

  // If we get here, state definitely has a value, but value_or(false)
  // satisfies the compiler's strict static analyzer.
  return state.value_or(false) ? "true" : "false";
}

auto adapterLabel(const dmn::Dmn_BluezAdapter &adapter) -> std::string {
  return adapter.name + " [" +
         (adapter.address.empty() ? adapter.object_path : adapter.address) +
         "]";
}

auto csvField(std::string_view value) -> std::string {
  if (value.find_first_of(",\"\r\n") == std::string_view::npos) {
    return std::string{value};
  }

  std::string escaped{"\""};

  for (const char character : value) {
    if (character == '"') {
      escaped += "\"\"";
    } else {
      escaped += character;
    }
  }

  escaped += '"';

  return escaped;
}

void printCsvRow(const std::vector<std::string> &fields) {
  for (std::size_t index = 0; index < fields.size(); ++index) {
    if (index != 0) {
      std::cout << ',';
    }

    std::cout << csvField(fields[index]);
  }

  std::cout << "\r\n";
  std::cout.flush();
}

void printCsvHeader() {
  printCsvRow({"action", "adapter_name", "adapter_address", "adapter_path",
               "adapter_powered", "device_address", "name", "alias",
               "reported_name", "paired", "connected", "services_resolved",
               "rssi", "device_path", "seen_during_discovery"});
}

auto adapterIdentity(const dmn::Dmn_BluezAdapter &adapter) -> std::string {
  return adapter.address.empty() ? adapter.object_path : adapter.address;
}

void sortInventory(std::vector<dmn::Dmn_BluezAdapter> &adapters,
                   std::vector<dmn::Dmn_BluezDevice> &devices) {
  std::sort(adapters.begin(), adapters.end(),
            [](const auto &left, const auto &right) {
              return std::tuple{adapterIdentity(left), left.object_path} <
                     std::tuple{adapterIdentity(right), right.object_path};
            });

  const auto identityFor{[&adapters](const std::string &adapterPath) {
    const auto adapter{std::find_if(adapters.begin(), adapters.end(),
                                    [&adapterPath](const auto &candidate) {
                                      return candidate.object_path ==
                                             adapterPath;
                                    })};

    return adapter == adapters.end() ? adapterPath : adapterIdentity(*adapter);
  }};

  std::sort(devices.begin(), devices.end(),
            [&identityFor](const auto &left, const auto &right) {
              return std::tuple{identityFor(left.adapter_path), left.address,
                                left.adapter_path} <
                     std::tuple{identityFor(right.adapter_path), right.address,
                                right.adapter_path};
            });
}

void printDevice(const dmn::Dmn_BluezDevice &device,
                 const std::vector<dmn::Dmn_BluezAdapter> &adapters,
                 std::string_view action = "inventory") {
  const auto adapter{std::find_if(
      adapters.begin(), adapters.end(), [&device](const auto &candidate) {
        return candidate.object_path == device.adapter_path;
      })};
  const auto adapterName{adapter == adapters.end() ? std::string{}
                                                   : adapter->name};
  const auto adapterAddress{adapter == adapters.end() ? std::string{}
                                                      : adapter->address};

  // Directly check the adapter pointer inside stateText to completely bypass
  // GCC's optional bug
  const auto poweredStr =
      (adapter == adapters.end()) ? std::string{} : stateText(adapter->powered);

  printCsvRow(
      {std::string{action}, adapterName, adapterAddress, device.adapter_path,
       poweredStr, device.address, device.name, device.alias.value_or(""),
       device.reported_name.value_or(""), stateText(device.paired),
       stateText(device.connected), stateText(device.services_resolved),
       device.rssi ? std::to_string(*device.rssi) : std::string{},
       device.object_path, device.seen_during_discovery ? "true" : "false"});
}

void printEvent(const dmn::Dmn_BluezEvent &event,
                std::vector<dmn::Dmn_BluezAdapter> &adapters,
                bool includeDeviceEvents = true) {
  switch (event.kind) {
  case dmn::Dmn_BluezEventKind::AdapterAdded:
    if (event.adapter) {
      const auto existing{std::find_if(
          adapters.begin(), adapters.end(), [&event](const auto &adapter) {
            return adapter.object_path == event.adapter->object_path;
          })};

      if (existing == adapters.end()) {
        adapters.push_back(*event.adapter);
      } else {
        *existing = *event.adapter;
      }
    }

    break;
  case dmn::Dmn_BluezEventKind::AdapterChanged:
    if (event.adapter) {
      const auto existing{std::find_if(
          adapters.begin(), adapters.end(), [&event](const auto &adapter) {
            return adapter.object_path == event.adapter->object_path;
          })};

      if (existing == adapters.end()) {
        adapters.push_back(*event.adapter);
      } else {
        *existing = *event.adapter;
      }
    }

    break;
  case dmn::Dmn_BluezEventKind::AdapterRemoved:
    if (event.adapter) {
      std::erase_if(adapters, [&event](const auto &adapter) {
        return adapter.object_path == event.adapter->object_path;
      });
    }

    break;
  case dmn::Dmn_BluezEventKind::DeviceAdded:
    if (includeDeviceEvents && event.device) {
      printDevice(*event.device, adapters, "added");
    }

    break;
  case dmn::Dmn_BluezEventKind::DeviceChanged:
    if (includeDeviceEvents && event.device) {
      printDevice(*event.device, adapters, "changed");
    }

    break;
  case dmn::Dmn_BluezEventKind::DeviceRemoved:
    if (includeDeviceEvents && event.device) {
      printDevice(*event.device, adapters, "removed");
    }

    break;
  case dmn::Dmn_BluezEventKind::ServiceUnavailable:
  case dmn::Dmn_BluezEventKind::Error:
    std::cerr << "BlueZ error: " << event.message << '\n';

    break;
  case dmn::Dmn_BluezEventKind::ServiceAvailable:
    std::cerr << event.message << '\n';

    break;
  }
}

auto selectAdapter(const std::vector<dmn::Dmn_BluezAdapter> &adapters,
                   const std::optional<std::string> &selector,
                   int &errorCode) -> std::optional<dmn::Dmn_BluezAdapter> {
  std::vector<dmn::Dmn_BluezAdapter> powered;
  std::copy_if(
      adapters.begin(), adapters.end(), std::back_inserter(powered),
      [](const auto &adapter) { return adapter.powered.value_or(false); });
  if (selector) {
    std::vector<dmn::Dmn_BluezAdapter> matches;
    std::copy_if(powered.begin(), powered.end(), std::back_inserter(matches),
                 [&selector](const auto &adapter) {
                   return normalizedAddress(adapter.address) ==
                              normalizedAddress(*selector) ||
                          adapter.object_path == *selector;
                 });

    if (matches.size() == 1) {
      return matches.front();
    }

    if (matches.size() > 1) {
      std::cerr << "Adapter selection is ambiguous: " << *selector << '\n';
      errorCode = exitCode(ExitStatus::AmbiguousAdapter);
    } else {
      std::cerr << "No powered adapter matches: " << *selector << '\n';
      errorCode = exitCode(ExitStatus::AdapterNotFound);
    }

    return std::nullopt;
  }

  if (powered.size() == 1) {
    return powered.front();
  }

  if (powered.empty()) {
    std::cerr << "No powered Bluetooth adapter is available\n";
    errorCode = exitCode(ExitStatus::NoPoweredAdapter);
  } else {
    std::cerr << "Multiple powered adapters are available; use --adapter "
                 "<address-or-object-path>\n";
    errorCode = exitCode(ExitStatus::AmbiguousAdapter);
  }

  return std::nullopt;
}

struct DeviceSelection {
  std::optional<dmn::Dmn_BluezDevice> device;
  bool ambiguous{};
};

auto selectDevice(const std::vector<dmn::Dmn_BluezDevice> &devices,
                  const std::vector<dmn::Dmn_BluezAdapter> &adapters,
                  std::string_view selector,
                  const std::optional<std::string> &adapterSelector)
    -> DeviceSelection {
  std::vector<dmn::Dmn_BluezDevice> matches;
  const bool isObjectPath{selector.starts_with('/')};

  for (const auto &device : devices) {
    const bool matchesSelector{isObjectPath
                                   ? device.object_path == selector
                                   : normalizedAddress(device.address) ==
                                         normalizedAddress(selector)};
    if (!matchesSelector) {
      continue;
    }

    if (adapterSelector) {
      const auto adapter{std::find_if(
          adapters.begin(), adapters.end(), [&device](const auto &candidate) {
            return candidate.object_path == device.adapter_path;
          })};
      if (device.adapter_path != *adapterSelector &&
          (adapter == adapters.end() ||
           normalizedAddress(adapter->address) !=
               normalizedAddress(*adapterSelector))) {
        continue;
      }
    }

    matches.push_back(device);
  }

  if (matches.size() == 1) {
    return {std::move(matches.front()), false};
  }

  return {std::nullopt, matches.size() > 1};
}

auto errorExitCode(const dmn::Dmn_DbusError &error) -> int {
  if (error.remote_name == "org.freedesktop.DBus.Error.ServiceUnknown") {
    return exitCode(ExitStatus::BluezUnavailable);
  }

  if (error.kind == dmn::Dmn_DbusErrorKind::NotConnected) {
    return exitCode(ExitStatus::BluezUnavailable);
  }

  if (error.kind == dmn::Dmn_DbusErrorKind::Timeout) {
    return exitCode(ExitStatus::RequestedStateNotReached);
  }
  if (error.kind == dmn::Dmn_DbusErrorKind::InvalidArgument) {
    return exitCode(ExitStatus::DeviceSelectionOrArgumentError);
  }

  return exitCode(ExitStatus::RuntimeError);
}

} // namespace

auto main(int argc, char **argv) -> int {
  if (argc == 2 && std::string_view{argv[1]} == "--version") {
    std::cout << "dmn-bluez-cli " << DMN_VERSION << '\n';

    return exitCode(ExitStatus::Success);
  }

  Options options;
  if (!parseOptions(argc, argv, options)) {
    usage(std::cerr);
    std::cerr << "Invalid or duplicate command-line option\n";

    return exitCode(ExitStatus::InvalidCommandLine);
  }

  if (options.help) {
    usage(std::cout);

    return exitCode(ExitStatus::Success);
  }

  std::mutex eventMutex;
  std::deque<dmn::Dmn_BluezEvent> events;
  std::unique_ptr<dmn::Dmn_BluezClient> client;
  dmn::Dmn_BluezClientConfig clientConfig;
#if defined(DMN_BLUEZ_TESTING)
  const char *testBusAddress = std::getenv("DMN_BLUEZ_TEST_BUS_ADDRESS");
  if (testBusAddress == nullptr || *testBusAddress == '\0') {
    std::cerr << "DMN_BLUEZ_TEST_BUS_ADDRESS is required by this test build\n";

    return exitCode(ExitStatus::RuntimeError);
  }

  clientConfig.test_bus_address = testBusAddress;
#endif
  try {
    client = std::make_unique<dmn::Dmn_BluezClient>(clientConfig);
  } catch (const std::exception &error) {
    std::cerr << "Unable to connect to the system D-Bus: " << error.what()
              << '\n';

    return exitCode(ExitStatus::RuntimeError);
  }

  client->setEventHandler([&](const auto &event) {
    std::lock_guard lock{eventMutex};

    events.push_back(event);
  });

  auto snapshot{client->snapshot()};
  if (!snapshot) {
    std::cerr << "Unable to read BlueZ inventory: " << snapshot.error().message;

    if (!snapshot.error().remote_name.empty()) {
      std::cerr << " (" << snapshot.error().remote_name << ")";
    }

    std::cerr << '\n';

    return errorExitCode(snapshot.error());
  }

  auto inventory{std::move(*snapshot)};
  auto adapters{std::move(inventory.adapters)};
  auto devices{std::move(inventory.devices)};
  sortInventory(adapters, devices);

  const bool deviceAction{options.connect.has_value() ||
                          options.disconnect.has_value() ||
                          options.pair.has_value()};
  printCsvHeader();
  if (!deviceAction) {
    for (const auto &device : devices) {
      printDevice(device, adapters);
    }
  }

  const auto processPendingEvents{[&]() {
    std::deque<dmn::Dmn_BluezEvent> pending;
    {
      std::lock_guard lock{eventMutex};

      pending.swap(events);
    }

    int errorCode{exitCode(ExitStatus::Success)};
    for (const auto &event : pending) {
      printEvent(event, adapters, !deviceAction);

      if (event.kind == dmn::Dmn_BluezEventKind::ServiceUnavailable) {
        errorCode = exitCode(ExitStatus::BluezUnavailable);
      } else if (event.kind == dmn::Dmn_BluezEventKind::Error) {
        errorCode = exitCode(ExitStatus::RuntimeError);
      }
    }

    return errorCode;
  }};

  if (!options.connect && !options.disconnect && !options.pair) {
    if (!options.discovery) {
      return exitCode(ExitStatus::Success);
    }

    int adapterError{exitCode(ExitStatus::Success)};
    const auto adapter{selectAdapter(adapters, options.adapter, adapterError)};
    if (!adapter) {
      return adapterError;
    }

    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);
    auto started{client->startDiscovery(adapter->object_path)};

    if (!started) {
      std::cerr << "Unable to start discovery: " << started.error().message;
      if (!started.error().remote_name.empty()) {
        std::cerr << " (" << started.error().remote_name << ")";
      }

      std::cerr << '\n';
      const auto errorCode{errorExitCode(started.error())};

      return errorCode == exitCode(ExitStatus::BluezUnavailable)
                 ? exitCode(ExitStatus::BluezUnavailable)
                 : exitCode(ExitStatus::DiscoveryFailure);
    }

    std::cerr << "Discovery active on " << adapterLabel(*adapter)
              << "; press Ctrl-C to stop\n";

    int resultCode{exitCode(ExitStatus::Success)};
    while (stopRequested == 0) {
      resultCode = processPendingEvents();

      if (resultCode != exitCode(ExitStatus::Success)) {
        stopRequested = 1;
        break;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }

    auto stopped{client->stopDiscovery()};
    if (!stopped) {
      std::cerr << "Unable to stop discovery: " << stopped.error().message
                << '\n';
      resultCode = exitCode(ExitStatus::DiscoveryFailure);
    }

    return resultCode;
  }

  const bool connect{options.connect.has_value()};
  const bool pair{options.pair.has_value()};
  const std::string &selector = connect ? *options.connect
                                : pair  ? *options.pair
                                        : *options.disconnect;
  const bool requestedConnected{connect};
  auto selected{
      selectDevice(client->devices(), adapters, selector, options.adapter)};
  if (selected.ambiguous) {
    std::cerr << "Device selection is ambiguous; specify --adapter: "
              << selector << '\n';

    return exitCode(ExitStatus::DeviceSelectionOrArgumentError);
  }

  if (!selected.device && connect && options.discovery) {
    int adapterError{exitCode(ExitStatus::Success)};
    const auto adapter{selectAdapter(adapters, options.adapter, adapterError)};
    if (!adapter) {
      return adapterError;
    }

    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);
    auto started{client->startDiscovery(adapter->object_path)};
    if (!started) {
      std::cerr << "Unable to start discovery: " << started.error().message;
      if (!started.error().remote_name.empty()) {
        std::cerr << " (" << started.error().remote_name << ")";
      }

      std::cerr << '\n';
      const auto errorCode{errorExitCode(started.error())};

      return errorCode == exitCode(ExitStatus::BluezUnavailable)
                 ? exitCode(ExitStatus::BluezUnavailable)
                 : exitCode(ExitStatus::DiscoveryFailure);
    }

    std::cerr << "Discovery active while waiting for selected device\n";
    const auto discoveryDeadline{std::chrono::steady_clock::now() +
                                 std::chrono::seconds{60}};
    int discoveryError{exitCode(ExitStatus::Success)};

    while (!selected.device && stopRequested == 0 &&
           std::chrono::steady_clock::now() < discoveryDeadline) {
      const auto eventError{processPendingEvents()};

      if (eventError != exitCode(ExitStatus::Success)) {
        discoveryError = eventError;
        std::cerr << "Unable to resolve the selected device while scanning\n";

        break;
      }

      selected =
          selectDevice(client->devices(), adapters, selector, options.adapter);
      if (selected.ambiguous) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }

    auto stopped{client->stopDiscovery()};
    if (!stopped) {
      std::cerr << "Unable to stop discovery: " << stopped.error().message
                << '\n';

      return exitCode(ExitStatus::DiscoveryFailure);
    }

    if (!selected.device) {
      if (discoveryError != exitCode(ExitStatus::Success)) {
        return discoveryError;
      }

      if (stopRequested != 0) {
        std::cerr << "Interrupted before the selected device was discovered\n";
      } else if (selected.ambiguous) {
        std::cerr << "Device selection is ambiguous; specify --adapter: "
                  << selector << '\n';

        return exitCode(ExitStatus::DeviceSelectionOrArgumentError);
      } else {
        std::cerr << "Selected device was not discovered before the deadline: "
                  << selector << '\n';
      }

      return exitCode(ExitStatus::RequestedStateNotReached);
    }
  }

  if (!selected.device) {
    std::cerr << "Selected device is not present in the current BlueZ "
                 "inventory: "
              << selector << '\n';

    return exitCode(ExitStatus::DeviceSelectionOrArgumentError);
  }

  const auto target{*selected.device};
  const auto targetAdapter{std::find_if(
      adapters.begin(), adapters.end(), [&target](const auto &candidate) {
        return candidate.object_path == target.adapter_path;
      })};
  const std::string targetAdapterSelector{options.adapter.value_or(
      targetAdapter == adapters.end() ? target.adapter_path
                                      : adapterIdentity(*targetAdapter))};

#if defined(DMN_BLUEZ_TEST_OPERATION_TIMEOUT_MS)
  // Keep action-deadline tests fast without changing the installed CLI.
  constexpr auto testOperationTimeout{
      std::chrono::milliseconds{DMN_BLUEZ_TEST_OPERATION_TIMEOUT_MS}};
  constexpr auto pairingOperationTimeout{
      std::chrono::milliseconds{DMN_BLUEZ_TEST_OPERATION_TIMEOUT_MS}};
#else
  constexpr auto testOperationTimeout{std::chrono::seconds{30}};
  constexpr auto pairingOperationTimeout{std::chrono::seconds{120}};
#endif
  const auto operationTimeout{pair ? pairingOperationTimeout
                                   : testOperationTimeout};
  const auto operationDeadline{std::chrono::steady_clock::now() +
                               operationTimeout};
  const auto remaining{[&]() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        operationDeadline - std::chrono::steady_clock::now());
  }};

  const auto actionResult{
      connect ? client->connectDevice(target.address, targetAdapterSelector,
                                      remaining())
      : pair  ? client->pairDevice(target.address, targetAdapterSelector,
                                   remaining())
              : client->disconnectDevice(target.address, targetAdapterSelector,
                                         remaining())};
  if (!actionResult) {
    std::cerr << "Unable to "
              << (connect ? "connect"
                  : pair  ? "pair"
                          : "disconnect")
              << " selected device: " << actionResult.error().message;
    if (!actionResult.error().remote_name.empty()) {
      std::cerr << " (" << actionResult.error().remote_name << ")";
    }

    std::cerr << '\n';
    if (actionResult.error().kind == dmn::Dmn_DbusErrorKind::Remote) {
      return exitCode(ExitStatus::DeviceActionRejected);
    }

    return errorExitCode(actionResult.error());
  }

  auto current{selectDevice(client->devices(), adapters, target.address,
                            targetAdapterSelector)};
  const auto requestedStateObserved{[&] {
    return pair ? current.device && current.device->paired &&
                      *current.device->paired
                : current.device && current.device->connected &&
                      *current.device->connected == requestedConnected;
  }};

  if (!requestedStateObserved()) {
    auto fresh{client->snapshot(remaining())};
    if (!fresh) {
      std::cerr << "Unable to confirm selected device state: "
                << fresh.error().message << '\n';

      return errorExitCode(fresh.error());
    }

    adapters = std::move(fresh->adapters);
    current = selectDevice(client->devices(), adapters, target.address,
                           targetAdapterSelector);
    if (!current.device) {
      // A removed target cannot produce the requested connection-state event.
      std::cerr << "Selected device disappeared before its requested state "
                   "was observed: "
                << target.address << '\n';

      return exitCode(ExitStatus::DeviceSelectionOrArgumentError);
    }
  }

  while (std::chrono::steady_clock::now() < operationDeadline) {
    const auto eventError{processPendingEvents()};
    if (eventError != 0) {
      return eventError;
    }

    current = selectDevice(client->devices(), adapters, target.address,
                           targetAdapterSelector);
    if (!current.device) {
      // Distinguish a removed device from owner loss racing event delivery.
      auto fresh{client->snapshot(remaining())};
      if (!fresh) {
        std::cerr << "Unable to confirm selected device state: "
                  << fresh.error().message << '\n';

        return errorExitCode(fresh.error());
      }

      adapters = std::move(fresh->adapters);
      current = selectDevice(client->devices(), adapters, target.address,
                             targetAdapterSelector);
    }

    if (!current.device) {
      // A removed target cannot produce the requested state transition.
      std::cerr << "Selected device disappeared before its requested state "
                   "was observed: "
                << target.address << '\n';

      return exitCode(ExitStatus::DeviceSelectionOrArgumentError);
    }

    if (requestedStateObserved()) {
      printDevice(*current.device, adapters,
                  pair                 ? "paired"
                  : requestedConnected ? "connected"
                                       : "disconnected");

      return exitCode(ExitStatus::Success);
    }

    if (stopRequested != 0) {
      break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds{50});
  }

  std::cerr << "Selected device did not reach the requested "
            << (pair                 ? "paired"
                : requestedConnected ? "connected"
                                     : "disconnected")
            << " state before the deadline\n";

  return exitCode(ExitStatus::RequestedStateNotReached);
}
