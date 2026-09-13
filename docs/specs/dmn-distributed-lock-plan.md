# Implementation Plan: DMN Distributed Range Lock

## 1. Goal

Implement `docs/specs/dmn-distributed-lock-spec.md` as vertical, independently
buildable layers. The first usable feature is an in-memory, no-wait range lock
with query, lease/fence, and unlock. Waiting, renewal, retry, observability, and
DMesg transport are added only after that path is complete.

Every layer:

1. scaffolds and compiles new files before adding behavior;
2. adds class lifetime and construction before operational methods;
3. adds a construction/lifetime smoke test;
4. adds one behavioral test and its minimum implementation at a time;
5. builds the smallest affected targets;
6. runs every completed lock target;
7. inspects the diff for accidental base-library coupling;
8. leaves no declared API with placeholder success behavior.

Public methods may be introduced incrementally. Do not declare a future method
and return a fake success or unrelated error until its layer is implemented.

Tests use manual clocks, deterministic ids/jitter, explicit callback drains,
promises, and barriers. They do not use wall-clock sleeps for correctness.

## 2. Base-Library Boundary

Lock support is additive. Do not change the behavior or public C++ API of:

- `Dmn_DMesg` or `Dmn_DMesgNet`;
- `Dmn_Runtime_Manager`, `Dmn_Async`, or `Dmn_Proc`;
- `Dmn_Pub` or any blocking queue.

The only approved edits to existing files are:

- add `dlock = 2` without renumbering existing values in
  `src/proto/dmn-dmesg-type.proto`;
- import the lock schema and add `DLockEnvelopePb dlock = 2` without
  renumbering fields in `src/proto/dmn-dmesg-body.proto`;
- register lock files in existing CMake lists;
- include public lock headers from `include/dmn.hpp`.

All executors, protocol conversion, conflict recovery, diagnostics, and test
accessors are implemented in new lock-specific files. If another existing
base implementation/header appears to require modification, stop that layer
for design review rather than changing it implicitly.

## 3. Dependency Direction

```text
public manager -> backend interface <- in-memory backend -> authority core
                               \---- DMesg backend

DMesg backend/service -> protocol conversion -> generated lock protobuf
DMesg authority service -> authority core
authority core -> public values + transport-neutral protocol + clock
```

The authority and manager include no DMesg or generated protobuf headers.
`Dmn_DMesgNet` is absent from all v1 lock source files.

## 4. Common Commands

Configure or reuse a compatible build:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target dmn
ctest --test-dir build -L dmn --output-on-failure
```

After Layers 1-2:

```bash
cmake --build build --target \
  dmn-test-dlock dmn-test-dlock-authority
ctest --test-dir build \
  -R '^dmn-test-dlock(-authority)?$' --output-on-failure
```

After the DMesg layer:

```bash
cmake --build build --target \
  dmn-test-dlock dmn-test-dlock-authority dmn-test-dlock-dmesg
ctest --test-dir build \
  -R '^dmn-test-dlock(-authority|-dmesg)?$' --output-on-failure
