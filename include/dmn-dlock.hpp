/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-dlock.hpp
 * @brief Prototype DLock values and local acquisition API.
 *
 * Overview
 * --------
 * This header currently provides DLock value types, snapshot/protobuf
 * conversion helpers, a lightweight DMesg-handler wrapper, and a basic
 * instance-local mutex/condition-variable acquisition prototype. Despite the
 * design target described by the companion spec, this implementation does
 * not publish snapshots, synchronize independent handlers, or implement a
 * distributed lock protocol.
 *
 * Phase boundary
 * --------------
 * The template is restricted to `Dmn_DMesg`, but this type constraint is not
 * evidence that publisher-serialized v1 is implemented. `Dmn_DMesgNet` and
 * its election state are not used as a lock authority.
 *
 * Key responsibilities
 * --------------------
 * - Define the public DLock result and lifecycle contract (`Dmn_DLock_Result`,
 *   `Dmn_DLock_ResultCode`, and `Dmn_DLock_LifecycleEvent`).
 * - Model a lock range, lock entry, and canonical table snapshot that can be
 *   serialized into `DLockTablePb` and rebuilt from its protobuf form.
 * - Provide deterministic snapshot conversion helpers and basic validation.
 * - Provide a local-only `Dmn_DLock` acquire/release prototype; no transport
 *   commit or cross-handler synchronization is performed.
 * - Use the additive `DMesgBodyPb` protobuf payload shape for snapshot values.
 *
 * Thread safety and execution model
 * ---------------------------------
 * The current acquire/release operations synchronize a single in-memory table
 * with a mutex and condition variable. They do not run in a DMesg handler
 * context or publish state. `acquireLockAsync()` launches a detached thread
 * that refers to the lock object, so the caller must keep the object alive
 * until the returned future is ready. This prototype is not safe to use as a
 * distributed lock.
 *
 * See also
 * - `dmn-dmesg.hpp` : DMesg handler type used by the prototype wrapper.
 * - `proto/dmn-dlock.proto` : canonical protobuf payload for the lock table.
 * - `docs/specs/dmn-distributed-lock-spec.md` : current phase specification.
 */

#ifndef DMN_DLOCK_HPP_
#define DMN_DLOCK_HPP_

#include "dmn-dmesg.hpp"
#include "dmn-interval-btree.hpp"

#include "proto/dmn-dlock.pb.h"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace dmn {

/**
 * @brief Final status reported by a DLock request.
 *
 * The currently declared result codes. The local prototype returns only a
 * subset; timeout, cancellation, shutdown, and lifecycle outcomes are not yet
 * implemented.
 */
enum class Dmn_DLock_ResultCode {
  kOk = 0,
  kInvalidState,
  kInvalidRange,
  kConflict,
  kTimeout,
  kCancelled,
  kShutdown,
  kNoWait,
  kError
};

/**
 * @brief Inclusive integer range owned by a lock request.
 *
 * The range must be non-negative and ordered, matching the DLock invariant that
 * `m_start >= 0` and `m_end >= m_start`. Overlap checks are inclusive: a range
 * shares a boundary with another range if the end/start values meet exactly.
 */
struct Dmn_DLock_Range {
  std::int64_t m_start{};
  std::int64_t m_end{};

  constexpr auto isValid() const noexcept -> bool {
    return m_start >= 0 && m_end >= m_start;
  }

  constexpr auto overlaps(const Dmn_DLock_Range &other) const noexcept -> bool {
    if (!isValid() || !other.isValid()) {
      return false;
    }

    return !(m_end < other.m_start || other.m_end < m_start);
  }

  static auto
  fromInterval(const Dmn_IntervalRange &range) noexcept -> Dmn_DLock_Range {
    return {range.m_start, range.m_end};
  }

  constexpr operator Dmn_IntervalRange() const noexcept {
    return {m_start, m_end};
  }
};

/**
 * @brief Result returned by a DLock operation.
 */
struct Dmn_DLock_Result {
  Dmn_DLock_ResultCode m_code{Dmn_DLock_ResultCode::kInvalidState};
  Dmn_DLock_Range m_range{};
  std::string m_request_id{};
  std::string m_message{};
};

/**
 * @brief Lifecycle-event value shape; the current prototype does not emit it.
 */
struct Dmn_DLock_LifecycleEvent {
  enum class Kind {
    kWaiting,
    kGranted,
    kReleased,
    kCancelled,
    kExpired,
    kError,
    kShutdown
  };

