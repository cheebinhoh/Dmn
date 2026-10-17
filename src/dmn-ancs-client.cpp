/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-ancs-client.cpp
 * @brief Bounded ANCS Notification Source and Data Source parsing.
 */

#include "dmn-ancs-client.hpp"
#include "dmn-proc.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string_view>
#include <utility>

namespace dmn::detail {
namespace {

constexpr std::size_t kNotificationSourceSize{8};
constexpr std::size_t kDataSourceHeaderSize{5};
constexpr std::size_t kAttributeTupleHeaderSize{3};
constexpr std::size_t kMaximumDataSourceResponseSize{8192};
constexpr std::array<std::uint8_t, 5> kRequestedAttributeIds{0, 1, 2, 3, 5};
constexpr std::array<std::size_t, 5> kMaximumAttributeSizes{255, 256, 256, 4096,
                                                            64};
constexpr std::string_view kAncsServiceUuid{
    "7905F431-B5CE-4E99-A40F-4B1E122D00D0"};
constexpr std::string_view kNotificationSourceUuid{
    "9FBF120D-6301-42D9-8C58-25E699A21DBD"};
constexpr std::string_view kControlPointUuid{
    "69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9"};
constexpr std::string_view kDataSourceUuid{
    "22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB"};

auto error(Dmn_AncsErrorKind kind, std::string message,
           bool fatal = true) -> Dmn_AncsError {
  return {kind, std::move(message), {}, fatal};
}

auto readUint32LittleEndian(std::span<const std::uint8_t> bytes,
                            std::size_t offset) -> std::uint32_t {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

auto readUint16LittleEndian(std::span<const std::uint8_t> bytes,
                            std::size_t offset) -> std::uint16_t {
  return static_cast<std::uint16_t>(bytes[offset]) |
         static_cast<std::uint16_t>(
             static_cast<std::uint16_t>(bytes[offset + 1]) << 8U);
}

auto isValidUtf8(std::span<const std::uint8_t> bytes) -> bool {
  std::size_t index{};
  while (index < bytes.size()) {
    const auto first = bytes[index];
    if (first <= 0x7fU) {
      ++index;
      continue;
    }

    std::size_t continuationCount{};
    std::uint32_t codePoint{};
    if (first >= 0xc2U && first <= 0xdfU) {
      continuationCount = 1;
      codePoint = first & 0x1fU;
    } else if (first >= 0xe0U && first <= 0xefU) {
      continuationCount = 2;
      codePoint = first & 0x0fU;
    } else if (first >= 0xf0U && first <= 0xf4U) {
      continuationCount = 3;
      codePoint = first & 0x07U;
    } else {
      return false;
    }

    if (continuationCount > bytes.size() - index - 1) {
      return false;
    }

    for (std::size_t offset = 1; offset <= continuationCount; ++offset) {
      const auto continuation = bytes[index + offset];
      if ((continuation & 0xc0U) != 0x80U) {
        return false;
      }

      codePoint = (codePoint << 6U) | (continuation & 0x3fU);
    }

    if ((continuationCount == 1 && codePoint < 0x80U) ||
        (continuationCount == 2 && codePoint < 0x800U) ||
        (continuationCount == 3 && codePoint < 0x10000U) ||
        (codePoint >= 0xd800U && codePoint <= 0xdfffU) ||
        codePoint > 0x10ffffU) {
      return false;
    }

    index += continuationCount + 1;
  }

  return true;
}

auto isRequestedAttribute(std::uint8_t attributeId) -> bool {
  return std::find(kRequestedAttributeIds.begin(), kRequestedAttributeIds.end(),
                   attributeId) != kRequestedAttributeIds.end();
}

auto hasFlag(const Dmn_GattCharacteristic &characteristic,
             std::string_view flag) -> bool {
  return std::find(characteristic.flags.begin(), characteristic.flags.end(),
                   flag) != characteristic.flags.end();
}

auto assignAttribute(Dmn_AncsAttributeSet &attributes, std::size_t index,
                     std::string value) -> void {
  switch (index) {
  case 0:
    attributes.app_identifier = std::move(value);
    break;
  case 1:
    attributes.title = std::move(value);
    break;
  case 2:
    attributes.subtitle = std::move(value);
    break;
  case 3:
    attributes.message = std::move(value);
    break;
  case 4:
    attributes.date = std::move(value);
    break;
  default:
    break;
  }
}

} // namespace

auto parseAncsNotificationSource(std::span<const std::uint8_t> value)
    -> Dmn_AncsValueResult<Dmn_AncsNotification> {
  if (value.size() != kNotificationSourceSize) {
    return std::unexpected(
        error(Dmn_AncsErrorKind::Protocol,
              "ANCS Notification Source value must be exactly eight bytes"));
  }

  Dmn_AncsNotification notification;
  switch (value[0]) {
  case 0:
    notification.kind = Dmn_AncsNotificationKind::Added;
    break;
  case 1:
    notification.kind = Dmn_AncsNotificationKind::Updated;
    break;
  case 2:
    notification.kind = Dmn_AncsNotificationKind::Removed;
    break;
  default:
    return std::unexpected(error(Dmn_AncsErrorKind::Unsupported,
                                 "unsupported ANCS event identifier", false));
  }

  notification.flags = value[1];
  notification.category = value[2];
  notification.category_count = value[3];
  notification.uid = readUint32LittleEndian(value, 4);

  return notification;
}

Dmn_AncsDataSourceReassembler::Dmn_AncsDataSourceReassembler(
    std::uint32_t expectedUid)
    : m_expected_uid{expectedUid} {}

auto Dmn_AncsDataSourceReassembler::consume(
    std::span<const std::uint8_t> fragment)
    -> Dmn_AncsValueResult<std::optional<Dmn_AncsAttributeSet>> {
  if (m_terminal) {
    return std::unexpected(error(
        Dmn_AncsErrorKind::NotReady,
        "ANCS Data Source response parser is already complete or failed"));
  }

  if (fragment.empty()) {
    return std::unexpected(error(Dmn_AncsErrorKind::Protocol,
                                 "ANCS Data Source fragment is empty"));
  }

  if (fragment.size() > kMaximumDataSourceResponseSize - m_total_bytes) {
    m_terminal = true;

    return std::unexpected(
        error(Dmn_AncsErrorKind::ResourceLimit,
              "ANCS Data Source response exceeds the 8192-byte limit"));
  }

  m_total_bytes += fragment.size();
  m_buffer.insert(m_buffer.end(), fragment.begin(), fragment.end());

  std::size_t consumed{};
  if (!m_header_seen) {
    if (m_buffer.size() < kDataSourceHeaderSize) {
      return std::optional<Dmn_AncsAttributeSet>{};
    }

    if (m_buffer[0] != 0) {
      m_terminal = true;

      return std::unexpected(
          error(Dmn_AncsErrorKind::Protocol,
                "unexpected ANCS Data Source response command identifier"));
    }

    const auto responseUid = readUint32LittleEndian(m_buffer, 1);
    if (responseUid != m_expected_uid) {
      m_terminal = true;

      return std::unexpected(error(
          Dmn_AncsErrorKind::Protocol,
          "ANCS Data Source response UID does not match the active request"));
    }

    consumed = kDataSourceHeaderSize;
    m_header_seen = true;
  }

  while (m_expected_attribute_index < kRequestedAttributeIds.size()) {
    const auto available = m_buffer.size() - consumed;
    if (available < kAttributeTupleHeaderSize) {
      break;
    }

    const auto attributeId = m_buffer[consumed];
    const auto attributeLength = readUint16LittleEndian(m_buffer, consumed + 1);
    const auto valueOffset = consumed + kAttributeTupleHeaderSize;
    const bool isExpectedAttribute =
        attributeId == kRequestedAttributeIds[m_expected_attribute_index];
    if (!isExpectedAttribute) {
      if (isRequestedAttribute(attributeId)) {
        m_terminal = true;

        return std::unexpected(
            error(Dmn_AncsErrorKind::Protocol,
                  "ANCS Data Source attributes are not in requested order"));
      }

      if (attributeLength > m_buffer.size() - valueOffset) {
        break;
      }

      consumed = valueOffset + attributeLength;
      continue;
    }

    if (attributeLength > kMaximumAttributeSizes[m_expected_attribute_index]) {
      m_terminal = true;

      return std::unexpected(
          error(Dmn_AncsErrorKind::ResourceLimit,
                "ANCS attribute value exceeds its configured byte limit"));
    }

    if (attributeLength > m_buffer.size() - valueOffset) {
      break;
    }

    const auto attributeBytes = std::span<const std::uint8_t>{
        m_buffer.data() + valueOffset, attributeLength};
    if (!isValidUtf8(attributeBytes)) {
      m_terminal = true;

      return std::unexpected(error(Dmn_AncsErrorKind::Protocol,
                                   "ANCS attribute is not valid UTF-8", false));
    }

    assignAttribute(
        m_attributes, m_expected_attribute_index,
        std::string{reinterpret_cast<const char *>(attributeBytes.data()),
                    attributeBytes.size()});
    consumed = valueOffset + attributeLength;
    ++m_expected_attribute_index;
  }

  if (m_expected_attribute_index == kRequestedAttributeIds.size()) {
    if (consumed != m_buffer.size()) {
      m_terminal = true;

      return std::unexpected(
          error(Dmn_AncsErrorKind::Protocol,
                "ANCS Data Source response has trailing unrequested data"));
    }

    m_terminal = true;

    return std::optional<Dmn_AncsAttributeSet>{std::move(m_attributes)};
  }

  if (consumed != 0) {
    m_buffer.erase(m_buffer.begin(),
                   m_buffer.begin() + static_cast<std::ptrdiff_t>(consumed));
  }

  return std::optional<Dmn_AncsAttributeSet>{};
}

} // namespace dmn::detail

namespace dmn {
namespace {

auto localDateString(std::chrono::system_clock::time_point timestamp)
    -> std::optional<std::string> {
  const auto time = std::chrono::system_clock::to_time_t(timestamp);
  std::tm localTime{};
  if (localtime_r(&time, &localTime) == nullptr) {
    return std::nullopt;
  }

  std::array<char, 16> buffer{};
  if (std::strftime(buffer.data(), buffer.size(), "%Y%m%dT%H%M%S",
                    &localTime) == 0) {
    return std::nullopt;
  }

  return std::string{buffer.data()};
}

auto isValidAncsDate(std::string_view date) -> bool {
  if (date.size() != 15 || date[8] != 'T') {
    return false;
  }

  for (std::size_t index = 0; index < date.size(); ++index) {
    if (index != 8 && (date[index] < '0' || date[index] > '9')) {
      return false;
    }
  }

  const auto numberAt = [date](std::size_t offset, std::size_t length) {
    unsigned int number{};
    for (std::size_t index = offset; index < offset + length; ++index) {
      number = number * 10U + static_cast<unsigned int>(date[index] - '0');
    }

    return number;
  };
  const std::chrono::year_month_day calendarDate{
      std::chrono::year{static_cast<int>(numberAt(0, 4))},
      std::chrono::month{numberAt(4, 2)}, std::chrono::day{numberAt(6, 2)}};

  return calendarDate.ok() && numberAt(9, 2) < 24 && numberAt(11, 2) < 60 &&
         numberAt(13, 2) < 60;
}

auto isBeforeCutoff(const std::optional<std::string> &date,
                    std::string_view cutoff) -> bool {
  return cutoff.empty() || !date || !isValidAncsDate(*date) || *date < cutoff;
}

auto makeAncsError(Dmn_AncsErrorKind kind, std::string message,
                   std::string remoteName = {},
                   bool fatal = true) -> Dmn_AncsError {
  return {kind, std::move(message), std::move(remoteName), fatal};
}

auto mapGattError(const Dmn_GattError &gattError) -> Dmn_AncsError {
  auto kind = Dmn_AncsErrorKind::Backend;
  switch (gattError.kind) {
  case Dmn_GattErrorKind::InvalidArgument:
    kind = Dmn_AncsErrorKind::InvalidArgument;
    break;
  case Dmn_GattErrorKind::NotReady:
    kind = Dmn_AncsErrorKind::NotReady;
    break;
  case Dmn_GattErrorKind::Timeout:
    kind = Dmn_AncsErrorKind::Timeout;
    break;
  case Dmn_GattErrorKind::Remote:
    kind = Dmn_AncsErrorKind::Remote;
    break;
  case Dmn_GattErrorKind::Backend:
  case Dmn_GattErrorKind::Decode:
    kind = Dmn_AncsErrorKind::Backend;
    break;
  }

  if (gattError.remote_name.find("NotAuthorized") != std::string::npos ||
      gattError.remote_name.find("NotPermitted") != std::string::npos) {
    kind = Dmn_AncsErrorKind::Authorization;
  }

  return makeAncsError(kind, gattError.message, gattError.remote_name);
}

auto makeAttributeRequest(std::uint32_t uid) -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> request{0};
  for (unsigned int shift = 0; shift < 32; shift += 8) {
    request.push_back(static_cast<std::uint8_t>((uid >> shift) & 0xffU));
  }

