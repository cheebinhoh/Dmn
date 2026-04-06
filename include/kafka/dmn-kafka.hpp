/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file kafka/dmn-kafka.hpp
 * @brief Dmn_Kafka — a Dmn_Io<std::string> adapter for Apache Kafka.
 *
 * Dmn_Kafka wraps the librdkafka C library and exposes a uniform
 * Dmn_Io<std::string> interface for both producing and consuming Kafka
 * messages.  A single instance acts as either a producer or consumer
 * depending on the Role argument supplied to the constructor.
 *
 * Configuration is supplied as a ConfigType map
 * (std::unordered_map<std::string, std::string>).  Most keys are passed through
 * to rd_kafka_conf_set(); three special keys are consumed by Dmn_Kafka itself
 * and are not forwarded:
 *  - Dmn_Kafka::Topic         — the Kafka topic to read from / write to.
 *  - Dmn_Kafka::Key           — an alternate topic name that, if set, overrides
 *                                the Topic value when determining which topic
 *                                to read from / write to.
 *  - Dmn_Kafka::PollTimeoutMs — consumer poll timeout in milliseconds.
 *
 * Thread-safety:
 *  - Concurrent calls to write() are serialised internally via an atomic flag;
 *    each write blocks until the delivery-report callback fires.
 *  - read() is not thread-safe; use a single reader thread per instance.
 */

#ifndef DMN_KAFKA_HPP_

#define DMN_KAFKA_HPP_

#include <atomic>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

#include "rdkafka.h"

#include "dmn-inflight-guard.hpp"
#include "dmn-io.hpp"
#include "dmn-kafka-util.hpp"

namespace dmn {

/**
 * @brief Apache Kafka producer/consumer that implements the Dmn_Io<std::string>
 *        interface.
 *
 * Wrap librdkafka so that Kafka topics can be used as drop-in Dmn_Io endpoints.
 * Construct with Role::kProducer to send messages or Role::kConsumer to receive
 * them.  Both directions use the same configuration map; Dmn_Kafka-specific
 * keys (Topic, Key, PollTimeoutMs) are extracted before the remainder is
 * forwarded to rd_kafka_conf_set().
 */
class Dmn_Kafka : public dmn::Dmn_Io<std::string>,
                  private Dmn_Inflight_Guard<> {
  using Inflight_Guard_Ticket = std::unique_ptr<Dmn_Inflight_Guard<>::Ticket>;

public:
  /**
   * @brief Configuration keys specific to the @c Dmn_Kafka module (not
   *        forwarded to librdkafka).
   *
   * These keys are extracted from the @c ConfigType map before the remaining
   * entries are passed to @c rd_kafka_conf_set().
   */
  const static std::string Topic; ///< Kafka topic to read from / write to.
  const static std::string
      Key; ///< Alternate topic that overrides @c Topic when set.
  const static std::string
      PollTimeoutMs; ///< Consumer poll timeout in milliseconds.

  using ConfigType = std::unordered_map<std::string, std::string>;

  using Dmn_Io<std::string>::write;

  /** @brief Selects whether this instance acts as a producer or a consumer. */
  enum class Role {
    kConsumer, ///< Consume messages from the configured Kafka topic.
    kProducer, ///< Produce messages to the configured Kafka topic.
  };

  /**
   * @brief Construct a Kafka producer or consumer with the supplied
   *        configuration.
   *
   * @param role    Whether this instance acts as a @c kProducer or @c
   * kConsumer.
   * @param configs Key/value configuration map; see class documentation for
   *                the reserved Dmn_Kafka-specific keys.
   */
  Dmn_Kafka(Role role, ConfigType configs = {});

  /**
   * @brief Destroy the Kafka handle, flushing any pending producer messages
   *        and closing the consumer session.
   */
  ~Dmn_Kafka() noexcept;

  Dmn_Kafka(const Dmn_Kafka &obj) = delete;
  Dmn_Kafka &operator=(const Dmn_Kafka &obj) = delete;
  Dmn_Kafka(Dmn_Kafka &&obj) = delete;
  Dmn_Kafka &operator=(Dmn_Kafka &&obj) = delete;

  /**
   * @brief Poll for the next message from the subscribed Kafka topic.
   *
   * Blocks for up to @c PollTimeoutMs milliseconds.  Returns @c std::nullopt
   * when no message arrives within the timeout or on shutdown.
   *
   * @return The message payload, or @c std::nullopt on timeout or shutdown.
   */
  auto read() -> std::optional<std::string> override;

  /**
   * @brief Produce a message (by copy) to the configured Kafka topic.
   *
   * @param item The message payload to send to the Kafka broker.
   */
  void write(const std::string &item) override;

  /**
   * @brief Produce a message (by move) to the configured Kafka topic.
   *
   * @param item The message payload; may be moved from.
   */
  void write(std::string &&item) override;

  /**
   * @brief Initiate an orderly shutdown of the Kafka instance, preventing
   *        further use and facilitating object teardown.
   */
  void shutdown() override;

private:
  static void cleanup_thunk_inflight(void *arg);

  virtual auto isInflightGuardClosed() -> bool override;

  /**
   * @brief librdkafka delivery-report callback invoked after each produced
   *        message is acknowledged (or fails) by the broker.
   *
   * @param kafka_handle The producer handle (unused).
   * @param rkmessage    Delivery report for the produced message.
   * @param opaque       Pointer to the owning @c Dmn_Kafka instance.
   */
  static void producerCallback(rd_kafka_t *kafka_handle,
                               const rd_kafka_message_t *rkmessage,
                               void *opaque);

  /**
   * @brief librdkafka generic error callback invoked for non-message-specific
   *        errors (e.g. broker connectivity issues).
   *
   * @param kafka_handle The producer/consumer handle (unused).
   * @param err          librdkafka error code.
   * @param reason       Human-readable reason string (unused).
   * @param opaque       Pointer to the owning @c Dmn_Kafka instance.
   */
  static void errorCallback(rd_kafka_t *kafka_handle, int err,
                            const char *reason, void *opaque);

  /**
   * @brief Internal helper that synchronously produces @p item to the Kafka
   *        topic and waits for the delivery report.
   *
   * @param item The message payload to produce.
   */
  void writeCopy(const std::string &item);

  Role m_role{}; ///< Whether this instance is a producer or a consumer.
  ConfigType
      m_configs{}; ///< Copy of the configuration map passed at construction.

  std::string m_key{};   ///< Kafka message key (overrides topic when set).
  std::string m_topic{}; ///< Kafka topic to produce to / consume from.
  long long m_poll_timeout_ms{}; ///< Consumer poll timeout in milliseconds.

  rd_kafka_t *m_kafka{}; ///< librdkafka producer or consumer handle.
  rd_kafka_resp_err_t
      m_kafka_err{}; ///< Error code set by delivery/error callbacks.

  std::atomic_flag
      m_write_atomic_flag{}; ///< Serialises concurrent write() calls.

  std::atomic_flag
      m_shutdown_flag{}; ///< Set when shutdown() has been requested.
}; // class Dmn_Kafka

} // namespace dmn

#endif // DMN_KAFKA_HPP_
