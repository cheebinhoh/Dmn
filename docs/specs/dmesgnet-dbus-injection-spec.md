# Option A: Inject D-Bus I/O into `Dmn_DMesgNet`

**Status:** Option A is implemented behind the optional `ENABLE_DBUS` build
flag and covered by private-session-bus endpoint and `Dmn_DMesgNet`
composition tests. Option B is implemented as the composition facade specified
in [`dmesgnet-dbus-facade-spec.md`](dmesgnet-dbus-facade-spec.md). Both use the
shared transport contract in [`dmesg-dbus-spec.md`](dmesg-dbus-spec.md).

## 1. Alternative under review

Create public D-Bus-backed `Dmn_Io<std::string>` input and output endpoints
and pass one of each directly to the existing `Dmn_DMesgNet` constructor:

```cpp
auto input = std::make_shared<dmn::Dmn_DbusInput>(config);
auto output = std::make_shared<dmn::Dmn_DbusOutput>(config);

dmn::Dmn_DMesgNet dmesgnet{"node-a", input, output};
```

`Dmn_DbusInput` and `Dmn_DbusOutput` are the implemented public names and
both implement `Dmn_Io<std::string>`. Keep the endpoints and their
private D-Bus connections distinct. A public pair factory may be provided to
make same-config construction and rollback convenient, but direct
construction of each endpoint must remain possible for Option A to be a real
injection API. The endpoints carry arbitrary bytes in a signal with a
configurable path/interface/member and fixed `ay` signature. Their default
tuple is the DMesgNet wire contract in `dmesg-dbus-spec.md`; leave those
defaults unchanged when composing an unmodified `Dmn_DMesgNet`.

The transport stack becomes:

```text
DMesg handler
  -> Dmn_DMesgNet
       -> output Dmn_Io<string>
            -> D-Bus signal

D-Bus signal
  -> input Dmn_Io<string>
       -> Dmn_DMesgNet input worker
            -> local DMesg handler
```

`Dmn_DMesgNet` already accepts independently supplied input and output
`Dmn_Io<std::string>` shared pointers. The injection path therefore needs no
changes to DMesg semantics, `DMesgPb`, or the `Dmn_DMesgNet` constructor.
`Dmn_Socket` is one concrete adapter choice, not an internal dependency of
`Dmn_DMesgNet`; replacing it with another `Dmn_Io` transport is the designed
extension point.

## 2. Feasibility assessment

**Technical feasibility: high.** The constructor types match the proposed
input and output endpoints exactly. The private-bus prototype established
that libdbus can broadcast an `ay` signal and preserve arbitrary serialized
bytes. The adapter can be developed/tested without changing DMesgNet code.

**Semantic feasibility: high for local IPC, not a network replacement.**
The existing DMesg protobuf, publisher counters, handler API, heartbeat
reconciliation, and playback continue to run inside `Dmn_DMesgNet`. The
transport provides same-daemon signal delivery. It does not carry messages
between separate per-host buses. If remote nodes are required, retain an
actual network transport (or separately specify a local-D-Bus-to-network
bridge).

**Reliability feasibility: bounded.** D-Bus signal send has no per-receiver
acknowledgement, durable queue, or retry contract visible through
`Dmn_Io::write()`. This is materially different from any transport that
provides stronger delivery semantics. The Dmn protocol's conflict/playback
logic does not turn signal transport into reliable delivery or consensus.

**Security feasibility: acceptable for same-user/session-bus cooperation; not
an identity boundary by itself.** The bus authenticates local connections and
applies match/send policy, but the `Dmn_Io<std::string>` API strips bus sender
metadata. `DMesgPb.sourceIdentifier` is payload data and can be forged by any
sender authorized to emit the signal. This option assumes trusted cooperating
processes. A deployment requiring authenticated per-node identity needs an
adapter API that carries authenticated peer metadata or a reviewed separate
control/authorization layer; do not infer identity from the message payload.

**Implementation risk: low-to-moderate.** The main risks are shutdown ordering,
libdbus dispatch threading, bounded backpressure, and D-Bus bus policy. One
important integration detail is favorable: `Dmn_DMesgNet` shuts down its
input adapter before its destructor emits the final `Destroyed` heartbeat on
the output adapter. Distinct objects let input shutdown occur without closing
the output connection prematurely.

### Assumptions and unresolved validation