  for (std::size_t index = 0; index < detail::kRequestedAttributeIds.size();
       ++index) {
    request.push_back(detail::kRequestedAttributeIds[index]);
    if (index >= 1 && index <= 3) {
      const auto maximum =
          static_cast<std::uint16_t>(detail::kMaximumAttributeSizes[index]);
      request.push_back(static_cast<std::uint8_t>(maximum & 0xffU));
      request.push_back(static_cast<std::uint8_t>((maximum >> 8U) & 0xffU));
    }
  }

  return request;
}

} // namespace

class Dmn_AncsClient::Impl {
public:
  explicit Impl(Dmn_GattClient &gatt)
      : m_gatt{gatt},
        m_worker{"dmn-ancs", [this] { run(); },
                 Dmn_Proc::ExceptionPolicy::kCaptureAndRethrowFromWait},
        m_control_point_worker{
            "dmn-ancs-control-point", [this] { runControlPointWrites(); },
            Dmn_Proc::ExceptionPolicy::kCaptureAndRethrowFromWait} {}

  ~Impl() noexcept {
    (void)stop();
    {
      std::lock_guard lock{m_mutex};
      m_shutdown = true;
      m_control_point_shutdown = true;
      m_control_point_jobs.clear();
    }

    m_condition.notify_all();
    m_control_point_condition.notify_all();

    if (m_worker_started) {
      try {
        (void)m_worker.wait();
      } catch (...) {
      }
    }

    if (m_control_point_worker_started) {
      try {
        (void)m_control_point_worker.wait();
      } catch (...) {
      }
    }
  }

