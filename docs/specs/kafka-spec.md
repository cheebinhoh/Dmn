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

## `Dmn_DMesgNet_Kafka`

The adapter creates one consumer and producer and owns a `Dmn_DMesgNet` using
them as input and output. It fixes the transport topic to `Dmn_dmesgnet`, uses
the supplied name as consumer group id, sets consumer reset to earliest and
producer acks to all, and forwards handler open/close methods. The destructor
relies on member destruction to release the underlying network node and Kafka
clients.

## Gaps / improvements

1. The subscription failure path destroys the topic list before reading its
   `cnt` field to format the error, causing a use-after-free. Capture the count
   before destroying the list, then report the subscription failure.
2. Correct the reserved `Key` parsing: current construction assigns it to the
   topic member while write uses a separate `m_key` that is never populated.
   Cover explicit topic/key combinations and defaults in tests.
3. Parse `PollTimeoutMs` strictly, reject invalid/negative/out-of-range values,
   and choose/document a default. Validate required topic and role-specific
   settings before creating a librdkafka handle.
4. `Dmn_Kafka::read()` uses `nullopt` for both ordinary poll timeout and
   shutdown/error outcomes. Add a distinct status/error channel if callers
   need to distinguish a healthy idle consumer from failure.
5. Producer callback and generic error callback share one error/flag channel.
   Define delivery correlation and behavior for asynchronous errors not
   associated with the currently blocked write; test broker loss and shutdown.
6. Make flush/poll deadlines configurable and avoid repeated one-second flush
   calls while waiting for each synchronous delivery.
7. Remove credentials from example source; use runtime configuration and
   ensure test/example credentials are never committed. Rotate any credentials
   that may have been active.
8. Kafka tests are behind `BUILD_KAFKA_TEST`; define a deterministic mock or
   local-broker test path and keep external broker integration tests separate.
