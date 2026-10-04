/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-pipe.hpp
 * @brief Dmn_Pipe: a FIFO pipe with non-blocking writers and optional
 * background processing.
 *
 * Overview
 * --------
 * - Dmn_Pipe<T, QueueType> implements a FIFO buffer that:
 *   - allows producers to write without blocking (write operations enqueue
 *     items immediately),
 *   - allows a consumer to read items either synchronously via `read()` or
 *     by providing a processing task via `readAndProcess()` or by launching a
 *     background processing thread (using Dmn_Proc::exec).
 * - The class combines QueueType (storage), Dmn_Io<T> (I/O
 *   interface) and Dmn_Proc (optional processing thread support).
 *
 * Threading and cancellation
 * --------------------------
 * - In ordinary mode, push/pop synchronization is handled by QueueType.
 *   Scheduled-mode queue selection and processed-item accounting are protected
 *   by the pipe mutex.
 * - QueueType maintains its own shutdown state independently of this pipe's
 *   worker shutdown state. Queue implementations with in-flight guards must
 *   base those guards on their own queue shutdown state.
 * - Dmn_Pipe adds a mutex and condition variable to:
 *   - keep a count of processed items (`m_count`), and
 *   - allow callers to wait until all currently inbound items have been
 *     processed (waitForEmpty()).
 * - readAndProcess() involves the caller-provided task and updates `m_count`
 *   under the mutex.
 * - An optional scheduled-write mode uses the same background worker and
 *   waits for the next deadline or a new write; it does not add a thread.
 *
 * Read / Write semantics
 * ----------------------
 * - write(const T&) copies `item` into the pipe.
 * - write(T&&) moves `item` into the pipe; move will be used when the move
 *   constructor is noexcept (via QueueType::push semantics).
 * - read() blocks until the next item is available and returns it wrapped
 *   in std::optional; when the pipe is closed it returns std::nullopt.
 * - readAndProcess(fn) blocks until the next item is available or timeout
 *   and invokes the provided task with the item (moved where possible).
 * - read(count, timeout) and readAndProcess(fn, count, timeout) functions
 *   behave like their counterparts without count and timeout, but with the
 *   following blocking behavior
 *     1. If the pipe already contains >= count items, it returns exactly
 *        `count` items immediately.
 *     2. If the pipe contains 0 items, it blocks:
 *        - If timeout == 0: blocks indefinitely until at least `count` items
 *          become available (returns exactly `count`).
 *        - If timeout > 0: waits up to `timeout` microseconds for items.
 *          * If enough items are available before timeout, returns exactly
 *            `count` items.
 *          * If the timeout expires, it returns whatever items are available
 *            up to `count` items, or no items if none are available.
 *
 *   Note: The timeout is interpreted as a maximum time to wait for the full
 *   `count` items (measured from the first blocking wait inside the call).
 *   A zero timeout value means "wait forever".
 *
 * waitForEmpty() blocks until all items that were inbound into the pipe
 * have been processed (or popped out). In scheduled-write mode it snapshots
 * all accepted ordinary and scheduled writes.
 *
 * Lifetime
 * - If a Task is provided to the constructor, a background processing
 *   thread is started via Dmn_Proc::exec. In ordinary mode it repeatedly calls
 *   readAndProcess(fn); in scheduled-write mode it selects immediate writes
 *   before due scheduled writes and waits for new work or the earliest
 *   deadline.
 * - Shutdown unblocks and joins the ordinary worker. In scheduled-write mode it
 *   first stops accepting writes, wakes the worker, and joins it after accepted
 *   work has drained (including waiting until scheduled deadlines).
 */

#ifndef DMN_PIPE_HPP_

#define DMN_PIPE_HPP_

#include "dmn-blockingqueue-mt.hpp"
#include "dmn-blockingqueue.hpp"
#include "dmn-debug.hpp"
#include "dmn-io.hpp"
#include "dmn-proc.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace dmn {

