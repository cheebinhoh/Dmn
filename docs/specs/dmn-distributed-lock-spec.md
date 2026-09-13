# Feature Specification: DMN Distributed Range Lock (`Dmn_DLock`)

Status: Accepted for incremental implementation.

## 1. Purpose

`Dmn_DLock` provides exclusive, owner-scoped locks over inclusive integer
ranges. It is designed as a correctness core plus replaceable transport
adapters:

- one authority serializes all mutations for a lock domain;
- clients keep a versioned mirror for queries and wait predicates;
- blocking APIs wait only in the caller thread;
- asynchronous workers publish commands, process replies, and schedule retries,
  but never wait on a condition variable;
- leases recover locks after client failure;
- fencing tokens let protected resources reject stale owners.

This specification is normative unless a section is explicitly marked
informative.

## 2. Repository Review and Design Consequences

The design must fit the current implementation rather than assume facilities
that do not exist.

### 2.1 Reusable repository facilities

- `Dmn_DMesg` provides topic-based publication, latest-message playback, and a
  per-topic running counter (`include/dmn-dmesg.hpp`,
  `src/dmn-dmesg.cpp`).
- `Dmn_DMesgNet` transports `DMesgPb` messages and maintains eventual
  membership/master information (`include/dmn-dmesgnet.hpp`,
  `src/dmn-dmesgnet.cpp`).
- `Dmn_Runtime_Manager` provides process-wide serialized immediate and timed
  jobs (`include/dmn-runtime.hpp`).
- `Dmn_Proc`, `Dmn_BlockingQueue_Mt`, and `Dmn_Inflight_Guard` provide the
  primitives needed for a stoppable manager-owned worker.
- Protobuf generation is centralized by `GENERATE_PROTOBUF` in
  `CMakeMacro.cmake`.
- Tests are standalone GoogleTest executables registered through
  `ADD_TEST_EXECUTABLE` in `test/CMakeLists.txt`.

### 2.2 Constraints that change the original draft

1. `Dmn_DMesgNet` master election is eventual membership convergence, not a
   quorum or consensus protocol. Two partitioned nodes can both consider
   themselves master. It therefore MUST NOT be used as the lock safety
   authority or automatic failover mechanism in v1.
2. A `DMesgPb.runningCounter` detects stale topic writers. It is transport
   metadata, not the lock table's compare-and-swap version. `table_version`
   remains an explicit lock-protocol field.
3. `Dmn_Runtime_Manager` timed jobs use process-wide timed scheduling and are
   not individually drained by lock-manager ownership. `Dmn_Async` also lacks
   the cancellable per-owner deadline queue required here. Strict lock-manager
   shutdown therefore requires a manager-owned stoppable executor.
4. `Dmn_Singleton` permanently preserves the first constructor arguments.
   That prevents isolated domains/configurations and makes shutdown-oriented
   unit tests order-dependent. `Dmn_DLock_Manager` is consequently a regular
   shared-owned object created by a factory, not a process-wide singleton.
5. Existing network tests frequently use sleeps. New lock tests MUST use
   explicit barriers, futures, fake clocks, and deterministic fault injection.

## 3. Goals, Scope, and Non-goals

### 3.1 Goals

- Exclusive locking for inclusive ranges `[start, end]`.
- Multiple concurrent grants for non-overlapping ranges.
- Deterministic ordering among overlapping contenders.
- Blocking, no-wait, and asynchronous acquisition APIs.
- Owner-checked query, cancellation, renewal, and release.
- Recovery from a failed lock holder through bounded leases.
- Idempotent command processing under duplicate delivery.
- Versioned full-snapshot mirror convergence after loss or reordering.
- Deterministic shutdown and testable timing behavior.

### 3.2 V1 safety boundary

Each `domain` has exactly one configured, live authority at a time. All
mutations for that domain MUST reach that authority. The authority processes
commands serially.

V1 transports commands through `Dmn_DMesg`. A future implementation MAY add a
`Dmn_DMesgNet` backend, but its election does not select or fail over the lock
authority. Starting two authorities for one domain is an invalid deployment
and voids the mutual-exclusion guarantee.

### 3.3 Non-goals

- quorum consensus, automatic leader failover, or partition-tolerant
  linearizability;
- shared/read locks;
- lock upgrades, downgrades, or atomic multi-range acquisition;
- reentrant acquisition;
- persistent authority state across process restart;
- authentication or authorization of `owner_id`;
- deadlock detection across multiple independently acquired locks;
- starvation freedom in v1.

## 4. Terminology and Core Invariants

| Term | Meaning |
|---|---|
| Domain | Independent lock namespace, identified by a non-empty string. |
| Authority | The only component permitted to order and commit mutations for a domain. |
| Client manager | A `Dmn_DLock_Manager` that submits commands and mirrors authority snapshots. |
| Request | One acquisition attempt, identified by `request_id`. |
| Operation | One create/cancel/release/renew command, identified by `operation_id`. |
| Lease | Authority-clock interval during which a granted lock remains valid. |
| Authority generation | Deployment-supplied, monotonically increasing authority incarnation number. |
| Authority epoch | `(authority_generation, authority_nonce)` identity for one authority process. |
| Fence | `(authority_generation, authority_nonce, fencing_token)` returned with a grant. |
| Active entry | A waiting, granting, or granted request participating in ordering. |
| Terminal record | Retained result metadata for released, cancelled, timed-out, expired, failed, or shutdown requests. |

The following invariants are mandatory:

- **I-1 Single authority:** only one authority commits a domain at a time.
- **I-2 Range exclusivity:** no two granted entries in one domain overlap.
- **I-3 Immutable identity:** `request_id`, `owner_id`, range, priority, and
  `sequence` never change after authority acceptance.
- **I-4 Monotonic version:** `table_version` increases exactly once for each
  committed authority transaction that changes snapshot-visible state. A
  transaction may include its triggering command or maintenance action plus
  all resulting grant reevaluation. Duplicate operations and no-op
  transactions do not increment it.
- **I-5 Monotonic fencing:** authority generation increases across authority
  restarts; within one generation/nonce, each grant receives a fencing token
  greater than every token previously issued in that epoch.
- **I-6 One-way terminality:** a terminal request never returns to an active
  state and a `request_id` is never reused.
- **I-7 Idempotency:** replaying a completed `operation_id` returns its cached
  outcome without applying the mutation twice; retryable version conflicts do
  not complete the operation id.
- **I-8 Bounded waiting:** every accepted asynchronous or blocking acquisition
  has a finite acquisition expiry.
- **I-9 Bounded ownership:** every grant has a finite lease expiry.
- **I-10 Worker non-blocking:** no manager worker or transport callback blocks
  on a client wait predicate.
- **I-11 Owner privacy:** an owner-scoped query for another owner returns
  `kNotFound`, not `kNotOwner`.
