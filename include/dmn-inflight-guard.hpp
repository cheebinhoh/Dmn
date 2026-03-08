/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-inflight-guard.hpp
 * @brief RAII-based guard for tracking in-flight operations.
 */

#ifndef DMN_INFLIGHT_GUARD_HPP_
#define DMN_INFLIGHT_GUARD_HPP_

#include <atomic>
#include <cstddef>
#include <memory>

namespace dmn {

class Dmn_Inflight_Guard {
  class Inflight_Ticket {
  public:
    Inflight_Ticket(Dmn_Inflight_Guard *inflightguard)
        : m_inflightguard{inflightguard} {

      m_inflightguard->m_inflight_count.fetch_add(1, std::memory_order_release);

      m_inflightguard->enterGateFnc();
    }

    virtual ~Inflight_Ticket() {
      m_inflightguard->leaveGateFnc();

      m_inflightguard->m_inflight_count.fetch_sub(1, std::memory_order_release);
      m_inflightguard->m_inflight_count.notify_all();
    }

  private:
    Dmn_Inflight_Guard *m_inflightguard{};
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

  auto inflight_count() -> uint64_t {
    return m_inflight_count.load(std::memory_order_acquire);
  }

private:
  std::atomic<std::uint64_t> m_inflight_count{};
};

} // namespace dmn

#endif /* DMN_INFLIGHT_GUARD_HPP_ */
