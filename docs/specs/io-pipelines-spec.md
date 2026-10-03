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
The background loop catches all exceptions and terminates without exposing the
failure to the caller. Bulk reads delegate to the selected queue; the pipe
does not re-arm a timeout after an empty return.

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
   cannot wait forever because a callback failed. Its implementation invokes
   the callback outside the bookkeeping mutex; documentation must match.
2. Make the pipe's bulk-read timeout behavior explicit and consistent with
   queue variants. The implementation returns when a timed queue read returns,
   including an empty vector; it does not re-arm the deadline.
3. Correct `Dmn_Socket`'s public TCP wording or provide a separate stream
   adapter with explicit framing. Check `inet_pton()` and port range, report
   datagram truncation, and distinguish zero-length UDP datagrams from EOF.
4. If socket construction throws after `socket()` succeeds, close the opened
   descriptor before propagating the error. Add failure-path tests.
5. State whether concurrent reads/writes on the same socket are supported and
   test shutdown/read coordination.
6. Either repair and test the deprecated tee-pipe/limited queue against current
   interfaces or remove them; do not imply they are current supported APIs.
