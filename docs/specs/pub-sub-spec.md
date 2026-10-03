# Publish/subscribe

**Status:** Code-derived specification of `Dmn_Pub<T>` and its nested
subscriber interface.

## Purpose and execution model

`Dmn_Pub<T>` derives from `Dmn_Async<QueueType>` and owns a serialized
asynchronous publisher context. `publish(item)` schedules processing in that
context; the default is non-blocking, while `publish(item, true)` waits for
completion and propagates an exception through the wait handle. The value
published by the non-blocking overload is copied into its queued closure.

`publishInternal()` appends each value to a bounded replay history, trims the
oldest values above capacity, then evaluates the optional per-subscriber filter
and calls `notify()` for each accepted subscriber. Notifications execute
synchronously in the publisher's async context and serially with publisher
operations. A slow subscriber delays subsequent publisher work.

A capacity of zero is supported: the history is trimmed to empty before
notifying current subscribers. Subscribers registering later therefore receive
no replay.

## Registration and lifetime

Registration is synchronized by a task posted to the publisher context and the
public registration call waits for it. A new subscriber is added once and
receives the retained replay suffix according to its `replayQuantity`:

- `-1`: all retained values;
- `0`: no replay;
- positive `N`: the most recent `N` values.

`Dmn_Sub` records raw back-pointers to publishers. Subscriber destruction
unregisters itself; publisher destruction clears its entries from subscribers.
The publisher holds registered subscribers through `shared_ptr`; callers
should not assume registering transfers exclusive ownership. Publisher
operations execute serially in the order their tasks are dequeued from that
publisher's async queue. A blocking publish, registration, or unregistration
waits for its own task to finish. Thus an unregistration queued after a
publication cannot remove the subscriber before that earlier publication is
processed. If multiple threads submit concurrently, queue order—not wall-clock
call-start order—determines the operation order.

The publisher retains registered subscribers until the serialized
unregistration task removes them, keeping derived subscriber state alive
through earlier notifications. Publisher destruction queues subscriber
back-pointer cleanup and drains queued work. Callers must stop and join threads
that may call publisher APIs before destruction begins; task serialization
cannot make a new method call on an object being destroyed safe.

A derived publisher must stop its API callers and call `waitForEmpty()` in its
own destructor before destroying state used by an overridden
`publishInternal()`. The base `Dmn_Pub` destructor runs after the derived
destructor body, which is too late to protect derived state from already
queued virtual calls. `Dmn_DMesg` follows this ordering by unregistering its
handlers and draining its publisher context in its derived destructor.

## Ordering, exceptions, and filtering

Queue order defines publication order within one publisher. Replay runs on the
same publisher context as new notifications. Filter and notify callbacks are
user code and execute inline. If a filter or notification throws, the
publisher records the exception and continues to the remaining subscribers.
A filter failure skips delivery to that subscriber because the publisher
cannot determine whether the item should be delivered. A notification failure
does not prevent other subscribers from receiving the item. Failed callbacks
are not retried.

`takeCallbackFailures()` schedules its drain as a task in the same serialized
publisher context used by publication and registration replay, then waits for
that task. The vector is therefore only accessed in that context and needs no
separate mutex. The method returns and clears recorded failures in callback
invocation order. Each record identifies whether the failure came from a filter
or a notification, copies the subscriber's diagnostic name, and retains the
`exception_ptr` for diagnosis. `Dmn_Sub::GetName()` returns `"unknown"` by
default and may be overridden by a subscriber; the name is copied into the
failure record so it remains valid if the subscriber is later destroyed. Tasks
already queued ahead of the drain record their failures before it runs; callers
should call `waitForEmpty()` before draining when they need to include all work
queued at that point. Work submitted concurrently may be ordered before or
after the drain according to queue insertion order. Do not call
`takeCallbackFailures()` from the publisher's async context (such as from a
subscriber callback), because it waits for a task that cannot run until that
callback returns.
Blocking `publish(item, true)` completes delivery to all eligible subscribers,
then rethrows the first callback exception to its caller. That failure is also
present in the recorded error list. Replay notification failures are recorded
and replay continues; they do not prevent registration from succeeding.

## Callback failure retention

Callback failure storage is configured by the optional
`callbackFailureCapacity` constructor argument, which defaults to `-1`.
Negative values retain all failures until drained. `0` disables storage, but
does not change callback handling or blocking-publish exception propagation.
A positive value limits the stored records to that many; when full, the oldest
record is discarded before storing a new one. The limit applies to failures
from filters, notifications, and registration replay.

Replay-limit tests verify `-1` replays all retained values, `0` replays none,
and a positive limit replays only the newest requested suffix. A duplicate
registration of the same subscriber is ignored: it does not replay a second
time and live publications still notify it once. A blocked subscriber test
verifies that later publisher work waits for the callback to return and that
notifications preserve queue order.
