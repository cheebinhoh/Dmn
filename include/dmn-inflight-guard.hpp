/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-inflight-guard.hpp
 * @brief RAII guard + in-flight counter for safe shutdown and (optional) epoch
 * / hazard-style bookkeeping.
 *
 * @details
 * Dmn_Inflight_Guard is a small utility intended to be inherited by a class
 * that exposes multi-threaded APIs (e.g. queue pop/push). It provides:
 *
 *  1) A fast in-flight operation counter that tracks how many operations have
 *     "entered" the object and not yet left.
 *
 *  2) A shutdown guard check (via isInflightGuardClosed()) that prevents new
 *     operations from entering after shutdown has begun.
 *
 *  3) A blocking wait (waitForEmptyInflight()) that waits until all previously
 *     entered operations have left, so destructors / stop() routines can safely
 *     tear down shared state.
 *
 * This module is used in two different ways in this repository:
 *
 *  - include/dmn-blockingqueue.hpp:
 *    Dmn_BlockingQueue privately inherits Dmn_Inflight_Guard<> (no payload).
 *    pop() creates a ticket at its public function entry:
 *
 *      [[maybe_unused]] auto t = this->enterInflightGate();
 *
 *    and the queue destructor does:
 *
 *      stop();
 *      this->waitForEmptyInflight();
 *
 *    This ensures that once stop() marks the queue shutdown
 *    (isInflightGuardClosed() becomes true), no new operations can enter, and
 *    the destructor waits for all already-running calls (possibly blocked) to
 *    return before the object is destroyed. Hence the stop() should either
 *    force already-running calls to be returned, or those calls are timed
 *    based and will return eventually.
 *
 *  - include/dmn-blockingqueue-lf.hpp:
 *    Dmn_BlockingQueue_Lf privately inherits Dmn_Inflight_Guard<uint64_t>.
 *    Here the ticket's payload value is an "epochIndex" derived in
 *    enterInflightGuardFnc(). popOptional() then uses ticket->getValue() to
 *    retire nodes into an epoch bucket:
 *
 *      retireNode(inflightTicket->getValue(), first);
 *
 *    On leaving inflight guard, leaveInflightGuardFnc(epochIndex) decrements
 *    the epoch's in-flight count and may reclaim retired nodes when safe.
 *
 * Key concepts
 * ------------
 * - "Inflight guard closed":
 *   Implemented by the derived class in isInflightGuardClosed(). It
 *   typically maps to a shutdown flag that is set in stop() / destructor. When
 *   the inflight guard is closed, enterInflightGate() throws (by design) to
 *   prevent new work from starting against a shutting-down object.
 *
 * - "In-flight":
 *   Any operation that successfully acquired a Ticket. The ticket
 *   lifetime should cover the entire public API call (or the entire critical
 *   section that must not overlap with teardown / reclamation).
 *
 * - Optional payload (template parameter T):
 *   The derived class can return a per-call value from enterInflightGuardFnc()
 *   that is stored inside the ticket and later passed to
 *   leaveInflightGuardFnc(). Examples include an epoch index, hazard slot, or
 *   other per-operation bookkeeping token. If you don't need a payload, use the
 *   default std::monostate.
 *
 * Threading / memory-order notes
 * ------------------------------
 * - m_inflight_count is incremented when a ticket is constructed and
 *   decremented when it is destructed. waitForEmptyInflight() uses
 *   atomic::wait/notify_all to efficiently sleep until the count reaches zero.
 *
 * - The guard checks isInflightGuardClosed() both before and after incrementing
 *   the counter to avoid a race where shutdown begins concurrently with entry:
 *      - if shutdown is observed before entry -> throw and do not increment
 *      - if shutdown happens after increment -> decrement and throw
 *
 * Exception / cancellation notes
 * ------------------------------
 * - enterInflightGate() may throw std::runtime_error if the inflight guard is
 *   closed.
 * - If enterInflightGuardFnc() throws, the ticket constructor rolls back the
 *   in-flight count and rethrows.
 * - Ticket::~Ticket() is noexcept and swallows exceptions thrown by
 *   leaveInflightGuardFnc() to keep teardown safe.
 *
 * Usage guidelines
 * ----------------
 * - Derived classes should:
 *     - Override isInflightGuardClosed() to reflect shutdown state.
 *     - Optionally override enterInflightGuardFnc()/leaveInflightGuardFnc() if
 *       per-call bookkeeping is required (e.g. epoch-based reclamation).
 * - Public APIs should acquire a ticket early (typically at the start of
 *   push/pop) and keep it alive until the function returns.
 * - stop()/destructor should close the inflight guard first, then call
 *   waitForEmptyInflight() before freeing shared resources that in-flight calls
 *   might touch.
 */

#ifndef DMN_INFLIGHT_GUARD_HPP_
#define DMN_INFLIGHT_GUARD_HPP_

#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <variant> // std::monostate

