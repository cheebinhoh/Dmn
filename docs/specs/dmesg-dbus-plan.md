# Implementation and Test Plan: DMesg over D-Bus

**Status:** Option A is implemented with a focused private-bus test target.
Option B remains unimplemented and must not start until Option A's build/test
gate is complete. The shared transport contract is in
[`dmesg-dbus-spec.md`](dmesg-dbus-spec.md); the options are detailed in
[`dmesgnet-dbus-injection-spec.md`](dmesgnet-dbus-injection-spec.md) and
[`dmesgnet-dbus-facade-spec.md`](dmesgnet-dbus-facade-spec.md).

Option A consists of `include/dmn-dbus-config.hpp`,
`include/dmn-dbus-io.hpp`, and `src/dmn-dbus-io.cpp`, built by the
optional `dmn-dbus` target. The checked-in
`test/dmn-test-dbus-io.cpp` runs under `dbus-run-session` and includes
real private-bus endpoint, disconnect, queue-limit, and `Dmn_DMesgNet`
composition cases. It validates behavior against libdbus rather than a fake
dispatch backend.

## 1. Architectural contract

Phase 1 implements Option A end-to-end: public byte-oriented D-Bus input/output
`Dmn_Io<std::string>` endpoints, direct construction of `Dmn_DMesgNet`, and
all endpoint/private-bus/DMesgNet composition tests. Phase 2 implements Option
B as a composition wrapper around the same Option A endpoint classes and an
internal `Dmn_DMesgNet`, forwarding only the selected application DMesg API.
Do not start phase 2 until every phase 1 exit criterion passes. Carry the
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

The current `dmn-test-dbus-io` executable is registered with the CTest
label `dbus` and launched inside `dbus-run-session`. It covers
configuration/address failures, exact byte and empty-payload delivery,
multiple subscribers, exact signal matching, malformed/oversized input,
input/output queue limits, unsupported `Dmn_Io` directions, input shutdown and
disconnect semantics, output failure state, and two-way `Dmn_DMesgNet`
application/lifecycle exchange.

Daemon send/receive policy fixtures, deterministic allocator-failure
injection, and a fake backend for forcing an output connection to stall
through the shutdown deadline remain separate test-infrastructure work.
Private-bus tests must never connect to the host system bus.

## 3. Implementation steps with test-first exits

### Step 0 — Freeze shared contract and phase gates

Review the common spec and both alternatives. Agree that v1 is local-bus IPC;
remote hosts still require `Dmn_DMesgNet` over an actual network transport.
Confirm libdbus public API, session-bus default, optional build dependency,
error/telemetry contract, finite message/queue limits, and the public Option A
endpoint names/types. Confirm Option B is a composition wrapper—not an
inheritance facade—with its API limited to the agreed DMesg subset.

**Exit:** No acceptance statement implies D-Bus signal delivery is acknowledged,
cross-host, or consensus-backed. The Option A phase gate and Option B wrapper
surface are agreed; phase 2 cannot begin before phase 1 passes.

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
concurrent shutdown/delivery race. Allocation-failure injection and a
separate AddMatch-denial policy fixture remain untested.

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
- The output worker never calls blocking `dbus_connection_flush()`; a fake
  read/write-dispatch backend verifies bounded progress and shutdown behavior.
- Shutdown is idempotent and rejects writes after it is called.
- Destroying the output endpoint without an earlier explicit shutdown applies
  the finite drain policy, closes the connection, and joins the worker.

**Current coverage:** Tests cover exact accepted payloads, synchronous
oversize/queue-cap rejection, shutdown rejection, and output worker failure
after bus disconnect. A deterministic stalled-writer/deadline test still
needs an internal test backend.

### Step 4 — Option A: Inject endpoints into `Dmn_DMesgNet`

Construct distinct input and output endpoints and pass them to
`Dmn_DMesgNet(node_id, input, output)`. Keep the default DMesg signal
path/interface/member stable in v1; support a separate validated tuple for
unrelated byte protocols. Make bus address, signal tuple, maximum payload,
queue message-count, and queue-byte limits explicit configuration.

**Unit/lifecycle tests**

- A fake-I/O composition test proves that `Dmn_DMesgNet` uses only the
  existing input/output contract.
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
  during active operation and after terminal failure.

**Exit:** No use-after-free, leaked D-Bus connection, or blocked destructor.

### Option A phase gate — Required before Option B