- **I-12 No abandoned grant:** if an authority grant is not returned to the
  caller because the caller timed out, cancelled, shut down, or lost the
  response race, the manager submits an idempotent release as soon as that
  grant becomes known while transport remains open. After transport closure,
  the finite authority lease is the recovery mechanism.

## 5. Range and Ordering Semantics

### 5.1 Range model

Ranges contain signed 64-bit non-negative endpoints and are inclusive:

```cpp
struct Dmn_DLock_Range {
  std::int64_t start{};
  std::int64_t end{};

  [[nodiscard]] constexpr auto isValid() const noexcept -> bool;
  [[nodiscard]] constexpr auto overlaps(
      const Dmn_DLock_Range &other) const noexcept -> bool;
};
```

A range is valid when `0 <= start && start <= end`. Two ranges overlap when:

```text
a.start <= b.end && b.start <= a.end
```

Thus `[1, 2]` and `[2, 3]` conflict, while `[1, 2]` and `[3, 4]` do not.

### 5.2 Request rank

The authority assigns a monotonically increasing `sequence` when it accepts a
new request. Rank is:

1. higher `priority` first;
2. lower `sequence` first.

Range endpoints are not ordering keys. They only determine whether two
requests contend. This avoids the original draft's incorrect implication that
sorting by `start` and `end` defines ownership.

### 5.3 Grant rule

A waiting request is grantable if:

1. it overlaps no granted request; and
2. it overlaps no active waiting/granting request with a better rank.

After every accepted create, release, cancel, or lease-expiry mutation, the
authority evaluates waiting requests in rank order and grants every request
that satisfies the rule. This permits concurrent non-overlapping locks while
preventing a later overlapping request from bypassing an earlier contender.

Priority may starve lower-priority requests. Acquisition expiry bounds the
lifetime of that starvation; priority aging is deferred.

## 6. Public Data Model

```cpp
namespace dmn {

enum class Dmn_DLock_State {
  kWaiting,
  kGranting,
  kGranted,
  kReleased
};

enum class Dmn_DLock_Terminal_Reason {
  kNone,
  kReleased,
  kCancelled,
  kAcquireTimeout,
  kLeaseExpired,
  kPublisherError,
  kShutdown,
  kAuthorityRestarted
};

struct Dmn_DLock_Entry {
  std::string domain;
  Dmn_DLock_Range range;
  Dmn_DLock_State state{Dmn_DLock_State::kWaiting};
  Dmn_DLock_Terminal_Reason terminal_reason{
      Dmn_DLock_Terminal_Reason::kNone};
  std::string request_id;
  std::string owner_id;
  int priority{};
  std::uint64_t sequence{};
  std::uint64_t authority_generation{};
  std::string authority_nonce;
  std::optional<std::uint64_t> fencing_token;
  std::optional<std::chrono::steady_clock::time_point> lease_deadline;
  std::chrono::steady_clock::time_point created_at;
  std::optional<std::chrono::steady_clock::time_point> terminal_at;
};

struct Dmn_DLock_Config {
  std::string domain;
  std::string client_id;
  std::string authority_id;
  std::uint64_t minimum_authority_generation{};
  std::chrono::milliseconds default_async_expiry{std::chrono::minutes{5}};
  std::chrono::milliseconds default_lease_duration{std::chrono::seconds{30}};
  std::chrono::milliseconds maximum_acquire_expiry{std::chrono::hours{1}};
  std::chrono::milliseconds maximum_lease_duration{std::chrono::minutes{5}};
  std::chrono::milliseconds authority_response_timeout{
      std::chrono::seconds{2}};
  std::chrono::milliseconds retry_min_backoff{std::chrono::milliseconds{10}};
  std::chrono::milliseconds retry_max_backoff{std::chrono::milliseconds{500}};
  double retry_jitter_ratio{0.20};
  std::chrono::milliseconds retained_terminal_ttl{std::chrono::hours{1}};
  std::chrono::milliseconds maintenance_interval{
      std::chrono::milliseconds{100}};
};

struct Dmn_DLock_Authority_Config {
  std::string domain;
  std::string authority_id;
  std::uint64_t authority_generation{};
  std::chrono::milliseconds maximum_acquire_expiry{std::chrono::hours{1}};
  std::chrono::milliseconds maximum_lease_duration{std::chrono::minutes{5}};
  std::chrono::milliseconds retained_terminal_ttl{std::chrono::hours{1}};
  std::chrono::milliseconds maintenance_interval{
      std::chrono::milliseconds{100}};
};

struct Dmn_DLock_RequestOptions {
  std::string owner_id;
  int priority{};
  std::chrono::milliseconds wait_timeout{};
  std::optional<std::chrono::milliseconds> acquire_expiry;
  std::optional<std::chrono::milliseconds> lease_duration;
  std::shared_ptr<std::atomic_bool> cancel_token;
};

struct Dmn_DLock_Result {
  enum class Code {
    kGranted,
    kWaiting,
    kReleased,
    kConflict,
    kTimeout,
    kLeaseExpired,
    kCancelled,
    kNotOwner,
    kNotFound,
    kInvalidState,
    kInvalidArg,
    kLocalSubmissionError,
    kPublisherError,
    kAuthorityRestarted,
    kShutdown
  };

  Code code{Code::kInvalidArg};
  std::string message;
  std::string request_id;
  std::string owner_id;
  std::uint64_t table_version{};
  std::optional<Dmn_DLock_Entry> entry;
};

class Dmn_DLock_Backend;
class Dmn_DLock_Event_Emitter;

class Dmn_DLock_Manager final
    : public std::enable_shared_from_this<Dmn_DLock_Manager> {
public:
  static auto create(
      Dmn_DLock_Config config,
      std::shared_ptr<Dmn_DLock_Backend> backend,
      std::shared_ptr<Dmn_DLock_Event_Emitter> emitter = {})
      -> std::shared_ptr<Dmn_DLock_Manager>;

  auto requestLock(Dmn_DLock_Range range,
                   const Dmn_DLock_RequestOptions &options)
      -> Dmn_DLock_Result;

  auto requestLockAsync(Dmn_DLock_Range range,
                        const Dmn_DLock_RequestOptions &options)
      -> Dmn_DLock_Result;

  auto renewLock(std::string_view request_id, std::string_view owner_id,
                 std::chrono::milliseconds lease_duration)
      -> Dmn_DLock_Result;

  auto releaseLock(std::string_view request_id, std::string_view owner_id)
      -> Dmn_DLock_Result;

  auto cancelRequest(std::string_view request_id, std::string_view owner_id)
      -> Dmn_DLock_Result;

  auto getRequestStateForOwner(std::string_view request_id,
                               std::string_view owner_id) const
      -> Dmn_DLock_Result;

  void shutdown() noexcept;
};

} // namespace dmn
```

### 6.1 Construction