```

### 4.1 Required scaffold-first micro-increments

Apply this sequence whenever the plan introduces a concrete production class.
Do not combine these checkpoints into one large implementation change.

#### A. File and build-system scaffold

1. Create the `.hpp` with the repository Doxygen prologue, include guard, and
   empty `namespace dmn` block.
2. Create the `.cpp` containing only its matching header include and an empty
   `namespace dmn` block.
3. Add the `.cpp` to the `dmn` library's private sources in
   `src/CMakeLists.txt`.
4. Add a public `.hpp` to the library interface sources. Add it to
   `include/dmn.hpp` only when it is part of the supported public facade.
5. Configure and build `dmn` before declaring a class.
6. Confirm the diff contains only the expected new files and approved CMake or
   umbrella-header edits.

Header-only value/interface files do not receive an artificial `.cpp`; their
checkpoint is a small compile-only test translation unit. Internal headers are
not added to `include/dmn.hpp`.

#### B. Class lifetime scaffold

1. Declare the final class and required inheritance only.
2. Declare its constructor/factory and destructor with the final signatures.
3. State the Rule of Five explicitly:
   - resource-owning concrete lock classes have an out-of-line `noexcept`
     destructor and delete copy/move construction and assignment;
   - abstract interfaces have a virtual `noexcept` destructor;
   - value types use compiler-generated copy/move operations.
4. Add only the members required to establish a valid empty lifetime.
5. Define constructor, destructor, and factory out of line in the `.cpp`.
6. Build `dmn` again before adding operational behavior.

An out-of-line destructor is required even if initially defaulted so ownership
of incomplete implementation types can remain in the `.cpp`.

#### C. Construction and destruction smoke test

1. Create/register the component's test executable if it does not exist.
2. Construct the object through its supported public constructor or factory.
3. Let it destruct at scope exit and assert construction invariants visible at
   that layer.
4. Add `static_assert` checks for intended base-class relationship and
   copy/move traits.
5. Build only that test target and run it.

The smoke test must not require a fake successful lock operation. If startup
is separate from construction, test construction first and startup in the next
behavioral micro-increment.

A concrete class derived from an abstract interface is the exception: declare
all final overrides during the lifetime scaffold, but do not instantiate it
until every pure-virtual override has a real out-of-line definition. Build the
library and use compile-time inheritance/signature checks first. Then implement
the smallest complete lifecycle slice and add the construction/destruction
smoke test. Never add dummy override bodies solely to make the vtable link.

#### D. Behavioral micro-increments

For each method or state transition:

1. add one focused failing test;
2. add the method declaration only when implementing its real behavior;
3. add the minimum implementation;
4. build and run the focused test;
5. run all tests completed for that component;
6. inspect the diff before continuing.

When a final protocol variant exists before its behavior layer, return the
specified permanent, tested error without mutation. Never return placeholder
success.

## 5. Layer 0: Baseline and Change Guard

### Work

- configure the existing project;
- build `dmn`;
- run the current `dmn`-label tests;
- record pre-existing failures without fixing unrelated behavior;
- record `git diff --name-only` so later layers can distinguish existing work.

### Exit gate

The library builds, the existing test baseline is known, and no lock file has
been added.

## 6. Layer 1: Values and Final Contracts

This layer defines types, not behavior.

### 1A. File and build scaffold

Create:

- `include/dmn-dlock.hpp`;
- `include/dmn-dlock-clock.hpp`;
- `include/dmn-dlock-protocol.hpp`;
- `include/dmn-dlock-backend.hpp`;
- `src/dmn-dlock.cpp`;
- `src/dmn-dlock-internal.hpp`;
- header-only `test/dmn-test-dlock-fakes.hpp`.

Initially, public headers contain only their prologue, includes required by
their empty declarations, guard, and namespace. `src/dmn-dlock.cpp` contains
only `#include "dmn-dlock.hpp"` and the namespace.

Register `src/dmn-dlock.cpp` as a private `dmn` source and the four public
headers as interface sources. Include `dmn-dlock.hpp` and
`dmn-dlock-clock.hpp` from `include/dmn.hpp` at this checkpoint;
protocol/backend headers remain directly includable but are not pulled into
every facade consumer.

Build `dmn` before adding types:

```bash
cmake --build build --target dmn
```

### 1B. Value and interface declarations

Define the final value and interface contracts from specification Sections
6.3, 8.1, 8.2, and 10.1:

- range, state, terminal reason, config, options, entry, and result values;
- event value and `Dmn_DLock_Event_Emitter`;
- protocol commands, replies, entries, snapshots, statuses, and conversion
  errors;
- backend submit result, backend error, and backend abstract interface;
- clock interface;
- private manager executor/dependency interfaces and friend test-access names.

The emitter/backend interfaces receive virtual `noexcept` destructors. Add
compile-time checks that they are abstract and that protocol/value types have
the intended copy/move properties.

Build `dmn` again before registering tests.

### 1C. Compile and value tests

Create `test/dmn-test-dlock.cpp` and cover:

- inclusive endpoint overlap;
- adjacent non-overlap;
- negative and reversed ranges;
- enum/default value construction;
- compile-time signatures for the public event interface.

Register only `dmn-test-dlock`.

