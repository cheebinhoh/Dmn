# I/O interfaces, pipes, and sockets

**Status:** Current behavior is extracted from `Dmn_Io`, `Dmn_Pipe`, and
`Dmn_Socket`; the tee-pipe is deprecated.

## `Dmn_Io<T>`

The interface defines `read() -> optional<T>`, bulk `read(count, timeout)`,
copy and move `write()` overloads, and `shutdown()`. It makes no general
thread-safety guarantee. A null optional is an implementation-specific
end/timeout/shutdown indication, so callers must check the concrete adapter
contract. The default bulk read returns an empty vector; implementations that
support batching must override it.

## `Dmn_Pipe<T, QueueType>`

The pipe combines `Dmn_Io`, a queue implementation, and `Dmn_Proc`. Without a
processing callback, consumers read synchronously. With a callback supplied to
the constructor, a background thread repeatedly dequeues up to the configured
batch count and invokes the callback for each item. Writes enqueue immediately.
`waitForEmpty()` first waits for the underlying queue to drain, then waits for
processed accounting to catch up.

`readAndProcess()` performs the dequeue and invokes callbacks before updating
the processed counter. A callback exception exits before that counter update.
The background worker catches all exceptions and exits without exposing the
failure to the caller. Bulk reads delegate to the selected queue; the pipe does
not re-arm a timeout after an empty return.

### Opt-in scheduled writes

`Dmn_Pipe` also has an opt-in background-worker constructor mode,
enabled by passing `true` as the final `enable_scheduled_writes` constructor
argument, and `writeAt(steady_clock::time_point, item)` overloads. Existing
constructor calls remain valid and retain their existing behavior because
scheduled writes default to disabled. In scheduled mode the processing callback
is required; construction throws if it is missing or the worker cannot start.
Calling `writeAt()` on a pipe constructed without scheduled writes enabled
throws `std::logic_error`; use ordinary `write()` for immediate FIFO writes.
The existing batch-count and pop-timeout arguments apply only to the ordinary
worker loop and are ignored in scheduled mode. Scheduled items are delivered by
the background worker; the synchronous `read()` and bulk-read APIs access only
ordinary FIFO writes.

Ordinary writes remain in the underlying FIFO and are selected before
deadline-scheduled items. Scheduled items are ordered by deadline, with equal
deadlines retaining submission order. A scheduled item is eligible only at or
after its deadline, but a continuous stream of ordinary writes can defer it
because ordinary work has priority. When no work is ready, the worker waits on
a condition variable until an ordinary/scheduled write arrives or the earliest
scheduled deadline is reached. Every write wakes the worker so an earlier
deadline or new ordinary item is noticed; spurious wakeups simply cause the
worker to re-check its queues.

In scheduled mode, `waitForEmpty()` snapshots accepted ordinary and scheduled
writes and waits for that snapshot to finish, returning the number accepted in
the snapshot. Shutdown rejects subsequent writes, drains already accepted
ordinary work, and runs accepted scheduled items only once due. Consequently,
shutdown/destruction can wait until the latest pending deadline. `waitForEmpty`
retains the pre-existing early-return-on-shutdown behavior if shutdown happens
while another caller is waiting.

Scheduled writes and shutdown use the pipe mutex to order the shutdown
transition against the write check-and-enqueue operation. The atomic shutdown
flag is set while holding that mutex; the worker is notified after the lock is
released and drains before the underlying queue is shut down.

The pipe shutdown flag and underlying queue shutdown flag are separate
lifecycles. The queue's in-flight guard checks its own shutdown state, so
setting the pipe flag to stop the scheduled worker does not prematurely reject
the worker's non-blocking queue polls. The underlying queue is shut down after
the worker joins.

As with the ordinary pipe worker, a processing callback exception ends the
scheduled worker; this mode does not yet expose worker failures. Pending
accepted work then remains unprocessed, and `waitForEmpty()` can remain blocked
until shutdown wakes it. When the scheduled worker cannot be started,
construction fails with an exception rather than returning an object that
would accept work it cannot process.

## `Dmn_Socket`

The public class implements `Dmn_Io<std::string>` and owns one file descriptor.
The current implementation creates an `AF_INET` `SOCK_DGRAM` socket, enables
broadcast, and binds to the configured address/port unless `write_only` is
true. `read()` receives one datagram into a `BUFSIZ` buffer and returns it as a
string; writes send a datagram to the configured address/port. Empty/error
reads return `nullopt`. The rvalue write overload delegates to the const
overload and does not transfer socket ownership.

This is message-oriented UDP, not TCP: there is no stream framing, connection
handshake, retry, delivery guarantee, or peer authentication. The receiver's
address binding and the sender's destination are currently derived from the
same `ip4` argument.

## Deprecated `Dmn_TeePipe`

The deprecated tee-pipe provides capacity-one input sources and a conveyor
that waits until each active source has data, gathers one item per source,
optionally transforms/reorders the batch, and forwards it to an outbound pipe.
Source removal waits for its buffer to drain. `wait()` also waits for source
owners to release their handles; `waitForEmpty()` waits for pending source and
outbound work. It is not included in the umbrella header.

## Gaps / improvements

1. Fix `Dmn_Pipe` worker failure/accounting: retain or report callback
   exceptions, define whether the worker continues, and ensure `waitForEmpty()`
   cannot wait forever because a callback failed. Callback invocation remains
   outside the bookkeeping mutex.
2. Decide whether `Dmn_Socket::read()` should preserve zero-length UDP
   datagrams; it currently maps them to `nullopt`, the same result as a receive
   error. Also check `inet_pton()` and port range and report datagram truncation.
3. If socket construction throws after `socket()` succeeds, close the opened
   descriptor before propagating the error. Add failure-path tests.
4. State whether concurrent reads/writes on the same socket are supported and
   test shutdown/read coordination.
5. Either repair and test the deprecated tee-pipe/limited queue against current
   interfaces or remove them; do not imply they are current supported APIs.