- The input callback only copies byte arrays after checking argument type and
  size; D-Bus sender metadata is not represented in `Dmn_Io<std::string>`.
- One private connection owns input dispatch and its match; another owns
  output. This avoids concurrent dispatch and input shutdown disrupting
  output.
- `Dmn_DMesgNet` writes from its publisher/async callback path. Calling
  libdbus `flush()` from that path could block until the entire outgoing queue
  is written and stall Dmn's shared async context. The adapter therefore uses
  a bounded writer queue and finite-timeout read/write/dispatch handling or
  non-blocking watches; neither
  `write()` nor endpoint destruction may call a potentially unbounded flush.
- Output queue acceptance is not daemon delivery. A later worker send failure
  must increment an observable error counter and produce a rate-limited
  diagnostic using the existing stderr-reporting convention; it cannot
  retroactively throw from `write()`.
  Queue-full and already-known-failed states must throw synchronously from
  `write()`.
- The current Dmn `Dmn_Io` API does not provide generic asynchronous error
  reporting or metrics. Therefore the public Option A endpoint types expose a
  thread-safe, read-only `Dmn_DbusIoStatus` snapshot and use the existing
  stderr-reporting convention for asynchronous errors. Do not add a user
  callback API in v1 or silently discard asynchronous output failures.

## 3. Semantics retained and semantics changed

### Retained from `Dmn_DMesgNet`

- The `Dmn_DMesgNet` instance name is still the local DMesg node identifier.
- Heartbeat construction/reconciliation, local master-selection heuristic,
  ready-state behavior, and shutdown heartbeat are unchanged.
- DMesg topic/body/counter semantics and the existing serialized `DMesgPb`
  envelope are unchanged.
- The network adapter's source-write-handler marker still prevents a node
  from re-forwarding its own received messages.
- Applications continue using `Dmn_DMesg` handler APIs.

### Changed by the selected transport

- The peer population is limited to clients of the same D-Bus daemon and
  permitted by its policy.
- Signals are broadcast to matching subscribers rather than sent to an
  identified remote host.
- There is no transport-level acknowledgement or durable delivery.
- The D-Bus daemon becomes a local IPC dependency and failure domain.
- D-Bus daemon policy, connection limits, and bus availability affect DMesg
  transport availability.
- Bus sender credentials are discarded by the `Dmn_Io<std::string>` contract;
  they are not exposed to DMesgNet.
- If the output endpoint uses an internal writer queue, `write()` success means
  that the adapter queue accepted the item, not that libdbus queued it or the
  daemon sent it. Queue bounds and asynchronous failures are part of the
  adapter contract.

“Keep the semantics” therefore means preserve the DMesgNet protocol and
application-facing handler semantics, not preserve the socket's network
topology, delivery behavior, or failure domain.

## 4. Required adapter contracts

### 4.0 Public Option A surface

Option A is the supported expert/composition API and therefore exposes the
input endpoint, output endpoint, shared config, and read-only endpoint status
types in the optional D-Bus public headers. Do not expose `DBusConnection *`,
libdbus message objects, or callback/filter registration that can bypass the
configured byte-signal contract.

The shared config/status types are in `include/dmn-dbus-config.hpp`; endpoint
classes are in `include/dmn-dbus-io.hpp`:

```cpp
struct Dmn_DbusConfig {
  // Empty selects the session bus for an unprivileged process.
  std::string bus_address;
  // Defaults identify the Dmn_DMesgNet wire signal; choose a separate tuple
  // for another byte-oriented protocol.
  std::string signal_path{"/org/dmn/DMesg1"};
  std::string signal_interface{"org.dmn.DMesg1.Transport"};
  std::string signal_member{"Message"};
  std::size_t max_message_bytes{1024 * 1024};
  std::size_t max_queued_messages{1024};
  std::size_t max_queued_bytes{16 * 1024 * 1024};
};
```

Implemented public headers are `dmn-dbus-config.hpp` and
`dmn-dbus-io.hpp`; the out-of-line implementation is in
`src/dmn-dbus-io.cpp`, built as the separate `dmn-dbus` library target.
`ENABLE_DBUS` defaults to `OFF`, so the core `dmn` target remains independent
of libdbus. Each configured maximum payload must be positive and no greater
than `INT_MAX`, because libdbus's fixed-array API accepts an `int` length.