  Kind m_kind{Kind::kWaiting};
  std::string m_request_id{};
  Dmn_DLock_Range m_range{};
  std::string m_detail{};
};

/**
 * @brief Request-specific lock intent options.
 *
 * The local prototype observes @c m_wait and @c m_no_wait. It copies
 * @c m_lease_ticks into the snapshot value but does not enforce a lease;
 * @c m_retries_allowed is not used.
 */
struct Dmn_DLock_RequestOptions {
  std::string m_request_id{};
  std::int64_t m_lease_ticks{0};

  bool m_wait{true};
  bool m_retries_allowed{false};
  bool m_no_wait{false};
};

/**
 * @brief Configuration values supplied to a DLock instance and handler.
 *
 * The local acquisition path currently uses only @c m_domain.
 * @c m_default_lease_ticks and @c m_require_canonical_snapshot are reserved
 * for the protocol implementation and are not enforced by the prototype.
 */
struct Dmn_DLock_Config {
  std::string m_domain{"default"};
  std::int64_t m_default_lease_ticks{1000};

  bool m_require_canonical_snapshot{true};
};

/**
 * @brief State recorded for an entry inside the canonical lock-table snapshot.
 */
enum class Dmn_DLock_EntryState {
  kUnspecified = 0,
  kWaiting = 1,
  kGranted = 2,
  kReleased = 3,
  kCancelled = 4,
  kExpired = 5,
  kError = 6
};

/**
 * @brief Terminal reason associated with a table entry after a lifecycle end.
 */
enum class Dmn_DLock_TerminalReason {
  kUnspecified = 0,
  kNone = 1,
  kReleased = 2,
  kCancelled = 3,
  kTimeout = 4,
  kExpired = 5,
  kShutdown = 6
};

/**
 * @brief Canonical representation of one lock request inside the lock table.
 */
struct Dmn_DLock_Entry {
  std::string m_domain{};
  std::string m_request_id{};
  std::string m_session_id{};
  std::string m_owner_id{};
  Dmn_DLock_Range m_range{};
  std::int32_t m_priority{0};
  std::uint64_t m_sequence{0};
  std::uint64_t m_fence{0};
  Dmn_DLock_EntryState m_state{Dmn_DLock_EntryState::kWaiting};
  Dmn_DLock_TerminalReason m_terminal_reason{Dmn_DLock_TerminalReason::kNone};
  std::int64_t m_acquire_deadline_ticks{0};
  std::int64_t m_lease_deadline_ticks{0};
  bool m_waiting{false};
  bool m_granted{false};

  auto
  canonicalKey() const -> std::tuple<std::int64_t, std::int64_t, std::int32_t,
                                     std::uint64_t, std::string> {
    return {m_range.m_start, m_range.m_end, m_priority, m_sequence,
            m_request_id};
  }

  auto isValid() const noexcept -> bool {
    return !m_domain.empty() && m_range.isValid() &&
           (!m_request_id.empty() || !m_session_id.empty());
  }
};

/**
 * @brief Whole-table snapshot value with protobuf conversion helpers.
 *
 * This object models the intended DMesg payload. The current prototype can
 * convert it to/from protobuf, but does not publish it or use it to synchronize
 * handler mirrors.
 */
struct Dmn_DLock_TableSnapshot {
  std::string m_domain{};
  std::uint32_t m_schema_version{1};
  std::uint32_t m_protocol_version{1};
  std::uint64_t m_publisher_incarnation{0};
  std::uint64_t m_base_table_version{0};
  std::uint64_t m_table_version{0};
  std::uint64_t m_next_sequence{0};
  std::uint64_t m_next_fencing_token{0};
  std::vector<Dmn_DLock_Entry> m_entries{};

  auto canonicalize() -> void {
    std::sort(m_entries.begin(), m_entries.end(),
              [](const Dmn_DLock_Entry &lhs, const Dmn_DLock_Entry &rhs) {
                const auto lhs_key = lhs.canonicalKey();
                const auto rhs_key = rhs.canonicalKey();

                return lhs_key < rhs_key;
              });
  }

