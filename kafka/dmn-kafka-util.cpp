/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 */

#include "dmn-kafka-util.hpp"

#include "rdkafka.h"

#include <cassert>
#include <expected>
#include <string>
#include <string_view>

namespace Dmn {

std::expected<rd_kafka_conf_res_t, std::string>
set_config(rd_kafka_conf_t *config, std::string_view key,
           std::string_view value) {
  char errstr[KAFKA_ERROR_STRING_LENGTH]{};
  rd_kafka_conf_res_t res{};

  assert(nullptr != config ||
         nullptr == "config parameter must not be nullptr");

  res = rd_kafka_conf_set(config, key.data(), value.data(), errstr,
                          sizeof(errstr));
  if (RD_KAFKA_CONF_OK != res) {
    std::string unexpectedErrStr = std::string(errstr);

    return std::unexpected(unexpectedErrStr);
  }

  return res;
}

} // namespace Dmn
