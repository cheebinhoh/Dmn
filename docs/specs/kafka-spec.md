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
5. Kafka tests are behind `BUILD_KAFKA_TEST` and require a broker. The
   bootstrap helper now provides a local broker; keep those integration tests
   separate from deterministic broker-independent tests.
6. The top-level build currently includes Kafka implementation sources in
   `dmn`, always discovers/fetches librdkafka, and links it even when Kafka
   integration tests are disabled. If feature-off builds are intended to omit
   Kafka and its dependency, split Kafka into a separately gated target (for
   example, `dmn-kafka`) and define compatibility/install rules for
   `Dmn_DMesgNet_Kafka`. This is a build/API architecture change and should be
   planned as a distinct feature; no such target exists today.

## Local integration-test broker

When `BUILD_KAFKA_TEST=ON`, CMake copies
`scripts/run-bookstrap-dmn.sh` to `build/scripts/` and installs it at
`<prefix>/scripts/`. Run it from either location to start a local broker and
create the test topic:

```sh
./build/scripts/run-bookstrap-dmn.sh
ctest --test-dir build -L kafka --output-on-failure
./build/scripts/run-bookstrap-dmn.sh stop
```

The script looks for an existing distribution in the build/install tree or
`DMN_KAFKA_HOME`, then downloads the pinned Apache Kafka archive and verifies
its published SHA-512 checksum. A writable build/install prefix stores the
distribution and broker state locally; a non-writable install prefix uses the
user's XDG cache/state directories. It does not require a globally installed
Kafka package or Docker. A Java runtime is still required; set `JAVA_HOME` if
it is not available on `PATH`. `curl`, `tar`, `sha512sum`, and `ps` are also
required for fetching, unpacking, checksum verification, and safe process
shutdown. Native runs bind to loopback by default; set
`DMN_KAFKA_LISTENERS` and `DMN_KAFKA_ADVERTISED_LISTENERS` to override the
Kafka listener configuration. The Docker test image uses the same script but
binds to all container interfaces and advertises its container hostname.