/**
 * @brief FIFO I/O pipe with optional background processing and deadline writes.
 *
 *
 * @tparam T Item type stored in the pipe.
 * @tparam QueueType Queue implementation used for ordinary writes.
 *
 * By default, the pipe retains its queue-backed read/write behavior. Supplying
 * a processing task starts one background worker. Passing
 * @c enable_scheduled_writes as @c true opts that worker into @c writeAt():
 * ordinary FIFO writes take priority, while scheduled items are ordered by
 * steady-clock deadline and become eligible at or after their deadline. When
 * no item is ready, the worker sleeps until new work arrives or the earliest
 * scheduled deadline. Calling @c writeAt() when scheduled writes are disabled
 * throws @c std::logic_error.
 * Scheduled items are delivered by the background worker; the synchronous
 * @c read() APIs access only ordinary FIFO writes.
 *
 * In scheduled mode, @c waitForEmpty() snapshots accepted writes and waits for
 * that snapshot to finish. Shutdown serializes its transition against writes
 * with the pipe mutex, rejects later writes, and drains accepted work before
 * joining the worker when callbacks complete normally; it can therefore wait
 * until pending deadlines. If a callback throws, the worker stops and remaining
 * accepted work is not drained. The first processing/worker exception is
 * retained; waitForEmpty() wakes and rethrows it instead of waiting for
 * unfinished accounting. Callers must serialize concurrent shutdown calls and
 * ensure the pipe outlives its callers and processing callback.
 *
 * A callback exception ends the background worker. The exception is observable
 * through waitForEmpty(), which rethrows the first processing/worker failure.
 */
template <typename T, typename QueueType = Dmn_BlockingQueue_Mt<T>>
class Dmn_Pipe : public Dmn_Io<T>, private QueueType, private Dmn_Proc {
  static_assert(std::is_base_of_v<Dmn_BlockingQueue<QueueType, T>, QueueType>,
                "QueueType must inherit from dmn::Dmn_BlockingQueue<QueueType, "
                "T>");

  using Task = std::function<void(T &&)>;

public:
  using Dmn_Io<T>::write;

  /**
   * @brief Construct a Dmn_Pipe and optionally start a background processing
   * thread.
   *
   * @param name    Human-readable name forwarded to the underlying @c Dmn_Proc.
   * @param fn      Optional processing task invoked for each item dequeued by
   * the background thread.  If empty, no background thread is
   * started and items must be consumed via read() or
   * readAndProcess().
   * @param count   Number of items to dequeue per ordinary background-thread
   * iteration. Defaults to 1; ignored when scheduled writes are enabled.
   * @param timeout Timeout in microseconds passed to each pop call in the
   * ordinary background loop.  0 means wait indefinitely; ignored when
   * scheduled writes are enabled.
   * @param enable_scheduled_writes Opt into deadline-scheduled writes with
   * @c writeAt(). Defaults to @c false, preserving existing behavior. When
   * enabled, @p fn must be non-empty.
   *
   * In this mode ordinary writes take priority over scheduled writes, and the
   * worker waits until new work arrives or the next scheduled deadline.
   *
   * @throws std::invalid_argument if scheduled writes are enabled without a
   * processing task.
   * @throws std::runtime_error if a requested background worker cannot be
   * started.
   */
  explicit Dmn_Pipe(std::string_view name, Dmn_Pipe::Task fn = {},
                    size_t count = 1, long timeout = 0,
                    bool enable_scheduled_writes = false);

  /**
   * @brief Destroy the pipe, stopping any background processing thread and
   * releasing resources.
   */
  virtual ~Dmn_Pipe() noexcept;

  Dmn_Pipe(const Dmn_Pipe<T, QueueType> &obj) = delete;
  Dmn_Pipe<T, QueueType> &operator=(const Dmn_Pipe<T, QueueType> &obj) = delete;
  Dmn_Pipe(Dmn_Pipe<T, QueueType> &&obj) = delete;
  Dmn_Pipe<T, QueueType> &operator=(Dmn_Pipe<T, QueueType> &&obj) = delete;

