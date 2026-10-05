# Option B: `Dmn_DMesgDbus` Composition Facade

**Status:** API and implementation proposal; not implemented. This design uses
the shared signal/endpoint contract in
[`dmesg-dbus-spec.md`](dmesg-dbus-spec.md) and is compared against direct
endpoint injection in
[`dmesgnet-dbus-injection-spec.md`](dmesgnet-dbus-injection-spec.md).

## 1. Purpose and relationship to Option A

Option B is implemented only after Option A's endpoints, direct-injection
composition, and required tests pass. It is a wrapper around the already
tested Option A implementation—not another transport implementation and not
a subclass of `Dmn_DMesgNet`.

Expose a focused application API for users who want DMesg messaging over
local D-Bus without constructing, seeing, or managing the `Dmn_Io` endpoints:

```cpp
dmn::Dmn_DMesgDbus node{"node-a", config};
```

`Dmn_DMesgDbus` is **not** a new DMesg protocol, D-Bus gateway for arbitrary
method calls, or cross-host transport. It owns a private `Dmn_DMesgNet`
instance configured with the same D-Bus `Dmn_Io<std::string>` endpoint types
as Option A. Its value is a smaller application-facing API and centralized
transport lifecycle/configuration—not new network or consensus guarantees.

Typical application code supplies a normalized handler spec and uses the
returned handler proxy for DMesg I/O; it never receives the internal node or
transport endpoints:

```cpp
dmn::Dmn_DbusConfig config;
dmn::Dmn_DMesgDbus node{"worker-a", config};
dmn::Dmn_DMesgDbus::HandlerSpec spec{"worker", "jobs"};
auto handler = node.openHandler(spec);
dmn::DMesgPb message;
handler->write(message);
```

## 2. Public API proposal and boundary

Use composition. The class owns an internal `Dmn_DMesgNet` and forwards only
the application-facing `Dmn_DMesg` operations below. It must not publicly
inherit from `Dmn_DMesgNet` or `Dmn_DMesg`: that would expose a broader
inherited API and allow callers to upcast around the wrapper's intended
boundary.

```cpp
#include <memory>
#include <optional>
#include <string_view>

#include "dmn-dmesg.hpp"
#include "dmn-dbus-config.hpp"

namespace dmn {

struct Dmn_DMesgDbusStatus {
  Dmn_DbusIoStatus input;
  Dmn_DbusIoStatus output;
};

class Dmn_DMesgDbus final {
public:
  using AsyncProcessTask = Dmn_DMesg::AsyncProcessTask;
  using FilterTask = Dmn_DMesg::FilterTask;
  using HandlerConfig = Dmn_DMesg::HandlerConfig;
  using HandlerFactory = Dmn_DMesg::HandlerFactory;
  using HandlerSpec = Dmn_DMesg::HandlerSpec;
  using HandlerType = Dmn_DMesg::HandlerType;

  Dmn_DMesgDbus(std::string_view node_id,
                const Dmn_DbusConfig &config = {});

  auto openHandler(const HandlerSpec &spec) -> HandlerType;
  auto openHandlerWithFactory(const HandlerSpec &spec,
                              const HandlerFactory &factory) -> HandlerType;
  void closeHandler(HandlerType &handler);
  auto getTopicLastMessage(std::string_view topic)
      -> std::optional<dmn::DMesgPb>;
  void resetConflictStateWithLastTopicMessage(std::string_view topic);

  [[nodiscard]] auto status() const -> Dmn_DMesgDbusStatus;

  Dmn_DMesgDbus(const Dmn_DMesgDbus &) = delete;
  auto operator=(const Dmn_DMesgDbus &) -> Dmn_DMesgDbus & = delete;
  Dmn_DMesgDbus(Dmn_DMesgDbus &&) = delete;
  auto operator=(Dmn_DMesgDbus &&) -> Dmn_DMesgDbus & = delete;

  ~Dmn_DMesgDbus() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace dmn
```

