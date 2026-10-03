# Dmn code-derived specifications and review

These documents record the current observable contracts of the public modules,
their principal implementation files, and concrete gaps that should be
addressed. They are a code review/specification baseline, not a claim that
recommended behavior is already implemented. Existing module specifications
remain the source for the modules listed there; this index keeps the full
module inventory together.

## Specification map

| Module(s) | Specification | Main implementation |
|---|---|---|
| `Dmn_Async`, `Dmn_Proc`, `Dmn_Timer`, `Dmn_Singleton` | [Asynchronous execution and lifetime](async-execution-spec.md) | `include/dmn-async.hpp`, `include/dmn-proc.hpp`, `src/dmn-proc.cpp`, `include/dmn-timer.hpp`, `include/dmn-singleton.hpp` |
| Blocking queue API, mutex queue, lock-free queue, in-flight guard, deprecated bounded queue | [Queues and shutdown safety](blocking-queues-spec.md) | `include/dmn-blockingqueue*.hpp`, `include/dmn-inflight-guard.hpp`, `include/deprecated/dmn-limit-blockingqueue.hpp` |
| `Dmn_Io`, `Dmn_Pipe`, deprecated `Dmn_TeePipe`, `Dmn_Socket` | [I/O and pipelines](io-pipelines-spec.md) | `include/dmn-io.hpp`, `include/dmn-pipe.hpp`, `include/deprecated/dmn-teepipe.hpp`, `include/dmn-socket.hpp`, `src/dmn-socket.cpp` |
| `Dmn_Pub` / `Dmn_Sub` | [Publish/subscribe](pub-sub-spec.md) | `include/dmn-pub-sub.hpp` |
| `Dmn_DMesg`, handler API, protobuf helpers and schemas | [DMesg and message schema](dmesg-spec.md) | `include/dmn-dmesg.hpp`, `src/dmn-dmesg.cpp`, `include/dmn-dmesg-pb-util.hpp`, `src/proto/dmn-dmesg*.proto` |
| Runtime manager and coroutine task | [Runtime scheduler](runtime-spec.md) | `include/dmn-runtime.hpp`, `include/dmn-runtime-task.hpp`, `src/dmn-runtime.cpp` |
| Synchronous state machine | [Caller-driven state machine](state-machine-spec.md) | `include/dmn-state.hpp`, `src/dmn-state.cpp` |
| Kafka I/O and DMesgNet adapter | [Kafka transport](kafka-spec.md) | `include/kafka/*.hpp`, `src/kafka/*.cpp` |
| Debug/utility helpers and umbrella include | [Utility and public-header surface](utility-headers-spec.md) | `include/dmn-debug.hpp`, `include/dmn-util.hpp`, `include/dmn.hpp` |
| Daemon and standalone transport examples | [Executables and fixtures](executables-spec.md) | `src/dmn-dmesg-daemon.cpp`, `src/dmn-kafka-{sender,receiver}.cpp` |
| `Dmn_DMesgNet` | Existing [network-layer specification](dmn-dmesgnet-spec.md) | `include/dmn-dmesgnet.hpp`, `src/dmn-dmesgnet.cpp` |
| `Dmn_IntervalBTree` | Existing [interval B-tree specification](dmn-interval-btree-spec.md) | `include/dmn-interval-btree.hpp`, `src/dmn-interval-btree.cpp` |
| `Dmn_DLock` and its protobuf | Existing [DLock specification](dmn-distributed-lock-spec.md) and [network design](dmn-distributed-lock-dmesgnet-spec.md) | `include/dmn-dlock.hpp`, `src/proto/dmn-dlock.proto` |
| Runtime-managed state and manager | Existing [runtime-state specification](runtime-state-machine-spec.md) | `include/dmn-runtime-state.hpp`, `src/dmn-runtime-state.cpp` |

`Dmn_DMesgNet` and `Dmn_Runtime_State` have dedicated specifications elsewhere
in this directory; their headers are included in this audit and their documented
limitations remain applicable. The corresponding `*-plan.md` documents are
implementation plans rather than additional module specifications.

## Review findings to address

