# Option B: `Dmn_DMesgDbus` facade

**Status:** Implemented behind `ENABLE_DBUS`. This facade composes the
implemented Option A byte-signal endpoints with `Dmn_DMesgNet`; it adds no
transport protocol or DMesg behavior of its own.

## Purpose and scope

Option A remains the lower-level API for callers that need custom I/O or direct
`Dmn_DMesgNet` access. Option B is a convenience API for applications that
want a DMesg node over D-Bus without constructing or managing those endpoints.
The facade is a composition wrapper, not a `Dmn_DMesgNet` subclass:

```cpp
dmn::Dmn_DMesgDbus node{"worker-a", config};
dmn::Dmn_DMesgDbus::HandlerSpec spec{"worker", "jobs"};
auto handler = node.openHandler(spec);
handler->write(message);
```

The facade is limited to handler operations, topic lookup, conflict reset, and
read-only transport status. Message serialization, heartbeat/membership logic,
playback, and handler behavior remain in `Dmn_DMesgNet`/`Dmn_DMesg`. It does
not provide cross-host transport, delivery acknowledgement, persistence,
reliability, or consensus.

This facade is a complete DMesgNet participant, not a client proxy to a
daemon-owned node. The proposed per-host daemon/client-proxy architecture is
specified separately in
[`dmesgnet-dbus-node-gateway-spec.md`](dmesgnet-dbus-node-gateway-spec.md).
No changes to this facade are required to implement that separate service:
the gateway should own the daemon's `Dmn_DMesgNet` and register its own
server-side handlers on the shared `Dmn_DMesg` publisher.

## Public API

The public header is `include/dmn-dmesgnet-dbus.hpp`, in namespace `dmn`. It
includes `dmn-dmesg.hpp` for the DMesg message and handler types and
`dmn-dbus-config.hpp` for the shared configuration/status types. It does not
include or expose `Dmn_DbusInput`, `Dmn_DbusOutput`, `Dmn_DMesgNet`, or any
libdbus type.

```cpp
struct Dmn_DMesgDbusStatus {
  Dmn_DbusIoStatus input;
  Dmn_DbusIoStatus output;
};

class Dmn_DMesgDbus final {
public:
  using AsyncProcessTask = Dmn_DMesg::AsyncProcessTask;
  using FilterTask = Dmn_DMesg::FilterTask;
  using HandlerEvent = Dmn_DMesg::HandlerEvent;
  using HandlerEventCallbackTask = Dmn_DMesg::HandlerEventCallbackTask;
  using HandlerEventType = Dmn_DMesg::HandlerEventType;
  using HandlerConfig = Dmn_DMesg::HandlerConfig;
  using HandlerFactory = Dmn_DMesg::HandlerFactory;
  using HandlerSpec = Dmn_DMesg::HandlerSpec;
  using HandlerType = Dmn_DMesg::HandlerType;

  explicit Dmn_DMesgDbus(std::string_view node_id,
                         const Dmn_DbusConfig &config = {});
  ~Dmn_DMesgDbus() noexcept;

  Dmn_DMesgDbus(const Dmn_DMesgDbus &) = delete;
  auto operator=(const Dmn_DMesgDbus &) -> Dmn_DMesgDbus & = delete;
  Dmn_DMesgDbus(Dmn_DMesgDbus &&) = delete;
  auto operator=(Dmn_DMesgDbus &&) -> Dmn_DMesgDbus & = delete;

  auto openHandler(const HandlerSpec &spec) -> HandlerType;
  auto openHandlerWithFactory(const HandlerSpec &spec,
                              const HandlerFactory &factory) -> HandlerType;
  void closeHandler(HandlerType &handler);
  auto getTopicLastMessage(std::string_view topic)
      -> std::optional<DMesgPb>;
  void resetConflictStateWithLastTopicMessage(std::string_view topic);
  [[nodiscard]] auto status() const -> Dmn_DMesgDbusStatus;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
```

