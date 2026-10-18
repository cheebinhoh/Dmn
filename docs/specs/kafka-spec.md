# Kafka transport

**Status:** Code-derived specification for `Dmn_Kafka`, its configuration
helper, and the `Dmn_DMesgNet_Kafka` composition adapter.

## `Dmn_Kafka`

`Dmn_Kafka` implements `Dmn_Io<std::string>` in either producer or consumer
role. Constructor configuration is a string map. Library keys identify the
topic, message key, and consumer poll timeout; other keys are passed to
librdkafka. A consumer subscribes to the configured topic and `read()` polls
for at most the configured milliseconds. No message, partition EOF, shutdown,
or a consumed error can result in `nullopt`; non-EOF errors are logged.

Producer `write()` serializes concurrent writers with an atomic flag, submits a
copy of the payload, polls for the delivery callback, and throws on produce or
delivery error. Shutdown closes admission and waits for in-flight calls. The
destructor closes the consumer or flushes the producer, then destroys the
librdkafka handle. One reader thread per consumer instance is the documented
usage.

`set_config()` wraps `rd_kafka_conf_set()` and returns `std::expected` with the
librdkafka error string on failure. Its string-view inputs are passed as C
strings; callers must supply null-terminated data unless the helper copies it.
Configuration errors identify the rejected property but do not echo its
configured value, which may contain credentials.

## `Dmn_DMesgNet_Kafka`

The adapter creates one consumer and producer and owns a `Dmn_DMesgNet` using
them as input and output. It fixes the transport topic to `Dmn_dmesgnet`, uses
the supplied name as consumer group id, sets consumer reset to earliest and
producer acks to all, and forwards handler open/close methods. The destructor
relies on member destruction to release the underlying network node and Kafka
clients.

## Resolved review findings

The constructor now captures the subscription topic count before freeing the
list, parses `Key` as the producer message key independently of `Topic`, and
does not include configuration values in error messages. The sender/receiver
examples now require `DMN_KAFKA_BOOTSTRAP_SERVERS`,
`DMN_KAFKA_SASL_USERNAME`, and `DMN_KAFKA_SASL_PASSWORD` from the environment;
the values are not stored in source. Previously committed credentials must be
revoked or rotated if they were ever valid because removing source literals
does not remove them from Git history.

## Gaps / improvements

1. Parse `PollTimeoutMs` strictly, reject invalid/negative/out-of-range values,
   and choose/document a default. Validate required topic and role-specific
   settings before creating a librdkafka handle.
2. `Dmn_Kafka::read()` uses `nullopt` for both ordinary poll timeout and
   shutdown/error outcomes. Add a distinct status/error channel if callers
   need to distinguish a healthy idle consumer from failure.
3. Producer callback and generic error callback share one error/flag channel.
   Define delivery correlation and behavior for asynchronous errors not
   associated with the currently blocked write; test broker loss and shutdown.
4. Make flush/poll deadlines configurable and avoid repeated one-second flush
   calls while waiting for each synchronous delivery.
5. Kafka tests are behind `BUILD_KAFKA_TEST`; define a deterministic mock or
   local-broker test path and keep external broker integration tests separate.
6. The top-level build currently includes Kafka implementation sources in
   `dmn`, always discovers/fetches librdkafka, and links it even when Kafka
   integration tests are disabled. If feature-off builds are intended to omit
   Kafka and its dependency, split Kafka into a separately gated target (for
   example, `dmn-kafka`) and define compatibility/install rules for
   `Dmn_DMesgNet_Kafka`. This is a build/API architecture change and should be
   planned as a distinct feature; no such target exists today.
