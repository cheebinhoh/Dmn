/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 */

#ifndef DMN_KAFKA_UTIL_HPP_HAVE_SEEN

#define DMN_KAFKA_UTIL_HPP_HAVE_SEEN

#include "rdkafka.h"

#include <expected>
#include <string>
#include <string_view>

namespace Dmn {

constexpr size_t KAFKA_ERROR_STRING_LENGTH = 512;

/**
 * @brief The method sets kafka configuration to key value.
 *
 * @param conf  The rd_kafka_conf_t object to set the configuration' key value
 * @param key   The configuration key
 * @param value The configuration value
 *
 * @return It returns the RD_KAFKA_CONF_OK if it has expected result, else
 *         a string describing the kafka error
 */
std::expected<rd_kafka_conf_res_t, std::string>
set_config(rd_kafka_conf_t *conf, std::string_view key, std::string_view value);

} /* End of namespace Dmn */

#endif /* End of macro DMN_KAFKA_UTIL_HPP_HAVE_SEEN */