Rank reflects impact and confidence in the code-derived finding, not a claim
that every item has been fixed or separately regression-tested.

| Priority | Module / location | Finding and recommended action |
|---|---|---|
| P0 | Kafka example programs, `src/dmn-kafka-sender.cpp` and `src/dmn-kafka-receiver.cpp` | Broker credentials are embedded in source. Remove them from the repository, rotate/revoke them if they were usable, and load credentials from a protected runtime source. The literal values are intentionally not reproduced in this documentation. |
| P0 | Kafka constructor, `src/kafka/dmn-kafka.cpp` | The subscription-failure diagnostic contains a malformed unary-plus expression; the error path also destroys the topic list before reading its count. Fix the expression and capture diagnostic data before destroying the list. |
| P1 | Kafka constructor, `src/kafka/dmn-kafka.cpp` | The reserved `Key` option is assigned to `m_topic`, while production reads `m_key`; the configured Kafka message key is therefore not applied. Correct key/topic parsing and test producer topic and key independently. |
| P1 | Socket adapter, `include/dmn-socket.hpp`, `src/dmn-socket.cpp` | The adapter uses UDP, but `read()` maps both zero-length datagrams and receive errors to `nullopt`; address parsing and port validity are not checked, and constructor failures after `socket()` can leak the descriptor. Define zero-length datagram behavior, validate inputs, and close the descriptor on construction failure. |
| P1 | Network input, `src/dmn-dmesgnet.cpp` | `ParseFromString()` and serialization results are ignored. Reject malformed frames, surface encoding failures, and add malformed/truncated-input tests. |
| P1 | Pipe processing, `include/dmn-pipe.hpp` | The background loop catches every exception and exits without reporting it; `readAndProcess()` advances processed accounting only after every callback succeeds. Define and expose worker failure behavior so exceptions do not silently stop consumption or strand `waitForEmpty()`. |
| P1 | Process wrapper, `src/dmn-proc.cpp` | Lifecycle state/task/thread-handle access is not synchronized although callers can invoke lifecycle operations from different threads; an exception escaping the pthread entry function terminates the process. Specify thread-safety, synchronize transitions, and capture/report task exceptions. |
| P1 | DMesgNet startup and membership, `src/dmn-dmesgnet.cpp` | The code does not validate incoming system/application message shape or authenticate source identity; advertised node lists are not merged and inactive peers are not generally aged out. Validate protocol inputs and define an explicit trust/failure-detection boundary before relying on it beyond trusted, cooperative peers. |
| P2 | Async delayed tasks, `include/dmn-async.hpp` | A not-yet-due task is immediately re-enqueued and yields, so delayed work can repeatedly consume the executor instead of sleeping until due. Use a timer/wait primitive or document and bound the polling cost; test ordering and idle CPU use. |
| P2 | Timer, `include/dmn-timer.hpp` | Only `std::exception` is caught in the timer callback; a non-standard exception escapes the thread entry. Decide whether callbacks must be non-throwing or catch/report all exceptions, and test it. |
| P2 | Deprecated queue, `include/deprecated/dmn-limit-blockingqueue.hpp` | The class is not part of the current test/build surface and its base template/signatures do not match the current two-parameter CRTP queue API. Remove it, repair it and add tests, or clearly mark it as unsupported and non-buildable. |
| P2 | API documentation and tests | Some DMesg/DMesgNet cases are disabled or non-default, leaving behavior unverified. Enable focused deterministic coverage for those paths. |
| P3 | Public include surface, `include/dmn.hpp` | The umbrella header does not include `dmn-dlock.hpp` or `dmn-runtime-state.hpp`; document these as intentionally opt-in or include them consistently. |

## Scope and evidence

The inventory covers the public headers, their implementation files, the
checked-in protobuf schemas, and the corresponding CMake-registered test
targets. Existing module specs were consulted for network, interval-tree,
distributed-lock, and runtime-state behavior. Test sources were used to map
coverage, not treated as proof that every listed behavior is correct. In
particular, tests for Kafka are optional, some DMesg/DMesgNet cases are disabled,
and deprecated code is outside the normal test target.