  auto setEventHandler(EventHandler handler) -> Dmn_AncsResult {
    std::lock_guard apiLock{m_api_mutex};
    std::lock_guard lock{m_mutex};
    if (m_state != Dmn_AncsState::Stopped) {
      return std::unexpected(
          makeAncsError(Dmn_AncsErrorKind::NotReady,
                        "ANCS event handler can only change while stopped"));
    }

    m_handler = std::move(handler);

    return {};
  }

  auto start(const std::string &deviceId,
             const Dmn_AncsConfig &config) -> Dmn_AncsResult {
    std::lock_guard apiLock{m_api_mutex};
    if (!validConfig(config) || deviceId.empty()) {
      return std::unexpected(makeAncsError(
          Dmn_AncsErrorKind::InvalidArgument,
          "ANCS device identifier and operation deadlines must be valid"));
    }

    std::optional<std::string> notificationDateCutoff;
    if (config.notification_date_cutoff) {
      notificationDateCutoff =
          localDateString(*config.notification_date_cutoff);
      if (!notificationDateCutoff) {
        return std::unexpected(makeAncsError(
            Dmn_AncsErrorKind::InvalidArgument,
            "notification date cutoff cannot be represented in local time"));
      }
    }

    {
      std::lock_guard lock{m_mutex};
      if (m_state != Dmn_AncsState::Stopped) {
        return std::unexpected(
            makeAncsError(Dmn_AncsErrorKind::NotReady,
                          "ANCS session can only start while stopped"));
      }

      if (!m_handler) {
        return std::unexpected(
            makeAncsError(Dmn_AncsErrorKind::InvalidArgument,
                          "ANCS event handler must be installed before start"));
      }

      m_cancel_requested = false;
      m_accept_gatt_events = true;
      m_ready = false;
      m_shutdown = false;
      m_control_point_shutdown = false;
      m_device_id = deviceId;
      m_config = config;
      m_notification_date_cutoff =
          notificationDateCutoff.value_or(std::string{});
    }

    if (!m_worker_started) {
      if (!m_worker.exec()) {
        return failStartup(
            makeAncsError(Dmn_AncsErrorKind::Backend,
                          "could not start the ANCS event worker"));
      }

      m_worker_started = true;
    }

    if (!m_control_point_worker_started) {
      if (!m_control_point_worker.exec()) {
        return failStartup(
            makeAncsError(Dmn_AncsErrorKind::Backend,
                          "could not start the ANCS Control Point worker"));
      }

      m_control_point_worker_started = true;
    }

    setState(Dmn_AncsState::Starting);
    setState(Dmn_AncsState::ResolvingServices);
    auto resolved = m_gatt.waitForServicesResolved(
        deviceId, config.service_resolution_timeout);
    if (!resolved) {
      return failStartup(mapGattError(resolved.error()));
    }

    if (cancelled()) {
      return failStartup(makeAncsError(Dmn_AncsErrorKind::NotReady,
                                       "ANCS session startup was cancelled"));
    }

    setState(Dmn_AncsState::DiscoveringANCS);
    auto services = m_gatt.discover(deviceId, config.gatt_operation_timeout);
    if (!services) {
      return failStartup(mapGattError(services.error()));
    }

    std::optional<Dmn_GattCharacteristic> notificationSource;
    std::optional<Dmn_GattCharacteristic> controlPoint;
    std::optional<Dmn_GattCharacteristic> dataSource;
    bool ancsServiceFound{};
    for (const auto &service : *services) {
      if (service.uuid != detail::kAncsServiceUuid) {
        continue;
      }

      ancsServiceFound = true;
      for (const auto &characteristic : service.characteristics) {
        if (characteristic.uuid == detail::kNotificationSourceUuid) {
          notificationSource = characteristic;
        } else if (characteristic.uuid == detail::kControlPointUuid) {
          controlPoint = characteristic;
        } else if (characteristic.uuid == detail::kDataSourceUuid) {
          dataSource = characteristic;
        }
      }
    }

    if (!ancsServiceFound) {
      return failStartup(makeAncsError(
          Dmn_AncsErrorKind::Unsupported,
          "ANCS service was not found in the resolved GATT services"));
    }

    if (!notificationSource || !controlPoint || !dataSource) {
      return failStartup(
          makeAncsError(Dmn_AncsErrorKind::Unsupported,
                        "ANCS service is missing one or more required "
                        "characteristics"));
    }

    if (!detail::hasFlag(*notificationSource, "notify") ||
        !detail::hasFlag(*dataSource, "notify") ||
        !detail::hasFlag(*controlPoint, "write")) {
      return failStartup(makeAncsError(
          Dmn_AncsErrorKind::Unsupported,
          "ANCS characteristics do not advertise required operations"));
    }

    {
      std::lock_guard lock{m_mutex};
      m_control_point_id = controlPoint->id;
    }

    setState(Dmn_AncsState::SubscribingDataSource);
    auto dataSubscription = m_gatt.startNotify(
        deviceId, dataSource->id,
        [this](const Dmn_GattEvent &event) { enqueueGattEvent(true, event); },
        config.gatt_operation_timeout);
    if (!dataSubscription) {
      return failStartup(mapGattError(dataSubscription.error()));
    }

    {
      std::lock_guard lock{m_subscription_mutex};
      m_data_source_subscription.emplace(std::move(*dataSubscription));
    }

    if (cancelled()) {
      return failStartup(makeAncsError(Dmn_AncsErrorKind::NotReady,
                                       "ANCS session startup was cancelled"));
    }

    setState(Dmn_AncsState::SubscribingNotificationSource);
    auto notificationSubscription = m_gatt.startNotify(
        deviceId, notificationSource->id,
        [this](const Dmn_GattEvent &event) { enqueueGattEvent(false, event); },
        config.gatt_operation_timeout);
    if (!notificationSubscription) {
      return failStartup(mapGattError(notificationSubscription.error()));
    }

    {
      std::lock_guard lock{m_subscription_mutex};
      m_notification_source_subscription.emplace(
          std::move(*notificationSubscription));
    }

    if (cancelled()) {
      return failStartup(makeAncsError(Dmn_AncsErrorKind::NotReady,
                                       "ANCS session startup was cancelled"));
    }

    {
      std::lock_guard lock{m_mutex};
      m_ready = true;
      m_state = Dmn_AncsState::Ready;
      Dmn_AncsEvent event;
      event.kind = Dmn_AncsEvent::Kind::StateChanged;
      event.state = Dmn_AncsState::Ready;
      m_event_queue.push_back(std::move(event));
    }

    m_condition.notify_one();

    return {};
  }