  auto validate() const -> bool {
    if (m_domain.empty()) {
      return false;
    }

    for (const auto &entry : m_entries) {
      if (!entry.isValid() || entry.m_domain != m_domain) {
        return false;
      }

      if (!entry.m_range.isValid()) {
        return false;
      }
    }

    for (std::size_t i = 0; i < m_entries.size(); ++i) {
      for (std::size_t j = i + 1; j < m_entries.size(); ++j) {
        const auto &lhs = m_entries[i];
        const auto &rhs = m_entries[j];
        const bool same_domain = lhs.m_domain == rhs.m_domain;
        const bool granted_overlap =
            lhs.m_granted && rhs.m_granted && lhs.m_range.overlaps(rhs.m_range);

        if (same_domain && granted_overlap) {
          return false;
        }
      }
    }

    return true;
  }

  auto classifyGrantCandidate(const Dmn_DLock_Entry &candidate) const
      -> Dmn_DLock_ResultCode {
    if (!candidate.isValid() || !candidate.m_range.isValid()) {
      return Dmn_DLock_ResultCode::kInvalidRange;
    }

    if (candidate.m_domain != m_domain) {
      return Dmn_DLock_ResultCode::kInvalidState;
    }

    for (const auto &entry : m_entries) {
      if (entry.m_request_id == candidate.m_request_id &&
          entry.m_session_id == candidate.m_session_id) {
        continue;
      }

      if (entry.m_granted && candidate.m_granted &&
          entry.m_domain == candidate.m_domain &&
          entry.m_range.overlaps(candidate.m_range)) {
        return Dmn_DLock_ResultCode::kConflict;
      }
    }

    return Dmn_DLock_ResultCode::kOk;
  }

  auto canAcceptEntry(const Dmn_DLock_Entry &candidate) const -> bool {
    return classifyGrantCandidate(candidate) == Dmn_DLock_ResultCode::kOk;
  }

  auto withCandidate(const Dmn_DLock_Entry &candidate) const
      -> Dmn_DLock_TableSnapshot {
    Dmn_DLock_TableSnapshot result = *this;
    if (classifyGrantCandidate(candidate) != Dmn_DLock_ResultCode::kOk) {
      return {};
    }

    result.m_entries.push_back(candidate);
    result.m_table_version += 1;
    result.m_next_sequence =
        std::max(result.m_next_sequence, candidate.m_sequence + 1);
    result.canonicalize();

    return result;
  }

  auto isGrantedForRange(const Dmn_DLock_Range &range) const -> bool {
    for (const auto &entry : m_entries) {
      if (entry.m_granted && entry.m_range.overlaps(range)) {
        return true;
      }
    }

    return false;
  }

  auto toProto() const -> dmn::DLockTablePb {
    dmn::DLockTablePb table{};
    table.set_domain(m_domain);
    table.set_schema_version(m_schema_version);
    table.set_protocol_version(m_protocol_version);
    table.set_publisher_incarnation(m_publisher_incarnation);
    table.set_base_table_version(m_base_table_version);
    table.set_table_version(m_table_version);
    table.set_next_sequence(m_next_sequence);
    table.set_next_fencing_token(m_next_fencing_token);

    auto snapshot = m_entries;
    std::sort(snapshot.begin(), snapshot.end(),
              [](const Dmn_DLock_Entry &lhs, const Dmn_DLock_Entry &rhs) {
                return lhs.canonicalKey() < rhs.canonicalKey();
              });

    for (const auto &entry : snapshot) {
      auto *pb_entry = table.add_entries();

      pb_entry->set_domain(entry.m_domain);
      pb_entry->set_request_id(entry.m_request_id);
      pb_entry->set_session_id(entry.m_session_id);
      pb_entry->set_owner_id(entry.m_owner_id);
      pb_entry->mutable_range()->set_start(entry.m_range.m_start);
      pb_entry->mutable_range()->set_end(entry.m_range.m_end);
      pb_entry->set_priority(entry.m_priority);
      pb_entry->set_sequence(entry.m_sequence);
      pb_entry->set_fence(entry.m_fence);
      pb_entry->set_state(static_cast<dmn::DLockEntryStatePb>(
          entry.m_state == Dmn_DLock_EntryState::kWaiting
              ? dmn::DLockEntryStatePb::DLOCK_ENTRY_STATE_WAITING
          : entry.m_state == Dmn_DLock_EntryState::kGranted
              ? dmn::DLockEntryStatePb::DLOCK_ENTRY_STATE_GRANTED
          : entry.m_state == Dmn_DLock_EntryState::kReleased
              ? dmn::DLockEntryStatePb::DLOCK_ENTRY_STATE_RELEASED
          : entry.m_state == Dmn_DLock_EntryState::kCancelled
              ? dmn::DLockEntryStatePb::DLOCK_ENTRY_STATE_CANCELLED
          : entry.m_state == Dmn_DLock_EntryState::kExpired
              ? dmn::DLockEntryStatePb::DLOCK_ENTRY_STATE_EXPIRED
              : dmn::DLockEntryStatePb::DLOCK_ENTRY_STATE_ERROR));
      pb_entry->set_terminal_reason(static_cast<dmn::DLockTerminalReasonPb>(
          entry.m_terminal_reason == Dmn_DLock_TerminalReason::kNone
              ? dmn::DLockTerminalReasonPb::DLOCK_TERMINAL_REASON_NONE
          : entry.m_terminal_reason == Dmn_DLock_TerminalReason::kReleased
              ? dmn::DLockTerminalReasonPb::DLOCK_TERMINAL_REASON_RELEASED
          : entry.m_terminal_reason == Dmn_DLock_TerminalReason::kCancelled
              ? dmn::DLockTerminalReasonPb::DLOCK_TERMINAL_REASON_CANCELLED
          : entry.m_terminal_reason == Dmn_DLock_TerminalReason::kTimeout
              ? dmn::DLockTerminalReasonPb::DLOCK_TERMINAL_REASON_TIMEOUT
          : entry.m_terminal_reason == Dmn_DLock_TerminalReason::kExpired
              ? dmn::DLockTerminalReasonPb::DLOCK_TERMINAL_REASON_EXPIRED
              : dmn::DLockTerminalReasonPb::DLOCK_TERMINAL_REASON_SHUTDOWN));
      pb_entry->set_acquire_deadline_ticks(entry.m_acquire_deadline_ticks);
      pb_entry->set_lease_deadline_ticks(entry.m_lease_deadline_ticks);
      pb_entry->set_waiting(entry.m_waiting);
      pb_entry->set_granted(entry.m_granted);
    }

    return table;
  }