Do not declare `Dmn_DLock_Manager` methods yet. Do not add a worker, authority,
backend implementation, protobuf, or DMesg dependency.

### Exit gate

All final cross-component value types compile independently, range tests pass,
and transport-neutral headers contain no DMesg/protobuf include.

## 7. Layer 2: Minimal Safe Authority

This layer implements the safety core needed by basic lock/unlock. It supports
no-wait create, release, query snapshots, lease expiry, fencing, idempotency,
and versioning. It does not support queued waiting, cancel, or renewal.

### 2A. Authority file scaffold

Create:

- `include/dmn-dlock-authority.hpp`;
- `src/dmn-dlock-authority.cpp`.

Register the header/source with `dmn` and add the public header to
`include/dmn.hpp`. Build `dmn` while both files contain only the standard
prologue, include/guard, and namespace.

### 2B. Authority lifetime scaffold

Declare `Dmn_DLock_Authority final` with:

- the final config/nonce/clock constructor;
- an out-of-line `noexcept` destructor;
- deleted copy constructor, copy assignment, move constructor, and move
  assignment;
- a private implementation pointer or the minimum empty-state members needed
  for the final representation.

Define construction, validation, and destruction in
`src/dmn-dlock-authority.cpp`. Do not declare `process()`, `snapshot()`, or
`runMaintenance()` until their first real behavioral increment.

Build `dmn`.

### 2C. Authority construction smoke test

Create and register `test/dmn-test-dlock-authority.cpp`. Add:

- successful construction with a valid config, nonce, and manual clock;
- invalid config, empty nonce, and null clock failures;
- scope-exit destruction;
- `static_assert` checks that the class is final and not copy/move
  constructible or assignable.

Build and run only `dmn-test-dlock-authority`.

### 2D. Authority behavior micro-increments

Add the following tests and implementation one item at a time:

- empty `snapshot()` and stable initial version;
- first no-wait create grants and assigns sequence/version/fence;
- non-overlapping no-wait creates grant concurrently;
- overlapping no-wait create returns cached conflict without consuming
  sequence, fence, or version;
- duplicate operation id returns the same logical outcome;
- changed immutable payload under an operation id is rejected;
- owner mismatch cannot release;
- release is idempotent and increments version once;
- lease expiry releases the range and a later grant receives a larger fence;
- one maintenance pass expiring several grants increments version once;
- authority configuration, duration limits, nonce, and clock validation;
- snapshot contains no overlapping grants and no process-local time point;
- duplicate cached grant reply recomputes a smaller lease remaining value;
- `snapshot()` changes timing metadata without changing structural version;
- concurrent external calls are documented as invalid until serialized by a
  backend.

Introduce methods in this order:

1. `snapshot()` with an empty snapshot;
2. `process()` with validation and snapshot-request behavior;
3. no-wait create;
4. overlap conflict;
5. operation-id deduplication and immutable-payload checks;
6. release and owner checks;
7. lease deadlines and fencing;
8. `runMaintenance()` and expiry;
9. timing-metadata recomputation for cached replies.

Use active/terminal request indexes, operation-result deduplication, monotonic
sequence/version/fence counters, authority-clock deadlines, and one grant
reevaluation helper. Store logical cached outcomes and deadlines, not frozen
remaining-duration replies.

Keep the complete Layer 1 protocol variant intact. In this layer,
`process()` executes only no-wait create, release, and snapshot request.
Waiting create, cancel, and renew return a permanent, tested
`kInvalidState` response without mutation or version change. Later layers
replace those explicit rejections with their real behavior; no path returns
placeholder success. Snapshot requests are read-only and exempt from epoch
matching after domain and authority-id validation.

### Exit gate

The authority alone safely grants and releases no-wait range locks, expires
leases, issues fences, and survives duplicate command delivery.

## 8. Layer 3: Usable In-Memory Lock and Unlock

This is the first user-usable vertical slice:

```text
create manager -> no-wait requestLock -> getRequestStateForOwner
               -> releaseLock -> shutdown
```

### 3A. In-memory backend file and lifetime scaffold

Create:

- `include/dmn-dlock-backend-memory.hpp`;
- `src/dmn-dlock-backend-memory.cpp`.

