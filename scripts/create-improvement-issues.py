#!/usr/bin/env python3
"""
Create GitHub improvement issues for the Dmn repository.

This script uses the GitHub CLI (gh) to create improvement issues derived
from code analysis of the repository. Run it with appropriate GITHUB_TOKEN
credentials (the workflow provides these automatically).

Usage:
    python3 scripts/create-improvement-issues.py
or, via the GitHub Actions workflow:
    .github/workflows/create-improvement-issues.yml (workflow_dispatch)
"""

import subprocess
import sys

REPO = "cheebinhoh/Dmn"

ISSUES = [
    {
        "title": "Refactor conflict detection between Dmn_DMesgNet nodes",
        "labels": ["enhancement"],
        "body": """\
## Summary

The current conflict detection in `Dmn_DMesgNet` / `Dmn_DMesg` relies on simple
per-topic running counters. When a handler publishes a message with a topic counter
older than the publisher's current counter for that topic, that handler is placed into
a conflict state. This mechanism does not handle complex distributed scenarios where
multiple nodes have diverging views of the counter state (e.g., network partitions,
message loss, or simultaneous writes from different nodes).

## Problem

In a distributed `Dmn_DMesgNet` cluster, conflict detection currently:
- Compares incoming message counter against the local per-topic counter
- Places only the offending local *handler* in conflict state
- Does not signal any conflict information to remote nodes
- Cannot distinguish between a slow node and a truly conflicting node

This results in incomplete conflict detection in multi-node scenarios.

## Proposed Solution

1. **Introduce per-node logical timestamps or vector clocks** at the
   `Dmn_DMesgNet` level so that each node can track which version of a topic
   it last observed from each peer, enabling precise detection of concurrent
   conflicting updates.
2. **Propagate conflict signals** via the heartbeat or a dedicated system message
   so that other nodes are notified when a local conflict is detected.
3. **Define a formal conflict detection protocol**:
   - A node detects a conflict when it receives a message whose counter diverges
     from its vector clock entry for the sender's topic.
   - The node marks the topic as conflicted and broadcasts a conflict notification
     via the `sys` topic (`kDMesgSysIdentifier`).
4. **Add unit tests** in `test/dmn-test-dmesgnet-*.cpp` covering simulated
   multi-node conflict detection scenarios (including network partition recovery).

## Files to Modify

- `include/dmn-dmesg.hpp` — extend conflict state to support per-node tracking
- `include/dmn-dmesgnet.hpp` — add cluster-level conflict detection and propagation
- `src/dmn-dmesg.cpp` — update counter comparison logic
- `src/dmn-dmesgnet.cpp` — integrate network-level conflict signalling in heartbeat
- `proto/dmn-dmesg.proto` — possibly add conflict/vector-clock fields to `DMesgPb`
- `test/dmn-test-dmesgnet-*.cpp` — add multi-node conflict detection tests

## References

- `TODOS`: "Refactor conflict detection between Dmn_DMesgNet nodes"
- `include/dmn-dmesg.hpp`: current per-handler conflict state and detection logic
- `include/dmn-dmesgnet.hpp`: heartbeat and master election protocol
""",
    },
    {
        "title": "Refactor conflict resolution protocol between Dmn_DMesgNet nodes",
        "labels": ["enhancement"],
        "body": """\
## Summary

The current `Dmn_DMesg` conflict resolution relies on a per-handler
`resolveConflict()` call that resets the local conflict state. There is no
coordinated cluster-level conflict resolution protocol: other nodes are not notified,
and the resolution decision is purely local. For a truly distributed system, conflict
resolution must be propagated and agreed upon by the cluster.

## Problem

- `resolveConflict()` only resets the local handler's conflict flag; no network-level
  message is sent.
- If two nodes simultaneously detect and resolve a conflict differently, the cluster
  may enter an inconsistent state.
- No policy is defined for how conflicts are resolved (e.g., last-write-wins,
  first-write-wins, quorum-vote, or application-controlled merge).

## Proposed Solution

1. **Define a conflict resolution policy interface** that can be plugged into
   `Dmn_DMesg` / `Dmn_DMesgNet` (e.g., a functor or strategy object passed at
   construction).
2. **Broadcast a conflict resolution message** over the `sys` topic when a conflict
   is resolved locally, so that all cluster members can synchronize their view.
3. **Support common built-in strategies**:
   - Last-write-wins (default, based on timestamp or counter value)
   - First-write-wins (reject later conflicting writes)
   - Application-controlled merge (invoke a user-supplied merge functor)
4. **Add integration tests** that simulate two nodes simultaneously writing to the
   same topic and verify that the cluster converges to a consistent state after
   resolution.

## Files to Modify

- `include/dmn-dmesg.hpp` — add conflict resolution policy/strategy interface
- `include/dmn-dmesgnet.hpp` — add broadcast of conflict resolution message
- `src/dmn-dmesg.cpp` — implement resolution strategies
- `src/dmn-dmesgnet.cpp` — handle incoming conflict resolution messages
- `test/dmn-test-dmesgnet-*.cpp` — add convergence tests

## References

- `TODOS`: "Refactor conflict resolution protocol between Dmn_DMesgNet level"
- `include/dmn-dmesg.hpp`: `resolveConflict()`, conflict callback hooks
""",
    },
    {
        "title": "Add IPC support via Dmn_Io wrapper and integrate into Dmn_DMesgNet",
        "labels": ["enhancement"],
        "body": """\
## Summary

Currently `Dmn_DMesgNet` supports network communication via `Dmn_Socket` (UDP) and
`Dmn_Kafka`. There is no IPC (inter-process communication) transport, which limits
`Dmn_DMesgNet` to either single-process pub/sub or network-level distribution.
Adding an IPC-backed `Dmn_Io` implementation (e.g., UNIX domain sockets, shared
memory, or POSIX message queues) would allow efficient local multi-process messaging
without network overhead.

## Problem

- No `Dmn_Io<std::string>` implementation for IPC transports (UNIX socket,
  shared memory, POSIX MQ).
- `Dmn_DMesgNet` is tightly coupled to network-level transport; adding a local
  multi-process scenario requires a full network stack.
- For high-throughput local IPC, UDP sockets add unnecessary serialization and
  kernel-crossing overhead.

## Proposed Solution

1. **Implement `Dmn_Ipc`** — a new `Dmn_Io<std::string>` subclass backed by
   UNIX domain sockets (SOCK_DGRAM or SOCK_STREAM). Follow the same interface
   as `Dmn_Socket` (`read()` / `write()` / `shutdown()`).
2. **Create `Dmn_DMesgNet_Ipc`** — analogous to `Dmn_DMesgNet_Kafka`, a thin
   composition class that wires `Dmn_Ipc` as input/output handlers for
   `Dmn_DMesgNet`.
3. **Add IPC-specific tests** covering:
   - Multi-process pub/sub via UNIX socket
   - Graceful shutdown when the peer process exits
   - Performance comparison against UDP for local traffic
4. **Document** the IPC transport option in `README` and the relevant header files.

## Files to Create / Modify

- `include/dmn-ipc.hpp` — new `Dmn_Ipc` class
- `src/dmn-ipc.cpp` — implementation
- `include/dmn-dmesgnet-ipc.hpp` — new `Dmn_DMesgNet_Ipc` wrapper
- `test/dmn-test-dmesgnet-ipc-*.cpp` — IPC-specific tests
- `include/dmn.hpp` — include new headers
- `CMakeLists.txt` — add new sources and tests

## References

- `TODOS`: "Add IPC stuff via wrapper Dmn_Io and allow integration into Dmn_DMesgNet"
- `include/dmn-socket.hpp` / `src/dmn-socket.cpp`: reference implementation
- `include/kafka/dmn-dmesgnet-kafka.hpp`: reference wrapper pattern
""",
    },
    {
        "title": "Integrate Dmn_DMesgNet into a reusable application framework (Dmn)",
        "labels": ["enhancement"],
        "body": """\
## Summary

`Dmn_DMesgNet` provides distributed messaging, but there is no higher-level
application framework that ties together the runtime manager, pub/sub,
IPC/networking, service discovery, and lifecycle management into a cohesive,
reusable application skeleton. A `Dmn_App` application framework class would
allow users to build distributed applications by simply subclassing or composing
framework components rather than wiring each piece manually.

## Problem

- Users must manually coordinate `Dmn_Runtime_Manager`, `Dmn_DMesgNet`,
  signal handlers, heartbeats, and lifecycle (start/stop) for each application.
- There is no standard `main()` pattern or application entry point.
- Boilerplate initialization code is duplicated across test programs.

## Proposed Solution

1. **Design a `Dmn_App` (or `Dmn`) base class** that encapsulates:
   - Runtime manager singleton lifecycle (`Dmn_Runtime_Manager`)
   - Signal handling setup (SIGTERM/SIGINT → graceful shutdown)
   - DMesgNet node creation and registration
   - Service discovery integration (once implemented)
   - A `run()` entry point that calls `enterMainLoop()`
   - Virtual `onStart()` / `onStop()` hooks for application-specific logic
2. **Provide a convenience `dmn_main()` helper** or macro that reduces `main()`
   to a single call.
3. **Add example applications** showing how to build a distributed app using
   the framework with minimal boilerplate.
4. **Update `include/dmn.hpp`** to expose the new framework header.

## Files to Create / Modify

- `include/dmn-app.hpp` — new `Dmn_App` framework class
- `src/dmn-app.cpp` — implementation
- `include/dmn.hpp` — include new header
- `test/` or `examples/` — example distributed app using the framework
- `CMakeLists.txt` — add new sources

## References

- `TODOS`: "Add integrate the Dmn_DMesgNet to an application framework (Dmn) for reuse"
- `include/dmn-runtime.hpp`: runtime manager API
- `include/dmn-dmesgnet.hpp`: network-aware messaging
""",
    },
    {
        "title": "Add service discovery wrapper for Avahi as a modern C++ layer",
        "labels": ["enhancement"],
        "body": """\
## Summary

`Dmn_DMesgNet` currently discovers peers only through manually configured IP
addresses and ports (or a fixed Kafka topic). There is no automatic service
discovery mechanism. Wrapping Avahi (mDNS/DNS-SD) as a modern C++ `Dmn_Io`-
compatible layer would allow `Dmn_DMesgNet` nodes to discover each other
automatically on a local network without any pre-configuration.

## Problem

- Peer addresses must be hard-coded or passed explicitly to `Dmn_DMesgNet`.
- Adding a new node to the cluster requires manual configuration of all other nodes.
- No standard service advertisement or discovery is built into the framework.

## Proposed Solution

1. **Create `Dmn_Avahi`** — a modern C++ RAII wrapper around the Avahi client
   library (`libavahi-client`) that:
   - Advertises a `Dmn_DMesgNet` node as a DNS-SD service (e.g.,
     `_dmesgnet._udp.local.`) with the node's IP address and port.
   - Browses for peer services of the same type.
   - Exposes discovered peers via a callback or an observable interface.
2. **Integrate with `Dmn_DMesgNet`** so that when a new peer is discovered,
   a new `Dmn_Socket` (or `Dmn_Ipc`) output handler is automatically registered.
3. **Graceful peer removal**: when a peer's service disappears (heartbeat timeout
   or explicit DNS-SD removal), remove the corresponding output handler.
4. **Add tests** (where Avahi is available) and document the dependency.

## Files to Create / Modify

- `include/dmn-avahi.hpp` — new `Dmn_Avahi` service discovery class
- `src/dmn-avahi.cpp` — Avahi client implementation
- `include/dmn-dmesgnet.hpp` — optional integration hook
- `CMakeLists.txt` — optional Avahi dependency detection (`find_package`)
- `Dockerfile` — add `libavahi-client-dev` if needed

## References

- `TODOS`: "Add service discovery wrapper to Avahi and simplify it to be a modern C++ layer"
- `include/dmn-socket.hpp`: socket-level peer communication
- `include/dmn-dmesgnet.hpp`: peer/heartbeat management
""",
    },
    {
        "title": "Integrate Avahi service discovery into the Dmn application framework",
        "labels": ["enhancement"],
        "body": """\
## Summary

Once `Dmn_Avahi` (service discovery) and the `Dmn_App` application framework are
implemented, they should be integrated so that a `Dmn_App`-based application can
automatically discover and connect to other `Dmn_DMesgNet` nodes on the local
network without any manual peer configuration.

## Problem

- Service discovery (`Dmn_Avahi`) and the application framework (`Dmn_App`)
  will be independent features; they need a defined integration point.
- Without integration, users still need to manually pass discovered peer
  addresses to `Dmn_DMesgNet`.

## Proposed Solution

1. **Add a `Dmn_App::enableServiceDiscovery()` method** that starts `Dmn_Avahi`
   advertisement and browsing as part of the application lifecycle.
2. **Wire discovery callbacks** to automatically open/close `Dmn_DMesgNet`
   output handlers when peers appear/disappear.
3. **Expose a `Dmn_App::onPeerDiscovered()` / `onPeerLost()` hook** for
   application-level reactions to topology changes.
4. **Add an example** that demonstrates zero-configuration two-node discovery
   and messaging.

## Dependencies

- Issue: "Add service discovery wrapper for Avahi as a modern C++ layer"
- Issue: "Integrate Dmn_DMesgNet into a reusable application framework (Dmn)"

## Files to Modify

- `include/dmn-app.hpp` — `enableServiceDiscovery()` and peer hooks
- `src/dmn-app.cpp` — integration logic
- `examples/` — zero-config discovery example

## References

- `TODOS`: "Integrate service discovery to the application frame (Dmn)"
""",
    },
    {
        "title": "Add more refined master election and conflict detection for RAFT",
        "labels": ["enhancement"],
        "body": """\
## Summary

The current `Dmn_DMesgNet` master election algorithm is a simplified cooperative
protocol based on node creation timestamps and heartbeats. It does not implement a
formal consensus algorithm. Moving toward RAFT-style leader election would make the
election process more robust, formally verifiable, and capable of handling network
partitions, node failures, and split-brain scenarios.

## Problem

Current limitations of the election algorithm in `Dmn_DMesgNet`:
- Leader election is deterministic (earliest creation timestamp wins) but not
  fault-tolerant: a node can self-elect without a quorum.
- No explicit vote-request / vote-grant messages (RAFT `RequestVote` RPC).
- No term/epoch number to detect stale leaders.
- Split-brain is not explicitly prevented.
- Re-election after master loss relies on heartbeat timeouts, not a formal protocol.

## Proposed Solution

1. **Introduce term numbers** in the heartbeat / system message to detect stale
   leaders and reject messages from outdated terms.
2. **Implement RAFT-style `RequestVote` / `VoteGrant` messages** via the `sys`
   topic to conduct explicit votes before a node claims leadership.
3. **Enforce quorum requirement**: a node can only become master after receiving
   a majority of `VoteGrant` responses from the known cluster.
4. **Add explicit `AppendEntries` / log replication hooks** (optional for the
   pub/sub use case, but provides foundation for future state-machine replication).
5. **Add RAFT-specific tests** simulating network partition, node failure, and
   term-change scenarios.

## Files to Modify

- `include/dmn-dmesgnet.hpp` — add term, vote request/grant message handling
- `src/dmn-dmesgnet.cpp` — implement RAFT election phases
- `proto/dmn-dmesg.proto` — add term number, vote request/grant fields to `DMesgPb`
- `test/dmn-test-dmesgnet-*.cpp` — RAFT election tests

## References

- `TODOS`: "Add more refined master election and conflict detection for RAFT"
- `include/dmn-dmesgnet.hpp`: current election and heartbeat protocol
- RAFT paper: https://raft.github.io/raft.pdf
""",
    },
    {
        "title": "Implement a Quorum algorithm for distributed consensus",
        "labels": ["enhancement"],
        "body": """\
## Summary

Both the RAFT master election improvement and the conflict resolution protocol
will benefit from, or require, a quorum-based voting mechanism. A reusable
`Dmn_Quorum` utility should be designed and implemented to provide the quorum
primitives needed by higher-level protocols in `Dmn_DMesgNet`.

## Problem

- No quorum primitive exists in the library; each protocol (election, conflict
  resolution) would have to implement its own ad-hoc majority counting.
- Without a shared abstraction, quorum logic may be duplicated and diverge.

## Proposed Solution

1. **Design a `Dmn_Quorum<N>` template utility** that:
   - Tracks votes/acknowledgements from up to `N` participants.
   - Exposes `vote(node_id)` to register a vote and `hasQuorum()` to check
     if a majority has been reached.
   - Is thread-safe (can be updated from the async publisher context).
2. **Integrate with RAFT election** (from the RAFT issue) for vote counting.
3. **Integrate with conflict resolution** for quorum-based conflict arbitration.
4. **Add unit tests** for edge cases: even vs odd cluster sizes, simultaneous
   votes, quorum loss when nodes leave.

## Files to Create / Modify

- `include/dmn-quorum.hpp` — new `Dmn_Quorum` template utility
- `include/dmn-dmesgnet.hpp` — use `Dmn_Quorum` in election / conflict resolution
- `src/dmn-dmesgnet.cpp` — integrate quorum checks
- `test/dmn-test-quorum-*.cpp` — quorum unit tests
- `CMakeLists.txt` — add new test targets

## Dependencies

- Issue: "Add more refined master election and conflict detection for RAFT"
- Issue: "Refactor conflict resolution protocol between Dmn_DMesgNet nodes"

## References

- `TODOS`: "Quorum algorithm?"
- `include/dmn-dmesgnet.hpp`: master election and neighbor tracking
""",
    },
    {
        "title": "Cache sockaddr_in in Dmn_Socket::write() to reduce per-call overhead",
        "labels": ["enhancement", "performance"],
        "body": """\
## Summary

`Dmn_Socket::write()` currently reconstructs the destination `sockaddr_in`
structure on every call by re-parsing the stored IP string and port number. For
write-heavy workloads this causes redundant work. There is an existing `FIXME`
comment in `src/dmn-socket.cpp` that notes this issue. The `sockaddr_in` should
be cached as a member variable and initialized once in the constructor.

## Problem

In `src/dmn-socket.cpp`, the `write()` method does:

```cpp
struct sockaddr_in servaddr{};
memset(&servaddr, 0, sizeof(servaddr));
servaddr.sin_family = AF_INET;
servaddr.sin_port = htons(m_port_no);
// ... inet_pton on every call
```

For high-frequency message publishing (as in `Dmn_DMesgNet` heartbeats and
pub/sub delivery), this is unnecessary repeated work.

## Proposed Solution

1. **Add `struct sockaddr_in m_servaddr{}` as a private member** of `Dmn_Socket`.
2. **Initialize `m_servaddr`** once in the constructor after the socket is created.
3. **Remove the per-call reconstruction** from `write()` and use `m_servaddr` directly.
4. **Verify** that existing tests still pass after the change.

## Files to Modify

- `include/dmn-socket.hpp` — add `m_servaddr` member
- `src/dmn-socket.cpp` — initialize in constructor, simplify `write()`

## References

- `src/dmn-socket.cpp`: FIXME comment in `write()`
  ```
  /* FIXME: it might be effective to store the socket address (sockaddr_in)
   *        as a member value per object to avoid reconstructing it on every
   *        write call.
   */
  ```
""",
    },
    {
        "title": "Evaluate UUID generation for unique Dmn_DMesgNet node identifiers",
        "labels": ["enhancement"],
        "body": """\
## Summary

`Dmn_DMesgNet` currently uses `m_name` (a user-supplied string) as the source
write-handler identifier. There is a `FIXME` comment in `src/dmn-dmesgnet.cpp`
questioning whether a UUID should be generated internally to guarantee uniqueness
across networked nodes or on the global Internet.

## Problem

- If two nodes are given the same `m_name`, their identifiers collide, causing
  undefined behaviour in conflict detection and heartbeat tracking.
- The current approach requires callers to ensure name uniqueness manually.
- There is no enforcement or detection of duplicate node names at the cluster level.

## Proposed Solution

1. **Evaluate UUID v4 generation** (using the OS `/proc/sys/kernel/random/uuid`
   or `<uuid/uuid.h>`) as the default node identifier if no name is supplied.
2. **Alternatively**, combine `m_name` with a node-unique suffix (creation
   timestamp + PID + hostname hash) as already done for the `m_identifier` field
   in `Dmn_DMesgNet`, and validate this is truly unique.
3. **Add a duplicate-name detection warning** that logs when a heartbeat is
   received from a node whose identifier matches a locally known node with a
   different creation time.
4. **Document the uniqueness requirement** clearly in `dmn-dmesgnet.hpp`.

## Files to Modify

- `include/dmn-dmesgnet.hpp` — document identifier uniqueness requirement
- `src/dmn-dmesgnet.cpp` — optional UUID generation, duplicate detection
- `include/dmn-util.hpp` — add UUID or unique-suffix helper if needed

## References

- `src/dmn-dmesgnet.cpp` ~line 327:
  ```cpp
  // FIXME: shall we generate UUID internally to guarantee
  // uniqueness across networked nodes or global Internet?
  ```
- `include/dmn-dmesgnet.hpp`: node identifier (`m_identifier`, `m_name`)
""",
    },
    {
        "title": "Improve Dmn_BlockingQueue_Lf blocking efficiency (reduce busy-spin)",
        "labels": ["enhancement", "performance"],
        "body": """\
## Summary

`Dmn_BlockingQueue_Lf` implements blocking by spinning with `Dmn_Proc::yield()`
in a tight loop. The header comment acknowledges this:

> "This is suitable for scenarios where latency is prioritized and threads are
> expected to wake soon, but it may be inefficient for long idle waits with
> constraints number of cpus."

For general-purpose use (including test environments and low-thread-count systems)
this busy-spin consumes unnecessary CPU cycles.

## Problem

The current `popOptional(true)` (blocking pop) implementation spins:

```cpp
while (true) {
    // try lock-free dequeue ...
    if (result) return result;
    Dmn_Proc::yield();  // CPU-burning spin
}
```

On systems with constrained CPU resources or when messages are infrequent, this
causes consistently high CPU usage.

## Proposed Solution

1. **Add an `std::atomic::wait()`-based sleep** to the spin loop so that the
   consumer thread yields to the OS scheduler when the queue is empty and wakes
   when a producer pushes an item (using `notify_one()` / `notify_all()` in the
   push path).
2. **Keep the busy-spin as a fast path** for a configurable number of iterations
   before falling back to the OS wait (adaptive spinning).
3. **Add a template parameter or constructor option** to control the spin/sleep
   threshold to preserve low-latency behaviour for callers that need it.
4. **Benchmark and document** the latency/throughput trade-off for both modes
   using the existing performance tests in `test/dmn-test-blockingqueue-perf-*.cpp`.

## Files to Modify

- `include/dmn-blockingqueue-lf.hpp` — update `popOptional()` to use adaptive
  spin + `std::atomic::wait()`
- `test/dmn-test-blockingqueue-lf-*.cpp` — verify correctness under new model
- `test/dmn-test-blockingqueue-perf-*.cpp` — benchmark new vs old behaviour

## References

- `include/dmn-blockingqueue-lf.hpp`: blocking model documentation
- `include/dmn-blockingqueue-mt.hpp`: reference mutex+condvar blocking implementation
- `test/dmn-test-blockingqueue-perf-*.cpp`: existing performance tests
""",
    },
    {
        "title": "Replace pthread_cancel in Dmn_Proc with cooperative cancellation",
        "labels": ["enhancement"],
        "body": """\
## Summary

`Dmn_Proc` uses `pthread_cancel` / `pthread_setcancelstate` / `pthread_testcancel`
for thread cancellation. This POSIX API is notoriously difficult to use correctly
with C++ (destructors, RAII, stack unwinding) and is considered deprecated in
modern C++ threading practice. Replacing it with a cooperative stop token or a
shutdown flag would make `Dmn_Proc` safer and more composable with the rest of
the modern C++ ecosystem.

## Problem

- `pthread_cancel` interacts poorly with C++ destructors and RAII: if a thread
  is cancelled while holding a mutex or inside a destructor, undefined behaviour
  or resource leaks can result.
- Callers must explicitly call `Dmn_Proc::yield()` at voluntary cancellation
  points — this requirement is invisible in the API and easy to forget.
- `pthread_cleanup_push` / `pthread_cleanup_pop` macros (`DMN_PROC_CLEANUP_PUSH` /
  `DMN_PROC_CLEANUP_POP`) further complicate code that needs mutex cleanup on
  cancellation.
- macOS workarounds already exist in the codebase due to `pthread_cleanup_pop`
  behaviour differences (see commit `c6b4817`).

## Proposed Solution

1. **Add a `std::stop_token`-based cancellation interface** to `Dmn_Proc`:
   - Replace the `pthread_cancel` call with setting a `std::stop_source`.
   - Pass a `std::stop_token` to the task functor (or expose it via a method)
     so it can check `token.stop_requested()` in its loop.
2. **Deprecate `yield()` / `DMN_PROC_CLEANUP_*` macros** once all callers have
   migrated to the cooperative model.
3. **Migrate existing callers** (`Dmn_Async`, `Dmn_Pipe`, `Dmn_Timer`,
   `Dmn_BlockingQueue_Lf`) to the new cooperative stop-token model.
4. **Remove macOS workarounds** related to `pthread_cleanup_pop` once the
   migration is complete.

## Files to Modify

- `include/dmn-proc.hpp` — add `std::stop_source` / `std::stop_token`, update `stopExec()`
- `src/dmn-proc.cpp` — replace `pthread_cancel` with stop-source notification and join
- `include/dmn-async.hpp` — update task lambdas to accept/check stop token
- `include/dmn-pipe.hpp` — update task lambdas
- `include/dmn-timer.hpp` — update timer loop
- `include/dmn-blockingqueue-lf.hpp` — check stop token in spin loop

## References

- `include/dmn-proc.hpp`: `DMN_PROC_CLEANUP_*` macros, `stopExec()`, `yield()`
- `src/dmn-proc.cpp`: `pthread_cancel`, `pthread_setcancelstate`
- Commit `c6b4817`: macOS-specific `pthread_cleanup_pop` workaround
""",
    },
]


def create_issue(title: str, labels: list[str], body: str) -> bool:
    """Create a single GitHub issue using the gh CLI. Returns True on success."""
    cmd = [
        "gh",
        "issue",
        "create",
        "--repo",
        REPO,
        "--title",
        title,
        "--body",
        body,
    ]
    for label in labels:
        cmd += ["--label", label]

    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode == 0:
        url = result.stdout.strip()
        print(f"  Created: {url}")
        return True
    else:
        stderr = result.stderr.strip()
        print(f"  FAILED: {stderr}")
        return False


def main() -> int:
    print(f"Creating {len(ISSUES)} improvement issues for {REPO}...\n")
    success = 0
    failure = 0
    for issue in ISSUES:
        title = issue["title"]
        print(f"Creating: {title!r}")
        ok = create_issue(
            title=title,
            labels=issue["labels"],
            body=issue["body"],
        )
        if ok:
            success += 1
        else:
            failure += 1

    print(f"\nDone: {success} created, {failure} failed.")
    return 0 if failure == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
