/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-dmesgnet-dbus.cpp
 * @brief Composition facade joining DMesgNet to D-Bus byte-signal endpoints.
 */

#include "dmn-dmesgnet-dbus.hpp"

#include "dmn-dbus-io.hpp"
#include "dmn-dmesgnet.hpp"

#ifdef FIU_ENABLE
#include <fiu.h>
#endif

#include <memory>
#include <new>
#include <string_view>

namespace dmn {

#ifdef FIU_ENABLE
namespace {

auto createOutputForFaultInjection(const Dmn_DbusConfig &config)
    -> std::shared_ptr<Dmn_DbusOutput> {
  // Fail after input setup to verify constructor rollback without a public
  // endpoint factory or test hook.
  if (fiu_fail("dmn/dbus/facade/output_endpoint_creation") != 0) {
    throw std::bad_alloc{};
  }

  return std::make_shared<Dmn_DbusOutput>(config);
}

} // namespace
#endif

struct Dmn_DMesgDbus::Impl {
  Impl(std::string_view node_id, const Dmn_DbusConfig &config)
      : input{std::make_shared<Dmn_DbusInput>(config)},
#ifdef FIU_ENABLE
        output{createOutputForFaultInjection(config)},
#else
        output{std::make_shared<Dmn_DbusOutput>(config)},
#endif
        node{node_id, input, output} {
  }

  // Keep endpoints alive through Dmn_DMesgNet destruction: its teardown stops
  // input first, then sends the final best-effort heartbeat through output.
  std::shared_ptr<Dmn_DbusInput> input;
  std::shared_ptr<Dmn_DbusOutput> output;
  Dmn_DMesgNet node;
};

Dmn_DMesgDbus::Dmn_DMesgDbus(std::string_view node_id,
                             const Dmn_DbusConfig &config)
    : m_impl{std::make_unique<Impl>(node_id, config)} {}

Dmn_DMesgDbus::~Dmn_DMesgDbus() noexcept = default;

auto Dmn_DMesgDbus::openHandler(const HandlerSpec &spec) -> HandlerType {
  return m_impl->node.openHandler(spec.m_name, spec.m_topic, spec.m_filter_fn,
                                  spec.m_async_process_fn, spec.m_configs);
}

auto Dmn_DMesgDbus::openHandlerWithFactory(
    const HandlerSpec &spec, const HandlerFactory &factory) -> HandlerType {
  return m_impl->node.openHandlerWithFactory(spec, factory);
}

void Dmn_DMesgDbus::closeHandler(HandlerType &handler) {
  m_impl->node.closeHandler(handler);
}

auto Dmn_DMesgDbus::getTopicLastMessage(std::string_view topic)
    -> std::optional<DMesgPb> {
  return m_impl->node.getTopicLastMessage(topic);
}

void Dmn_DMesgDbus::resetConflictStateWithLastTopicMessage(
    std::string_view topic) {
  m_impl->node.resetConflictStateWithLastTopicMessage(topic);
}

auto Dmn_DMesgDbus::status() const -> Dmn_DMesgDbusStatus {
  return Dmn_DMesgDbusStatus{m_impl->input->status(), m_impl->output->status()};
}

} // namespace dmn
