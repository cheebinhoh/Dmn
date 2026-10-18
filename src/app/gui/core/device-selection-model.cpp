/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file device-selection-model.cpp
 * @brief GTK-independent device inventory and selection state.
 */

#include "device-selection-model.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace dmn::gui {
namespace {

auto compareCaseInsensitive(const std::string_view left,
                            const std::string_view right) -> int {
  const auto commonLength = std::min(left.size(), right.size());
  for (std::size_t index = 0; index < commonLength; ++index) {
    const auto leftCharacter = static_cast<unsigned char>(left[index]);
    const auto rightCharacter = static_cast<unsigned char>(right[index]);
    const auto leftLower = std::tolower(leftCharacter);
    const auto rightLower = std::tolower(rightCharacter);

    if (leftLower != rightLower) {
      return leftLower < rightLower ? -1 : 1;
    }
  }

  if (left.size() == right.size()) {
    return 0;
  }

  return left.size() < right.size() ? -1 : 1;
}

auto normalizeAddress(std::string address) -> std::string {
  std::transform(address.begin(), address.end(), address.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::toupper(character));
                 });

  return address;
}

auto normalizeIdentity(Dmn_DeviceIdentity identity) -> Dmn_DeviceIdentity {
  identity.m_adapter_address =
      normalizeAddress(std::move(identity.m_adapter_address));
  identity.m_device_address =
      normalizeAddress(std::move(identity.m_device_address));

  return identity;
}

auto containsIdentity(const std::vector<Dmn_DeviceIdentity> &identities,
                      const Dmn_DeviceIdentity &identity) -> bool {
  return std::find(identities.begin(), identities.end(), identity) !=
         identities.end();
}

} // namespace

auto deviceDisplayName(const Dmn_DeviceEntry &device) -> std::string {
  if (device.m_display_name.empty() ||
      compareCaseInsensitive(device.m_display_name,
                             device.m_identity.m_device_address) == 0) {
    return device.m_identity.m_device_address;
  }

  return device.m_display_name;
}

auto deviceDisplayStatus(const Dmn_DeviceEntry &device) -> std::string {
  if (!device.m_present) {
    return "Unavailable";
  }

  auto status = device.m_paired ? (*device.m_paired ? "Paired" : "Not paired")
                                : "Pairing status unavailable";
  std::string result{status};

  if (device.m_connected) {
    result += *device.m_connected ? " · Connected" : " · Disconnected";
  } else {
    result += " · Connection status unavailable";
  }

  if (device.m_seen_during_discovery) {
    result += " · Found by scan";
  }

  return result;
}

Dmn_DeviceSelectionModel::Dmn_DeviceSelectionModel(
    const std::size_t selectionLimit)
    : m_selection_limit{selectionLimit} {
  if (m_selection_limit == 0) {
    throw std::invalid_argument{"device selection limit must be positive"};
  }
}

void Dmn_DeviceSelectionModel::replaceInventory(
    std::vector<Dmn_DeviceEntry> inventory) {
  for (auto &device : inventory) {
    device.m_identity = normalizeIdentity(std::move(device.m_identity));
  }

  m_devices = std::move(inventory);
  mergeSavedDevices();
}

void Dmn_DeviceSelectionModel::loadSavedSelection(
    std::vector<Dmn_DeviceIdentity> selection) {
  std::vector<Dmn_DeviceIdentity> normalized;
  normalized.reserve(selection.size());

  for (auto &identity : selection) {
    auto stableIdentity = normalizeIdentity(std::move(identity));
    if (stableIdentity.m_adapter_address.empty() ||
        stableIdentity.m_device_address.empty()) {
      continue;
    }

    if (!containsIdentity(normalized, stableIdentity)) {
      normalized.push_back(std::move(stableIdentity));
    }
  }

  if (normalized.size() > m_selection_limit) {
    normalized.resize(m_selection_limit);
  }

  m_saved_selection = normalized;
  m_draft_selection = std::move(normalized);
  mergeSavedDevices();
}