`create()` validates configuration before starting threads. Invalid
configuration throws `std::invalid_argument`; a null backend also throws.
Construction is all-or-nothing. The factory is required so worker callbacks
never observe a partially constructed object.

Multiple managers may exist in one process. `(domain, client_id)` SHOULD be
unique among live managers. `authority_id` identifies the one configured
authority for the domain; it is not inferred from `Dmn_DMesgNet` membership or
master state.

`Dmn_DLock_Entry::lease_deadline` is a client-local conservative
`steady_clock` deadline derived from lease validity received from the
authority. It is useful for deciding when the local client must stop using a
grant. It is not serialized and is never compared with another process's
`steady_clock` value.

### 6.2 Configuration validation

- `domain`, `client_id`, and `authority_id` are non-empty.
- `minimum_authority_generation` may be zero to accept the first observed
  generation, or may pin a deployment minimum;
- all configured durations are greater than zero;
- `default_async_expiry <= maximum_acquire_expiry`;
- `default_lease_duration <= maximum_lease_duration`;
- `retry_min_backoff <= retry_max_backoff`;
- `retry_jitter_ratio` is finite and in `[0.0, 1.0]`.

Authority configuration is validated independently: identifiers are non-empty,
`authority_generation` is non-zero, all durations are positive, and both
maximum durations fit the protocol's checked millisecond representation.
Authority limits are final even when a client's locally configured limits are
more permissive.

Request validation:

- range is valid;
- `owner_id` is non-empty;
- `wait_timeout >= 0`;
- explicit `acquire_expiry` and `lease_duration` are greater than zero;
- explicit `acquire_expiry <= maximum_acquire_expiry` and
  `lease_duration <= maximum_lease_duration`;
- the effective acquisition expiry is no greater than
  `maximum_acquire_expiry`;
- `requestLock()` requires:
  - `wait_timeout == 0` and no explicit `acquire_expiry` for no-wait; or
  - `wait_timeout > 0` and effective acquisition expiry not shorter than the
    wait timeout;
- `requestLockAsync()` ignores `wait_timeout` only if it is zero; a non-zero
  value is invalid to avoid ambiguous behavior.

The effective acquisition expiry is:

- explicit `acquire_expiry`, when present;
- otherwise `default_async_expiry` for asynchronous requests;
- otherwise `wait_timeout` for blocking requests;
- not applicable for no-wait requests.

No-wait requests have no waiting lifetime at the authority: they either grant
in their create transaction or create no authority record.

For blocking and asynchronous requests, the acquisition deadline starts when
the manager accepts the request locally, including time spent waiting for
authority bootstrap or retry backoff. Every create attempt carries only the
remaining duration; neither delivery nor retry can extend the original
deadline.

## 7. API Semantics

### 7.1 `requestLock()` no-wait mode

When `wait_timeout == 0`:

Here "no-wait" means the request never queues behind another lock at the
authority. The API call still waits for one authority response, bounded by
`authority_response_timeout`.

1. validate before accepting lifecycle state;
2. allocate stable request and operation ids, submit one create operation, and
   wait only for its authority response, up to
   `authority_response_timeout`;
3. return `kGranted` if atomically accepted and granted;
4. return `kConflict` if accepted ordering would require waiting;
5. retain no ordinary lifecycle record for a confirmed `kConflict`;
6. never schedule a conflict retry.

A granted result contains a non-empty `request_id`, entry, authority epoch,
fencing token, and lease deadline.

Once the manager has allocated the ids and admitted the operation through its
local submission gate, every result contains the request id, including a
bootstrap or response timeout. If local admission fails, the result is
`kLocalSubmissionError` with an empty request id. If the response deadline
expires after admission, return `kPublisherError` and retain a cleanup
tombstone if any create attempt may have been delivered. A bootstrap timeout
atomically removes an unsent create and needs no authority cleanup. A cleanup
tombstone remains until a conflict is confirmed, a late grant is released, the
authority generation changes, or the manager shuts down. A late reply or
snapshot that reveals a grant MUST enqueue release; it MUST NOT expose the
grant to the caller after `requestLock()` returned an error. After manager
shutdown, the finite lease is final recovery for a command delivered later.

### 7.2 `requestLock()` blocking mode

When `wait_timeout > 0`:

1. create and retain local lifecycle state;
2. submit through the worker;
3. block only the API caller on the mirror predicate;
4. return `kGranted`, `kTimeout`, `kCancelled`, `kPublisherError`,
   `kAuthorityRestarted`, or `kShutdown`;
5. return the accepted `request_id` for every post-acceptance outcome.

On caller timeout, the manager atomically marks the local request terminal and
enqueues an idempotent cancel command. A grant already committed by the
authority wins over a simultaneous local timeout; the final under-lock
predicate check determines the immediate result. If a grant was committed but
was not visible by that final check, later reconciliation releases it under
I-12 rather than changing the already returned timeout into success.

### 7.3 `requestLockAsync()`

The method validates, creates local lifecycle state, and enqueues the initial
operation. It returns immediately:

- `kWaiting` plus non-empty `request_id` after local acceptance;
- `kInvalidArg`, `kShutdown`, or `kLocalSubmissionError` with empty
  `request_id` before acceptance.

`kWaiting` means local submission acceptance. The authority may grant before
the caller performs its first query.

The effective acquisition expiry is the explicit `acquire_expiry` or
`default_async_expiry`.

### 7.4 Query

`getRequestStateForOwner()` is read-only and concurrency-safe.

- waiting/granting -> `kWaiting`;
- granted -> `kGranted` with entry and fence;
- released -> `kReleased`;
- acquisition timeout -> `kTimeout`;
- lease expiry -> `kLeaseExpired`;
- cancellation -> `kCancelled`;
- publisher failure -> `kPublisherError`;
- authority epoch replacement -> `kAuthorityRestarted`;
- shutdown terminalization -> `kShutdown`.

Unknown requests and owner mismatches both return `kNotFound`. Queries do not
trigger transport calls or pruning.

### 7.5 Cancel

`cancelRequest()`:

- waiting/granting owned request: submit cancel and return its terminal result;
- granted owned request: `kInvalidState`; callers must release it;
- terminal owned request: return the persisted terminal result (idempotent);
- owner mismatch: `kNotOwner`;
- unknown request: `kNotFound`;
- after shutdown begins: `kShutdown`.

The cancel token has the same effect as owner-authorized cancellation but is
observed by maintenance/worker execution rather than a dedicated polling
thread. It applies only while the request is waiting/granting. Once a grant is
visible to the application, setting the token does not release the lock;
applications call `releaseLock()` explicitly. Token observation latency is
bounded by `maintenance_interval`.

### 7.6 Renew

`renewLock()` is valid only for an owned, currently granted, unexpired lock.
The authority computes the new deadline from its own monotonic clock; client
wall-clock values are never authoritative.

