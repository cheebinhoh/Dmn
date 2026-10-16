/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-bluez-discovery.cpp
 * @brief Read-only BlueZ inventory and explicitly requested discovery CLI.
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

volatile std::sig_atomic_t stopRequested{};

void requestStop(int) { stopRequested = 1; }

struct Options {
  bool discovery{};
  bool help{};
  std::optional<std::string> adapter;
  std::optional<std::string> connect;
  std::optional<std::string> disconnect;
};

void usage(std::ostream &out) {
  out << "Usage: dmn-bluez-discovery [--discovery] "
         "[--adapter <address-or-object-path>] [--help]\n"
         "       dmn-bluez-discovery [--adapter <address-or-object-path>] "
         "--connect <device-address-or-path> [--discovery]\n"
         "       dmn-bluez-discovery [--adapter <address-or-object-path>] "
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
    } else if (argument == "--connect" || argument == "--disconnect") {
      if (index + 1 >= argc || argv[index + 1][0] == '-') {
        return false;
      }

      auto &action =
          argument == "--connect" ? options.connect : options.disconnect;
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

  return !(options.connect && options.disconnect) &&
         !(options.disconnect && options.discovery);
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

  return *state ? "true" : "false";
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
  const auto identityFor = [&adapters](const std::string &adapterPath) {
    const auto adapter =
        std::find_if(adapters.begin(), adapters.end(),
                     [&adapterPath](const auto &candidate) {
                       return candidate.object_path == adapterPath;
                     });
    return adapter == adapters.end() ? adapterPath : adapterIdentity(*adapter);
  };
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
  const auto adapter = std::find_if(
      adapters.begin(), adapters.end(), [&device](const auto &candidate) {
        return candidate.object_path == device.adapter_path;
      });
  const auto adapterName =
      adapter == adapters.end() ? std::string{} : adapter->name;
  const auto adapterAddress =
      adapter == adapters.end() ? std::string{} : adapter->address;
  const auto adapterPowered =
      adapter == adapters.end() ? std::optional<bool>{} : adapter->powered;
  printCsvRow({std::string{action}, adapterName, adapterAddress,
               device.adapter_path, stateText(adapterPowered), device.address,
               device.name, device.alias.value_or(""),
               device.reported_name.value_or(""), stateText(device.paired),
               stateText(device.connected), stateText(device.services_resolved),
               device.rssi ? std::to_string(*device.rssi) : std::string{},
               device.object_path,
               device.seen_during_discovery ? "true" : "false"});
}