Register both with `dmn` and add the public header to `include/dmn.hpp`. Build
the empty files first.

Declare `Dmn_DLock_InMemory_Backend final` with its final inheritance from
`Dmn_DLock_Backend`, `create()` factory, out-of-line `noexcept` destructor, and
deleted copy/move operations. Declare all four final overrides with `override`.
Define factory validation, construction, and destruction in the `.cpp`, then
build `dmn` without instantiating the class. Add compile-time base-class,
signature, and copy/move checks that do not odr-use its unresolved vtable.

### 3B. In-memory backend behavior

First implement the smallest complete real lifecycle slice for all inherited
operations:

- `start()` installs callbacks and schedules the initial authority snapshot;
- `submit()` serializes authority processing and schedules reply/snapshot
  callbacks;
- `requestSnapshot()` schedules the current snapshot;
- `shutdown()` closes submission and drains callback delivery.

Do not add placeholder bodies. Once all overrides link, add construction and
destruction smoke tests:

- valid factory construction and scope-exit destruction;
- invalid authority config/option rejection;
- explicit shutdown before destruction;
- `static_assert` base-class and copy/move traits.

Build and run the authority target. Then add one fault/lifecycle test and
implementation refinement at a time:

- backend starts once with snapshot/error callbacks;
- second start fails;
- submit callback is asynchronous and authority entry is serialized;
- deterministic callback drain and maintenance wake;
- shutdown rejects submissions and delivers no later callback.

Add `start()`, `submit()`, `requestSnapshot()`, and `shutdown()` only with
their real behavior. The backend owns one authority and one serialized
execution lane. Its maintenance runs on that lane. Each backend instance is
exclusive to one manager.

### 3C. Manager lifetime scaffold

`include/dmn-dlock.hpp` and `src/dmn-dlock.cpp` already exist from Layer 1.
Declare `Dmn_DLock_Manager final` with:

- final `enable_shared_from_this` inheritance;
- `create()` with its final arguments;
- an out-of-line `noexcept` destructor;
- `shutdown() noexcept`;
- deleted copy/move operations;
- only the private state needed for empty running/closing/closed lifetime.

Define construction, transactional startup, destruction, and idempotent
shutdown in `src/dmn-dlock.cpp`. Do not declare request/query/release methods
yet. Build `dmn`.

Extend `dmn-test-dlock.cpp` with:

- valid manager construction through `create()`;
- scope-exit destruction;
- explicit repeated shutdown;
- invalid configuration/null backend rejection;
- startup failure rollback;
- `static_assert` inheritance and copy/move traits.

Build and run `dmn-test-dlock`.

### 3D. Manager lock/query/unlock behavior

Add one focused test and its method implementation at a time:

- manager configuration/request validation;
- backend startup delivers the initial authority snapshot;
- a no-wait request admitted before bootstrap does not submit a mutation with
  an unknown epoch;
- no-wait bootstrap plus reply share one
  `authority_response_timeout` budget;
- bootstrap completion releases the no-wait submission, while bootstrap
  timeout removes an unsent request without authority cleanup;
- no-wait grant returns request id, fence, and conservative local deadline;
- no-wait overlap returns conflict and retains no ordinary request;
- owner-safe query hides owner mismatch as not found;
- release success, repeated release, wrong owner, and unknown request;
- response timeout retains cleanup state and a late grant is released;
- local submission failure returns an empty request id;
- multiple manager/backend pairs are independent;
- shutdown wakes any API completion waiter.

Introduce the operational methods in this order:

1. bootstrap state and initial snapshot handling;
2. `getRequestStateForOwner()` for unknown/local records;
3. no-wait `requestLock()` validation and local admission;
4. no-wait command submission/reply mapping;
5. conservative lease deadline and granted query;
6. overlap conflict projection;
7. `releaseLock()` and idempotent released query;
8. response-timeout and late-grant cleanup.

The manager owns local lifecycle state and a minimal immediate worker. Backend
callbacks capture weak state, copy values to the worker, and never wait.
External backend/emitter calls occur without the lifecycle mutex.

