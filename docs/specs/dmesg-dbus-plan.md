# Implementation and Test Plan: DMesg over D-Bus

**Status:** Options A and B are implemented behind `ENABLE_DBUS` and have
focused private-bus test targets. Option A provides direct endpoint injection;
Option B is the composition facade. Neither is the proposed daemon RPC/client
proxy. The shared transport contract is in
[`dmesg-dbus-spec.md`](dmesg-dbus-spec.md); the options are detailed in
[`dmesgnet-dbus-injection-spec.md`](dmesgnet-dbus-injection-spec.md) and
[`dmesgnet-dbus-facade-spec.md`](dmesgnet-dbus-facade-spec.md).

Option A consists of `include/dmn-dbus-config.hpp`,
`include/dmn-dbus-io.hpp`, and `src/dmn-dbus-io.cpp`, built by the
optional `dmn-dbus` target. The checked-in `dmn-test-dbus-io` and
`dmn-test-dbus-facade` executables are registered through the existing
`ADD_TEST_EXECUTABLE(dmn ...)` mechanism. Each starts a private session daemon
for its session-bus clients, so CTest does not depend on the caller's session
bus; tests exercise the real libdbus path rather than a fake dispatch backend.

The proposed daemon-owned node with local D-Bus handler clients is a separate
architecture, not another mode of either option. Its Phase 1 DMesg observer
seam is implemented and covered by `dmn-test-dmesg-conflict`; the daemon
handler core, D-Bus RPC service, and client proxy remain future work described
in [`dmesg-dbus-local-conflict-spec.md`](dmesg-dbus-local-conflict-spec.md).
`Dmn_DMesg::openHandler()` completes publisher-side registration and waits for a
handler-context barrier behind initial-playback callbacks before returning.
The future `OpenHandler` service therefore must perform the blocking open on a
worker rather than the shared D-Bus dispatch thread. `ActivateHandler` remains
necessary so the client receives its handler ID before the daemon begins
delivering the staged callbacks.

## 1. Architectural contract

Phase 1 implemented Option A end-to-end: public byte-oriented D-Bus input/output
`Dmn_Io<std::string>` endpoints, direct construction of `Dmn_DMesgNet`, and
endpoint/private-bus/DMesgNet composition tests. After its exit criteria
passed, Phase 2 implemented Option B as a composition wrapper around the same
Option A endpoint classes and an internal `Dmn_DMesgNet`, forwarding only the
selected application DMesg API. Carry the
existing serialized `DMesgPb` in the body of the default D-Bus signal with
signature `ay`; the generic endpoints also accept a validated custom signal
tuple for other byte protocols. Keep separate input and output
connections/endpoints. V1 is
same-daemon, same-host IPC only; it does not replace network transports,
federate Linux buses, or provide consensus/reliable delivery.

Do not add protobuf fields or dependencies to core Dmn for the signal body.
Use libdbus public APIs behind a private implementation. Keep libdbus optional
at CMake configure time until the project approves dependency policy.

Both options must share one code path for signal codec, input/output endpoint,
limits, errors, and telemetry. The facade must not duplicate transport
implementation.

## 2. Checked-in test target and remaining validation

The `dmn-test-dbus-io` executable is registered through
`ADD_TEST_EXECUTABLE(dmn ...)` and uses the existing `dmn` CTest label; it
does not have a dedicated D-Bus label or a `dbus-run-session` command wrapper.
The executable starts a private session daemon for its session-bus clients. It
covers configuration/limit/address failures, exact byte and empty-payload delivery,
multiple subscribers, exact signal matching, malformed/oversized input,
input/output queue limits, unsupported `Dmn_Io` directions, input shutdown and
disconnect semantics, output failure state, and two-way `Dmn_DMesgNet`
application/lifecycle exchange.

Deterministic allocation-failure and retained-output-queue tests are in the
separate fault-injection executables described in Steps 2 and 3. Daemon
policy coverage includes a private-daemon denial of the client's AddMatch
method call; broader send/receive policy fixtures remain future
test-infrastructure work.
Private-bus tests must never connect to the host system bus.

## 3. Implementation steps with test-first exits

### Step 0 — Freeze shared contract and phase gates

Review the common spec and both alternatives. Agree that v1 is local-bus IPC;
remote hosts still require `Dmn_DMesgNet` over an actual network transport.
Confirm libdbus public API, session-bus default, optional build dependency,
error/telemetry contract, finite message/queue limits, and the public Option A
endpoint names/types. Confirm Option B is a composition wrapper—not an
inheritance facade—with its API limited to the agreed DMesg subset.