`Dmn_DbusIoStatus` is defined in that shared config header with the status
fields specified in `dmesg-dbus-spec.md`. The signal tuple is validated as
D-Bus path/interface/member syntax before opening a connection.

```cpp
class Dmn_DbusInput : public Dmn_Io<std::string> {
public:
  explicit Dmn_DbusInput(const Dmn_DbusConfig &config);
  ~Dmn_DbusInput() noexcept override;
  auto read() -> std::optional<std::string> override;
  void write(const std::string &item) override;
  void write(std::string &&item) override;
  void shutdown() noexcept override;
  [[nodiscard]] auto status() const -> Dmn_DbusIoStatus;
};

class Dmn_DbusOutput : public Dmn_Io<std::string> {
public:
  explicit Dmn_DbusOutput(const Dmn_DbusConfig &config);
  ~Dmn_DbusOutput() noexcept override;
  auto read() -> std::optional<std::string> override;
  void write(const std::string &item) override;
  void write(std::string &&item) override;
  void shutdown() noexcept override;
  [[nodiscard]] auto status() const -> Dmn_DbusIoStatus;
};
```

The concrete endpoints are non-copyable and non-movable because they own
connections and worker lifetimes. Each concrete destructor must call its
idempotent `shutdown()` while its dynamic type is still active; relying on
`Dmn_Io<T>`'s base destructor is insufficient because virtual dispatch during
base destruction does not invoke the derived shutdown override.
`Dmn_DbusConfig` and
`Dmn_DbusIoStatus` are shared by Options A and B, declared once in the
optional public config header, and included by both API headers. The facade
header does not include the endpoint header.
All configured size/count limits must be positive, and
`max_queued_bytes >= max_message_bytes` so any individually valid maximum-size
payload can fit in an empty queue. Invalid limits throw `std::invalid_argument`
before opening connections. An explicit address failure is reported directly;
never fall back to the session/system bus.
Because `Dmn_Io<T>` requires both directions, input `write()` overloads and
output `read()` must throw `std::system_error` with
`std::errc::operation_not_supported`; never silently discard a call to an
unsupported direction.
`shutdown()` is `noexcept`, idempotent, and records/report errors through the
status and diagnostic contract rather than throwing during teardown.

The implemented direct-construction pattern is:

```cpp
Dmn_DbusConfig config{/* bus and limits */};
auto input = std::make_shared<Dmn_DbusInput>(config);
auto output = std::make_shared<Dmn_DbusOutput>(config);
Dmn_DMesgNet node{"node-a", input, output};
```

The current API has no endpoint-pair factory: callers construct the two
strongly typed endpoints directly. They retain independent connections even
when they share the same bus address and limits. The facade also constructs
these endpoint classes directly in its private implementation; it has no
endpoint-factory seam or second adapter path.

### 4.1 Input endpoint

`read()` blocks on a finite queue populated by one D-Bus dispatch worker.
Dispatch matches exactly the configured signal tuple and `ay` signature. For
the default DMesgNet configuration this is:

```text
path      /org/dmn/DMesg1
interface org.dmn.DMesg1.Transport
member    Message
signature ay
```

Install the exact match and wait for the daemon's `AddMatch` reply before
publishing a fully constructed `Dmn_DMesgNet` to application code. The
callback copies a bounded payload and returns; it does not block on the DMesg
reader or call `Dmn_DMesgNet` synchronously. On queue overflow, drop the
newest signal and increment an observable counter; do not block the bus
dispatch thread.

Reject malformed signal arguments and payloads exceeding the configured
maximum before copying them into the queue. Check both queue caps before
allocating the payload copy. `shutdown()` wakes a blocked `read()`, stops
dispatch, removes the local filter after joining the worker, and closes the
private connection (which releases its daemon-side match without a separate
blocking `RemoveMatch` call). It is safe to call more than once. Preserve
already queued input for
the blocked reader to drain; after the queue empties, throw
`std::system_error(std::errc::operation_canceled)` rather than returning
`std::nullopt`. This is important because current `Dmn_DMesgNet` treats
`std::nullopt` as a temporary empty read (Kafka uses it for polling timeout)
and would otherwise continue its input loop.

### 4.2 Output endpoint