  auto stop() -> Dmn_AncsResult {
    m_cancel_requested = true;
    std::lock_guard apiLock{m_api_mutex};

    bool isStopped{};
    {
      std::lock_guard lock{m_mutex};
      isStopped = m_state == Dmn_AncsState::Stopped;
    }

    if (isStopped && !hasSubscriptions()) {
      return {};
    }

    {
      std::lock_guard lock{m_mutex};
      m_ready = false;
      m_accept_gatt_events = false;
      clearSessionData();
    }

    std::unique_lock workerLock{m_worker_work_mutex};
    {
      std::lock_guard lock{m_mutex};
      clearSessionData();
    }

    setState(Dmn_AncsState::Stopping);
    {
      std::lock_guard operationLock{m_operation_mutex};
      auto cleanup = stopSubscriptions();
      if (!cleanup) {
        auto error = cleanup.error();
        error.kind = Dmn_AncsErrorKind::Cleanup;
        Dmn_AncsEvent event;
        event.kind = Dmn_AncsEvent::Kind::Error;
        event.error = error;
        enqueueApplicationEvent(std::move(event));
        setState(Dmn_AncsState::Failed);

        return std::unexpected(std::move(error));
      }
    }

    setState(Dmn_AncsState::Stopped);
    {
      std::lock_guard lock{m_mutex};
      m_shutdown = true;
      m_control_point_shutdown = true;
      m_control_point_jobs.clear();
    }

    m_condition.notify_all();
    m_control_point_condition.notify_all();
    workerLock.unlock();
    if (m_worker_started) {
      try {
        (void)m_worker.wait();
      } catch (...) {
        return std::unexpected(
            makeAncsError(Dmn_AncsErrorKind::Backend,
                          "ANCS event worker failed while stopping"));
      }

      m_worker_started = false;
    }

    if (m_control_point_worker_started) {
      try {
        (void)m_control_point_worker.wait();
      } catch (...) {
        return std::unexpected(
            makeAncsError(Dmn_AncsErrorKind::Backend,
                          "ANCS Control Point worker failed while stopping"));
      }

      m_control_point_worker_started = false;
    }

    return {};
  }