- success -> `kGranted` with unchanged fence and updated lease deadline;
- owner mismatch -> `kNotOwner`;
- waiting or terminal -> `kInvalidState`;
- already expired -> `kLeaseExpired`;
- unknown -> `kNotFound`.

Renewal is not automatic in v1. Applications must renew early enough to absorb
transport delay.

### 7.7 Release

`releaseLock()` is owner checked and idempotent:

- first successful release -> `kReleased`;
- repeated release by the same owner -> persisted `kReleased`;
- waiting request -> `kInvalidState`;
- owner mismatch -> `kNotOwner`;
- unknown/pruned request -> `kNotFound`.

Before shutdown starts, release follows the rules above. After shutdown starts,
new release calls return `kShutdown`; shutdown has already closed the
submission gate and cannot promise transport delivery. Applications SHOULD
release known grants before calling `shutdown()`. Any unreleased grant becomes
invalid at its conservative local lease deadline and is recovered at the
authority by lease expiry.

### 7.8 Common mutation-call completion

`cancelRequest()`, `renewLock()`, and `releaseLock()` are synchronous from the
caller's perspective but do not block the manager worker. Each creates a
request-local completion object, enqueues the operation, and lets only the API
caller wait up to `authority_response_timeout`.

- A confirmed authority reply returns the mapped result.
- A pre-acceptance local failure returns `kLocalSubmissionError`.
- A response timeout or permanent post-acceptance transport failure returns
  `kPublisherError` with the request id.
- A timed-out cancel or release continues retry/cleanup asynchronously while
  allowed; query exposes the eventual authority-confirmed request state.
- A timed-out renewal never extends the caller's current local lease deadline.
- Once release is requested, the caller MUST stop using the protected resource
  even if the release response is lost.

These rules prevent a blocked worker and prevent transport uncertainty from
being reported as a successful mutation.

## 8. Lifecycle State Machine

```text
local accepted
    |
    v
 kWaiting ----candidate selected----> kGranting ----authority commit----> kGranted
    |                                     |                                  |
    | cancel/timeout/shutdown/error        | cancel/shutdown/error            | release
    v                                     v                                  v
 kReleased(terminal reason)           kReleased(terminal reason)       kReleased
                                                                              ^
                                                                              |
                                                                    lease expiry
```

`kReleased` is the internal inactive state; `terminal_reason` distinguishes
release, cancellation, acquisition timeout, lease expiry, publisher failure,
shutdown, and authority restart.

Rules:

- terminal transitions are one-way;
- a late stale reply cannot overwrite terminal state;
- an authority-confirmed grant is returned only when it is visible at the
  caller's final predicate check; otherwise the manager preserves the returned
  terminal outcome and performs I-12 cleanup;
- release and lease expiry trigger grant reevaluation;
- the external result projection is defined in Section 7, not inferred from
  `Dmn_DLock_State` alone.

`kGranting` is authority-internal transient state used while one serialized
transaction selects grants and builds its resulting snapshot. Published
snapshots normally contain only waiting, granted, and terminal records.

## 9. Authority Model

### 9.1 Serialized command processing

The authority owns:

- active entries indexed by `request_id`;
- terminal records retained for deduplication/query;
- `table_version`;
- next request `sequence`;
- next `fencing_token`;
- deployment-supplied `authority_generation` and immutable random
  `authority_nonce`;
- operation result cache indexed by `operation_id`.

It processes one command at a time. For every command:

1. validate domain, authority target, and fields;
2. return the cached logical outcome if `operation_id` already completed,
   recomputing time-varying reply metadata such as lease remaining;
3. for an existing `request_id`, verify immutable request fields;
4. when the command carries an optional `expected_table_version`, compare it;
5. on mismatch return `kVersionConflict` plus the current full snapshot,
   without mutation and without finalizing the operation id;
6. apply the mutation;
7. reevaluate grants;
8. increment `table_version` once if the complete transaction changed
   snapshot-visible state;
9. cache the final logical outcome and publish the resulting full snapshot.

Validation failures that can never become valid are final and may be cached.
`kVersionConflict` is retryable and is not cached as a completed operation.
A retry reuses the operation id and immutable business payload but may update
`expected_table_version` from the returned snapshot. This avoids permanently
caching the first transient conflict while preserving exactly-once mutation.

The v1 public manager normally omits `expected_table_version` because current
authority state, ownership, and operation id are sufficient to serialize its
create/cancel/renew/release operations. The optional precondition remains in
the transport-neutral protocol for callers whose operation explicitly depends
on a mirrored version. This avoids version-conflict retry storms under normal
contention while retaining a testable compare-and-snapshot mechanism.

An accepted create records an authority-side acquisition deadline derived from
the command's remaining acquisition duration. The authority rejects an
acquisition or lease duration above its configured maximum. Authority
maintenance expires waiting requests. Retries of an accepted create cannot
extend that deadline.

### 9.2 Idempotency

Each logical mutation gets one stable `operation_id`. Retries reuse it.

- duplicate create cannot allocate a second sequence;
- duplicate release cannot unlock another request;
- duplicate renewal cannot extend the lease twice;
- duplicate cancel cannot replace a later grant/release result;
- cached operation results are retained at least as long as the corresponding
  terminal record.

The authority stores a digest or comparable immutable copy of the logical
operation payload with every completed operation id. Reuse of an operation id
for a different command kind, request, owner, range, priority, or lease
duration is a permanent protocol error. Retry metadata such as attempt number
and `expected_table_version` is excluded from that identity.

### 9.3 No-wait create

A no-wait create is a single authority transaction. The authority evaluates
whether it can grant before committing the create. If not immediately
grantable, it returns protocol conflict and creates no request record, consumes
no sequence or fencing token, and does not increment `table_version`.

`kConflict` is nevertheless a final, cached result for that operation id.
Duplicates received after the range becomes free return the cached conflict
and can never create a grant. Because this result has no request record, its
standalone operation-cache/tombstone entry is retained for the authority
process lifetime.

### 9.4 Authority restart

V1 authority state is in memory. Each start generates a new
random `authority_nonce`, but the deployment MUST also supply an
`authority_generation` strictly greater than the generation used by every
previous authority for that domain. Reusing a generation with a different
nonce is a split-authority/configuration error.

A client receiving a valid snapshot for the configured authority:

1. rejects a generation below its configured minimum or current generation;
2. treats the same generation with a different nonce as a protocol error;
3. accepts a higher generation and records the old epoch as retired;
4. atomically replaces its mirror;
5. terminalizes all locally retained requests from the old epoch as
   `kAuthorityRestarted`;
6. wakes waiters;
7. ignores later snapshots/replies from retired or lower generations.

Consumers of protected resources compare
`(authority_generation, fencing_token)` lexicographically and also require the
nonce associated with the accepted generation. The deployment must register a
new generation/nonce with the protected resource before the restarted
authority serves grants; registration invalidates every old-epoch fence.
Persisting lock-table contents is deferred, but external generation management
is required for restart-safe fencing.

