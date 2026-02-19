/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-kafka-util.hpp
 * @brief Utility helpers for configuring librdkafka instances.
 *
 * This header provides a thin, type-safe wrapper around
 * rd_kafka_conf_set() that returns a std::expected value instead of
 * relying on an output error-string buffer. Use set_config() wherever
 * you need to apply a single key/value pair to an rd_kafka_conf_t
 * object and want idiomatic C++ error handling.
 *
 * The constant kKafkaErrorStringLength defines the size of the
 * temporary error string buffer required by rd_kafka_conf_set() and
 * should be used whenever that buffer is allocated on the stack.
 */

#ifndef DMN_KAFKA_UTIL_HPP_

#define DMN_KAFKA_UTIL_HPP_

#include <expected>
#include <string>
#include <string_view>

#include "rdkafka.h"

namespace dmn {

constexpr size_t kKafkaErrorStringLength = 512;

/**
 * @brief The method sets kafka configuration to key value.
 *
 * @param conf  The rd_kafka_conf_t object to set the configuration' key value
 * @param key   The configuration key
 * @param value The configuration value
 *
 * @return It returns the RD_KAFKA_CONF_OK if everything is ok, else a string
 *         describing the kafka error
 */
auto set_config(rd_kafka_conf_t *conf, std::string_view key,
                std::string_view value)
    -> std::expected<rd_kafka_conf_res_t, std::string>;

} // namespace dmn

#endif // DMN_KAFKA_UTIL_HPP_