The facade is DMesg-specific; its internal byte endpoints use the shared
`Dmn_DbusConfig`, whose default path/interface/member identify the DMesgNet
wire contract. Applications may select another signal tuple through config
only when all participants use that same protocol configuration.
`bus_address` must not silently fall
back to another bus. Empty address is permitted only for an unprivileged
session-bus constructor; a system bus or arbitrary address must be explicit
and must not be selected from an unsafe implicit environment in a privileged
service.
`Dmn_DbusIoStatus` is the implemented read-only snapshot type defined by the
fields and meanings in the shared transport contract. `Dmn_DbusConfig`
and `Dmn_DbusIoStatus` are declared once in
`include/dmn-dbus-config.hpp`. The facade header includes that small
shared-types header, not `dmn-dbus-io.hpp` or
`dmn-dmesgnet.hpp`; the latter headers remain available to Option A callers.

The forwarded DMesg surface is deliberately limited to handler creation,
handler closure, topic-state inspection, conflict reset, and read-only
transport status. `Dmn_DMesgNet` currently adds no public operations beyond
its constructor/destructor, so no election/membership internals are forwarded.
Do not expose a `Dmn_Io<std::string>` parameter, endpoint type, endpoint
factory, raw `Dmn_DMesgNet` reference/pointer, or transport-specific callback
through the facade. Keep handler aliases so clients can use the normal DMesg
handler configuration and return type without naming the hidden core member.

This is a proposed v1 forwarding surface, not a commitment to forward every
future `Dmn_DMesg` method automatically. If a new user-facing DMesg operation
is added later, explicitly decide whether the facade should forward it and
add a compile-time/API test for that decision.
`openHandler` accepts the normalized `HandlerSpec` rather than mirroring
`Dmn_DMesg`'s variadic template. This keeps the wrapper header independent of
the hidden implementation type while preserving topic, filter, async callback,
and handler configuration.

| Forwarded API | Wrapper behavior |
|---|---|
| `openHandler(spec)` | Creates a standard handler from all `HandlerSpec` fields (`name`, `topic`, filter, async callback, and config); returns the same `HandlerType` proxy. |
| `openHandlerWithFactory(spec, factory)` | Delegates unchanged, preserving the existing factory extension point. |
| `closeHandler(handler)` | Delegates closure and proxy reset to the internal node. |
| `getTopicLastMessage(topic)` | Returns the internal node's `std::optional<DMesgPb>`. |
| `resetConflictStateWithLastTopicMessage(topic)` | Delegates the existing conflict reset operation. |
| `status()` | Returns thread-safe snapshots for the input and output D-Bus endpoints. |

Do not forward the internal node's construction, `Dmn_Io` injection,
publishing internals, `Dmn_Pub` registration surface, or protected/private
election and membership operations. Normal message publication/read remains
through the returned handler proxy, matching how applications use
`Dmn_DMesg`.

The facade's public class should live in an optional header
`include/dmn-dmesgnet-dbus.hpp`, in `namespace dmn`. It may be included by an
optional D-Bus umbrella header (or the main umbrella only under the enabled
feature). The core `dmn` target must remain buildable without libdbus.
Installation must expose the Option A endpoint classes/config/status and the
facade/config/status/handler aliases, but the facade header must not expose
the endpoint class declarations. Keep libdbus headers and connection objects
private implementation details.

## 3. Construction and ownership design

Because this is composition, construct a complete endpoint pair first and then
construct the internal `Dmn_DMesgNet` member with those endpoints. Declare
members in this order so reverse destruction destroys the node first and
endpoints second:

```cpp
namespace dmn {

struct DbusEndpoints {
  std::shared_ptr<Dmn_DbusInput> input;
  std::shared_ptr<Dmn_DbusOutput> output;
};

struct Dmn_DMesgDbus::Impl {
  DbusEndpoints endpoints;
  Dmn_DMesgNet node;

  Impl(std::string_view node_id, const Dmn_DbusConfig &config)
      : endpoints{makeDbusEndpoints(config)},
        node{node_id, endpoints.input, endpoints.output} {}
};

} // namespace dmn
```