Each `write()` accepts one item into a finite output queue; one writer worker
creates one broadcast signal on the configured tuple whose `ay` argument is
byte-for-byte equal to the string. Embedded NUL bytes are ordinary payload
bytes. Enforce the payload,
message-count, and queued-byte limits before queueing. If any queue cap is
reached, shutdown has begun, or the worker has entered a known failed state,
throw `std::system_error`. `write()` success means only that the adapter's
local queue accepted the item. It does not guarantee libdbus accepted it, the
daemon sent it, or any peer received it.

`dbus_connection_send()` only adds a message to libdbus's outgoing queue; it
does not write that message to the bus. Drive output using a bounded
read/write/dispatch loop (such as `dbus_connection_read_write_dispatch()`
with a finite timeout) or non-blocking watches; do not use
`dbus_connection_flush()`, which blocks until the outgoing queue is empty.
Bound the adapter queue by both count and bytes, and stop submitting to
libdbus when its outgoing byte count reaches the configured cap. Surface
connection/send errors through the status snapshot and a rate-limited
diagnostic. On shutdown, attempt to submit adapter-queued output until the
finite deadline; report remaining adapter-queue count/bytes and pending
libdbus bytes before closing. Closing may discard messages already queued
inside libdbus. The `Dmn_DMesgNet` destructor is `noexcept`, so its final
heartbeat is best effort and cannot be represented as successful delivery.

Use a separate connection and object from the input endpoint. `shutdown()` is
idempotent. Keep the output endpoint alive for the full `Dmn_DMesgNet`
destructor so its final system-state message can be sent.

### 4.3 Node identity and echo

Each `Dmn_DMesgNet` instance sharing the same signal tuple must have a
unique configured name. Its incoming path already ignores messages whose
`sourceWriteHandlerIdentifier` equals its own name; preserve and test this
loop-prevention behavior. Do not replace that field with the D-Bus sender
unique name. The latter is connection-scoped and not a Dmn node identity.

Unique node names prevent ordinary self-filter collisions; they do not prevent
another bus client from forging the source-write-handler field. Signal senders
must be trusted under the v1 security model.

## 5. Composition lifecycle and constructor concerns

Direct composition makes construction resource order visible:

```cpp
auto input = std::make_shared<dmn::Dmn_DbusInput>(config);
auto output = std::make_shared<dmn::Dmn_DbusOutput>(config);
dmn::Dmn_DMesgNet node{"node-a", input, output};
```

Do not expose `node` before both adapter constructors and match installation
have succeeded. If output construction fails after input succeeds, normal
RAII destroys input and leaves no active subscriber behind. A typed pair
helper was considered as an optional future convenience:

```cpp
auto endpoints = dmn::makeDmnDbusIoPair(config);
dmn::Dmn_DMesgNet node{"node-a", endpoints.input, endpoints.output};
```

No pair helper is currently implemented or required; direct endpoint
construction is the supported API. If a pair helper is added later, document
its connection ownership and explicit `shutdown()` behavior. Do not expose raw
`DBusConnection *`.

Because `Dmn_DMesgNet` owns copies of the adapter `shared_ptr`s, the caller may
drop its local pointers after construction. During teardown, the base
`Dmn_DMesgNet` destructor shuts down/releases input and then sends the final
heartbeat through output. Verify the actual implementation's destructor
sequence with a fake endpoint test. Do not explicitly shut down output before
the node destructor completes. Under Option B, member declaration order must
keep both endpoints alive until the internal node's destructor returns.

If applications create many nodes in one process, each node needs distinct
adapter connections and matching signal configuration within its protocol.
Each node needs a unique node ID. A future shared-connection optimization is out of scope until
its multiplexing, routing, shutdown, and authentication behavior are specified.

## 6. Comparison with the `Dmn_DMesgDbus` facade

