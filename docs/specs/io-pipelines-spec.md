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
the processed counter. If a callback throws, items whose callbacks completed
successfully are accounted; the failed item and any remaining items in that
batch are not. The first processing exception is retained. A background
callback exception stops the worker, and `waitForEmpty()` wakes and rethrows
the retained exception rather than waiting for failed work to be accounted.
The callback is invoked outside the bookkeeping mutex. Bulk reads delegate to
the selected queue; the pipe does not re-arm a timeout after an empty return.

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
scheduled worker. The first processing/worker exception is retained, and
`waitForEmpty()` wakes and rethrows it; pending accepted work remains
unprocessed. When the scheduled worker cannot be started, construction fails
with an exception rather than returning an object that would accept work it
cannot process.

## `Dmn_Socket`

The public class implements `Dmn_Io<std::string>` and owns one file descriptor.
The implementation creates an `AF_INET` `SOCK_DGRAM` socket, enables
broadcast, and binds to the configured address/port unless `write_only` is
true. IPv4 literals and ports are validated before socket creation. Read-mode
sockets allow an empty address for wildcard binding and port 0 for an
OS-assigned local port; write-only sockets require a destination address and a
nonzero port. Writing through a wildcard-bound or port-zero socket is rejected
because neither has a configured datagram destination.

`read()` receives one datagram into a `BUFSIZ` buffer and returns an engaged
optional, including an empty string for a valid zero-length datagram. Receive
errors throw `std::system_error`; oversized datagrams are discarded and
reported as `std::errc::message_size`, not returned as partial strings.
`write()` throws `std::system_error` on send failure. The rvalue write overload
delegates to the const overload and does not transfer socket ownership.

`Dmn_Socket` is not thread-safe: callers must externally serialize operations
when sharing an instance. `read()` may block indefinitely, and the inherited
`Dmn_Io::shutdown()` is a no-op for this adapter; it does not interrupt a
blocked receive. Callers must stop and join all I/O threads before destroying
the socket. Closing its descriptor during destruction is not a supported way
to cancel another thread's blocked read. `Dmn_DMesgNet` owns and stops its
input worker before releasing its input adapter.

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

## Remaining gaps / improvements

The deprecated tee-pipe and limited-queue remain in `include/deprecated/` and
are intentionally kept outside the supported public API; they do not currently
represent a public-contract gap once isolated from the active `Dmn_Io`/
`Dmn_Pipe` interfaces.