  static auto
  fromProto(const dmn::DLockTablePb &table) -> Dmn_DLock_TableSnapshot {
    Dmn_DLock_TableSnapshot snapshot{};
    snapshot.m_domain = table.domain();
    snapshot.m_schema_version = table.schema_version();
    snapshot.m_protocol_version = table.protocol_version();
    snapshot.m_publisher_incarnation = table.publisher_incarnation();
    snapshot.m_base_table_version = table.base_table_version();
    snapshot.m_table_version = table.table_version();
    snapshot.m_next_sequence = table.next_sequence();
    snapshot.m_next_fencing_token = table.next_fencing_token();

    snapshot.m_entries.reserve(static_cast<std::size_t>(table.entries_size()));
    for (const auto &entry : table.entries()) {
      Dmn_DLock_Entry parsed{};
      parsed.m_domain = entry.domain();
      parsed.m_request_id = entry.request_id();
      parsed.m_session_id = entry.session_id();
      parsed.m_owner_id = entry.owner_id();
      parsed.m_range = {entry.range().start(), entry.range().end()};
      parsed.m_priority = entry.priority();
      parsed.m_sequence = entry.sequence();
      parsed.m_fence = entry.fence();
      parsed.m_state =
          entry.state() == dmn::DLockEntryStatePb::DLOCK_ENTRY_STATE_WAITING
              ? Dmn_DLock_EntryState::kWaiting
          : entry.state() == dmn::DLockEntryStatePb::DLOCK_ENTRY_STATE_GRANTED
              ? Dmn_DLock_EntryState::kGranted
          : entry.state() == dmn::DLockEntryStatePb::DLOCK_ENTRY_STATE_RELEASED
              ? Dmn_DLock_EntryState::kReleased
          : entry.state() == dmn::DLockEntryStatePb::DLOCK_ENTRY_STATE_CANCELLED
              ? Dmn_DLock_EntryState::kCancelled
          : entry.state() == dmn::DLockEntryStatePb::DLOCK_ENTRY_STATE_EXPIRED
              ? Dmn_DLock_EntryState::kExpired
              : Dmn_DLock_EntryState::kError;
      parsed.m_terminal_reason =
          entry.terminal_reason() ==
                  dmn::DLockTerminalReasonPb::DLOCK_TERMINAL_REASON_NONE
              ? Dmn_DLock_TerminalReason::kNone
          : entry.terminal_reason() ==
                  dmn::DLockTerminalReasonPb::DLOCK_TERMINAL_REASON_RELEASED
              ? Dmn_DLock_TerminalReason::kReleased
          : entry.terminal_reason() ==
                  dmn::DLockTerminalReasonPb::DLOCK_TERMINAL_REASON_CANCELLED
              ? Dmn_DLock_TerminalReason::kCancelled
          : entry.terminal_reason() ==
                  dmn::DLockTerminalReasonPb::DLOCK_TERMINAL_REASON_TIMEOUT
              ? Dmn_DLock_TerminalReason::kTimeout
          : entry.terminal_reason() ==
                  dmn::DLockTerminalReasonPb::DLOCK_TERMINAL_REASON_EXPIRED
              ? Dmn_DLock_TerminalReason::kExpired
              : Dmn_DLock_TerminalReason::kShutdown;
      parsed.m_acquire_deadline_ticks = entry.acquire_deadline_ticks();
      parsed.m_lease_deadline_ticks = entry.lease_deadline_ticks();
      parsed.m_waiting = entry.waiting();
      parsed.m_granted = entry.granted();

      snapshot.m_entries.push_back(parsed);
    }

    snapshot.canonicalize();

    return snapshot;
  }
};