**Exit (passed):** No acceptance statement implies D-Bus signal delivery is
acknowledged, cross-host, or consensus-backed. The Option A phase gate and
Option B wrapper surface were agreed before Option B implementation.

### Step 1 — Implement and test signal encoding/decoding

Encoding and parsing are implemented within the endpoint implementation; no
separate codec API or target is exposed:

```text
path      /org/dmn/DMesg1
interface org.dmn.DMesg1.Transport
member    Message
signature ay
```

The endpoint codec accepts/returns byte sequences, not NUL-terminated strings.
It validates the payload limit before queue-copy allocation and rejects
configured limits that cannot fit libdbus's `int` array-length argument.

**Unit tests**

- Round-trip empty, ordinary, embedded-NUL, all-byte-values, and maximum-size
  payloads exactly.
- Reject maximum-size-plus-one before allocation/copy.
- Reject wrong path/interface/member/signature and extra arguments.
- Surface allocation/append/parse errors; never report success with a partial
  payload.
- Verify byte-array length, not C-string length, controls serialization and
  deserialization.

The private-bus tests exercise byte-array append/parse, empty and arbitrary
binary payloads, malformed signatures/extra arguments, and oversized input.
The core `dmn` target remains independent of libdbus.

### Step 2 — Implement the input-only `Dmn_Io<std::string>` endpoint

Create one private D-Bus connection; install the exact match rule and wait for
the bus daemon's AddMatch reply before reporting ready. Dispatch on one worker
thread. Its signal callback performs only validation, bounded queue insertion,
and counter/diagnostic updates; it never blocks on the Dmn consumer.
Initialize libdbus threading once before any D-Bus API use in the process.
For an explicit address, open a private connection and register it as a bus
connection; for the session bus, use the documented session-bus address. Do
not silently substitute another bus.

**Unit tests**

- `read()` blocks until enqueue and returns exact bytes.
- Direction misuse is explicit: input `write()` overloads and output `read()`
  throw `std::system_error(std::errc::operation_not_supported)`.
- Shutdown wakes a blocked reader, is idempotent, drains queued items, then
  throws `std::system_error(std::errc::operation_canceled)` on the next read.
  Verify the exception stops the existing `Dmn_DMesgNet` input worker without
  turning shutdown into a busy `std::nullopt` polling loop.
- If the bus disconnects with queued payloads, deliver queued payloads in
  order first, then throw the preserved mapped terminal connection error.
- Queue-full behavior drops the newest item, increments the loss counter, and
  does not block the dispatch callback.
- Byte-cap overflow is tested independently of message-count overflow; status
  reports each drop and queue memory remains within the configured bound.
- Oversized and malformed messages are rejected before queueing.
- Concurrent shutdown and signal delivery cannot access destroyed state or
  deadlock.
- A consumer draining immediately does not lose a signal because the match is
  not yet active.
- Shutdown is called repeatedly and during empty, queued, and active read
  states; each case wakes/finishes exactly once and does not leave a joinable
  worker or registered callback.
- Destroying each concrete endpoint without an earlier explicit shutdown
  still stops/joins its worker; test the derived destructor path rather than
  relying on `Dmn_Io<T>` base destruction.
- Invalid signal with extra args, wrong signature, wrong path, wrong
  interface/member, and non-signal message never enter the input queue.
- Callback allocation failure and full queue are visible and do not block
  dispatch; both increment status and produce rate-limited diagnostics.
- Bus disconnect is a terminal `std::system_error` with its connection error
  code, while explicit shutdown uses `std::errc::operation_canceled`; test
  these conditions separately.
- Zero limits, a queue byte cap smaller than the maximum payload, malformed
  explicit addresses, and failed bus setup are rejected explicitly before a
  usable endpoint/node is published.

**Current coverage:** The checked-in tests cover blocked-reader cancellation,
queued-input draining, bus disconnect, input count/byte overflow, malformed
and oversized signals, explicit-address errors, repeated shutdown, and a
concurrent shutdown/delivery race. With `ENABLE_FAULT_INJECTION=ON`, the
dedicated `dmn-test-fi-dbus-input-allocation` executable enables the private
`dmn/dbus/input/payload_allocation` point to exercise allocation-failure
status accounting. This test is labeled `fault-injection`; a separate
private-daemon policy fixture denies the client's AddMatch method call and
verifies that input construction fails with an explicit error and releases the
connection. This exercises match-setup failure handling, not general bus
policy correctness.

