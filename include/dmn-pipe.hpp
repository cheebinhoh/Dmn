/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-pipe.hpp
 * @brief Dmn_Pipe: a FIFO pipe with non-blocking writers and optional
 *        background processing.
 *
 * Overview
 * - Dmn_Pipe<T> implements a FIFO buffer that:
 *   - allows producers to write without blocking (write operations enqueue
 *     items immediately),
 *   - allows a consumer to read items either synchronously via `read()` or
 *     by providing a processing task via `readAndProcess()` or by launching a
 *     background processing thread (using Dmn_Proc::exec).
 * - The class combines Dmn_BlockingQueue<T> (storage), Dmn_Io<T> (I/O
 interface)
 *   and Dmn_Proc (optional processing thread support).
 *
 * Threading and cancellation
 * - push/pop and internal synchronization are handled by Dmn_BlockingQueue<T>.
 * - Dmn_Pipe adds a mutex and condition variable to:
 *   - keep a count of processed items (`m_count`), and
 *   - allow callers to wait until all currently inbound items have been
 *     processed (waitForEmpty()).
 * - readAndProcess() invokes the caller-provided task and updates `m_count`
 *   under the mutex.
 *
 * Read / Write semantics
 * - write(T&) copies `item` into the pipe.
 * - write(T&&) moves `item` into the pipe; move will be used when the move
 *   constructor is noexcept (via Dmn_BlockingQueue<T>::push semantics).
 * - read() blocks until the next item is available and returns it wrapped
 *   in std::optional; when the pipe is closed it returns std::nullopt.
 * - readAndProcess(fn) blocks until the next item is available or timeout
 *   and invokes the provided task with the item (moved where possible).
 *
 * - read(count, timeout) and readAndProcss(fn, count, timeout) function
 *   behaves like it counterpart without count and timeout but with the
 *   following blocking behevior
 *     1. If the pipe already contains >= count items, it returns exactly
 *        `count` items immediately.
 *     2. If the pipe contains 0 items, it blocks:
 *        - If timeout == 0: blocks indefinitely until at least `count` items
 *          become available (returns exactly `count`).
 *        - If timeout > 0: waits up to `timeout` microseconds for items.
 *          * If enough items are available before timeout, returns exactly
 *            `count` items.
 *          * If the timeout expires and it returns whatever items available
 *            up to `count` items or no item.
 *
 *   Note: The timeout is interpreted as a maximum time to wait for the full
 *   `count` items (measured from the first blocking wait inside the call).
 *   A zero timeout value means "wait forever".

 *
 * waitForEmpty()
 * - waitForEmpty() blocks until all items that were inbound into the pipe
 *   has been processed (or pop out).
 *
 * Lifetime
 * - If a Task is provided to the constructor, a background processing
 *   thread is started via Dmn_Proc::exec which repeatedly calls
 *   readAndProcess(fn).
 * - The destructor stops the background processor (Dmn_Proc::stopExec()),
 *   signals the condition variable.
 */

#ifndef DMN_PIPE_HPP_

#define DMN_PIPE_HPP_

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

#include "dmn-blockingqueue.hpp"
#include "dmn-debug.hpp"
#include "dmn-io.hpp"
#include "dmn-proc.hpp"

