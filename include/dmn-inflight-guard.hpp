/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-inflight-guard.hpp
 * @brief RAII-based guard for tracking in-flight operations.
 */

#ifndef DMN_INFLIGHT_GUARD_HPP_
#define DMN_INFLIGHT_GUARD_HPP_

#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
namespace dmn {

class Dmn_Inflight_Guard {
  class Inflight_Ticket {
  public:
    Inflight_Ticket(Dmn_Inflight_Guard *inflightguard)
        : m_inflightguard{inflightguard} {
      if (m_inflightguard->isGateClosed()) {
        throw std::runtime_error(
            "fail to acquire Inflight_Ticket, it is shutting down");
      }

      m_inflightguard->m_inflight_count.fetch_add(1, std::memory_order_release);
      if (m_inflightguard->isGateClosed()) {
        m_inflightguard->m_inflight_count.fetch_sub(1,
                                                    std::memory_order_release);
        m_inflightguard->m_inflight_count.notify_all();

        throw std::runtime_error(
            "fail to acquire Inflight_Ticket, it is shutting down");
      }

      m_entered = true;

      try {
        m_inflightguard->enterGateFnc();
      } catch (...) {
        // Roll back the in-flight count and notify waiters if enterGateFnc
        // throws.
        m_inflightguard->m_inflight_count.fetch_sub(1,
                                                    std::memory_order_release);
        m_inflightguard->m_inflight_count.notify_all();
        throw;
      }
    }

    virtual ~Inflight_Ticket() {
      if (!m_entered) {
        return;
      }

      m_inflightguard->leaveGateFnc();

      m_entered = false;
      m_inflightguard->m_inflight_count.fetch_sub(1, std::memory_order_release);
      m_inflightguard->m_inflight_count.notify_all();
    }

  private:
    Dmn_Inflight_Guard *m_inflightguard{};
    bool m_entered{false};
  };

public:
  Dmn_Inflight_Guard() {};
  virtual ~Dmn_Inflight_Guard() { waitForEmptyInflight(); };

  Dmn_Inflight_Guard(const Dmn_Inflight_Guard &obj) = delete;
  const Dmn_Inflight_Guard &operator=(const Dmn_Inflight_Guard &obj) = delete;
  Dmn_Inflight_Guard(const Dmn_Inflight_Guard &&obj) = delete;
  Dmn_Inflight_Guard &operator=(Dmn_Inflight_Guard &&obj) = delete;

  virtual auto enterInflightGate() -> std::shared_ptr<Inflight_Ticket> final {
    auto ticket = std::make_shared<Inflight_Ticket>(this);

    return ticket;
  }

  void waitForEmptyInflight() {
    uint64_t val{};

    while ((val = m_inflight_count.load(std::memory_order_acquire)) > 0) {
      m_inflight_count.wait(val, std::memory_order_acquire);
    }
  }

protected:
  virtual void enterGateFnc() {}

  virtual void leaveGateFnc() {}

  virtual auto isGateClosed() -> bool { return false; }

  auto inflight_count() -> uint64_t {
    return m_inflight_count.load(std::memory_order_acquire);
  }

private:
  std::atomic<std::uint64_t> m_inflight_count{};
};

} // namespace dmn

#endif /* DMN_INFLIGHT_GUARD_HPP_ */