Track an explicit unset/bootstrap-complete authority epoch. Backend `start()`
initiates delivery of the current full snapshot. The manager may admit a
no-wait call locally but does not submit its mutation until that snapshot is
validated and applied. Bootstrap and reply consume one
`authority_response_timeout` deadline measured from local admission. If the
deadline expires before submission, atomically remove the unsent operation and
return publisher error with its admitted request id; no cleanup tombstone is
needed because delivery was impossible.

Measure send/receive times for every attempt. Derive the local deadline only
from `reply.entry->lease_remaining` using the Section 13 RTT formula. Snapshot
timing may shorten but never establish or extend a usable lease.

Implement cleanup for an undisclosed late no-wait grant before declaring this
layer complete. Basic lock/unlock is not usable if a timed-out call can leave
an unknown grant.

### Exit gate

A user can safely lock, query, and unlock an inclusive range through the
in-memory backend. Overlap is excluded, leases/fences are returned, timeout
cleanup works, and no base-library implementation changed.

## 9. Layer 4: Queued Acquisition and Cancellation

Add deterministic contention without adding retry complexity.

### 4A. Authority waiting behavior

Add one failing authority test and its minimum implementation in this order:

1. waiting create is accepted but not granted over an existing overlap;
2. priority/sequence ordering;
3. simultaneous non-overlapping grants;
4. earlier overlapping waiter prevents bypass;
5. cancel and owner/state validation;
6. acquisition-expiry maintenance and reevaluation.

The protocol variants already exist. Replace their Layer 2 permanent
`kInvalidState` responses only when the corresponding test is added.

### 4B. Manager API behavior

Add each manager method declaration together with its first real test and
implementation:

1. `requestLockAsync()` local validation and accepted waiting result;
2. async snapshot/reply projection;
3. blocking `requestLock()` using the condition predicate;
4. timeout final-predicate check and cleanup;
5. `cancelRequest()` explicit cancellation;
6. cancel-token maintenance.

Cover:

- async acceptance returns waiting plus request id;
- after bootstrap, accepted asynchronous work submits without a second
  bootstrap path;
- blocking acquisition grants without a missed wakeup;
- final timeout predicate lets a simultaneous visible grant win;
- blocking timeout enqueues cancel and never exposes a later grant;
- explicit cancel and cancel token;
- acquisition expiry reevaluates later waiters;
- another request progresses while an API thread waits;
- owner and invalid-state rules for cancellation.

Replace the minimal worker with the final internal immediate FIFO plus
condition-variable notification contract. Blocking occurs only in the API
caller. Add cancellation-token observation through deterministic maintenance.

Introduce no future API in placeholder form.

### Exit gate

The in-memory feature supports no-wait, blocking, and asynchronous acquisition,
including cancellation and acquisition expiry, with deterministic contention.

## 10. Layer 5: Renewal and Mutation Serialization

### 5A. Renewal method increments

Add one authority test and implementation for renew validation, successful
renewal, fence preservation, and authority-time extension. Then declare
`renewLock()` on the manager together with its first public API test and real
submission/reply implementation.

### 5B. Mutation serialization increments

Add these tests and their minimum state transitions one at a time:

- renewal extends from authority time and preserves the fence;
- renewal of waiting, expired, terminal, or foreign requests;
- local expiry occurs conservatively when RTT consumes the reply duration;
- snapshot can only shorten a local lease;
- identical concurrent cancel/release calls join one completion;
- identical renewals join only when lease duration also matches;
- different-payload or incompatible mutations return `kInvalidState`;
- release intent prevents admission or application of a late renewal;
- response timeout behavior for cancel, renew, and release;
- setting a cancel token after a visible grant does not release it.

Implement the Section 7.9 per-request mutation gate:

- one active caller-originated mutation;
- identical business payload joins its completion;
- release intent is sticky;
- late renewal never extends state after release intent;
- backend calls remain outside manager locks.

Add local lease-expiry maintenance and stop protected use at the conservative
deadline. Renewal is manual; do not add auto-renewal.

### Exit gate

The full normal in-memory lifecycle—acquire, wait, cancel, renew, release, and
expire—is complete.

## 11. Layer 6: Snapshot, Retry, Epoch, and Cleanup Resilience

### 6A. Delayed-executor scaffold

