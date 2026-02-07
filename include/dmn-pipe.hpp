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
 * - The class combines Dmn_Buffer<T> (storage), Dmn_Io<T> (I/O interface)
 *   and Dmn_Proc (optional processing thread support).
 *
 * Threading and cancellation
 * - push/pop and internal synchronization are handled by Dmn_Buffer<T>.
 * - Dmn_Pipe adds a pthread_mutex and pthread_cond to:
 *   - keep a count of processed items (`m_count`), and
 *   - allow callers to wait until all currently inbound items have been
 *     processed (waitForEmpty()).
 * - readAndProcess() holds `m_mutex` while invoking the caller-provided task
 *   and updates `m_count` under the mutex. It uses pthread cancellation
 *   points and cleanup macros to remain cancellation-safe.
 *
 * Read / Write semantics
 * - write(T&) copies `item` into the pipe.
 * - write(T&&) moves `item` into the pipe; move will be used when the move
 *   constructor is noexcept (via Dmn_Buffer<T>::push semantics).
 * - read() blocks until the next item is available and returns it wrapped
 *   in std::optional; when the pipe is closed it returns std::nullopt.
 * - readAndProcess(fn) blocks until the next item is available and invokes
 *   the provided task with the item (moved where possible).
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
 *          * If the timeout expires and the pipe contains at least 1 item,
 *            returns however many items are currently available (between 1
 *            and `count`).
 *          * If the timeout expires and the pipe is still empty, the wait
 *            restarts (the implementation re-arms the absolute-time
 *            deadline). This behavior avoids returning an empty result on
 *            spurious timeouts; the function only returns due to timeout when
 *            there is at least one item in the pipe at expiry.
 *
 *   Note: The timeout is interpreted as a maximum time to wait for the full
 *   `count` items (measured from the first blocking wait inside the call).
 *   A zero timeout value means "wait forever".

 *
 * waitForEmpty()
 * - waitForEmpty() blocks until all items that were inbound at the time
 *   of the call have been popped and processed. It returns the number of
 *   items that were passed through the pipe in total during that wait.
 *
 * Error handling
 * - pthread API failures are converted to std::runtime_error with the
 *   strerror message.
 *
 * Lifetime
 * - If a Task is provided to the constructor, a background processing
 *   thread is started via Dmn_Proc::exec which repeatedly calls
 *   readAndProcess(fn).
 * - The destructor stops the background processor (Dmn_Proc::stopExec()),
 *   signals the condition variable, and destroys pthread primitives.
 */

#ifndef DMN_PIPE_HPP_

#define DMN_PIPE_HPP_

#include <pthread.h>

#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

#include "dmn-buffer.hpp"
#include "dmn-debug.hpp"
#include "dmn-io.hpp"
#include "dmn-proc.hpp"

namespace dmn {

template <typename T>
class Dmn_Pipe : public Dmn_Buffer<T>, public Dmn_Io<T>, public Dmn_Proc {
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
   * @brief Read and return the next count of items from the pipe.
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
   * @throws std::runtime_error on pthread errors.
   */
  auto read(size_t count, long timeout = 0) -> std::vector<T> override;

  /**
   * @brief Read the next item from the pipe and invoke the provided task.
   *
   * Blocks until the next item is available. The task is invoked while the
   * internal mutex is held to update processing bookkeeping (`m_count`)
   * and to signal waiting threads. The item is passed to `fn` using move
   * semantics when possible.
   *
   * Note: this method participates in pthread cancellation points and uses
   * the Dmn proc mutex cleanup macros to ensure the mutex is released on
   * cancellation or exceptions.
   *
   * @param fn The functor to process the next item popped from the pipe
   */
  void readAndProcess(Dmn_Pipe::Task fn, size_t count = 1, long timeout = 0);

  /**
   * @brief Write (copy) an item into the pipe.
   *
   * This call enqueues a copy of `item` into the FIFO. Writing is non-blocking;
   * any blocking behavior is determined by the underlying Dmn_Buffer
   * implementation.
   *
   * @param item The data item to be copied into the pipe
   */
  void write(T &item) override;

  /**
   * @brief Write (move) an item into the pipe.
   *
   * This call attempts to move `item` into the FIFO. If move construction is
   * noexcept it will move; otherwise behavior follows Dmn_Buffer push policy.
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
  using Dmn_Buffer<T>::pop;
  using Dmn_Buffer<T>::popNoWait;
  using Dmn_Buffer<T>::push;

  std::mutex m_mutex{};
  std::condition_variable m_empty_cond{};
  size_t m_count{};
}; // class Dmn_Pipe

template <typename T>
Dmn_Pipe<T>::Dmn_Pipe(std::string_view name, Dmn_Pipe::Task fn, size_t count,
                      long timeout)
    : Dmn_Proc{name} {
  if (fn) {
    exec([this, fn, count, timeout]() {
      while (true) {
        readAndProcess(fn, count, timeout);
      }
    });
  }
}

template <typename T> Dmn_Pipe<T>::~Dmn_Pipe() noexcept try {
  // stopExec is not noexcept, so we need to resolve it in destructor
  Dmn_Proc::stopExec();

  m_empty_cond.notify_all();
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

template <typename T> auto Dmn_Pipe<T>::read() -> std::optional<T> {
  T data{};

  readAndProcess([&data](T &&item) { data = item; });

  return std::move_if_noexcept(data);
}

template <typename T>
auto Dmn_Pipe<T>::read(size_t count, long timeout) -> std::vector<T> {
  std::vector<T> dataList{};

  readAndProcess([&dataList](T &&item) { dataList.push_back(std::move(item)); },
                 count, timeout);

  return std::move(dataList);
}

template <typename T>
void Dmn_Pipe<T>::readAndProcess(Dmn_Pipe::Task fn, size_t count,
                                 long timeout) {
  auto dataList = this->pop(count, timeout);

  pthread_testcancel();

  std::unique_lock lock{m_mutex};

  for (auto &item : dataList) {
    fn(std::move_if_noexcept(item));
    ++m_count;
  }

  lock.unlock();

  m_empty_cond.notify_all();

  pthread_testcancel();
}

template <typename T> void Dmn_Pipe<T>::write(T &item) {
  Dmn_Buffer<T>::push(item, false);
}

template <typename T> void Dmn_Pipe<T>::write(T &&item) {
  Dmn_Buffer<T>::push(item, true);
}

template <typename T> auto Dmn_Pipe<T>::waitForEmpty() -> size_t {
  size_t inbound_count{};

  inbound_count = Dmn_Buffer<T>::waitForEmpty();

  std::unique_lock lock{m_mutex};

  pthread_testcancel();

  m_empty_cond.wait(lock,
                    [this, inbound_count] { return inbound_count >= m_count; });

  lock.unlock();

  pthread_testcancel();

  return inbound_count;
}

} // namespace dmn

#endif // DMN_PIPE_HPP_