Before a client has accepted its first authority snapshot, its mirror epoch and
version are unset. The manager may accept asynchronous requests locally but
does not submit them until bootstrap completes. No-wait/blocking requests count
bootstrap time against `authority_response_timeout`/`wait_timeout`.

### 9.5 Authority service ownership

`Dmn_DLock_Authority` is a synchronous, transport-neutral state machine.
`Dmn_DLock_DMesg_Authority_Service` owns one authority instance, one
single-threaded dispatch executor, and its DMesg handlers. Handler callbacks
only validate enough framing to copy an envelope into that executor. The
executor performs protocol validation, calls the authority, and publishes
reply/snapshot messages.

The service is configured with `Dmn_DLock_Authority_Config`, a fresh nonce,
clock, and DMesg handle. The in-memory backend and DMesg authority service each
own the scheduling that invokes authority maintenance on the same serialized
command lane; the manager never maintains authority state. It announces itself
by publishing an empty/current snapshot before accepting commands. If it
observes a snapshot for its domain with:

- a higher generation, it stops serving;
- the same generation and a different nonce, it stops serving and reports a
  split-authority protocol error;
- a lower generation, it ignores and reports the stale authority.

This detection is diagnostic containment, not consensus. Deployment still
guarantees I-1.

## 10. Backend and Transport Contracts

### 10.1 Backend interface

The public manager delegates transport; it does not subclass a backend.

```cpp
enum class Dmn_DLock_Submit_Code {
  kAccepted,
  kRetryableFailure,
  kPermanentFailure,
  kClosed
};

struct Dmn_DLock_Submit_Result {
  Dmn_DLock_Submit_Code code;
  std::string message;
};

class Dmn_DLock_Backend {
public:
  using ReplyHandler = std::function<void(Dmn_DLock_ProtocolReply)>;
  using SnapshotHandler = std::function<void(Dmn_DLock_Snapshot)>;

  virtual ~Dmn_DLock_Backend() noexcept = default;
  virtual void start(SnapshotHandler on_snapshot) = 0;
  virtual auto submit(Dmn_DLock_ProtocolCommand command,
                      ReplyHandler on_reply)
      -> Dmn_DLock_Submit_Result = 0;
  virtual auto requestSnapshot() -> Dmn_DLock_Submit_Result = 0;
  virtual void shutdown() noexcept = 0;
};
```

`start()` is called exactly once and either completes successfully or throws;
the manager factory then rolls back construction. `submit()` and
`requestSnapshot()` return promptly with local acceptance or a classified
failure. `kAccepted` does not mean the authority committed the operation.

The backend must not invoke callbacks while holding backend-internal locks.
Callback delivery may occur on any backend-owned thread, including before a
different caller returns from `submit()`, so the manager registers callbacks
and releases its lifecycle mutex before calling the backend. The manager
copies callback payloads into its executor and serializes all state mutation
there.

After `shutdown()` returns, the backend delivers no new callbacks. A callback
already entered before shutdown is covered by manager inflight accounting.
Backend implementations classify only known transient failures as retryable;
malformed protocol, identity mismatch, and closed transport are permanent for
that submission.

The DMesg backend catches handler conflict/write exceptions and maps them to
`kRetryableFailure`. Before retrying, its executor performs DMesg conflict
recovery and requests a fresh lock snapshot. A silently conflicted or dropped
DMesg write is detected by the missing-response deadline and follows the same
recovery path. None of these recovery calls execute in a DMesg handler
callback.

The DMesg authority service applies the same rule to reply/snapshot handlers.
It detects a conflicted publish lane, resolves it from the last topic message,
and force-republishes the current full snapshot from its own executor. A
snapshot lane that remains conflicted stops authority service rather than
continuing with invisible commits.

Two implementations are required:

- `Dmn_DLock_InMemory_Backend`: authority and transport in one process;
- `Dmn_DLock_DMesg_Backend`: commands/replies/snapshots over `Dmn_DMesg`.

The manager and authority depend only on this transport-neutral contract.
Future `Dmn_DMesgNet` support is a new backend, or a transport injected into
the DMesg backend, rather than a change to manager ordering, lifecycle, or
public locking APIs.

### 10.2 DMesg protocol

Add `src/proto/dmn-dlock.proto`, import it from
`src/proto/dmn-dmesg-body.proto`, and reserve library message type value `2`
as `dlock` in `src/proto/dmn-dmesg-type.proto`.

`DMesgBodyPb` adds a `DLockEnvelopePb dlock = 2` oneof member. The envelope
contains exactly one command, reply, or snapshot and always carries:

- protocol version;
- domain;
- authority id;
- authority generation and nonce when known;
- client id;
- correlation/request/operation ids as applicable.

The schema defines explicit command variants for create, cancel, renew,
release, and snapshot request; explicit reply status codes; and full snapshot
entries. Every enum has an `*_UNSPECIFIED = 0` value. Durations use integer
milliseconds with checked conversion. The protocol never serializes
`steady_clock::time_point`. Active snapshot entries carry authority-computed
acquisition or lease remaining milliseconds as applicable; these are
time-varying metadata, not part of the table-version identity.

The manager generates a fresh random `client_incarnation` at construction and
embeds it in every request, operation, and correlation id. DMesg reply messages
marked as playback are dropped. These rules prevent a restarted manager that
reuses `client_id` from matching the previous incarnation's cached reply.

V1 process-local DMesg topics:

```text
dmn.dlock.v1.<domain-token>.command.<client-token>
dmn.dlock.v1.<domain-token>.reply.<client-token>
dmn.dlock.v1.<domain-token>.snapshot
```

Each `<domain-token>` and `<client-token>` is the canonical unpadded Base64url
encoding from RFC 4648 Section 5 of the corresponding identifier bytes. A
decoder rejects padding, non-canonical encodings, and decoded identifiers that
do not exactly match the envelope. Raw user input is never concatenated into a
topic. The `sys` message type and topic are reserved for DMesg/DMesgNet control
traffic and are not used by the lock protocol.

Current DMesg matching is exact-topic or no-topic-filter; it has no wildcard
subscription. The authority therefore uses a no-topic-filter handler and
strictly parses and filters the lock command prefix, protocol version, domain,
and authority id before dispatch. Each client writes only its own command
topic, while the authority is the sole writer of reply and snapshot topics.
This avoids independent handlers competing in one topic's running-counter
space.

A future multi-node `Dmn_DMesgNet` backend preserves the same single-writer
topic rule and client-specific ingress topology. The transport topology remains
an adapter concern and does not change lock protocol identity, table
versioning, snapshots, or fencing. DMesgNet master election MUST NOT select the
lock authority.

