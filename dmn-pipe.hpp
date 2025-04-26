/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * This class implements a fifo pipe that:
 * - write is not blocking
 * - a thread (via Proc) can be setup to process each item pop out from
 *   the fifo pipe.
 * - client can call read() to read the next item pop out of the fifo pipe,
 *   or blocked waiting for one if the fifo pipe is empty.
 * - client can call readAndProcess() to read the next item pop out of the
 *   fifo pipe and invoke the Task to process the item or blocked waiting
 *   for one if the fifo pipe is empty.
 */

#ifndef DMN_PIPE_HPP_

#define DMN_PIPE_HPP_

#include <pthread.h>

#include <cstring>
#include <functional>
#include <optional>

#include "dmn-buffer.hpp"
#include "dmn-debug.hpp"
#include "dmn-io.hpp"
#include "dmn-proc.hpp"

namespace dmn {

template <typename T> class Pipe : public Buffer<T>, public Io<T>, public Proc {
  using Task = std::function<void(T &&)>;

public:
  Pipe(std::string_view name, Pipe::Task fn = {});

  virtual ~Pipe() noexcept;

  Pipe(const Pipe<T> &dmnPipe) = delete;
  const Pipe<T> &operator=(const Pipe<T> &dmnPipe) = delete;
  Pipe(Pipe<T> &&dmnPipe) = delete;
  Pipe<T> &operator=(Pipe<T> &&dmnPipe) = delete;

  /**
   * @brief The method will read and return an item from the pipe or
   *        std::nullopt if the pipe is closed. The read is blocked waiting
   *        for next pop out of the pipe.
   *
   * @return optional item if there is next item from pipe, or std::nullopt
   *         if pipe is closed
   */
  std::optional<T> read() override;

  /**
   * @brief The method read the next item pop out of pipe and call fn functor
   *        to process item, the method is blocked waiting for next item
   *        from the pipe.
   *
   * @param fn functor to process next item pop out of pipe
   */
  void readAndProcess(Pipe::Task fn);

  /**
   * @brief The method will write data into the pipe, the data is copied
   *        than move semantic.
   *
   * @param rItem The data item to be copied into pipe
   */
  void write(T &item) override;

  /**
   * @brief The method will write data into the pipe, the data is moved
   *        into pipe if noexcept.
   *
   * @param item The data item to be moved into pipe
   */
  void write(T &&item) override;

  /**
   * @brief The method will put the client on blocking wait until
   *        the pipe is empty and all items have been pop out and processed,
   *        it returns number of items that were passed through the pipe in
   *        total upon return.
   *
   * @return The number of items that were passed through the pipe
   *         in total
   */
  long long waitForEmpty() override;

private:
  using Buffer<T>::pop;
  using Buffer<T>::popNoWait;
  using Buffer<T>::push;

  pthread_mutex_t m_mutex{};
  pthread_cond_t m_empty_cond{};
  long long m_count{};
}; // class Pipe

template <typename T>
Pipe<T>::Pipe(std::string_view name, Pipe::Task fn) : Proc{name} {
  int err = pthread_mutex_init(&m_mutex, NULL);
  if (err) {
    throw std::runtime_error(strerror(err));
  }

  err = pthread_cond_init(&m_empty_cond, NULL);
  if (err) {
    throw std::runtime_error(strerror(err));
  }

  if (fn) {
    exec([this, fn]() {
      while (true) {
        readAndProcess(fn);
      }
    });
  }
}

template <typename T> Pipe<T>::~Pipe() noexcept try {
  // stopExec is not noexcept, so we need to resolve it in destructor
  Proc::stopExec();

  pthread_cond_signal(&m_empty_cond);
  pthread_cond_destroy(&m_empty_cond);
  pthread_mutex_destroy(&m_mutex);
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

template <typename T> std::optional<T> Pipe<T>::read() {
  T data{};

  try {
    readAndProcess([&data](T &&item) { data = item; });
  } catch (...) {
    return {};
  }

  return std::move_if_noexcept(data);
}

template <typename T> void Pipe<T>::readAndProcess(Pipe::Task fn) {
  T &&item = this->pop();

  int err = pthread_mutex_lock(&m_mutex);
  if (err) {
    throw std::runtime_error(strerror(err));
  }

  DMN_PROC_ENTER_PTHREAD_MUTEX_CLEANUP(&m_mutex);

  pthread_testcancel();

  fn(std::move_if_noexcept(item));

  ++m_count;

  err = pthread_cond_signal(&m_empty_cond);
  if (err) {
    pthread_mutex_unlock(&m_mutex);

    throw std::runtime_error(strerror(err));
  }

  pthread_testcancel();

  DMN_PROC_EXIT_PTHREAD_MUTEX_CLEANUP();

  err = pthread_mutex_unlock(&m_mutex);
  if (err) {
    throw std::runtime_error(strerror(err));
  }
}

template <typename T> void Pipe<T>::write(T &item) {
  Buffer<T>::push(item, false);
}

template <typename T> void Pipe<T>::write(T &&item) {
  Buffer<T>::push(item, true);
}

template <typename T> long long Pipe<T>::waitForEmpty() {
  long long inboundCount{};

  inboundCount = Buffer<T>::waitForEmpty();

  int err = pthread_mutex_lock(&m_mutex);
  if (err) {
    throw std::runtime_error(strerror(err));
  }

  DMN_PROC_ENTER_PTHREAD_MUTEX_CLEANUP(&m_mutex);

  pthread_testcancel();

  while (m_count < inboundCount) {
    err = pthread_cond_wait(&m_empty_cond, &m_mutex);
    if (err) {
      throw std::runtime_error(strerror(err));
    }

    pthread_testcancel();
  }

  DMN_PROC_EXIT_PTHREAD_MUTEX_CLEANUP();

  err = pthread_mutex_unlock(&m_mutex);
  if (err) {
    throw std::runtime_error(strerror(err));
  }

  return inboundCount;
}

} // namespace dmn

#endif // DMN_PIPE_HPP_