Define the PIMPL in the optional facade implementation file; the public
header exposes no endpoint or `Dmn_DMesgNet` member. Inside `Impl`, declare
`endpoints` before `node`, initialize the pair first, then initialize `node`
with `(node_id, endpoints.input, endpoints.output)`. Construct input first
and complete its D-Bus `AddMatch` handshake, then construct output. If either
step fails, RAII releases acquired resources; the wrapper constructor throws
and no partially initialized object escapes. Input and output remain distinct
because the node shuts down input before its final `Destroyed` heartbeat uses
output.

Define the facade constructor/destructor and non-template forwarders out of
line, after `Impl` is complete. `openHandler(spec)` delegates using the
normalized fields in `HandlerSpec`; `openHandlerWithFactory` passes the spec
and factory to the internal node. This keeps `Dmn_DMesgNet` and endpoint
definitions out of the facade header without weakening the public API.

```cpp
namespace dmn {

Dmn_DMesgDbus::Dmn_DMesgDbus(
    std::string_view node_id, const Dmn_DbusConfig &config)
    : m_impl{std::make_unique<Impl>(node_id, config)} {}

Dmn_DMesgDbus::~Dmn_DMesgDbus() noexcept = default;

auto Dmn_DMesgDbus::openHandler(const HandlerSpec &spec) -> HandlerType {
  return m_impl->node.openHandler(spec.m_name, spec.m_topic, spec.m_filter_fn,
                                  spec.m_async_process_fn, spec.m_configs);
}

} // namespace dmn
```

Important C++ lifetime constraints:

- Member declaration order inside `Impl` is the lifetime guarantee:
  `endpoints` is declared before `node`, so `node` is destroyed first. The node
  retains shared ownership through its destructor; endpoint owners then
  release naturally. Do not explicitly shut output down before node teardown.
- Endpoint callbacks must own/reference endpoint state safely and must never
  capture the facade `this` from a partially constructed or already-destroyed
  wrapper.
- The input endpoint's shutdown from `Dmn_DMesgNet::~Dmn_DMesgNet`
  wakes the blocking read and joins the D-Bus dispatch thread. Output remains
  open for the final heartbeat write.
- `Dmn_DMesgNet`'s destructor is `noexcept` and catches failures from its
  shutdown/final heartbeat path. The facade must not promise that its final
  broadcast was received by peers.

The pair helper must return a complete `DbusEndpoints` pair or throw; it must
perform rollback if constructing the second endpoint throws. Option B may use
Option A's public pair factory if one exists, or an internal helper that
constructs the same public endpoint types. In either case, there is one
endpoint implementation and one configuration interpretation. Avoid public
ownership of raw `DBusConnection *`; hide libdbus objects behind adapter RAII.

## 4. Build and dependency boundaries

The implementation should introduce an opt-in build flag such as
`ENABLE_DBUS=OFF`:

- With the option off, existing `dmn` library, public headers, applications,
  and default tests must not include or link libdbus.
- With it on, find `dbus-1` through pkg-config/imported target; if unavailable,
  fail configuration with an actionable error.
- Link libdbus only into the optional D-Bus adapter target/component. Avoid
  adding a mandatory `PkgConfig::DBUS` dependency to core `dmn`.
- Install/export the optional endpoint and facade headers/target consistently
  with the repository's current install model.
- Do not list the new optional headers in the always-built standalone-header
  check unless that target receives the correct optional include/link
  dependency. Add a separate optional header/build test instead.
- Add CTest labels `dmn` for fake-backend tests and `dbus` for tests requiring
  a private daemon. No test may access the developer's system bus.