DMesg handler callbacks execute in DMesg async context. They MUST only copy and
enqueue protocol data. They MUST NOT publish a reply/snapshot, inspect or reset
conflict state, query counters, close handlers, or wait from that callback.
Those operations run on the backend/authority-service executor to avoid
self-deadlock in `Dmn_DMesg`.

DMesg latest-message playback is useful for snapshots but unsafe for commands.
The authority command handler drops every message with `playback == true`.
It also rejects commands without the currently announced authority generation
and nonce. A client therefore bootstraps from the snapshot topic before sending
commands, and replayed commands from a prior authority epoch cannot mutate a
restarted authority.

Client-specific topics remain cached by the current DMesg implementation.
Stable, bounded client identifiers are recommended in v1 to limit topic-cache
and playback growth.

### 10.3 Snapshot convergence

Snapshots are complete, not deltas. A client:

- accepts only the configured domain and authority id;
- has no valid mirror until it accepts the first snapshot;
- accepts a higher authority generation, rejects a same-generation foreign
  nonce, and ignores retired/lower generations;
- ignores an older `table_version` in the current epoch;
- treats the same version as an idempotent duplicate only when all structural
  state is identical; time-varying remaining-duration metadata may decrease,
  but any other same-version divergence is a protocol error;
- atomically replaces its mirror for a newer version;
- requests a snapshot after a malformed reply, unknown correlation, detected
  protocol gap, or backend reconnect;
- validates the snapshot invariant that granted ranges do not overlap before
  applying it; invalid authoritative data is a `kPublisherError`, not silently
  accepted.

`DMesgPb.runningCounter` may suppress/replay transport messages, but it never
replaces these rules.

Applying a snapshot does not complete a command reply promise unless the
snapshot contains a terminal state that definitively resolves that request.
A snapshot alone never exposes a new usable grant because it cannot establish
the conservative local lease deadline defined in Section 13. Reply correlation
and mirror convergence are related but separate mechanisms.

If a current-epoch snapshot reveals a grant for a locally live request whose
grant reply was lost, the manager retries the original create operation id to
obtain a cached reply with fresh lease-remaining metadata. It keeps projecting
that request as `kWaiting` until the correlated reply establishes a
conservative local deadline. If the local request is already terminal, the
same observation follows I-12 cleanup instead.

The protocol has an explicit `kEpochMismatch` reply. It is not mapped to
`kPublisherError`: the manager requests a snapshot, applies Section 9.4, and
projects affected old-generation requests as `kAuthorityRestarted`.

## 11. Mirror, Waiting, and Notifications

The following state is protected by one manager lifecycle mutex:

- mirrored request map;
- current authority generation/nonce;
- `table_version`;
- `retention_version`;
- local request records and terminal outcomes;
- shutdown gate.

The condition-variable contract is:

```cpp
std::unique_lock lock{lifecycle_mutex};
while (!is_terminal_or_granted(request_id) && !shutting_down) {
  const auto observed_table = table_version;
  const auto observed_retention = retention_version;

  if (!cv.wait_until(lock, deadline, [&] {
        return is_terminal_or_granted(request_id) ||
               table_version != observed_table ||
               retention_version != observed_retention ||
               shutting_down;
      })) {
    // Final predicate check occurs while the same mutex remains held.
    if (!is_terminal_or_granted(request_id)) {
      terminalize_local_timeout_and_enqueue_cancel(request_id);
    }
    break;
  }
}
```

Updates mutate state under the same mutex, release it, and then call
`notify_all()`. Predicate helpers require the caller to hold the mutex. No code
waits while holding a backend, emitter, queue, or authority lock.

## 12. Retry and Timing Rules

Retries occur only for:

- `kVersionConflict`;
- missing authority response before `authority_response_timeout`;
- explicitly retryable transport failures.

Permanent protocol rejection and invalid authoritative data are terminal
`kPublisherError`.

Create, cancel, release, and renew operations have independent stable operation
ids. Only a retry of the same logical operation reuses an id. A cleanup release
created under I-12 is a new logical operation with its own stable id.

Backoff for zero-based attempt `n`:

```text
base = min(max_backoff, min_backoff * 2^n)
jitter = uniform(-ratio * base, +ratio * base)
delay = clamp(base + jitter, 0, max_backoff)
```

Overflow-safe saturating arithmetic is required. Randomness is injected in
tests. Before every submission and before scheduling the next retry, the worker
checks terminal state, cancellation, acquisition deadline, and shutdown.

A request whose caller-visible outcome is already terminal does not retry its
original operation for acquisition. Cleanup is the exception: when create
delivery is uncertain, it replays the original stable create operation id only
to obtain its final cached/committed outcome. That outcome is never returned as
a late success. Conflict means cleanup is complete; waiting is cancelled; grant
is released.

Cleanup first submits cancel for a request that is not known granted. If that
cancel returns `kInvalidState`, or any reply/snapshot reveals that the
authority granted the request, the manager creates a cleanup release with a
new stable operation id. It retries that release under these timing rules until
the release is confirmed, the authority epoch changes, or transport closes.
If cancel returns `kNotFound`, cleanup retains the tombstone and replays the
original create operation: a dropped create may still arrive, so `kNotFound`
alone is not proof that cleanup is complete.

A retry stores no blocking thread. Delayed work is represented by a cancellable
item in the manager executor's deadline queue.

## 13. Lease and Fencing Contract

The acquisition result is not a perpetual mutex. A granted lock is valid only
until its authority-generated lease deadline.

- The authority uses `std::chrono::steady_clock`.
- A grant/renew reply carries `lease_remaining_ms` recomputed when that reply
  is emitted, including for a cached idempotent outcome, and echoes the
  operation attempt/correlation id. Authority monotonic timestamps are
  diagnostic only; clients never compare process clocks.
- The manager records local steady-clock send and receive times for each
  attempt. For a successful create or renewal it derives
  `local_deadline = reply_receive_time + lease_remaining_at_reply -
  round_trip_time`. Since the return path is no longer than the measured full
  round trip, this deadline does not exceed the authority deadline. If the
  round trip consumes the reported remaining duration, the grant is
  immediately locally expired and cleaned up.
- A snapshot may shorten an existing local deadline but never extend it and
  never establishes a usable grant without a correlated reply.
- Local clients treat a lease as definitely invalid once the locally derived
  deadline is reached and stop protected work immediately.
- Renewal preserves the fencing token.
- A later re-grant, including after expiry, receives a larger token.
- An application acting on a protected external resource MUST pass
  `(authority_generation, authority_nonce, fencing_token)` to that resource.
  The resource rejects a lower generation, a nonce other than the registered
  nonce for that generation, or a non-increasing token where monotonic update
  is required. Without resource-side fence enforcement, a paused stale client
  may act after lease expiry; the library cannot prevent that.

## 14. Retention and Maintenance

Granted records are never TTL-pruned. Full terminal records are queryable for
at least `retained_terminal_ttl`.