  auto state() const -> Dmn_AncsState {
    std::lock_guard lock{m_mutex};

    return m_state;
  }

private:
  struct NotificationRecord {
    Dmn_AncsNotification notification;
    bool added_not_reported{true};
    bool unavailable{};
    bool ignored_before_start{};
  };

  struct GattPacket {
    bool data_source{};
    Dmn_GattEvent event;
  };

  struct ControlPointJob {
    std::uint32_t uid{};
    std::string device_id;
    std::string characteristic_id;
    std::vector<std::uint8_t> value;
    std::chrono::milliseconds timeout{};
  };

  struct ControlPointResult {
    std::uint32_t uid{};
    Dmn_GattResult result;
  };

  auto validConfig(const Dmn_AncsConfig &config) const -> bool {
    const auto validTimeout = [](std::chrono::milliseconds timeout) {
      return timeout > std::chrono::milliseconds::zero() &&
             timeout <= std::chrono::seconds{600};
    };

    return validTimeout(config.service_resolution_timeout) &&
           validTimeout(config.gatt_operation_timeout) &&
           validTimeout(config.attribute_response_timeout);
  }

  auto cancelled() const -> bool { return m_cancel_requested; }

  void setState(Dmn_AncsState state) {
    {
      std::lock_guard lock{m_mutex};
      m_state = state;
      Dmn_AncsEvent event;
      event.kind = Dmn_AncsEvent::Kind::StateChanged;
      event.state = state;
      m_event_queue.push_back(std::move(event));
    }

    m_condition.notify_one();
  }

  auto failStartup(Dmn_AncsError error) -> Dmn_AncsResult {
    {
      std::lock_guard lock{m_mutex};
      m_ready = false;
      m_accept_gatt_events = false;
      clearSessionData();
      Dmn_AncsEvent event;
      event.kind = Dmn_AncsEvent::Kind::Error;
      event.error = error;
      m_event_queue.push_back(std::move(event));
    }

    auto cleanup = stopSubscriptions();
    if (!cleanup) {
      error = cleanup.error();
      error.kind = Dmn_AncsErrorKind::Cleanup;
    }

    setState(Dmn_AncsState::Failed);

    return std::unexpected(std::move(error));
  }

  auto hasSubscriptions() const -> bool {
    std::lock_guard lock{m_subscription_mutex};

    return m_data_source_subscription.has_value() ||
           m_notification_source_subscription.has_value();
  }

  auto stopSubscriptions() -> Dmn_AncsResult {
    std::lock_guard lock{m_subscription_mutex};
    std::optional<Dmn_AncsError> firstError;
    if (m_notification_source_subscription) {
      auto result = m_notification_source_subscription->stop();
      if (result) {
        m_notification_source_subscription.reset();
      } else {
        firstError = mapGattError(result.error());
      }
    }

    if (m_data_source_subscription) {
      auto result = m_data_source_subscription->stop();
      if (result) {
        m_data_source_subscription.reset();
      } else if (!firstError) {
        firstError = mapGattError(result.error());
      }
    }

    if (firstError) {
      return std::unexpected(std::move(*firstError));
    }

    return {};
  }

  void clearSessionData() {
    m_gatt_queue.clear();
    m_control_point_jobs.clear();
    m_control_point_results.clear();
    m_records.clear();
    m_pending_requests.clear();
    m_queued_uids.clear();
    m_active_request.reset();
    m_control_point_id.clear();
    m_device_id.clear();
  }

  void enqueueGattEvent(bool dataSource, const Dmn_GattEvent &event) {
    {
      std::lock_guard lock{m_mutex};
      if (!m_accept_gatt_events) {
        return;
      }

      m_gatt_queue.push_back(GattPacket{dataSource, event});
    }

    m_condition.notify_one();
  }

  void enqueueApplicationEvent(Dmn_AncsEvent event) {
    std::lock_guard lock{m_mutex};
    m_event_queue.push_back(std::move(event));
    m_condition.notify_one();
  }