`openHandler` forwards the complete `HandlerSpec` to `Dmn_DMesg::openHandler`,
including the optional publisher-ordered handler-event observer.
`openHandlerWithFactory` preserves the existing derived-handler extension
point and delegates the factory and spec unchanged. Handler-event, message
processing, and conflict callbacks run on the handler's async context in
publisher event order and must return promptly without blocking; the
handler-event observer must enqueue bounded gateway work. The handler filter
remains synchronous on the publisher context.
`closeHandler`, `getTopicLastMessage`, and
`resetConflictStateWithLastTopicMessage` directly forward to the internal
`Dmn_DMesgNet`. `status()` returns snapshots of the two owned endpoints; its
fields describe local endpoint activity only and do not indicate peer
delivery.

Do not add variadic handler forwarding, raw node/endpoint accessors, an
endpoint factory, publishing internals, or a second callback/error API. If
`Dmn_DMesg` gains a new user-facing operation, decide explicitly whether this
facade should expose it.

## Ownership and lifecycle

The implementation is private to `src/dmn-dmesgnet-dbus.cpp`. Its `Impl`
owns the endpoint pair and then the node, in that declaration order:

```cpp
struct Dmn_DMesgDbus::Impl {
  std::shared_ptr<Dmn_DbusInput> input;
  std::shared_ptr<Dmn_DbusOutput> output;
  Dmn_DMesgNet node;
};
```

Members are destroyed in reverse declaration order, so the node shuts down
input and emits its final best-effort heartbeat through output before the
facade releases its endpoint owners. Construct input first (including its
match handshake), then output, then the node. If construction fails, ordinary
RAII destroys any successfully constructed endpoints; no partially
constructed facade escapes. Do not explicitly shut down output before node
destruction.

The facade does not capture its own `this` in endpoint callbacks. It is
non-copyable and non-movable because it owns non-movable endpoint lifetimes and
a node bound to them. Its status method is a point-in-time pair of thread-safe
endpoint snapshots.

## Build boundary

The facade is available only when `ENABLE_DBUS=ON`, as part of the optional
`dmn-dbus` target. The core `dmn` target remains independent of libdbus. The
facade header is installed only when the optional feature is enabled. The
header itself must compile without exposing libdbus declarations; the facade
implementation links against the existing D-Bus endpoint target and does not
duplicate its transport implementation.

## Required tests

The facade tests use the same private `dbus-daemon` isolation as Option A and
are registered through the existing `ADD_TEST_EXECUTABLE(dmn ...)` mechanism.
They must not access the developer's session or system bus.

- Compile the facade header on its own; verify aliases and supported operations
  are usable, the class is not derived from `Dmn_DMesgNet`, and copying/moving
  is disabled. The `dmn-standalone-header-check` target compiles every public
  header in an otherwise empty translation unit, including this header.
- Verify invalid configuration and an unusable explicit address fail during
  construction rather than falling back to another bus.
- Verify `HandlerSpec` fields are forwarded; exercise the handler factory,
  handler read/write, handler-event observer delivery, topic lookup, conflict
  reset, and close/proxy invalidation.
- Exchange DMesg messages between a facade node and an Option A
  `Dmn_DMesgNet` using the same configured signal tuple. Check topics and
  payloads in both directions.
- Verify input and output status are separately observable and that facade
  destruction leaves output available long enough for the node's final
  best-effort heartbeat. After stopping the configured private bus, verify
  both status snapshots expose their endpoint's terminal disconnect error.
- With `ENABLE_FAULT_INJECTION=ON`, fail output worker startup inside the
  output endpoint after the facade has created its input endpoint. Verify the
  startup error propagates and construction rollback releases both endpoint
  connections. This complements the facade-level injected output-creation
  failure, which verifies rollback before output endpoint construction.
- With `ENABLE_FAULT_INJECTION=ON`, fail output-endpoint creation after input
  creation and verify construction unwinds the input connection without
  exposing a partially constructed facade or adding a public test hook.

The tests should use predicates/time bounds for asynchronous readiness and
delivery rather than relying on fixed sleeps. They assert the facade boundary
and delegation contract, not D-Bus delivery guarantees.