Pruning a full terminal record leaves a compact authority tombstone containing
request id, owner/immutable identity digest, terminal outcome, and completed
operation ids/outcomes. Because the transport does not define a maximum
duplicate-delivery age, authority tombstones are retained for the authority
process lifetime. This is required for I-6/I-7: an arbitrarily late duplicate
create, including a no-wait conflict, must never recreate a request. A future
bounded tombstone policy requires a separately specified maximum replay window
or persistent deduplication.

Client cleanup tombstones may be pruned only after the request is
authority-confirmed terminal or the authority generation changes. A duration
measured from local submission is insufficient because the transport does not
specify a maximum command-delivery delay. Manager shutdown may discard local
cleanup state because no further callbacks or submissions are possible; any
already delivered grant is then recovered by its finite authority lease.

The manager executor performs periodic maintenance:

- observe cancel tokens;
- expire acquisition deadlines;
- expire locally known leases;
- prune eligible terminal records;
- request a snapshot after detected staleness.

The in-memory backend or authority service performs authority maintenance on
the serialized authority lane:

- expire authority acquisition and lease deadlines;
- compact full terminal records without discarding epoch-lifetime
  deduplication identity.

Every committed manager prune increments `retention_version`. Query start and
prune commit serialize on the lifecycle mutex, so a query observes all pruning
committed before it acquired that mutex.

Authority maintenance uses the same serialized authority command lane. Lease
expiry and authority retention pruning are logical transactions and increment
`table_version` when they mutate visible state.

## 15. Shutdown

`shutdown()` is idempotent, synchronous, and `noexcept`.

Order:

1. under one lifecycle-mutex critical section, change lifecycle from
   `kRunning` to `kClosing`, close all public mutation gates, stop acceptance
   of new retry/maintenance work, and terminalize non-granted local requests
   as `kShutdown`;
2. notify all API waiters;
3. call backend shutdown without holding the lifecycle mutex; backend shutdown
   prevents new callbacks and waits for backend-owned callback dispatch to
   leave;
4. drain manager tasks accepted before callback closure without performing new
   transport submissions;
5. join the manager worker and mark lifecycle `kClosed`;
6. perform a final synchronous local retention sweep.

The destructor calls `shutdown()`.

Internal worker and backend/DMesg callbacks MUST NOT hold strong ownership of
the public manager. They operate through a separately lifetime-guarded internal
state and inflight accounting, so finishing an internal callback cannot become
the event that destroys the manager on that callback thread. The event emitter
is the only user-provided code invoked on the manager worker. Its contract
forbids blocking, manager API re-entry, and retaining/releasing a manager
handle; callers retain the manager through shutdown on their own control
thread.

After shutdown:

- acquisition, cancel, and renew return `kShutdown`;
- owner query returns persisted state;
- release returns `kShutdown`; callers release known grants before shutdown;
- retained granted state becomes locally unusable at its conservative lease
  deadline and authority lease expiry eventually frees it;
- no retry, expiry, pruning, backend, or emitter callback may access destroyed
  manager state.

## 16. Concurrency and Locking Discipline

Lock order, when more than one lock is unavoidable:

```text
manager lifecycle mutex -> request-local state -> executor queue mutex
authority mutex         -> authority tables
```

The authority algorithm is synchronous. A service/backend either confines it
to one executor thread or protects entry with the authority mutex; it never
allows two command or maintenance transactions to execute concurrently.

The executor releases its queue mutex before running any task. A task may then
acquire the lifecycle mutex, but no worker path holds the queue mutex while
waiting for lifecycle state. This makes timeout-under-lifecycle-lock enqueueing
safe under the stated order.

Manager and authority locks MUST never be nested with backend or emitter locks.
External calls (`backend->submit`, emitter callbacks, user-visible hooks) occur
after internal locks are released.

The manager-owned executor has:

- one worker thread;
- an immediate FIFO;
- a min-heap of delayed tasks keyed by `(due_time, insertion_sequence)`;
- cancellable request tokens;
- close, drain, and join operations.

The executor is internal to `Dmn_DLock`; it should reuse
`Dmn_BlockingQueue_Mt`/`Dmn_Proc` where their shutdown contracts fit, but it
must not change existing queue behavior merely to support this feature.

## 17. Errors and Observability

### 17.1 Deterministic error mapping

| Condition | Result |
|---|---|
| invalid manager config | `create()` throws `std::invalid_argument` |
| invalid request argument | `kInvalidArg` |
| local enqueue fails before acceptance | `kLocalSubmissionError` |
| no-wait request would wait | `kConflict` |
| acquisition deadline reached | `kTimeout` |
| lease expired | `kLeaseExpired` |
| owner cancellation | `kCancelled` |
| owner mismatch on mutation | `kNotOwner` |
| owner mismatch on query | `kNotFound` |
| illegal lifecycle operation | `kInvalidState` |
| permanent backend/protocol failure after acceptance | `kPublisherError` |
| authority epoch replaced | `kAuthorityRestarted` |
| manager shutdown | `kShutdown` |

No broad catch may convert an unknown failure into success. Exception text may
populate `message`, but callers branch on `Code`.

### 17.2 Event emitter

The optional emitter receives immutable events after state commit:

- request accepted;
- command submitted;
- retry scheduled;
- waiting;
- granted;
- renewed;
- released;
- cancelled;
- acquisition timeout;
- lease expired;
- authority restarted;
- shutdown terminalized;
- protocol error.

Payload includes domain, client/authority ids, request/operation ids, range,
priority, old/new state, terminal reason, table version, sequence, fence, retry
attempt, and timestamp.

Emitter exceptions are caught at the adapter boundary, reported through the
repository's diagnostic mechanism, and never change a lock result or state.
Emitter implementations MUST return promptly and MUST NOT call manager APIs or
manage the lifetime of the manager that invokes them. These restrictions avoid
worker re-entry and self-destruction during callback execution.

## 18. File-Level Design

| File | Responsibility |
|---|---|
| `include/dmn-dlock.hpp` | Public types, manager API, API documentation. |
| `include/dmn-dlock-protocol.hpp` | Transport-neutral command, reply, snapshot, and status types. |
| `include/dmn-dlock-backend.hpp` | Abstract backend interface over protocol types. |
| `include/dmn-dlock-authority.hpp` | Authority API for embedded/server use. |
| `src/dmn-dlock.cpp` | Validation, local lifecycle, waiting, retries, retention, shutdown. |
| `src/dmn-dlock-authority.cpp` | Ordering, overlap, idempotency, versions, leases, snapshots. |
| `src/dmn-dlock-backend-memory.cpp` | Deterministic in-memory adapter. |
| `src/dmn-dlock-backend-dmesg.cpp` | DMesg command/reply/snapshot adapter. |
| `src/dmn-dlock-authority-service-dmesg.cpp` | DMesg authority service and dispatch executor. |
| `src/proto/dmn-dlock.proto` | Lock wire protocol. |
| `test/dmn-test-dlock.cpp` | Public manager and in-memory integration tests. |
| `test/dmn-test-dlock-authority.cpp` | Pure authority/order/protocol tests. |
| `test/dmn-test-dlock-dmesg.cpp` | DMesg transport integration tests. |