  void run() {
    while (true) {
      Dmn_AncsEvent appEvent;
      GattPacket gattPacket;
      bool hasAppEvent{};
      bool hasGattPacket{};
      bool hasControlPointResult{};
      ControlPointResult controlPointResult;
      {
        std::unique_lock lock{m_mutex};
        const auto deadline =
            m_active_request ? m_active_request->deadline
                             : std::chrono::steady_clock::time_point::max();
        m_condition.wait_until(lock, deadline, [this] {
          return m_shutdown || !m_event_queue.empty() ||
                 (m_ready && !m_gatt_queue.empty()) ||
                 !m_control_point_results.empty();
        });

        if (m_shutdown && m_event_queue.empty()) {
          return;
        }

        if (!m_event_queue.empty()) {
          appEvent = std::move(m_event_queue.front());
          m_event_queue.pop_front();
          hasAppEvent = true;
        } else if (m_ready && !m_gatt_queue.empty()) {
          gattPacket = std::move(m_gatt_queue.front());
          m_gatt_queue.pop_front();
          hasGattPacket = true;
        } else if (!m_control_point_results.empty()) {
          controlPointResult = std::move(m_control_point_results.front());
          m_control_point_results.pop_front();
          hasControlPointResult = true;
        }
      }

      {
        std::lock_guard workerLock{m_worker_work_mutex};
        if (hasAppEvent) {
          dispatch(std::move(appEvent));
        } else if (hasGattPacket) {
          processGattPacket(std::move(gattPacket));
        } else if (hasControlPointResult) {
          if (!controlPointResult.result) {
            failRuntime(mapGattError(controlPointResult.result.error()));
          } else {
            acknowledgeControlPointWrite(controlPointResult.uid);
          }
        } else if (requestTimedOut()) {
          failTimedOutRequest();
        }

        startNextRequest();
      }
    }
  }

  void dispatch(Dmn_AncsEvent event) {
    EventHandler handler;
    {
      std::lock_guard lock{m_mutex};
      handler = m_handler;
    }

    if (handler) {
      try {
        handler(event);
      } catch (...) {
        failRuntime(makeAncsError(Dmn_AncsErrorKind::Backend,
                                  "ANCS event handler threw an exception"));
      }
    }
  }

  void processGattPacket(GattPacket packet) {
    {
      std::lock_guard lock{m_mutex};
      if (!m_ready) {
        return;
      }
    }

    if (packet.event.kind == Dmn_GattEvent::Kind::Invalidated) {
      failRuntime(makeAncsError(Dmn_AncsErrorKind::NotReady,
                                packet.event.message.empty()
                                    ? "ANCS GATT session was invalidated"
                                    : packet.event.message));
      return;
    }

    if (packet.event.kind == Dmn_GattEvent::Kind::Error) {
      failRuntime(makeAncsError(Dmn_AncsErrorKind::Backend,
                                packet.event.message.empty()
                                    ? "ANCS GATT notification failed"
                                    : packet.event.message));
      return;
    }

    if (packet.data_source) {
      processDataSourceValue(packet.event.value);
    } else {
      processNotificationSourceValue(packet.event.value);
    }
  }

  void processNotificationSourceValue(const std::vector<std::uint8_t> &value) {
    auto parsed = detail::parseAncsNotificationSource(value);
    if (!parsed) {
      if (parsed.error().kind == Dmn_AncsErrorKind::Unsupported &&
          !parsed.error().fatal) {
        Dmn_AncsEvent event;
        event.kind = Dmn_AncsEvent::Kind::Error;
        event.error = parsed.error();
        enqueueApplicationEvent(std::move(event));

        return;
      }

      failRuntime(parsed.error());

      return;
    }

    const auto notification = *parsed;
    if (notification.kind == Dmn_AncsNotificationKind::Removed) {
      bool ignoredBeforeStart{};
      {
        std::lock_guard lock{m_mutex};
        const auto found = m_records.find(notification.uid);
        ignoredBeforeStart =
            m_config.notification_date_cutoff.has_value() &&
            (found == m_records.end() || found->second.ignored_before_start ||
             found->second.added_not_reported);
        m_records.erase(notification.uid);
        std::erase(m_pending_requests, notification.uid);
        m_queued_uids.erase(notification.uid);
        if (m_active_request &&
            m_active_request->notification.uid == notification.uid) {
          m_active_request->removed = true;
        }
      }

      if (!ignoredBeforeStart) {
        Dmn_AncsEvent event;
        event.kind = Dmn_AncsEvent::Kind::NotificationRemoved;
        event.notification = notification;
        enqueueApplicationEvent(std::move(event));
      }

      return;
    }

    bool reportBacklogLimit{};
    {
      std::lock_guard lock{m_mutex};
      auto &record = m_records[notification.uid];
      record.notification.kind = notification.kind;
      record.notification.uid = notification.uid;
      record.notification.category = notification.category;
      record.notification.category_count = notification.category_count;
      record.notification.flags = notification.flags;
      if (!record.unavailable && m_queued_uids.count(notification.uid) == 0 &&
          (!m_active_request ||
           m_active_request->notification.uid != notification.uid)) {
        const auto maximum = m_config.maximum_pending_notifications;
        if (maximum != 0 && m_pending_requests.size() >= maximum) {
          const auto droppedUid = m_pending_requests.front();
          m_pending_requests.pop_front();
          m_queued_uids.erase(droppedUid);
          m_records.erase(droppedUid);
          if (!m_backlog_limit_reported) {
            m_backlog_limit_reported = true;
            reportBacklogLimit = true;
          }
        } else if (maximum == 0 || m_pending_requests.size() < maximum) {
          m_backlog_limit_reported = false;
        }

        m_pending_requests.push_back(notification.uid);
        m_queued_uids.insert(notification.uid);
      }
    }

    m_condition.notify_one();
    if (reportBacklogLimit) {
      Dmn_AncsEvent event;
      event.kind = Dmn_AncsEvent::Kind::Error;
      event.error = makeAncsError(
          Dmn_AncsErrorKind::ResourceLimit,
          "ANCS reached the configured pending-notification limit; oldest "
          "pending notification requests are being discarded",
          {}, false);
      enqueueApplicationEvent(std::move(event));
    }
  }