### Step 3 — Implement the output-only endpoint

Create a separate private connection and one output worker. `write()` applies
the size limit and enqueues into a finite queue; the worker serializes one
D-Bus signal per accepted write, preserving the exact byte range. Use
`std::system_error` for synchronous queue-full, oversize, shutdown, and
already-known failed-state errors. Worker-side errors update status and
produce a rate-limited diagnostic using the existing `Dmn_DMesgNet`
stderr-reporting convention. Do not silently discard accepted items.

**Unit tests**

- Every accepted write emits one exact signal payload; lvalue and rvalue
  overloads follow the documented ownership behavior.
- Embedded NUL is preserved.
- Oversize and full-queue writes throw; known failed/shutdown endpoints reject
  writes.
- Both the message-count and byte-count caps are tested; rejected writes do
  not increase pending queue depth or bytes.
- Worker-side connection/send failure marks the endpoint failed, increments
  status/error counts, reports a rate-limited diagnostic, and
  causes later writes to fail. No accepted queued item disappears silently.
- A stalled/non-writable output connection does not block `write()` or
  destruction; shutdown stops at its finite deadline and reports unsent
  application-queue count/bytes and remaining libdbus outgoing bytes in the
  final status snapshot and diagnostic.
- The output worker never calls blocking `dbus_connection_flush()`; a
  fault-injected retained-queue condition verifies bounded progress and
  shutdown behavior.
- Shutdown is idempotent and rejects writes after it is called.
- Destroying the output endpoint without an earlier explicit shutdown applies
  the finite drain policy, closes the connection, and joins the worker.

**Current coverage:** Tests cover exact accepted payloads, synchronous
oversize/queue-cap rejection, shutdown rejection, and output worker failure
after bus disconnect. With `ENABLE_FAULT_INJECTION=ON`, the dedicated
`dmn-test-fi-dbus-output-stall` executable exercises the private
`dmn/dbus/output/send_stall` point, holding accepted messages in the
application queue and verifying shutdown reaches its finite drain deadline
and reports unsent messages. The test is labeled `fault-injection` and runs
with `ctest --test-dir build -L fault-injection`.

### Step 4 — Option A: Inject endpoints into `Dmn_DMesgNet`

Construct distinct input and output endpoints and pass them to
`Dmn_DMesgNet(node_id, input, output)`. Keep the default DMesg signal
path/interface/member stable in v1; support a separate validated tuple for
unrelated byte protocols. Make bus address, signal tuple, maximum payload,
queue message-count, and queue-byte limits explicit configuration.

**Unit/lifecycle tests**

- A fake-I/O composition test proves that `Dmn_DMesgNet` uses only the
  existing input/output contract and records that input shutdown occurs before
  its final serialized `Destroyed` heartbeat write.
- Both D-Bus input and output endpoints are present and use separate
  connections.
- If a pair factory is exposed, it is all-or-nothing when input or output
  setup fails; any existing connection, match, and worker is released.
- Destruction unblocks/joins the input worker, then the base emits its final
  Destroyed heartbeat through the still-open output connection.
- Output is not accidentally shut down when the base shuts down input.
- Node IDs are required to be distinct among participants in one DMesg group.
- Caller may release its original `shared_ptr`s after construction; base
  ownership keeps adapters alive through base destruction.
- Input/output status counters and queue byte/message depths remain readable
  during active operation and after terminal failure. Input depths include
  only unread queued payloads; output depths exclude libdbus's internal queue.

**Exit:** No use-after-free, leaked D-Bus connection, or blocked destructor.

### Option A phase gate — Passed

Steps 1–6 and the documented endpoint/config/status API were completed, and
the default build without D-Bus was verified before beginning the facade work.

### Step 5 — Private-bus transport integration (Option A)

Run a private `dbus-daemon` in the test process or through an explicit private
address. Start at least two input subscribers and one output publisher.

**Integration tests**

- In a controlled no-fault local-bus test, each single publication is observed
  once by every matching subscriber after match setup/readiness. This validates
  the expected local path, not a general exactly-once guarantee.
- Nonmatching signal path/interface/member is not delivered to the adapter.
- A sender not authorized by the private bus policy cannot publish; test
  receiver policy using private-daemon policy/log evidence, not a claimed
  per-message error notification.
- A late subscriber receives no historical bus signal; document that Dmn
  playback, not D-Bus, may later provide last-value state.