/**
 * @brief Lightweight DMesg handler wrapper used by the DLock prototype.
 *
 * `publishSnapshot()` only validates the supplied snapshot; it does not write
 * to DMesg. `postToOwnContext()` schedules work in the inherited handler
 * context, but the local acquire implementation does not use that path.
 */
class Dmn_DLock_Handler : public Dmn_DMesg::Dmn_DMesgHandler {
public:
  Dmn_DLock_Handler(std::string_view name, std::string_view topic,
                    Dmn_DLock_Config config = {},
                    Dmn_DMesg::FilterTask filter_fn = {},
                    Dmn_DMesg::AsyncProcessTask async_process_fn = {})
      : Dmn_DMesg::Dmn_DMesgHandler(name, topic, std::move(filter_fn),
                                    std::move(async_process_fn),
                                    Dmn_DMesg::HandlerConfig{}),
        m_config{std::move(config)} {
    m_config.m_domain = config.m_domain;
  }

  auto publishSnapshot(const Dmn_DLock_TableSnapshot &snapshot) -> bool {
    if (!snapshot.validate()) {
      return false;
    }

    return true;
  }

  auto postToOwnContext(std::function<void()> fn) -> void {
    this->scheduleInHandlerContext(std::move(fn));
  }

private:
  Dmn_DLock_Config m_config{};
};

/**
 * @brief Lightweight proxy to the lock-specific DMesg handler.
 */
class Dmn_DLock_HandlerProxy {
public:
  auto operator->() const -> std::shared_ptr<Dmn_DLock_Handler> {
    if (!m_handler) {
      throw std::runtime_error("lock handler has been closed");
    }

    return m_handler;
  }

  explicit operator bool() const noexcept {
    return static_cast<bool>(m_handler);
  }

  void reset() { m_handler.reset(); }

private:
  friend class Dmn_DLock_Base;

  std::shared_ptr<Dmn_DLock_Handler> m_handler{};
};

/**
 * @brief Minimal local handler/proxy helper used by the DLock prototype.
 */
class Dmn_DLock_Base {
public:
  using HandlerType = Dmn_DLock_HandlerProxy;

  explicit Dmn_DLock_Base(std::string_view name, Dmn_DLock_Config config = {})
      : m_name{name}, m_config{std::move(config)} {}

  auto openHandler(std::string_view session_name = "session",
                   std::string_view topic = "dlock") -> HandlerType {
    auto handler =
        std::make_shared<Dmn_DLock_Handler>(session_name, topic, m_config);
    HandlerType proxy{};
    proxy.m_handler = handler;

    return proxy;
  }

  void closeHandler(HandlerType &handlerToClose) { handlerToClose.reset(); }

protected:
  std::string m_name{};
  Dmn_DLock_Config m_config{};
};

/**
 * @brief Local-only DLock prototype constrained to a `Dmn_DMesg` base.
 *
 * This class currently serializes acquisitions only within its own in-memory
 * table. It does not publish snapshots, synchronize handlers, enforce leases,
 * or implement the publisher-serialized distributed-lock design. The template
 * constraint prevents instantiation with `Dmn_DMesgNet`; it does not make this
 * prototype suitable for distributed use.
 */