  void processDataSourceValue(const std::vector<std::uint8_t> &value) {
    std::optional<
        Dmn_AncsValueResult<std::optional<detail::Dmn_AncsAttributeSet>>>
        parsed;
    std::uint32_t uid{};
    bool removed{};
    {
      std::lock_guard lock{m_mutex};
      if (!m_active_request) {
        return;
      }

      uid = m_active_request->notification.uid;
      removed = m_active_request->removed;
      auto &reassembler = m_active_request->reassembler;
      parsed.emplace(reassembler.consume(value));
    }

    if (!*parsed) {
      const auto error = parsed->error();
      if (!error.fatal) {
        emitAttributesUnavailable(uid, error);
        completeActiveRequest();

        return;
      }

      failRuntime(error);

      return;
    }

    if (!parsed->value()) {
      return;
    }

    if (removed) {
      completeActiveRequest();

      return;
    }

    Dmn_AncsEvent event;
    bool missingRecord{};
    bool ignoreNotification{};
    {
      std::lock_guard lock{m_mutex};
      const auto found = m_records.find(uid);
      if (found == m_records.end()) {
        missingRecord = true;
      } else {
        auto &notification = found->second.notification;
        notification.app_identifier = parsed->value()->app_identifier;
        notification.title = parsed->value()->title;
        notification.subtitle = parsed->value()->subtitle;
        notification.message = parsed->value()->message;
        notification.date = parsed->value()->date;
        ignoreNotification =
            m_config.notification_date_cutoff.has_value() &&
            isBeforeCutoff(notification.date, m_notification_date_cutoff);
        found->second.ignored_before_start = ignoreNotification;
        event.kind = found->second.added_not_reported
                         ? Dmn_AncsEvent::Kind::NotificationAdded
                         : Dmn_AncsEvent::Kind::NotificationUpdated;
        event.notification = notification;
        found->second.added_not_reported = false;
      }
    }

    if (missingRecord) {
      completeActiveRequest();

      return;
    }

    if (!ignoreNotification) {
      enqueueApplicationEvent(std::move(event));
    }

    completeActiveRequest();
  }

  struct ActiveRequest {
    Dmn_AncsNotification notification;
    detail::Dmn_AncsDataSourceReassembler reassembler;
    std::chrono::steady_clock::time_point deadline;
    bool write_pending{true};
    bool removed{};

    explicit ActiveRequest(const Dmn_AncsNotification &value)
        : notification{value}, reassembler{value.uid},
          deadline{std::chrono::steady_clock::time_point::max()} {}
  };

  void startNextRequest() {
    Dmn_AncsNotification notification;
    std::string controlPointId;
    std::string deviceId;
    std::chrono::milliseconds timeout;
    {
      std::lock_guard lock{m_mutex};
      if (!m_ready || m_active_request || m_pending_requests.empty()) {
        return;
      }

      const auto uid = m_pending_requests.front();
      m_pending_requests.pop_front();
      m_queued_uids.erase(uid);
      const auto found = m_records.find(uid);
      if (found == m_records.end() || found->second.unavailable) {
        return;
      }

      notification = found->second.notification;
      m_active_request.emplace(notification);
      controlPointId = m_control_point_id;
      deviceId = m_device_id;
      timeout = m_config.gatt_operation_timeout;
    }

    const auto request = makeAttributeRequest(notification.uid);
    {
      std::lock_guard lock{m_mutex};
      if (!m_ready || m_control_point_shutdown) {
        return;
      }

      m_control_point_jobs.push_back(
          ControlPointJob{notification.uid, std::move(deviceId),
                          std::move(controlPointId), request, timeout});
    }

    m_control_point_condition.notify_one();
  }

  void runControlPointWrites() {
    while (true) {
      ControlPointJob job;
      {
        std::unique_lock lock{m_mutex};
        m_control_point_condition.wait(lock, [this] {
          return m_control_point_shutdown || !m_control_point_jobs.empty();
        });
        if (m_control_point_shutdown) {
          return;
        }

        job = std::move(m_control_point_jobs.front());
        m_control_point_jobs.pop_front();
      }

      Dmn_GattResult result;
      {
        std::lock_guard operationLock{m_operation_mutex};
        result = m_gatt.writeWithResponse(job.device_id, job.characteristic_id,
                                          job.value, job.timeout);
      }

      {
        std::lock_guard lock{m_mutex};
        if (m_control_point_shutdown) {
          return;
        }

        m_control_point_results.push_back(
            ControlPointResult{job.uid, std::move(result)});
      }

      m_condition.notify_all();
    }
  }

  void acknowledgeControlPointWrite(std::uint32_t uid) {
    {
      std::lock_guard lock{m_mutex};
      if (!m_active_request || m_active_request->notification.uid != uid) {
        return;
      }

      m_active_request->write_pending = false;
      m_active_request->deadline = std::chrono::steady_clock::now() +
                                   m_config.attribute_response_timeout;
    }

    m_condition.notify_all();
  }