Whether the D-Bus implementation is a separate `dmn-dbus` library or an
optional source added to `dmn` is a maintainer decision. The decisive
constraint is that a default non-D-Bus configure/build stays dependency-free
with respect to libdbus.

## 5. Facade behavior

The wrapper forwards only the selected DMesg application methods. Each
forwarder delegates directly to its private `m_impl->node`; no message,
heartbeat, queue, protobuf, playback, or routing logic is duplicated. The
constructor validates config, creates the endpoint pair, and initializes the
internal node.

The facade uses the common contract:

- configured broadcast signal with an `ay` payload; the default
  `Dmn_DbusConfig` tuple interoperates with `Dmn_DMesgNet`;
- one private input D-Bus connection and one private output connection;
- finite message/queue limits;
- exact `AddMatch` installation before constructor success;
- self-echo suppression by existing DMesg source-write-handler marker;
- explicit failures on local setup/send/read errors;
- no ACK, persistence, delivery guarantee, cross-host bus federation, or
  consensus.

If wrapper-specific concerns grow beyond construction, config validation,
the documented DMesg forwarding surface, and status, move behavior into the
adapters or retain the direct-injection API; do not turn the wrapper into a
second D-Bus protocol implementation. The `status()` accessor is the sole
additional transport-operational behavior: it
returns a thread-safe snapshot of both private endpoints so asynchronous
transport failures are inspectable without exposing libdbus objects. V1
reports operational errors through the repository's stderr diagnostic
convention; it does not add a user callback.

## 6. Feasibility and tradeoff

**Feasibility: high.** Composition is viable because `Dmn_DMesgNet` accepts
the exact endpoint types and exposes the application DMesg operations the
wrapper needs to delegate. No inheritance or change to the DMesgNet
constructor is needed.

**Primary tradeoff:** smaller, transport-hiding application API versus a
deliberately narrower surface that may need explicit forwarding when the
underlying DMesg API grows. Option A remains available to advanced callers
who need custom `Dmn_Io` adapters or direct `Dmn_DMesgNet` access.

**Main risks:** incomplete forwarding and member destruction ordering.
Documenting the forwarded set and testing node destruction before
endpoint release mitigate these. No custom destructor should shut down output
early.

## 7. Facade-specific tests

In addition to all common adapter and private-bus tests:

1. Default config selects only the documented session bus behavior.
2. Explicit private-bus address is used exactly; no environment bus is chosen
   instead.
3. Invalid limits/address and failed input connection cause construction to
   throw with no leaked connection.
4. Input connection succeeds but output connection fails: the input match is
   removed and worker/connection resources are released.
5. `openHandler(HandlerSpec)` preserves its name, topic, filter, async callback,
   and config; handler read/write/conflict behavior remains identical to the
   underlying DMesg methods. Verify `openHandlerWithFactory` delegation and
   exception propagation, `closeHandler` proxy invalidation, topic lookup
   before/after publication, and conflict reset.
6. A compile-only API test includes only the facade header, verifies
   supported aliases/forwarders, confirms the wrapper is not a
   `Dmn_DMesgNet` base, and confirms its public constructor does not accept
   endpoint objects. The facade header does not include or expose Option A
   endpoint declarations.
7. Destroying the wrapper destroys its `Impl` and internal node first; input
   shutdown precedes the final output heartbeat. A private test-only endpoint
   factory seam supplies fakes to assert call order and verify output remains
   usable until that heartbeat.
8. Endpoint owners outlive the internal node destructor and are released
   afterward.
9. Configured bounds are passed unchanged to both endpoints.
10. A facade private-bus integration test exchanges messages with an
    Option-A directly injected peer, proving both construction approaches
    share one wire contract.

## 8. Acceptance / non-goals

Accept the facade when it passes all common D-Bus transport tests plus
facade-specific construction/lifetime tests, and when it adds no DMesgNet code
path beyond endpoint construction. It is not accepted as a substitute for
`Dmn_DMesgNet` over a real network, nor as a reliable or consensus transport.