- Daemon shutdown/disconnect wakes/errors readers and reports output worker
  failure through status/diagnostics; a timed-out writer shutdown reports
  undelivered queued items.
- Send-policy denial is surfaced as output failure. Receive-policy denial
  does not imply a per-message error notification.
- A private-daemon send-policy denial of the client's AddMatch method call
  fails input endpoint construction with an explicit error before a usable
  `Dmn_DMesgNet` instance is exposed.
- Signal flood reaches input queue cap; memory stays bounded, newest-message
  drop counter increments, and dispatch remains responsive.
- Slow or stalled bus output does not grow queues without bound; worker errors
  and undelivered queue depth are reported.
- Oversize input is discarded before queue allocation; oversize output fails
  before queue acceptance.
- Two independent private bus daemons do not exchange signals.
- A bus connection's unique name is not substituted for DMesg identity.

**Exit:** Tests use no external/system bus and are stable under repetitions.

### Step 6 — Compose with `Dmn_DMesgNet` (Option A)

Connect two `Dmn_DMesgNet` objects in separate processes or test fixtures
through directly injected Option A endpoint pairs. Use unique node IDs,
independent publishers, and deterministic synchronization; do not use
arbitrary sleeps as the only wait condition.

**Integration tests**

- Application DMesg published by A is delivered to B with topic/body preserved.
- A participant does not accept its own broadcast echo as a remote write.
- All participating adapters observe the expected sys heartbeat and Destroyed
  notification when a peer shuts down normally.
- Two simultaneous publishers do not share one transport connection or
  collide in source identity; expected DMesg conflict behavior is asserted.
- A late node's behavior is explicitly tested against current heartbeat and
  playback logic; do not infer D-Bus replay.
- Drop/overflow/disconnect scenarios do not claim delivery or commit.
- Existing socket/Kafka `Dmn_DMesgNet` tests remain unchanged.

**Exit:** Composition tests demonstrate same-host IPC only; no test implies
cross-host D-Bus federation or consensus.

**Option A completion gate:** Complete. Direct injection is usable without
the facade, private-bus tests use an isolated daemon, and the core build works
with the optional feature disabled.

### Step 7 — Option B: Implement the composition wrapper

**Status:** Implemented in `include/dmn-dmesgnet-dbus.hpp` and
`src/dmn-dmesgnet-dbus.cpp`. `Dmn_DMesgDbus` owns a private Option A endpoint
pair and a `Dmn_DMesgNet`; it forwards the documented handler/topic/conflict
operations and exposes endpoint status without exposing the node or endpoint
types. The optional target and private-bus test are controlled by
`ENABLE_DBUS`; the facade test is registered with the existing `dmn` test
macro.

**Facade-specific tests**

- Tests verify invalid tuple/address rejection; handler spec and factory
  forwarding, including factory exception propagation; async/filter handler
  delivery; close/proxy invalidation; topic lookup; conflict reset; and
  separate input/output status snapshots.
- Compile-time assertions verify the facade is non-copyable/non-movable and
  does not derive from `Dmn_DMesgNet`. The generated standalone-header target
  compiles the facade header without libdbus declarations.
- A private-bus test exchanges application messages in both directions
  between a facade node and an Option A directly injected participant.
- The facade status test stops its configured private bus and verifies the
  input and output status snapshots report their respective terminal errors.
- Member order keeps endpoint owners alive through node destruction, allowing
  input shutdown before the final best-effort output heartbeat.
- With `ENABLE_FAULT_INJECTION=ON`, the dedicated
  `dmn-test-fi-dbus-facade-rollback` test activates the private
  `dmn/dbus/facade/output_endpoint_creation` point, which throws after input
  setup; the test verifies RAII closes the input connection. The private
  FIU-guarded helper is named `createOutputForFaultInjection` to distinguish
  it from ordinary endpoint construction; the injection adds no facade API or
  endpoint factory.
- With `ENABLE_FAULT_INJECTION=ON`, the dedicated
  `dmn-test-fi-dbus-output-worker-start-failure` test fails startup inside the
  output endpoint after the input endpoint has started. It verifies the
  endpoint startup error is propagated and both connections are released.
- The regular D-Bus endpoint tests start a private daemon with a send policy
  denying the client's AddMatch method call. Input construction must report
  the AddMatch error and release its connection; this fixture does not certify
  broader bus policy behavior.
- The fake-I/O shutdown test waits until `Dmn_DMesgNet`'s input task is inside
  the blocking `read()` before destroying the node, then verifies the read
  exits after input shutdown and before the final heartbeat write.