  auto requestTimedOut() const -> bool {
    std::lock_guard lock{m_mutex};

    return m_active_request && !m_active_request->write_pending &&
           std::chrono::steady_clock::now() >= m_active_request->deadline;
  }

  void failTimedOutRequest() {
    std::uint32_t uid{};
    {
      std::lock_guard lock{m_mutex};
      if (!m_active_request ||
          std::chrono::steady_clock::now() < m_active_request->deadline) {
        return;
      }

      uid = m_active_request->notification.uid;
      if (const auto found = m_records.find(uid); found != m_records.end()) {
        found->second.unavailable = true;
      }

      m_active_request.reset();
    }

    auto error = makeAncsError(Dmn_AncsErrorKind::Timeout,
                               "ANCS attribute response deadline expired; "
                               "session correlation is unsafe");
    emitAttributesUnavailable(uid, error);
    failRuntime(std::move(error));
  }

  void emitAttributesUnavailable(std::uint32_t uid,
                                 const Dmn_AncsError &error) {
    Dmn_AncsEvent event;
    event.kind = Dmn_AncsEvent::Kind::AttributesUnavailable;
    event.error = error;
    {
      std::lock_guard lock{m_mutex};
      if (const auto found = m_records.find(uid); found != m_records.end()) {
        found->second.unavailable = true;
        event.notification = found->second.notification;
      } else if (m_active_request &&
                 m_active_request->notification.uid == uid) {
        event.notification = m_active_request->notification;
      }
    }

    enqueueApplicationEvent(std::move(event));
  }

  void completeActiveRequest() {
    {
      std::lock_guard lock{m_mutex};
      m_active_request.reset();
    }

    m_condition.notify_one();
  }

  void failRuntime(Dmn_AncsError error) {
    {
      std::lock_guard lock{m_mutex};
      if (m_state == Dmn_AncsState::Failed ||
          m_state == Dmn_AncsState::Stopped) {
        return;
      }

      m_ready = false;
      m_accept_gatt_events = false;
      clearSessionData();
      m_state = Dmn_AncsState::Failed;
    }

    std::lock_guard operationLock{m_operation_mutex};
    const auto cleanup = stopSubscriptions();
    if (!cleanup) {
      error.kind = Dmn_AncsErrorKind::Cleanup;
      error.message +=
          "; subscription cleanup failed: " + cleanup.error().message;
      if (error.remote_name.empty()) {
        error.remote_name = cleanup.error().remote_name;
      }
    }

    {
      std::lock_guard lock{m_mutex};
      Dmn_AncsEvent appEvent;
      appEvent.kind = Dmn_AncsEvent::Kind::Error;
      appEvent.error = error;
      m_event_queue.push_back(std::move(appEvent));
      Dmn_AncsEvent stateEvent;
      stateEvent.kind = Dmn_AncsEvent::Kind::StateChanged;
      stateEvent.state = Dmn_AncsState::Failed;
      m_event_queue.push_back(std::move(stateEvent));
    }

    m_condition.notify_one();
  }

  Dmn_GattClient &m_gatt;
  mutable std::mutex m_mutex;
  std::mutex m_api_mutex;
  std::mutex m_operation_mutex;
  std::mutex m_worker_work_mutex;
  mutable std::mutex m_subscription_mutex;
  std::condition_variable m_condition;
  std::condition_variable m_control_point_condition;
  EventHandler m_handler;
  Dmn_AncsState m_state{Dmn_AncsState::Stopped};
  Dmn_AncsConfig m_config{};
  std::string m_device_id;
  std::string m_control_point_id;
  std::string m_notification_date_cutoff;
  std::deque<Dmn_AncsEvent> m_event_queue;
  std::deque<GattPacket> m_gatt_queue;
  std::deque<ControlPointJob> m_control_point_jobs;
  std::deque<ControlPointResult> m_control_point_results;
  std::map<std::uint32_t, NotificationRecord> m_records;
  std::deque<std::uint32_t> m_pending_requests;
  std::set<std::uint32_t> m_queued_uids;
  std::optional<ActiveRequest> m_active_request;
  std::optional<Dmn_GattNotification> m_data_source_subscription;
  std::optional<Dmn_GattNotification> m_notification_source_subscription;
  bool m_ready{};
  bool m_accept_gatt_events{};
  bool m_shutdown{};
  bool m_control_point_shutdown{};
  bool m_worker_started{};
  bool m_control_point_worker_started{};
  bool m_backlog_limit_reported{};
  std::atomic_bool m_cancel_requested{};
  Dmn_Proc m_worker;
  Dmn_Proc m_control_point_worker;
};

Dmn_AncsClient::Dmn_AncsClient(Dmn_GattClient &gatt)
    : m_impl{std::make_unique<Impl>(gatt)} {}

Dmn_AncsClient::~Dmn_AncsClient() noexcept = default;

auto Dmn_AncsClient::setEventHandler(EventHandler handler) -> Dmn_AncsResult {
  return m_impl->setEventHandler(std::move(handler));
}

auto Dmn_AncsClient::start(const std::string &deviceId,
                           const Dmn_AncsConfig &config) -> Dmn_AncsResult {
  return m_impl->start(deviceId, config);
}

auto Dmn_AncsClient::stop() -> Dmn_AncsResult { return m_impl->stop(); }

auto Dmn_AncsClient::state() const -> Dmn_AncsState { return m_impl->state(); }

} // namespace dmn
