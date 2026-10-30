/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-bluetooth-cabi.cpp
 * @brief C ABI over the GUI-independent Bluetooth monitor engine.
 */

#include "app/gui/cabi/dmn-bluetooth-cabi.h"

#include "app/engine/dmn-engine.hpp"

#include <cstdlib>
#include <cstring>
#include <memory>

struct dmn_bt_engine {
  std::unique_ptr<dmn::gui::Dmn_Engine> m_engine;
};

namespace {

auto makeHandle(std::unique_ptr<dmn::gui::Dmn_EngineBackend> backend)
    -> dmn_bt_engine * {
  try {
    auto handle{std::make_unique<dmn_bt_engine>()};
    handle->m_engine =
        std::make_unique<dmn::gui::Dmn_Engine>(std::move(backend));

    return handle.release();
  } catch (...) {
    return nullptr;
  }
}

} // namespace

extern "C" {

dmn_bt_engine *dmn_bt_engine_create(void) {
  try {
    return makeHandle(dmn::gui::makeBluezBackend());
  } catch (...) {
    return nullptr;
  }
}

dmn_bt_engine *dmn_bt_engine_create_demo(void) {
  try {
    return makeHandle(dmn::gui::makeDemoBackend());
  } catch (...) {
    return nullptr;
  }
}

void dmn_bt_engine_destroy(dmn_bt_engine *engine) {
  try {
    delete engine;
  } catch (...) {
  }
}

int dmn_bt_engine_send(dmn_bt_engine *engine, const char *command_json) {
  if (engine == nullptr || command_json == nullptr) {
    return 1;
  }

  try {
    return engine->m_engine->send(command_json) ? 0 : 1;
  } catch (...) {
    return 1;
  }
}

char *dmn_bt_engine_next_message(dmn_bt_engine *engine, int timeout_ms) {
  if (engine == nullptr) {
    return nullptr;
  }

  try {
    auto message { engine->m_engine->nextMessage(
        std::chrono::milliseconds{timeout_ms < 0 ? 0 : timeout_ms})};
    if (!message) {
      return nullptr;
    }

    auto *copy = static_cast<char *>(std::malloc(message->size() + 1));
    if (copy == nullptr) {
      return nullptr;
    }

    std::memcpy(copy, message->c_str(), message->size() + 1);

    return copy;
  } catch (...) {
    return nullptr;
  }
}

void dmn_bt_string_free(char *text) { std::free(text); }

const char *dmn_bt_version(void) { return "1"; }

} // extern "C"