namespace dmn {

template <class T = std::monostate> class Dmn_Inflight_Guard {
public:
  /**
   * @brief RAII ticket that tracks a single in-flight operation.
   *
   * Acquiring a Ticket increments the inflight counter (and invokes
   * enterInflightGuardFnc()). Releasing it decrements the counter (and
   * invokes leaveInflightGuardFnc()) and notifies any thread blocked in
   * waitForEmptyInflight().
   */
  class Ticket {
  public:
    /**
     * @brief Acquire an inflight ticket, incrementing the in-flight counter.
     *
     * @param inflightguard Owning guard (must not be null).
     * @throws std::runtime_error if the guard is already closed/shutting down.
     */
    Ticket(Dmn_Inflight_Guard *inflightguard) : m_inflightguard{inflightguard} {
      assert(nullptr != m_inflightguard);

      if (m_inflightguard->isInflightGuardClosed()) {
        throw std::runtime_error(
            "failed to acquire inflight ticket: guard is shutting down/closed");
      }

      m_inflightguard->m_inflight_count.fetch_add(1, std::memory_order_acq_rel);
      if (m_inflightguard->isInflightGuardClosed()) {
        m_inflightguard->m_inflight_count.fetch_sub(1,
                                                    std::memory_order_acq_rel);
        m_inflightguard->m_inflight_count.notify_all();

        throw std::runtime_error(
            "failed to acquire inflight ticket: guard is shutting down/closed");
      }

      m_entered = true;

      try {
        m_value = m_inflightguard->enterInflightGuardFnc();
      } catch (...) {
        // Roll back the in-flight count and notify waiters if
        // enterInflightGuardFnc throws.
        m_inflightguard->m_inflight_count.fetch_sub(1,
                                                    std::memory_order_acq_rel);
        m_inflightguard->m_inflight_count.notify_all();
        throw;
      }
    }

    /// @brief Release the ticket, decrement the in-flight counter, and notify
    /// waiters.
    virtual ~Ticket() noexcept {
      assert(nullptr != m_inflightguard);

      if (!m_entered) {
        return;
      }

      try {
        m_inflightguard->leaveInflightGuardFnc(m_value);
      } catch (...) {
        // Swallow exceptions to uphold noexcept and ensure proper teardown.
      }

      m_entered = false;
      m_inflightguard->m_inflight_count.fetch_sub(1, std::memory_order_release);
      m_inflightguard->m_inflight_count.notify_all();
    }

    Ticket(const Ticket &obj) = delete;
    Ticket &operator=(const Ticket &obj) = delete;
    Ticket(Ticket &&obj) = delete;
    Ticket &operator=(Ticket &&obj) = delete;

    /// @brief Return true if the ticket was successfully acquired.
    explicit operator bool() const noexcept { return m_entered; }

    /// @brief Return the per-call payload value set by enterInflightGuardFnc().
    virtual auto getValue() const -> const T & { return m_value; }

  private:
    Dmn_Inflight_Guard *m_inflightguard{};
    bool m_entered{false};
    T m_value{}; // std::monostate{} when "no payload"
  };

  /// @brief Default-construct the guard with an in-flight count of zero.
  Dmn_Inflight_Guard() = default;

  /// @brief Wait for all in-flight operations to complete before destruction.
  virtual ~Dmn_Inflight_Guard() noexcept { waitForEmptyInflight(); };

  Dmn_Inflight_Guard(const Dmn_Inflight_Guard &obj) = delete;
  Dmn_Inflight_Guard &operator=(const Dmn_Inflight_Guard &obj) = delete;
  Dmn_Inflight_Guard(Dmn_Inflight_Guard &&obj) = delete;
  Dmn_Inflight_Guard &operator=(Dmn_Inflight_Guard &&obj) = delete;

  /**
   * @brief Acquire a new inflight ticket; throws if the guard is closed.
   *
   * @return A unique_ptr to the acquired Ticket.
   * @throws std::runtime_error if isInflightGuardClosed() returns true.
   */
  virtual auto enterInflightGate() -> std::unique_ptr<Ticket> final {
    auto ticket = std::make_unique<Ticket>(this);

    return ticket;
  }

  /**
   * @brief Block until all outstanding inflight tickets have been released.
   *
   * Uses atomic::wait/notify_all to efficiently sleep rather than spin.
   */
  void waitForEmptyInflight() const {
    uint64_t val{};

    while ((val = m_inflight_count.load(std::memory_order_acquire)) > 0) {
      m_inflight_count.wait(val, std::memory_order_acquire);
    }
  }

protected:
  /// @brief Called when a Ticket is acquired; may return a per-call payload
  /// value.
  virtual auto enterInflightGuardFnc() -> T { return T{}; }

  /// @brief Return true when the guard is closed and no new tickets should be
  /// issued.
  virtual auto isInflightGuardClosed() -> bool { return false; }

  /// @brief Called when a Ticket is released with the payload value from
  /// enterInflightGuardFnc().
  virtual void leaveInflightGuardFnc(const T &) noexcept {}

  /// @brief Return the current number of live (in-flight) tickets.
  auto inflight_count() const -> uint64_t {
    return m_inflight_count.load(std::memory_order_acquire);
  }

private:
  std::atomic<std::uint64_t> m_inflight_count{};
};

} // namespace dmn

#endif /* DMN_INFLIGHT_GUARD_HPP_ */