| Concern | Option A: direct injected I/O | Option B: `Dmn_DMesgDbus` composition wrapper |
|---|---|---|
| Transport/wire semantics | Same configured D-Bus signal tuple and endpoint implementation. | Same config and endpoint implementation; defaults match Option A. |
| Construction | Caller creates input and output endpoints and passes both to `Dmn_DMesgNet`. | Caller passes node ID/config; wrapper creates endpoints and initializes a private node member. |
| `Dmn_DMesgNet` changes | None. | None; wrapper owns a private instance. |
| Public API | Public input/output adapters, config, and status; no pair factory. | Adds a first-class wrapper/config/status and forwards a selected DMesg API subset. |
| Application ergonomics | More explicit and flexible; caller can accidentally pair wrong buses/configs. | Small, safe default constructor; fewer assembly mistakes. |
| Ownership | Caller owns endpoint refs; node also shares ownership. | Wrapper owns endpoints before the internal node member so node teardown runs first. |
| Lifecycle risk | Caller can mismatch config or shut down an endpoint early. | Constructor rollback and member declaration/destruction order must be tested. |
| Failure visibility | Caller can inspect each endpoint snapshot. | Wrapper exposes a read-only combined status snapshot. |
| Build/link | Optional adapter target; core stays independent if adapters are out-of-line/optional. | Optional adapter target plus public facade header/install/export and standalone-header integration. |
| ABI/source compatibility | Does not alter existing DMesgNet API; public endpoint API is a support commitment. | New wrapper ABI/API; existing users unaffected, but users link the optional target. |
| Testing | Strong isolation: codec, each endpoint, injected fake I/O, then composition. | All shared tests plus facade-specific config/rollback/base-destruction tests. |
| Extension to another transport | Natural: any `Dmn_Io<std::string>` pair can be supplied. | D-Bus-specific API is intentionally narrow; use Option A for another transport. |
| Multiple DmnNet instances | Caller controls and can share factories/config while retaining separate connections. | Each facade constructs an endpoint pair; sharing needs a separate explicit design. |
| Operational configuration | Explicit endpoint construction can support custom factories/options. | Central validated defaults are easier; only stable user-facing options should be exposed. |
| Best fit | Advanced users, tests, custom transports, and explicit lifecycle control. | Application users who want a small DMesg API without endpoint assembly. |

### Implementation sequence

Option A was implemented and validated first as the standalone adapter and
direct-injection API. After its exit criteria, including private-bus and
`Dmn_DMesgNet` composition tests, passed, Option B was implemented as a wrapper
over the same public Option A endpoints. It forwards the DMesg application
subset specified in `dmesgnet-dbus-facade-spec.md` and adds no second transport
path.

## 7. Test coverage and remaining requirements

`test/dmn-test-dbus-io.cpp` covers byte-signal matching and delivery,
configuration/address failures, queue limits and overflow, malformed and
oversized payloads, endpoint shutdown/disconnect behavior, and
`Dmn_DMesgNet` composition over a private bus. The Option B facade test also
checks bidirectional message exchange with a directly injected Option A peer.
With `ENABLE_FAULT_INJECTION=ON`, separate test programs inject input payload
allocation failure, retain output messages through the finite drain deadline,
fail D-Bus worker startup, and fail facade output-endpoint creation after
input setup to verify rollback. Each fault-injection test enables only its
corresponding libfiu point through `ADD_TEST_FAULT_INJECTION_EXECUTABLE`; all
have the `fault-injection` CTest label and run with
`ctest --test-dir build -L fault-injection`. The facade's private construction
helper is explicitly named `createOutputForFaultInjection`; the seam is
compiled only with fault injection enabled and is not public API.

The regular endpoint suite includes a private-daemon send-policy fixture that
denies the client's `org.freedesktop.DBus.AddMatch` method call. It verifies
input construction fails with an explicit error and releases the connection
rather than exposing a partially initialized endpoint. This exercises
AddMatch setup failure only; it is not a general bus-policy certification.

`test/dmn-test-dmesgnet-shutdown.cpp` uses fake endpoints to verify that
`Dmn_DMesgNet` calls input shutdown before writing the final serialized
`Destroyed` heartbeat. Its blocking fake input must signal when `read()` has
entered its wait; the test waits for that signal before destroying the node
and verifies the read exits after shutdown and before the final heartbeat.
This proves shutdown wakes an already-blocked input read, not merely that a
read started after shutdown returns promptly.

These tests make no claims about remote-host reachability, consensus, quorum,
or exactly-once delivery.

No public endpoint-pair factory was added; callers construct the independently
owned input and output endpoints directly. Broader send/receive policy
behavior and deployment policy certification remain outside this test scope.

## 8. Go/no-go assessment

**Go** for the implemented Option A `Dmn_DMesgNet` injection API and Option B
composition facade, using private-bus tests and trusted local participants.
Option A's tests served as the phase gate for Option B. The type seam and
binary signal mechanism are confirmed.

**No-go** to describing this as a replacement for `Dmn_Socket` for remote
machines, a secure node-identity transport, reliable/acknowledged delivery, or
a consensus transport. Those would be distinct requirements and require
different designs.