auto Dmn_DeviceSelectionModel::setDraftSelected(
    const Dmn_DeviceIdentity &identity, const bool selected) -> bool {
  const auto stableIdentity = normalizeIdentity(identity);
  const auto existing = std::find(m_draft_selection.begin(),
                                  m_draft_selection.end(), stableIdentity);
  if (!selected && existing != m_draft_selection.end()) {
    m_draft_selection.erase(existing);

    return true;
  }

  const bool wasSaved = containsIdentity(m_saved_selection, stableIdentity);
  const auto device = std::find_if(
      m_devices.begin(), m_devices.end(), [&stableIdentity](const auto &entry) {
        return entry.m_identity == stableIdentity && entry.m_present;
      });
  if (device == m_devices.end() && !wasSaved) {
    return false;
  }

  if (selected) {
    if (existing != m_draft_selection.end()) {
      return true;
    }

    if (m_draft_selection.size() == m_selection_limit) {
      return false;
    }

    m_draft_selection.push_back(stableIdentity);

    return true;
  }

  return true;
}

void Dmn_DeviceSelectionModel::saveSelection() {
  m_saved_selection = m_draft_selection;
  mergeSavedDevices();
}

void Dmn_DeviceSelectionModel::discardChanges() {
  m_draft_selection = m_saved_selection;
  mergeSavedDevices();
}

auto Dmn_DeviceSelectionModel::devices() const
    -> const std::vector<Dmn_DeviceEntry> & {
  return m_devices;
}

void Dmn_DeviceSelectionModel::sortDevices(const Dmn_DeviceSortColumn column,
                                           const bool ascending) {
  m_sort_column = column;
  m_sort_ascending = ascending;
  applySort();
}

auto Dmn_DeviceSelectionModel::sortColumn() const -> Dmn_DeviceSortColumn {
  return m_sort_column;
}

auto Dmn_DeviceSelectionModel::sortAscending() const -> bool {
  return m_sort_ascending;
}

auto Dmn_DeviceSelectionModel::savedSelection() const
    -> const std::vector<Dmn_DeviceIdentity> & {
  return m_saved_selection;
}

auto Dmn_DeviceSelectionModel::draftSelection() const
    -> const std::vector<Dmn_DeviceIdentity> & {
  return m_draft_selection;
}

auto Dmn_DeviceSelectionModel::hasUnsavedChanges() const -> bool {
  return m_saved_selection != m_draft_selection;
}

auto Dmn_DeviceSelectionModel::canStartMonitoring() const -> bool {
  if (hasUnsavedChanges() || m_saved_selection.empty()) {
    return false;
  }

  return std::all_of(m_saved_selection.begin(), m_saved_selection.end(),
                     [this](const auto &identity) {
                       return std::any_of(m_devices.begin(), m_devices.end(),
                                          [&identity](const auto &device) {
                                            return device.m_identity ==
                                                       identity &&
                                                   device.m_present;
                                          });
                     });
}

auto Dmn_DeviceSelectionModel::selectionLimit() const -> std::size_t {
  return m_selection_limit;
}

void Dmn_DeviceSelectionModel::mergeSavedDevices() {
  std::erase_if(m_devices,
                [](const auto &device) { return !device.m_present; });

  for (const auto &identity : m_saved_selection) {
    const auto found = std::find_if(m_devices.begin(), m_devices.end(),
                                    [&identity](const auto &device) {
                                      return device.m_identity == identity;
                                    });
    if (found == m_devices.end()) {
      m_devices.push_back(Dmn_DeviceEntry{identity, identity.m_device_address,
                                          std::nullopt, std::nullopt, false,
                                          false});
    }
  }

  applySort();
}

void Dmn_DeviceSelectionModel::applySort() {
  std::stable_sort(
      m_devices.begin(), m_devices.end(),
      [this](const auto &left, const auto &right) {
        const auto leftName = deviceDisplayName(left);
        const auto rightName = deviceDisplayName(right);
        const auto leftPrimary = m_sort_column == Dmn_DeviceSortColumn::kName
                                     ? leftName
                                     : deviceDisplayStatus(left);
        const auto rightPrimary = m_sort_column == Dmn_DeviceSortColumn::kName
                                      ? rightName
                                      : deviceDisplayStatus(right);
        const auto primaryOrder =
            compareCaseInsensitive(leftPrimary, rightPrimary);
        if (primaryOrder != 0) {
          return m_sort_ascending ? primaryOrder < 0 : primaryOrder > 0;
        }

        const auto nameOrder = compareCaseInsensitive(leftName, rightName);
        if (nameOrder != 0) {
          return nameOrder < 0;
        }

        if (left.m_identity.m_adapter_address !=
            right.m_identity.m_adapter_address) {
          return left.m_identity.m_adapter_address <
                 right.m_identity.m_adapter_address;
        }

        return left.m_identity.m_device_address <
               right.m_identity.m_device_address;
      });
}

} // namespace dmn::gui
