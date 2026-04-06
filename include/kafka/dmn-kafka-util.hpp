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
 * @brief Set a single configuration key/value pair on an @c rd_kafka_conf_t
 *        object.
 *
 * @param conf  The @c rd_kafka_conf_t object to configure.
 * @param key   The configuration key.
 * @param value The configuration value.
 *
 * @return @c RD_KAFKA_CONF_OK wrapped in @c std::expected on success, or an
 *         error string describing the failure.
 */
auto set_config(rd_kafka_conf_t *conf, std::string_view key,
                std::string_view value)
    -> std::expected<rd_kafka_conf_res_t, std::string>;

} // namespace dmn

#endif // DMN_KAFKA_UTIL_HPP_