The internal executor interface already exists in
`src/dmn-dlock-internal.hpp`. Add its concrete production implementation to
`src/dmn-dlock.cpp` or a new lock-private `.cpp`. If a new `.cpp` is used,
first create/register/build the empty file.

Declare the executor with an out-of-line `noexcept` destructor and deleted
copy/move operations. Build `dmn`, then add a test-only construction/lifetime
smoke test through `detail::Dmn_DLock_Manager_Test_Access`.

Implement and test immediate FIFO, delayed ordering, cancellation, manual
wake, close, drain, and join one operation at a time before connecting retry
behavior to manager requests.

### 6B. Resilience behavior

Add these tests and their implementation one item at a time:

- newer snapshot atomically replaces the mirror;
- older snapshot is ignored;
- structurally identical same-version metadata only shortens deadlines;
- reordered larger same-version remaining duration is ignored;
- same-version structural divergence is a protocol error;
- overlapping grants in a snapshot are rejected;
- wrong domain/authority is reported and ignored;
- higher generation replaces the epoch and terminalizes old requests;
- lower/retired generation is ignored;
- same-generation foreign nonce is a protocol error;
- snapshot-discovered live grant replays create for fresh lease metadata;
- snapshot-discovered terminal grant enters cleanup rather than becoming
  visible;
- version conflict updates only expected version and reuses operation id;
- dropped/retryable response uses bounded exponential jitter;
- permanent backend error is terminal;
- reconnect/protocol-gap error requests a snapshot;
- malformed/fatal backend error terminalizes affected requests;
- cleanup cancel invalid-state transitions to release;
- cleanup cancel not-found replays the original create;
- terminalization cancels original retries but preserves cleanup.

The final delayed executor provides:

- immediate FIFO;
- delayed min-heap ordered by due time/insertion sequence;
- per-request cancellation;
- overflow-safe saturated backoff and injected jitter;
- deterministic `runReady()`/`advanceAndRun()` test control;
- close, drain, and join.

Implement one atomic snapshot-application path and the typed backend error
path. Snapshot request commands may use an unknown/stale epoch; mutations may
not. Keep original-operation replay and cleanup-release operation ids distinct.

### Exit gate

Duplicate, lost, delayed, reordered, and epoch-changing deliveries converge
without exposing an invalid grant or blocking the worker.

## 12. Layer 7: Retention and Strict Lifetime

### 7A. Retention increments

Add one test and implementation at a time for:

- terminal record remains before TTL and compacts after TTL without query;
- compact authority tombstones retain operation outcomes for the epoch;
- granted records are never TTL-pruned;
- query observes pruning committed before it acquires the lifecycle mutex.

### 7B. Shutdown/lifetime increments

After retention passes, add explicit barriers and implement one shutdown
property at a time:

- shutdown wakes blocking callers and cancels delayed retries;
- callback racing shutdown cannot access destroyed state;
- worker/callback tasks do not strongly own the public manager;
- query remains available after shutdown;
- post-shutdown mutations return shutdown;
- unreleased grants become unusable at their local deadline;
- backend shutdown returns only after callback dispatch leaves;
- no callback, task, or emitter runs after manager destruction.

Add `retention_version`, periodic manager compaction, authority-lifetime
tombstones, inflight accounting, and explicit running/closing/closed states.

Use the exact shutdown order from specification Section 15:

1. close public mutation/retry acceptance and terminalize non-granted work;
2. notify API waiters;
3. stop backend callback production;
4. drain already accepted manager work without new transport submission;
5. join the worker and mark closed;
6. perform final local retention sweep.

Do not claim transport cleanup after backend closure; finite leases are the
recovery boundary.

### Exit gate

Manager/backend destruction is deterministic and race-safe under explicit test
barriers.

## 13. Layer 8: Observability

### 8A. Event emission increments

Add one transition event at a time: first its failing emitter test, then event
construction under the lifecycle lock, then post-lock delivery. Cover every
event type and required payload, retry attempt/delay, authority restart,
emitter exception during immediate result, and emitter exception during later
state projection.