**Exit:** The facade adds only construction, status, and the documented
forwarders; no D-Bus protocol or DMesg state logic is duplicated.

### Step 8 — Build, packaging, and security gate

`ENABLE_DBUS` (off by default), pkg-config discovery for `dbus-1`, and the
separate `dmn-dbus` target are implemented. The optional endpoint headers are
installed only when the feature is enabled. The private-bus test requires
`dbus-daemon`; system-bus deployment still requires a separately reviewed
least-privilege policy example, which this specification does not provide.

**Tests/review**

- Configure/build/test with `ENABLE_DBUS=OFF`.
- Configure/build/test with `ENABLE_DBUS=ON`.
- Enabling the private-bus test target requires `dbus-daemon` to be available.
- Missing libdbus with option off remains a successful core configuration;
  missing libdbus with option on fails with an explicit actionable message.
- The standalone-header check includes the D-Bus endpoint headers in either
  build mode because they expose no libdbus types; it must pass with the
  option both on and off. The optional implementation and private-bus test
  target are built only with `ENABLE_DBUS=ON`.
- Review bus match policy, local-user trust, signal visibility, queue flood
  limits, and source-identity assumptions.
- Add policy-fixture tests before treating bus authorization behavior as fully
  verified. Allocation failure and bounded stalled-output shutdown have
  deterministic fault-injection coverage when the optional injection feature
  is enabled.
- Run relevant CTest groups, repeated private-bus tests, and sanitizers or
  Valgrind when available.

**Exit:** Both supported build modes work and the security review approves the
actual deployment policy.

## 4. Option comparison and implementation order

| Criterion | Option A: direct I/O injection | Option B: `Dmn_DMesgDbus` wrapper |
|---|---|---|
| Transport implementation | Shared D-Bus endpoints | Same Option A endpoint classes |
| Changes to `Dmn_DMesgNet` | None | None; wrapper owns a private instance |
| User setup | Caller constructs input/output adapters and injects them | Caller supplies node ID/config; wrapper creates adapters |
| DMesg API | Full `Dmn_DMesgNet`/inherited API | Explicitly forwarded DMesg subset; no exposed node instance |
| Transport visibility | Endpoint types/config/status are public | Endpoint integration details are hidden behind the wrapper |
| Extensibility | Custom `Dmn_Io` implementations and configurations | Convenient supported D-Bus default |
| Lifecycle ownership | Caller owns endpoint refs; node shares them | Wrapper owns endpoints and internal node in defined destruction order |
| Error visibility | Query each endpoint status | Wrapper provides both endpoint snapshots |
| Testability | Codec/endpoints/injected node composition | Additional forwarding, API-boundary, rollback, and lifetime tests |
| ABI/build surface | Public endpoint types and optional D-Bus target | Additional public wrapper/config/status header on same target |
| Performance | Shared endpoint path | Same path; wrapper adds only method forwarding |
| Implementation order | Phase 1; completion gate | Phase 2, strictly after Option A tests pass |

Both implemented options remain supported as different construction
boundaries. Option A provides direct endpoint injection; Option B owns those
same endpoint types and an internal `Dmn_DMesgNet`. They share the signal and
endpoint implementation and do not provide a daemon-owned shared publisher
with remote local-client proxies. That RPC/client-proxy architecture is
separate deferred work; do not extend the Option B facade to imply that role.

## 5. Explicitly deferred work

- Multi-host D-Bus federation or use of a network-exposed central daemon.
- Bridging host-local D-Bus traffic into a separate `Dmn_DMesgNet` network
  instance.
- A per-host daemon exposing its single network-facing `Dmn_DMesgNet` node to
  local applications through a distinct D-Bus RPC/client-proxy protocol. This
  architecture and its first two-client conflict milestone are proposed
  separately in
  [`dmesgnet-dbus-node-gateway-spec.md`](dmesgnet-dbus-node-gateway-spec.md)
  and [`dmesg-dbus-local-conflict-spec.md`](dmesg-dbus-local-conflict-spec.md).
  The DMesg observer foundation is implemented, but the gateway is not part of
  the implemented Option A/B signal transport.
- Cross-host service names, method-call proxying, remote credentials, Unix FD
  forwarding, activation, signal subscription federation, or bus policy
  replication.
- Consensus, quorum commit, durable message log, exactly-once method execution,
  or failover.

Each deferred feature needs its own threat model, wire/API contract, failure
semantics, and tests before implementation.
