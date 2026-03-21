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
   * @brief The configuration key specific to the Dmn_Kafka module (not directly
   *        to rdkafka).
   */
  const static std::string Topic; // topic to read from or write to
  const static std::string Key;   // alternate topic name that overrides Topic
  const static std::string PollTimeoutMs; // timeout in ms for consumer poll to
                                          // break out

  using ConfigType = std::unordered_map<std::string, std::string>;

  using Dmn_Io<std::string>::write;

  enum class Role {
    kConsumer,
    kProducer,
  };

  /**
   * @brief The constructor method that creates kafka consumer or producer with
   *        the set of configurations passed in.
   *
   * @param role    The object created acts as a kafka consumer or producer
   * @param configs The set of key value configurations to rdkafka or Dmn_Kafka
   */
  Dmn_Kafka(Role role, ConfigType configs = {});
  ~Dmn_Kafka() noexcept;

  Dmn_Kafka(const Dmn_Kafka &obj) = delete;
  Dmn_Kafka &operator=(const Dmn_Kafka &obj) = delete;
  Dmn_Kafka(Dmn_Kafka &&obj) = delete;
  Dmn_Kafka &operator=(Dmn_Kafka &&obj) = delete;

  /**
   * @brief The method returns next topic' message that kafka consumer fetches
   *        from kafka broker, or nullptr if the PollTimeoutMs is elapsed
   *        without next message.
   *
   * @return nullptr if no message before PollTimeoutMs is elapsed, or next
   *         message.
   */
  auto read() -> std::optional<std::string> override;

  /**
   * @brief The method writes the message (of the topic) to Kafka broker.
   *
   * @param item The string to be written to the Kafka broker
   */
  void write(const std::string &item) override;

  /**
   * @brief The method writes the message (of the topic) to Kafka broker, this
   *        might move the string if implementation desires and supports it.
   *
   * @param item The string to be written to the Kafka broker
   */
  void write(std::string &&item) override;

  /**
   * @brief Shutdown the Kafka RAII and prevent it from further use to
   *        faciliate object teardown.
   */
  void shutdown() override;

private:
  static void cleanup_thunk_inflight(void *arg);

  virtual auto isInflightGuardClosed() -> bool override;

  /**
   * @brief The callback for rdkafka message specific error, so it is callback
   *        for producer.
   *
   * @param kafka_handle The kafka handler from kafka c++ module
   * @param rkmessage    The kafka message sent delivered by kafka producer
   * @param opaque       The pointer to Dmn_Kafka object
   */
  static void producerCallback(rd_kafka_t *kafka_handle,
                               const rd_kafka_message_t *rkmessage,
                               void *opaque);

  /**
   * @brief The callback invoked by kafka c++ module for generic rdkafka error
   *        (not message specific error) for producer.
   *
   * @param kafka_handle The kafka handler from kafka c++ module
   * @param err          The kafka error code related to deliverying the message
   * @param reason       The kafka error code explanation in string
   * @param opaque       The pointer to Dmn_Kafka object
   */
  static void errorCallback(rd_kafka_t *kafka_handle, int err,
                            const char *reason, void *opaque);

  /**
   * @brief The method publishes the data item to kafka broker, the data item
   *        is moved (if supported) if move is true, or copy otherwise.
   *
   * @param item The data item to be published
   * @param move The data item is moved (if support) and then published if true
   */
  void writeCopy(const std::string &item);

  /**
   * data members for constructor to instantiate the object.
   */
  Role m_role{};
  ConfigType m_configs{};

  /**
   * data members for internal logic.
   */
  std::string m_key{};
  std::string m_topic{};
  long long m_poll_timeout_ms{};

  rd_kafka_t *m_kafka{};
  rd_kafka_resp_err_t m_kafka_err{};

  std::atomic_flag m_write_atomic_flag{};

  std::atomic_flag m_shutdown_flag{};
}; // class Dmn_Kafka

} // namespace dmn

#endif // DMN_KAFKA_HPP_
