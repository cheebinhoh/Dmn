/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-bluez-discovery.cpp
 * @brief Read-only BlueZ inventory and explicitly requested discovery CLI.
 */

#include "dmn-bluez-client.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <exception>
#include <iostream>
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
};

void usage(std::ostream &out) {
  out << "Usage: dmn-bluez-discovery [--discovery] "
         "[--adapter <address-or-object-path>] [--help]\n"
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
    } else {
      return false;
    }
  }

  return true;
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
                   return adapter.address == *selector ||
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

auto errorExitCode(const dmn::Dmn_DbusError &error) -> int {
  if (error.remote_name == "org.freedesktop.DBus.Error.ServiceUnknown") {
    return 4;
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
    std::deque<dmn::Dmn_BluezEvent> pending;
    {
      std::lock_guard lock{eventMutex};
      pending.swap(events);
    }

    for (const auto &event : pending) {
      printEvent(event, adapters);
      if (event.kind == dmn::Dmn_BluezEventKind::ServiceUnavailable) {
        resultCode = 4;
        stopRequested = 1;
      } else if (event.kind == dmn::Dmn_BluezEventKind::Error) {
        resultCode = 8;
        stopRequested = 1;
      }
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