namespace dmn {

template <typename T>
class Dmn_Pipe : public Dmn_BlockingQueue<T>,
                 public Dmn_Io<T>,
                 public Dmn_Proc {
  using Task = std::function<void(T &&)>;

public:
  explicit Dmn_Pipe(std::string_view name, Dmn_Pipe::Task fn = {},
                    size_t count = 1, long timeout = 0);

  virtual ~Dmn_Pipe() noexcept;

  Dmn_Pipe(const Dmn_Pipe<T> &obj) = delete;
  const Dmn_Pipe<T> &operator=(const Dmn_Pipe<T> &obj) = delete;
  Dmn_Pipe(Dmn_Pipe<T> &&obj) = delete;
  Dmn_Pipe<T> &operator=(Dmn_Pipe<T> &&obj) = delete;

  /**
   * @brief Read and return the next item from the pipe.
   *
   * Blocks until the next item is available. If the pipe has been closed and
   * no further items will arrive, returns std::nullopt.
   *
   * @return optional item if available, or std::nullopt if the pipe is closed
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
   *     * If timeout expires and the pipe is still empty, the function keeps
   *       waiting (re-arming the absolute deadline) until at least one item is
   *       available.
   *
   * The returned vector contains moved items removed from the pipe.
   *
   * @param count   Number of desired items (must be > 0).
   * @param timeout Timeout in microseconds for waiting for the full count.
   *                A value of 0 means wait forever.
   * @return Vector of items (size == count on success without timeout, or
   *         between 1 and count if a timeout occurred after at least one item
   *         was produced).
   */
  auto read(size_t count, long timeout = 0) -> std::vector<T> override;

  /**
   * @brief Read the next item from the pipe and invoke the provided task.
   *
   * Blocks until the next item is available. The task is invoked without
   * holding m_mutex which is only held after items are processed and update the
   * m_count.
   *
   * @param fn The functor to process the next item popped from the pipe
   * @return The number of items read and processed.
   */
  auto readAndProcess(Dmn_Pipe::Task fn, size_t count = 1,
                      long timeout = 0) noexcept -> size_t;

  /**
   * @brief Write (copy) an item into the pipe.
   *
   * This call enqueues a copy of `item` into the FIFO. Writing is non-blocking;
   * any blocking behavior is determined by the underlying Dmn_BlockingQueue
   * implementation.
   *
   * @param item The data item to be copied into the pipe
   */
  void write(T &item) override;

  /**
   * @brief Write (move) an item into the pipe.
   *
   * This call attempts to move `item` into the FIFO. If move construction is
   * noexcept it will move; otherwise behavior follows Dmn_BlockingQueue push
   * policy.
   *
   * @param item The data item to be moved into the pipe
   */
  void write(T &&item) override;

  /**
   * @brief Block until the pipe is empty and all inbound items are processed.
   *
   * The function waits until all items that were reported inbound at the
   * time of the call have been popped and processed (i.e., `m_count` has
   * advanced to cover them). It returns the number of items that were
   * passed through the pipe during the wait.
   *
   * @return The number of items that were passed through the pipe in total
   */
  auto waitForEmpty() -> size_t override;

private:
  using Dmn_BlockingQueue<T>::pop;
  using Dmn_BlockingQueue<T>::popNoWait;
  using Dmn_BlockingQueue<T>::push;

  std::mutex m_mutex{};
  std::condition_variable m_empty_cond{};
  size_t m_count{};
  std::atomic<bool> m_shutdown{false};
}; // class Dmn_Pipe

template <typename T>
Dmn_Pipe<T>::Dmn_Pipe(std::string_view name, Dmn_Pipe::Task fn, size_t count,
                      long timeout)
    : Dmn_Proc{name} {
  if (fn) {
    exec([this, fn, count, timeout]() {
      while (true) {
        readAndProcess(fn, count, timeout);

        if (m_shutdown.load(std::memory_order_acquire)) {
          break;
        }

        Dmn_Proc::yield();
      }
    });
  }
}

template <typename T> Dmn_Pipe<T>::~Dmn_Pipe() noexcept try {
  // stopExec is not noexcept, so we need to resolve it in destructor
  std::unique_lock<std::mutex> lock(m_mutex);
  m_shutdown.store(true, std::memory_order_release);
  lock.unlock();

  Dmn_BlockingQueue<T>::stop();
  Dmn_Proc::wait();
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

template <typename T> auto Dmn_Pipe<T>::read() -> std::optional<T> {
  std::optional<T> data{};

  readAndProcess([&data](T &&item) { data = std::move_if_noexcept(item); });

  return data;
}

template <typename T>
auto Dmn_Pipe<T>::read(size_t count, long timeout) -> std::vector<T> {
  std::vector<T> dataList{};

  readAndProcess([&dataList](T &&item) { dataList.push_back(std::move(item)); },
                 count, timeout);

  return dataList;
}

template <typename T>
auto Dmn_Pipe<T>::readAndProcess(Dmn_Pipe::Task fn, size_t count,
                                 long timeout) noexcept -> size_t {
  auto dataList = this->pop(count, timeout);

  size_t processedCount = dataList.size();

  if (fn) {
    try {
      for (auto &item : dataList) {
        fn(std::move_if_noexcept(item));
      }
    } catch (...) {
      //
    }
  }

  std::unique_lock<std::mutex> lock(m_mutex);

  m_count += processedCount;

  m_empty_cond.notify_all();

  return processedCount;
}

template <typename T> void Dmn_Pipe<T>::write(T &item) {
  Dmn_BlockingQueue<T>::push(item);
}

template <typename T> void Dmn_Pipe<T>::write(T &&item) {
  Dmn_BlockingQueue<T>::push(std::move_if_noexcept(item));
}

template <typename T> auto Dmn_Pipe<T>::waitForEmpty() -> size_t {
  size_t inbound_count{};

  inbound_count = Dmn_BlockingQueue<T>::waitForEmpty();

  std::unique_lock<std::mutex> lock(m_mutex);

  Dmn_Proc::testcancel();

  m_empty_cond.wait(lock,
                    [this, inbound_count] { return m_count >= inbound_count; });

  return inbound_count;
}

} // namespace dmn

#endif // DMN_PIPE_HPP_