void printEvent(const dmn::Dmn_BluezEvent &event,
                std::vector<dmn::Dmn_BluezAdapter> &adapters) {
  switch (event.kind) {
  case dmn::Dmn_BluezEventKind::AdapterAdded:
    if (event.adapter) {
      const auto existing = std::find_if(
          adapters.begin(), adapters.end(), [&event](const auto &adapter) {
            return adapter.object_path == event.adapter->object_path;
          });
      if (existing == adapters.end()) {
        adapters.push_back(*event.adapter);
      } else {
        *existing = *event.adapter;
      }
    }

    break;
  case dmn::Dmn_BluezEventKind::AdapterChanged:
    if (event.adapter) {
      const auto existing = std::find_if(
          adapters.begin(), adapters.end(), [&event](const auto &adapter) {
            return adapter.object_path == event.adapter->object_path;
          });
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
    if (event.device) {
      printDevice(*event.device, adapters, "added");
    }

    break;
  case dmn::Dmn_BluezEventKind::DeviceChanged:
    if (event.device) {
      printDevice(*event.device, adapters, "changed");
    }

    break;
  case dmn::Dmn_BluezEventKind::DeviceRemoved:
    if (event.device) {
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
      errorCode = 6;
    } else {
      std::cerr << "No powered adapter matches: " << *selector << '\n';
      errorCode = 7;
    }

    return std::nullopt;
  }

  if (powered.size() == 1) {
    return powered.front();
  }

  if (powered.empty()) {
    std::cerr << "No powered Bluetooth adapter is available\n";
    errorCode = 3;
  } else {
    std::cerr << "Multiple powered adapters are available; use --adapter "
                 "<address-or-object-path>\n";
    errorCode = 6;
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
  const bool isObjectPath = selector.starts_with('/');
  for (const auto &device : devices) {
    const bool matchesSelector =
        isObjectPath
            ? device.object_path == selector
            : normalizedAddress(device.address) == normalizedAddress(selector);
    if (!matchesSelector) {
      continue;
    }

    if (adapterSelector) {
      const auto adapter = std::find_if(
          adapters.begin(), adapters.end(), [&device](const auto &candidate) {
            return candidate.object_path == device.adapter_path;
          });
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
    return 4;
  }

  if (error.kind == dmn::Dmn_DbusErrorKind::NotConnected) {
    return 4;
  }

  if (error.kind == dmn::Dmn_DbusErrorKind::Timeout) {
    return 11;
  }
  if (error.kind == dmn::Dmn_DbusErrorKind::InvalidArgument) {
    return 9;
  }

  return 8;
}

} // namespace

auto main(int argc, char **argv) -> int {
  Options options;
  if (!parseOptions(argc, argv, options)) {
    usage(std::cerr);
    std::cerr << "Invalid or duplicate command-line option\n";
    return 2;
  }

  if (options.help) {
    usage(std::cout);
    return 0;
  }

  std::mutex eventMutex;
  std::deque<dmn::Dmn_BluezEvent> events;
  std::unique_ptr<dmn::Dmn_BluezClient> client;
  dmn::Dmn_BluezClientConfig clientConfig;
#if defined(DMN_BLUEZ_TESTING)
  const char *testBusAddress = std::getenv("DMN_BLUEZ_TEST_BUS_ADDRESS");
  if (testBusAddress == nullptr || *testBusAddress == '\0') {
    std::cerr << "DMN_BLUEZ_TEST_BUS_ADDRESS is required by this test build\n";
    return 8;
  }

  clientConfig.test_bus_address = testBusAddress;
#endif
  try {
    client = std::make_unique<dmn::Dmn_BluezClient>(clientConfig);
  } catch (const std::exception &error) {
    std::cerr << "Unable to connect to the system D-Bus: " << error.what()
              << '\n';
    return 8;
  }

  client->setEventHandler([&](const auto &event) {
    std::lock_guard lock{eventMutex};
    events.push_back(event);
  });

  auto snapshot = client->snapshot();
  if (!snapshot) {
    std::cerr << "Unable to read BlueZ inventory: " << snapshot.error().message;
    if (!snapshot.error().remote_name.empty()) {
      std::cerr << " (" << snapshot.error().remote_name << ")";
    }

    std::cerr << '\n';
    return errorExitCode(snapshot.error());
  }

  auto inventory = std::move(*snapshot);
  auto adapters = std::move(inventory.adapters);
  auto devices = std::move(inventory.devices);
  sortInventory(adapters, devices);

  printCsvHeader();
  for (const auto &device : devices) {
    printDevice(device, adapters);
  }

  const auto processPendingEvents = [&]() {
    std::deque<dmn::Dmn_BluezEvent> pending;
    {
      std::lock_guard lock{eventMutex};
      pending.swap(events);
    }

    int errorCode{};
    for (const auto &event : pending) {
      printEvent(event, adapters);
      if (event.kind == dmn::Dmn_BluezEventKind::ServiceUnavailable) {
        errorCode = 4;
      } else if (event.kind == dmn::Dmn_BluezEventKind::Error) {
        errorCode = 8;
      }
    }

    return errorCode;
  };

  if (!options.connect && !options.disconnect) {
    if (!options.discovery) {
      return 0;
    }

    int adapterError{};
    const auto adapter = selectAdapter(adapters, options.adapter, adapterError);
    if (!adapter) {
      return adapterError;
    }

    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);
    auto started = client->startDiscovery(adapter->object_path);
    if (!started) {
      std::cerr << "Unable to start discovery: " << started.error().message;
      if (!started.error().remote_name.empty()) {
        std::cerr << " (" << started.error().remote_name << ")";
      }

      std::cerr << '\n';
      const auto errorCode = errorExitCode(started.error());
      return errorCode == 4 ? 4 : 5;
    }

    std::cerr << "Discovery active on " << adapterLabel(*adapter)
              << "; press Ctrl-C to stop\n";

    int resultCode{};
    while (stopRequested == 0) {
      resultCode = processPendingEvents();
      if (resultCode != 0) {
        stopRequested = 1;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }

    auto stopped = client->stopDiscovery();
    if (!stopped) {
      std::cerr << "Unable to stop discovery: " << stopped.error().message
                << '\n';
      resultCode = 5;
    }

    return resultCode;
  }

  const bool connect = options.connect.has_value();
  const std::string &selector =
      connect ? *options.connect : *options.disconnect;
  const bool requestedConnected = connect;
  auto selected =
      selectDevice(client->devices(), adapters, selector, options.adapter);
  if (selected.ambiguous) {
    std::cerr << "Device selection is ambiguous; specify --adapter: "
              << selector << '\n';
    return 9;
  }

  if (!selected.device && connect && options.discovery) {
    int adapterError{};
    const auto adapter = selectAdapter(adapters, options.adapter, adapterError);
    if (!adapter) {
      return adapterError;
    }

    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);
    auto started = client->startDiscovery(adapter->object_path);
    if (!started) {
      std::cerr << "Unable to start discovery: " << started.error().message;
      if (!started.error().remote_name.empty()) {
        std::cerr << " (" << started.error().remote_name << ")";
      }
      std::cerr << '\n';
      const auto errorCode = errorExitCode(started.error());
      return errorCode == 4 ? 4 : 5;
    }
    std::cerr << "Discovery active while waiting for selected device\n";
    const auto discoveryDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{60};
    int discoveryError{};
    while (!selected.device && stopRequested == 0 &&
           std::chrono::steady_clock::now() < discoveryDeadline) {
      const auto eventError = processPendingEvents();
      if (eventError != 0) {
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
    auto stopped = client->stopDiscovery();
    if (!stopped) {
      std::cerr << "Unable to stop discovery: " << stopped.error().message
                << '\n';
      return 5;
    }

    if (!selected.device) {
      if (discoveryError != 0) {
        return discoveryError;
      }
      if (stopRequested != 0) {
        std::cerr << "Interrupted before the selected device was discovered\n";
      } else if (selected.ambiguous) {
        std::cerr << "Device selection is ambiguous; specify --adapter: "
                  << selector << '\n';
        return 9;
      } else {
        std::cerr << "Selected device was not discovered before the deadline: "
                  << selector << '\n';
      }
      return 11;
    }
  }

  if (!selected.device) {
    std::cerr << "Selected device is not present in the current BlueZ "
                 "inventory: "
              << selector << '\n';
    return 9;
  }

  const auto target = *selected.device;
  const auto targetAdapter = std::find_if(
      adapters.begin(), adapters.end(), [&target](const auto &candidate) {
        return candidate.object_path == target.adapter_path;
      });
  const std::string targetAdapterSelector = options.adapter.value_or(
      targetAdapter == adapters.end() ? target.adapter_path
                                      : adapterIdentity(*targetAdapter));

  constexpr auto operationTimeout = std::chrono::seconds{30};
  const auto operationDeadline =
      std::chrono::steady_clock::now() + operationTimeout;
  const auto remaining = [&]() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        operationDeadline - std::chrono::steady_clock::now());
  };

  const auto actionResult =
      connect ? client->connectDevice(target.address, targetAdapterSelector,
                                      remaining())
              : client->disconnectDevice(target.address, targetAdapterSelector,
                                         remaining());
  if (!actionResult) {
    std::cerr << "Unable to " << (connect ? "connect" : "disconnect")
              << " selected device: " << actionResult.error().message;
    if (!actionResult.error().remote_name.empty()) {
      std::cerr << " (" << actionResult.error().remote_name << ")";
    }
    std::cerr << '\n';
    if (actionResult.error().kind == dmn::Dmn_DbusErrorKind::Remote) {
      return 10;
    }
    return errorExitCode(actionResult.error());
  }

  auto current = selectDevice(client->devices(), adapters, target.address,
                              targetAdapterSelector);
  if (!current.device || !current.device->connected) {
    auto fresh = client->snapshot(remaining());
    if (!fresh) {
      std::cerr << "Unable to confirm selected device state: "
                << fresh.error().message << '\n';
      return errorExitCode(fresh.error());
    }
    adapters = std::move(fresh->adapters);
  }

  while (std::chrono::steady_clock::now() < operationDeadline) {
    const auto eventError = processPendingEvents();
    if (eventError != 0) {
      return eventError;
    }

    current = selectDevice(client->devices(), adapters, target.address,
                           targetAdapterSelector);
    if (current.device && current.device->connected &&
        *current.device->connected == requestedConnected) {
      printDevice(*current.device, adapters,
                  requestedConnected ? "connected" : "disconnected");
      return 0;
    }

    if (stopRequested != 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
  }

  std::cerr << "Selected device did not reach the requested "
            << (requestedConnected ? "connected" : "disconnected")
            << " state before the deadline\n";
  return 11;
}