  /**
   * @brief Read and return the item from the pipe.
   *
   * Blocks until the next item is available. If shutdown closes the underlying
   * queue before an item is available, the queue's blocking pop throws.
   *
   * @return The next item, wrapped in an engaged optional.
   * @throws std::runtime_error if shutdown closes the empty queue while waiting
   * or before this call starts.
   */
  auto read() -> std::optional<T> override;

  /**
   * @brief Read and return the next item from the pipe.
   *
   * Detailed semantics:
   * - count > 0 is required (asserted).
   * - If the pipe has >= count items, this returns exactly count items.
   * - If the pipe is empty or less than count:
   *   - timeout == 0: wait indefinitely for count items (return exactly count).
   *   - timeout > 0: wait up to timeout microseconds for items.
   *     * If timeout expires and there is at least one item, return 1..count
   *       items (the current pipe data size).
   *     * If timeout expires and the pipe is still empty, the function returns
   *       an empty vector.
   *
   * The returned vector contains moved items removed from the pipe.
   *
   * @param count   Number of desired items (must be > 0).
   * @param timeout Timeout in microseconds for waiting for the full count.
   *
   * @return Vector of items (size == count on success without timeout, or
   * between 1 and count if a timeout occurred after at least one item
   * was produced).
   * @throws std::runtime_error if the underlying queue has been shut down.
   */
  auto read(size_t count, long timeout = 0) -> std::vector<T> override;

  /**
   * @brief Read the next item from the pipe and invoke the provided task.
   *
   * Blocks until items are available or the read timeout expires. The task is
   * invoked before the internal bookkeeping mutex is acquired to update the
   * processed-item count (`m_count`) and signal waiting threads. Items are
   * passed to `fn` using move semantics when possible. If a callback throws,
   * the successful prefix is accounted, the exception is retained, and it is
   * rethrown to the caller.
   *
   * @param fn The functor to process the next item popped from the pipe
   *
   * @return The number of items read and processed.
   * @throws The callback exception, if processing fails.
   */
  auto readAndProcess(Dmn_Pipe::Task fn, size_t count = 1,
                      long timeout = 0) -> size_t;

  /**
   * @brief Write (copy) an item into the pipe.
   *
   * This call enqueues a copy of `item` into the FIFO. Writing is non-blocking;
   * any blocking behavior is determined by the underlying Dmn_BlockingQueue
   * implementation. In scheduled-write mode it wakes the background worker
   * and counts as accepted work for @c waitForEmpty().
   *
   * @param item The data item to be copied into the pipe
   */
  void write(const T &item) override;

  /**
   * @brief Write (move) an item into the pipe.
   *
   * This call attempts to move `item` into the FIFO. If move construction is
   * noexcept it will move; otherwise behavior follows Dmn_BlockingQueue push
   * policy. In scheduled-write mode it wakes the background worker and counts
   * as accepted work for @c waitForEmpty().
   *
   * @param item The data item to be moved into the pipe
   */
  void write(T &&item) override;

  /**
   * @brief Schedule an item to be processed no earlier than @p deadline.
   *
   * This is only available on pipes constructed with
   * @c enable_scheduled_writes set to @c true. The scheduled item is ordered by
   * deadline; ordinary @c write() items retain priority and FIFO ordering.
   * Equal deadlines are processed in submission order. The worker is notified
   * so it can adjust an existing timed wait if this deadline is earlier.
   *
   * @param deadline Earliest processing time according to
   * @c std::chrono::steady_clock.
   * @param item Item to copy into the scheduled queue.
   * @throws std::logic_error if scheduled writes were not enabled.
   * @throws std::runtime_error if the pipe is shutting down.
   * @throws std::exception or another exception propagated by T's copy
   * constructor or by the scheduled queue's allocator.
   */
  void writeAt(std::chrono::steady_clock::time_point deadline, const T &item);