Do not begin facade implementation until Steps 1–6 pass, Option A's public
endpoint/config/status API is documented, and the default build without D-Bus
remains unaffected. Failures or unresolved endpoint/API behavior return to
Option A work; do not paper over them with wrapper-specific behavior.

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
- Match setup denial fails input endpoint construction before a usable
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

**Option A completion gate:** Steps 1–6 pass, public direct injection is
usable without the facade, all required private-bus tests use an isolated
daemon, and no regression is introduced when the optional feature is off.
Before starting Option B, configure/build/test with `ENABLE_DBUS=OFF`, then
configure/build Option A and run its focused unit, private-bus, and
`Dmn_DMesgNet` composition test targets with `ENABLE_DBUS=ON`.

### Step 7 — Option B: Implement the composition wrapper

Only after the Option A completion gate passes, implement `Dmn_DMesgDbus` as
specified in `dmesgnet-dbus-facade-spec.md`. It owns an endpoint pair and a
private `Dmn_DMesgNet` member; it must not derive from the node class. Forward
the specified DMesg handler/topic/conflict operations and expose read-only
input/output status. Keep endpoint types private in the facade's API while
retaining their independent public Option A API.

**Facade-specific tests**

- Wrapper construction creates the same Option A endpoint types and applies
  the exact shared config; default and explicit addresses never silently
  fall back.
- Invalid config and failures while constructing input, match, or output
  throw and release every resource acquired earlier.
- Configured maximum payload, queue-count, and queue-byte limits reach both
  endpoints unchanged.
- `openHandler(HandlerSpec)` preserves all spec fields and handler
  read/write/conflict behavior. Test `openHandlerWithFactory` delegation and
  exception propagation, `closeHandler` proxy invalidation, topic lookup
  before/after publication, and conflict reset.
- `Dmn_DMesgDbus` is not derived from `Dmn_DMesgNet`; no public method accepts
  `Dmn_Io<std::string>` or returns the internal node/endpoints.
- A compile-only API test verifies supported aliases/forwarders, confirms the
  facade header exposes no endpoint declarations, and checks the wrapper is
  not a `Dmn_DMesgNet` base or constructible with injected endpoints.
- Endpoint owners outlive the internal node in the `Impl`; destruction shuts
  input down, sends the best-effort final heartbeat through output, then
  releases the endpoints. A private, test-only endpoint factory seam verifies
  rollback and destruction ordering without adding public test hooks.
- A facade participant exchanges messages with an Option A directly injected
  participant over one private bus, proving both use the same wire path.
- Copy/move is disabled unless separately designed; verify that rule at
  compile time.

**Exit:** The facade adds only construction, configuration, status, and the
explicitly listed forwarders; no D-Bus protocol or DMesg state logic is
duplicated.

### Step 8 — Build, packaging, and security gate

`ENABLE_DBUS` (off by default), pkg-config discovery for `dbus-1`, and the
separate `dmn-dbus` target are implemented. The optional endpoint headers are
installed only when the feature is enabled. Document session-bus invocation
and provide a least-privilege policy example before system-bus deployment.

**Tests/review**

- Configure/build/test with `ENABLE_DBUS=OFF`.
- Configure/build/test with `ENABLE_DBUS=ON`.
- Missing libdbus with option off remains a successful core configuration;
  missing libdbus with option on fails with an explicit actionable message.
- Core standalone-header check passes with the option off; optional D-Bus
  endpoint/facade headers have their own enabled-build check.
- Review bus match policy, local-user trust, signal visibility, queue flood
  limits, and source-identity assumptions.
- Add policy-fixture tests and deterministic allocation/stalled-writer failure
  injection before treating those behaviors as fully verified.
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

Both options are required in the requested delivery. Implement the codec,
endpoints, public injection API, private-bus behavior, and direct
`Dmn_DMesgNet` composition under Option A first. Once that is verified,
implement the Option B wrapper and its specific tests on the exact same
endpoint path. Do not create two wire or endpoint implementations.

## 5. Explicitly deferred work

- Multi-host D-Bus federation or use of a network-exposed central daemon.
- Bridging host-local D-Bus traffic into a separate `Dmn_DMesgNet` network
  instance.
- Cross-host service names, method-call proxying, remote credentials, Unix FD
  forwarding, activation, signal subscription federation, or bus policy
  replication.
- Consensus, quorum commit, durable message log, exactly-once method execution,
  or failover.

Each deferred feature needs its own threat model, wire/API contract, failure
semantics, and tests before implementation.