`include/dmn.hpp`, `src/CMakeLists.txt`, and `test/CMakeLists.txt` are updated as
their corresponding implementation slices are introduced.

## 19. Testability Requirements

The implementation must inject:

- monotonic clock;
- jitter source;
- backend;
- event emitter;
- executor wakeup/fault hooks needed to reproduce races.

Production defaults use `steady_clock` and a real random source. Tests use a
manual clock and deterministic sequence. Tests MUST NOT depend on wall-clock
sleeps for correctness.

Fault-capable fakes must support:

- accept/reject local submission;
- version conflict then success;
- dropped, delayed, duplicate, and reordered replies/snapshots;
- terminal transport failure;
- callback during shutdown;
- authority epoch change;
- emitter exception.

## 20. Required Test Matrix

### 20.1 Value, validation, and overlap tests

1. inclusive overlap at a shared endpoint;
2. adjacent non-overlapping ranges;
3. invalid negative and reversed ranges;
4. empty owner/domain/client/authority identifiers;
5. invalid duration, backoff, and jitter configuration;
6. request rank by priority then sequence;
7. unrelated ranges grant concurrently;
8. earlier overlapping waiter prevents bypass.

### 20.2 Authority tests

1. create assigns immutable sequence and monotonic table version;
2. duplicate operation is idempotent;
3. stale expected version returns conflict and full snapshot;
4. retry after version conflict reuses the operation id, updates only retry
   metadata, and can commit;
5. no-wait conflict creates no record and consumes no sequence or fence;
6. release/cancel causes deterministic reevaluation;
7. owner mismatch cannot mutate;
8. lease renewal preserves fence;
9. lease expiry releases and grants the next contender;
10. every new grant increases fencing token;
11. snapshot never contains overlapping grants;
12. authority restart changes epoch;
13. malformed immutable-field replay is rejected;
14. waiting acquisition expiry causes reevaluation;
15. duplicate no-wait conflict remains conflict after the range becomes free;
16. higher generation replaces an epoch and delayed retired-epoch data is
    ignored;
17. same-generation foreign nonce is rejected;
18. fences compare across generations;
19. over-limit acquisition and lease durations are rejected without mutation;
20. one maintenance transaction that expires multiple records increments the
    table version once.

### 20.3 Public API tests

1. no-wait grant and conflict;
2. blocking grant, timeout, cancellation, publisher error, authority restart,
   and shutdown;
3. async accepted return always contains request id and `kWaiting`;
4. async lifecycle query for every outcome;
5. owner-safe query hiding;
6. cancel waiting request and reject cancel of granted request;
7. renew and release ownership/state checks;
8. release idempotency;
9. acquisition expiry and lease expiry are distinct;
10. request ids are empty only for pre-acceptance outcomes;
11. release is rejected after shutdown and an unreleased grant expires by
    lease;
12. terminal retention before TTL and pruning after TTL;
13. cancel token cancels waiting state but does not release a visible grant;
14. cancel/renew/release response-timeout behavior follows Section 7.8;
15. RTT-derived local lease deadline never exceeds the authority deadline;
16. snapshot cannot extend or independently establish usable lease validity;
17. a snapshot-discovered live grant retries create for lease metadata before
    becoming caller-visible.

### 20.4 Concurrency and timing tests

1. notification before waiter sleeps does not hang;
2. table-version and retention-version changes wake waiters;
3. final deadline predicate lets a simultaneous grant win;
4. a grant discovered after timeout/cancel is released and never exposed;
5. cleanup cancel `kInvalidState` transitions to release, while `kNotFound`
   replays the original create for a final outcome;
6. retry delay respects exponential bounds and deterministic jitter;
7. terminalization cancels original retries but preserves required cleanup;
8. worker runs another request while an API caller waits;
9. concurrent query/cancel/release is race safe;
10. shutdown concurrent with reply delivery is race safe;
11. destructor leaves no callback or worker accessing manager state;
12. high-contention test completes without deadlock and without overlapping
    grants.

### 20.5 DMesg adapter tests

1. protobuf round trip for every command/reply/snapshot variant;
2. domain/authority/client/protocol filtering;
3. duplicate and reordered delivery convergence;
4. reconnect requests a full snapshot;
5. `DMesgPb.runningCounter` conflict triggers transport resynchronization,
   not lock-state corruption;
6. two clients using one configured authority preserve range exclusivity;
7. client-specific command topics avoid multi-writer counter conflict;
8. the no-topic-filter authority handler rejects unrelated topics;
9. an unexpected second authority is rejected and reported;
10. command playback is ignored after authority restart;
11. authority publication occurs off the DMesg callback context;
12. DMesg handler conflict is classified and recovered before retry;
13. a fresh client bootstraps from snapshot before command submission;
14. playback reply from a prior client incarnation is ignored;
15. epoch mismatch triggers snapshot resynchronization and authority-restarted
    projection;
16. authority snapshot conflict is recovered/republished or stops service.

### 20.6 Observability tests

1. each committed transition emits the required payload;
2. retries include attempt and delay;
3. emitter exception does not change immediate result;
4. emitter exception does not change persisted query state.

## 21. Requirement Traceability

| Requirement | Primary tests |
|---|---|
| I-2 range exclusivity | authority 6, 9, 11; concurrency 12; DMesg 6 |
| deterministic ordering | value 6-8; authority 6 |
| idempotency/versioning | authority 1-5, 13; DMesg 3 |
| caller-only waiting | public API 2; concurrency 1-8 |
| bounded acquisition | public API 2, 4, 9 |
| lease/fencing | authority 8-10, 16-18; public API 7, 9, 15-16 |
| mirror convergence | authority 3, 11, 16; DMesg 2-5, 10, 13 |
| owner safety/privacy | authority 7; public API 5-8 |
| shutdown/lifetime | public API 11; concurrency 10-11 |
| no abandoned grant | public API 2, 14; concurrency 4-5, 7 |
| DMesg callback safety | DMesg 11-12, 16; concurrency 10-11 |
| retention | public API 12; concurrency 2 |
| observability isolation | observability 1-4 |

## 22. Acceptance Criteria

The feature is complete when:

- all invariants in Section 4 are enforced;
- all public API outcomes are deterministic and covered;
- authority and in-memory manager tests pass without timing sleeps;
- DMesg protocol tests demonstrate convergence under duplication and
  reordering;
- no worker blocks on an API wait;
- shutdown joins owned execution and prevents use-after-destruction;
- granted results carry leases and fencing data;
- the documented single-authority deployment boundary is preserved;
- the full existing `dmn` test label remains green.