  /**
   * @brief Schedule a moved item to be processed no earlier than @p deadline.
   *
   * This is only available on pipes constructed with
   * @c enable_scheduled_writes set to @c true. Ordinary @c write() items retain
   * priority and FIFO ordering; equal scheduled deadlines retain submission
   * order.
   *
   * @param deadline Earliest processing time according to
   * @c std::chrono::steady_clock.
   * @param item Item to move into the scheduled queue.
   * @throws std::logic_error if scheduled writes were not enabled.
   * @throws std::runtime_error if the pipe is shutting down.
   * @throws std::exception or another exception propagated by T's move or copy
   * constructor or by the scheduled queue's allocator.
   */
  void writeAt(std::chrono::steady_clock::time_point deadline, T &&item);

  /**
   * @brief Block until the pipe is empty and all inbound items are processed.
   *
   * The function waits until all items that were reported inbound at the
   * time of the call have been popped and processed (i.e., `m_count` has
   * advanced to cover them). In scheduled-write mode it snapshots accepted
   * ordinary and scheduled writes and waits for that snapshot. Shutdown may
   * cause a waiting call to return before the snapshot has completed. A
   * processing/worker exception wakes the call and is rethrown.
   *
   * Producers must be externally coordinated when callers require the
   * snapshot to include every write in a larger logical operation.
   *
   * @return The number of items in the completed snapshot.
   * @throws The first exception raised during processing or by the worker.
   */
  auto waitForEmpty() -> uint64_t override;

protected:
  /**
   * @brief Return @c true if the shutdown flag has been set on this pipe.
   *
   * @return @c true when shutdown() has been called, @c false otherwise.
   */
  virtual auto isShutdown() -> bool override {
    return m_shutdown_flag.test(std::memory_order_acquire);
  }

  /**
   * @brief Initiate an orderly shutdown of the pipe.
   *
   * In scheduled mode, sets the atomic shutdown flag while holding @c m_mutex,
   * then notifies and joins the worker before shutting down the underlying
   * queue so accepted work can drain if callbacks complete normally. Ordinary
   * mode sets the flag and shuts down the underlying queue first to unblock the
   * worker. Subsequent calls are no-ops. Callers must serialize concurrent
   * calls to shutdown(), since the underlying @c Dmn_Proc lifecycle is not
   * synchronized.
   *
   * @throws std::runtime_error if joining the background worker fails.
   */
  virtual void shutdown() override {
    if (isShutdown()) {
      return;
    }

    if (m_scheduled_writes_enabled) {
      {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (isShutdown()) {
          return;
        }

        m_shutdown_flag.test_and_set(std::memory_order_release);
      }

      m_work_cond.notify_all();
      m_empty_cond.notify_all();
      Dmn_Proc::wait();
      QueueType::shutdown();

      return;
    }

    m_shutdown_flag.test_and_set(std::memory_order_release);

    QueueType::shutdown();

    if (m_hasFn) {
      Dmn_Proc::wait();
    }
  }

private:
  using ScheduledQueue =
      std::multimap<std::chrono::steady_clock::time_point, T>;

  using QueueType::pop;
  using QueueType::popNoWait;
  using QueueType::push;

  /**
   * @brief Run the scheduled worker until shutdown and accepted work drain.
   *
   * The caller holds no pipe lock on entry. The loop selects ordinary queue
   * items before due scheduled items, releases @c m_mutex while invoking @p fn,
   * and waits on @c m_work_cond when no item is ready. A callback exception
   * propagates to the worker boundary and stops the loop.
   *
   * @param fn Callback invoked for each selected item.
   */
  void scheduledProcessingLoop(const Dmn_Pipe::Task &fn);

  void recordProcessingFailure(std::exception_ptr failure) {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (!m_processing_exception) {
        m_processing_exception = std::move(failure);
      }
    }

    m_empty_cond.notify_all();
    m_work_cond.notify_all();
  }

  auto hasProcessingFailure() -> bool {
    std::lock_guard<std::mutex> lock(m_mutex);

    return static_cast<bool>(m_processing_exception);
  }

  std::mutex m_mutex{}; ///< Protects scheduled work and processed accounting.
  std::condition_variable
      m_empty_cond{}; ///< Signalled when processed count reaches inbound count.
  std::condition_variable
      m_work_cond{}; ///< Wakes scheduled worker for new work or shutdown.
  ScheduledQueue m_scheduled_queue{}; ///< Work ordered by deadline.
  size_t m_count{}; ///< Total number of items processed (popped and handled).
  uint64_t m_submitted_count{}; ///< Accepted writes in scheduled mode.
  std::exception_ptr
      m_processing_exception{};       ///< First processing/worker failure.
  std::atomic_flag m_shutdown_flag{}; ///< Set when shutdown() is called.
  bool m_hasFn{}; ///< @c true when a background processing task was provided.
  bool m_scheduled_writes_enabled{}; ///< Selects the deadline-aware worker.
}; // class Dmn_Pipe