template <class DMesgBase = Dmn_DMesg>
class Dmn_DLock : public DMesgBase, public Dmn_DLock_Base {
public:
  static_assert(
      std::is_same_v<DMesgBase, Dmn_DMesg>,
      "Dmn_DLock v1 requires Dmn_DMesg and forbids net-authority use");

  using HandlerType = Dmn_DLock_HandlerProxy;

  explicit Dmn_DLock(std::string_view name, Dmn_DLock_Config config = {})
      : DMesgBase{name}, Dmn_DLock_Base{name, std::move(config)} {
    m_table.m_domain = m_config.m_domain;
    m_table.m_schema_version = 1;
    m_table.m_protocol_version = 1;
  }

  auto openHandler(std::string_view session_name = "session",
                   std::string_view topic = "dlock") -> HandlerType {
    return Dmn_DLock_Base::openHandler(session_name, topic);
  }

  void closeHandler(HandlerType &handlerToClose) {
    Dmn_DLock_Base::closeHandler(handlerToClose);
  }

  auto acquireLock(Dmn_DLock_Range range,
                   const Dmn_DLock_RequestOptions &options = {})
      -> Dmn_DLock_Result {
    if (!range.isValid()) {
      return {Dmn_DLock_ResultCode::kInvalidRange, range, options.m_request_id,
              "invalid range"};
    }

    std::unique_lock<std::mutex> lock(m_mutex);
    std::string request_id = options.m_request_id;
    if (request_id.empty()) {
      request_id =
          std::string("req-") + std::to_string(m_table.m_next_sequence + 1);
    }

    while (m_table.isGrantedForRange(range)) {
      if (!options.m_wait || options.m_no_wait) {
        return {Dmn_DLock_ResultCode::kNoWait, range, request_id,
                "range is already granted"};
      }

      m_cv.wait(lock);
    }

    Dmn_DLock_Entry entry{};

    entry.m_domain = m_config.m_domain;
    entry.m_request_id = request_id;
    entry.m_session_id = std::string("session-") + request_id;
    entry.m_owner_id = request_id;
    entry.m_range = range;
    entry.m_priority = 1;
    entry.m_sequence = m_table.m_next_sequence + 1;
    entry.m_fence = m_table.m_next_fencing_token + 1;
    entry.m_state = Dmn_DLock_EntryState::kGranted;
    entry.m_terminal_reason = Dmn_DLock_TerminalReason::kNone;
    entry.m_acquire_deadline_ticks = 0;
    entry.m_lease_deadline_ticks = options.m_lease_ticks;
    entry.m_waiting = false;
    entry.m_granted = true;

    const auto candidate = m_table.withCandidate(entry);
    if (candidate.m_entries.empty()) {
      return {Dmn_DLock_ResultCode::kConflict, range, request_id,
              "range already unavailable"};
    }

    m_table = candidate;
    m_table.m_next_sequence = entry.m_sequence;
    m_table.m_next_fencing_token = entry.m_fence;
    m_cv.notify_all();

    return {Dmn_DLock_ResultCode::kOk, range, request_id, "granted"};
  }

  auto acquireLockAsync(Dmn_DLock_Range range,
                        const Dmn_DLock_RequestOptions &options = {})
      -> std::future<Dmn_DLock_Result> {
    auto promise = std::make_shared<std::promise<Dmn_DLock_Result>>();
    auto future = promise->get_future();
    std::thread([this, range, options, promise]() {
      auto result = acquireLock(range, options);

      promise->set_value(result);
    }).detach();

    return future;
  }

  auto releaseLock(std::string_view request_id) -> Dmn_DLock_Result {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = std::find_if(m_table.m_entries.begin(), m_table.m_entries.end(),
                           [&](const Dmn_DLock_Entry &entry) {
                             return entry.m_request_id == request_id;
                           });
    if (it == m_table.m_entries.end()) {
      return {Dmn_DLock_ResultCode::kInvalidState,
              {},
              std::string(request_id),
              "request not found"};
    }

    const auto range = it->m_range;
    m_table.m_entries.erase(it);
    m_cv.notify_all();

    return {Dmn_DLock_ResultCode::kOk, range, std::string(request_id),
            "released"};
  }

  auto currentSnapshot() const -> Dmn_DLock_TableSnapshot {
    std::lock_guard<std::mutex> lock(m_mutex);

    return m_table;
  }

private:
  mutable std::mutex m_mutex{};
  std::condition_variable m_cv{};
  Dmn_DLock_TableSnapshot m_table{};
};

} // namespace dmn

#endif // DMN_DLOCK_HPP_