Build immutable events while committing state, then call
`Dmn_DLock_Event_Emitter::emit()` after releasing locks on the manager worker.
Catch exceptions, report with `DMN_DEBUG_PRINT(std::cerr << ...)`, and do not
change lock state/result. The diagnostic is best-effort and may compile out in
release builds.

### Exit gate

Every lifecycle transition is observable without making observability part of
correctness or object ownership.

## 14. Layer 9: Protobuf Without Transport

### 9A. Schema and conversion file scaffold

Create:

- `src/proto/dmn-dlock.proto` with syntax, package, and an initially empty
  `DLockEnvelopePb`;
- `include/dmn-dlock-pb-util.hpp`;
- `src/dmn-dlock-pb-util.cpp`.

Add the new proto to `GENERATE_PROTOBUF`, register its generated source/header,
and register the conversion source/header. Build `dmn` before changing the
existing DMesg schemas.

Then make only the approved additive edits:

- `DMesgTypePb.dlock = 2`;
- `DMesgBodyPb.dlock = 2` plus the lock-proto import.

Build `dmn` again. Existing field numbers and enum values must be unchanged.

### 9B. Protobuf smoke test

Create/register `test/dmn-test-dlock-dmesg.cpp`. Construct, serialize, parse,
and destroy an empty/default `DLockEnvelopePb`. Add compatibility smoke tests
for existing `sys` and `message` bodies before adding lock fields or conversion
behavior. Build and run only `dmn-test-dlock-dmesg`.

### 9C. Schema and conversion behavior

Add one schema variant, conversion function, and round-trip/error test at a
time. Cover:

- every command/reply/snapshot/status/reason/fence round trip;
- missing fields, unknown variants, invalid durations, and identity mismatch;
- `sys` and existing `message` body round trips remain unchanged;
- enum/field numbers for existing DMesg values remain unchanged.

Use explicit zero enum values. Serialize checked millisecond durations, never
`steady_clock::time_point`. Add generated files to CMake. Do not modify
hand-written DMesg C++.

### Exit gate

The complete wire model converts independently of handlers and preserves
existing protobuf compatibility.

## 15. Layer 10: DMesg Backend and Authority Service

### 10A. Public file scaffolds

Create:

- `include/dmn-dlock-backend-dmesg.hpp`;
- `include/dmn-dlock-authority-service-dmesg.hpp`;
- `src/dmn-dlock-backend-dmesg.cpp`;
- `src/dmn-dlock-authority-service-dmesg.cpp`.

Register all four files and add the public headers to `include/dmn.hpp`. Build
`dmn` while the new files contain only prologues, guards/includes, and
namespaces.

### 10B. DMesg backend lifetime scaffold

Declare `Dmn_DLock_DMesg_Backend final` with its final backend inheritance,
factory, out-of-line `noexcept` destructor, and deleted copy/move operations.
Declare all four final overrides with `override`. Define only argument
validation, construction, and destruction in its `.cpp`. Build `dmn` without
instantiating it, and add compile-time inheritance/signature/copy-move checks
that do not odr-use the unresolved vtable.

### 10C. Authority-service lifetime scaffold

Declare `Dmn_DLock_DMesg_Authority_Service final` with its factory,
out-of-line `noexcept` destructor, and deleted copy/move operations. Define
only argument validation, construction, and destruction. Build `dmn`, then add
construction/destruction and type-trait smoke tests.

### 10D. DMesg behavior micro-increments

First implement the smallest complete real backend lifecycle:

- `start()` opens its client handlers and initiates bootstrap;
- `submit()` publishes a validated command;
- `requestSnapshot()` publishes a snapshot request;
- `shutdown()` closes handlers and drains callback dispatch.

Then add backend construction/destruction smoke tests using an existing
in-process `Dmn_DMesg`. After that checkpoint, implement the authority-service
startup and each transport behavior below one focused test at a time.

Using an in-process DMesg harness and explicit barriers, cover:

- concrete factories, exclusive start, rollback, and idempotent shutdown;
- canonical unpadded Base64url topic tokens and envelope matching;
- client-specific command topics and independent running counters;
- no-topic-filter authority rejects unrelated messages;
- command/reply correlation and full snapshot delivery;
- fresh client bootstrap before mutation;
- snapshot request with empty/stale epoch returns the current snapshot;
- mutations with stale epoch are rejected;
- command and prior-incarnation reply playback are ignored;
- duplicate/reordered reply and snapshot convergence;
- reconnect and running-counter conflict recovery;
- publication occurs outside DMesg callback context;
- malformed envelope uses typed backend error;
- reply/snapshot publication conflict recovers or stops the service;
- unexpected authority generation/nonce behavior;
- shutdown closes handlers and delivers no later callback.

The client backend owns command, reply, and snapshot handlers. The authority
service owns a synchronous authority and a dedicated serialized executor.
DMesg callbacks only validate framing enough to copy and enqueue. Publication,
counter inspection, conflict recovery, snapshot requests, and handler closure
run outside callback context.

Use:

```text
dmn.dlock.v1.<domain-token>.command.<client-token>
dmn.dlock.v1.<domain-token>.reply.<client-token>
dmn.dlock.v1.<domain-token>.snapshot
```

The authority is the sole reply/snapshot writer. Each client is the sole writer
of its command topic. Do not use `sys`, `Dmn_DMesgNet`, or master election.

Use only lock-specific friend test accessors for callback drains, maintenance
wakes, barriers, and injected faults. Do not add hooks to `Dmn_DMesg`.

### Exit gate

Two clients using one configured authority preserve the same semantics as the
in-memory backend under duplicate, reorder, reconnect, and conflict faults.

## 16. Layer 11: Contention, Documentation, and Regression

### Tests first

Add bounded integration tests:

- many overlapping clients never hold overlapping grants;
- non-overlapping clients progress concurrently;
- timeout/cancel/release churn leaves no active orphan;
- delayed grants after caller timeout are released;
- authority restart terminates old-epoch requests;
- shutdown during contention terminates under test control;
- fencing tokens increase monotonically;
- a shared monitor fails immediately on overlapping granted snapshots.

### Documentation

Update `README` with:

- no-wait lock/unlock and queued acquisition examples;
- one-authority-per-domain deployment requirement;
- resource-side fence propagation and validation;
- renewal timing and conservative local lease use;
- release-before-shutdown ordering;
- client-specific topics and no automatic failover;
- future DMesgNet constraints.

Add repository-standard Doxygen prologues and register compatible lock tests
for valgrind.

### Final checks

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_VALGRIND=ON
cmake --build build
ctest --test-dir build -R '^dmn-test-dlock' --output-on-failure
ctest --test-dir build -L dmn --output-on-failure
ctest --test-dir build -L valgrind --output-on-failure
```

If valgrind is unavailable, configure without it and record the environmental
limitation rather than installing a new tool solely for this feature.

Inspect `git diff --name-only`. Apart from the explicitly approved protobuf,
CMake, and umbrella-header edits, every changed production file must be
lock-specific.

## 17. Suggested Commit Boundaries

Each commit includes its tests and remains buildable:

1. lock values and transport-neutral contracts;
2. minimal safe authority;
3. usable in-memory no-wait lock/unlock;
4. queued acquisition and cancellation;
5. renewal and mutation serialization;
6. snapshot/retry/epoch resilience;
7. retention and shutdown hardening;
8. observability;
9. protobuf schema and conversion;
10. DMesg backend and authority service;
11. contention, documentation, and regression.

## 18. Definition of Done

- Layer 3 proves a usable in-memory lock/query/unlock path before advanced
  features are added.
- Every final public method has success, invalid-input, ownership, terminal,
  timeout, and shutdown behavior where applicable.
- Authority tests prove exclusivity, deterministic ordering, idempotency,
  leases, expiry, and fencing.
- Manager tests prove caller-only waiting, missed-wakeup safety, cleanup,
  retention, and teardown.
- DMesg tests prove filtering, compatibility, callback safety, and convergence
  under duplicate/reordered/lost delivery.
- No correctness test uses arbitrary sleep.
- Existing `dmn` tests and existing DMesg protobuf compatibility tests pass.
- No base-library C++ behavior or public API changes.
- Only approved additive protobuf/CMake/umbrella integration edits exist.
- Documentation states that one configured authority and resource-side fencing
  are required for safety.