template <typename T, typename QueueType>
Dmn_Pipe<T, QueueType>::Dmn_Pipe(std::string_view name, Dmn_Pipe::Task fn,
                                 size_t count, long timeout,
                                 bool enable_scheduled_writes)
    : Dmn_Proc{name}, m_hasFn{nullptr != fn},
      m_scheduled_writes_enabled{enable_scheduled_writes} {
  if (m_scheduled_writes_enabled && !fn) {
    throw std::invalid_argument(
        "Dmn_Pipe scheduled writes require a processing task");
  }

  if (fn) {
    bool execSuccess{};

    if (m_scheduled_writes_enabled) {
      execSuccess = exec([this, fn = std::move(fn)]() {
        try {
          scheduledProcessingLoop(fn);
        } catch (...) {
          recordProcessingFailure(std::current_exception());
        }
      });
    } else {
      execSuccess = exec([this, fn = std::move(fn), count, timeout]() {
        while (true) {
          Dmn_Proc::yield();

          try {
            readAndProcess(fn, count, timeout);
          } catch (...) {
            recordProcessingFailure(std::current_exception());

            break;
          }

          // This check runs inside the worker after startup and determines
          // when its processing loop should exit.
          if (isShutdown() || hasProcessingFailure()) {
            break;
          }
        }
      });
    }

    if (!execSuccess) {
      // Startup failure leaves no worker to process queued work, so fail
      // construction instead of returning an unusable pipe.
      throw std::runtime_error("failed to start Dmn_Pipe worker");
    }
  }
}

template <typename T, typename QueueType>
Dmn_Pipe<T, QueueType>::~Dmn_Pipe() noexcept try {
  shutdown();
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

template <typename T, typename QueueType>
auto Dmn_Pipe<T, QueueType>::read() -> std::optional<T> {
  std::optional<T> data{};

  readAndProcess([&data](T &&item) { data = std::move_if_noexcept(item); });

  return data;
}

template <typename T, typename QueueType>
auto Dmn_Pipe<T, QueueType>::read(size_t count,
                                  long timeout) -> std::vector<T> {
  std::vector<T> dataList{};

  readAndProcess([&dataList](T &&item) { dataList.push_back(std::move(item)); },
                 count, timeout);

  return std::move(dataList);
}

template <typename T, typename QueueType>
auto Dmn_Pipe<T, QueueType>::readAndProcess(Dmn_Pipe::Task fn, size_t count,
                                            long timeout) -> size_t {
  auto dataList = this->pop(count, timeout);

  size_t processedCount{};

  try {
    if (fn) {
      for (auto &item : dataList) {
        Dmn_Proc::testcancel();

        fn(std::move_if_noexcept(item));
        ++processedCount;
      }
    } else {
      processedCount = dataList.size();
    }
  } catch (...) {
    const auto failure = std::current_exception();

    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_count += processedCount;

      if (!m_processing_exception) {
        m_processing_exception = failure;
      }
    }

    m_empty_cond.notify_all();
    m_work_cond.notify_all();

    throw;
  }

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_count += processedCount;
  }

  m_empty_cond.notify_all();

  return processedCount;
}

template <typename T, typename QueueType>
void Dmn_Pipe<T, QueueType>::write(const T &item) {
  if (m_scheduled_writes_enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (isShutdown()) {
      throw std::runtime_error("Dmn_Pipe: write attempted on shutdown pipe");
    }

    QueueType::push(item);
    ++m_submitted_count;
    m_work_cond.notify_one();

    return;
  }

  QueueType::push(item);
}

template <typename T, typename QueueType>
void Dmn_Pipe<T, QueueType>::write(T &&item) {
  if (m_scheduled_writes_enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (isShutdown()) {
      throw std::runtime_error("Dmn_Pipe: write attempted on shutdown pipe");
    }

    QueueType::push(std::move_if_noexcept(item));
    ++m_submitted_count;
    m_work_cond.notify_one();

    return;
  }

  QueueType::push(std::move_if_noexcept(item));
}

template <typename T, typename QueueType>
void Dmn_Pipe<T, QueueType>::writeAt(
    std::chrono::steady_clock::time_point deadline, const T &item) {
  if (!m_scheduled_writes_enabled) {
    throw std::logic_error(
        "Dmn_Pipe: writeAt requires scheduled writes to be enabled");
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  if (isShutdown()) {
    throw std::runtime_error("Dmn_Pipe: writeAt attempted on shutdown pipe");
  }

  m_scheduled_queue.emplace(deadline, item);
  ++m_submitted_count;
  m_work_cond.notify_one();
}

template <typename T, typename QueueType>
void Dmn_Pipe<T, QueueType>::writeAt(
    std::chrono::steady_clock::time_point deadline, T &&item) {
  if (!m_scheduled_writes_enabled) {
    throw std::logic_error(
        "Dmn_Pipe: writeAt requires scheduled writes to be enabled");
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  if (isShutdown()) {
    throw std::runtime_error("Dmn_Pipe: writeAt attempted on shutdown pipe");
  }

  m_scheduled_queue.emplace(deadline, std::move_if_noexcept(item));
  ++m_submitted_count;
  m_work_cond.notify_one();
}

template <typename T, typename QueueType>
auto Dmn_Pipe<T, QueueType>::waitForEmpty() -> uint64_t {
  if (m_scheduled_writes_enabled) {
    std::unique_lock<std::mutex> lock(m_mutex);

    Dmn_Proc::testcancel();
    const auto inbound_count = m_submitted_count;

    m_empty_cond.wait(lock, [this, inbound_count] {
      return m_count >= inbound_count || m_processing_exception || isShutdown();
    });

    if (m_processing_exception) {
      std::rethrow_exception(m_processing_exception);
    }

    return inbound_count;
  }

  uint64_t inbound_count{};

  inbound_count = QueueType::waitForEmpty();

  std::unique_lock<std::mutex> lock(m_mutex);

  Dmn_Proc::testcancel();

  m_empty_cond.wait(lock, [this, inbound_count] {
    return m_count >= inbound_count || m_processing_exception || isShutdown();
  });

  if (m_processing_exception) {
    std::rethrow_exception(m_processing_exception);
  }

  return inbound_count;
}

template <typename T, typename QueueType>
void Dmn_Pipe<T, QueueType>::scheduledProcessingLoop(const Dmn_Pipe::Task &fn) {
  while (true) {
    std::optional<T> item{};

    {
      std::unique_lock<std::mutex> lock(m_mutex);

      while (true) {
        if (m_processing_exception) {
          return;
        }

        auto immediate_item = QueueType::popNoWait();
        if (immediate_item) {
          item.emplace(std::move_if_noexcept(*immediate_item));

          break;
        }

        if (!m_scheduled_queue.empty()) {
          const auto deadline = m_scheduled_queue.begin()->first;
          const auto now = std::chrono::steady_clock::now();

          if (deadline <= now) {
            item.emplace(std::move(m_scheduled_queue.begin()->second));
            m_scheduled_queue.erase(m_scheduled_queue.begin());

            break;
          }

          m_work_cond.wait_until(lock, deadline);

          continue;
        }

        if (isShutdown()) {
          return;
        }

        m_work_cond.wait(lock);
      }
    }

    fn(std::move_if_noexcept(*item));

    {
      std::lock_guard<std::mutex> lock(m_mutex);
      ++m_count;
    }

    m_empty_cond.notify_all();
  }
}

} // namespace dmn

#endif // DMN_PIPE_HPP_
