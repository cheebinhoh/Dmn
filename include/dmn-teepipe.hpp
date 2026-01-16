/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-teepipe.hpp
 * @brief Tee-pipe utility used to merge multiple input sources into a single
 *        outbound pipe in a controlled, ordered way.
 *
 * Overview
 * --------
 * Dmn_TeePipe implements a tee/merge pipeline abstraction: multiple clients can
 * obtain their own Dmn_TeePipeSource and write data into it. The Dmn_TeePipe
 * collects one item from each active source and forwards them into the single
 * outbound Dmn_Pipe<T> (the base class), preserving a per-collection ordering
 * defined by an optional PostProcessingTask.
 *
 * Behavior and semantics
 * ----------------------
 * - Each Dmn_TeePipeSource contains a small, bounded buffer (Dmn_LimitBuffer)
 *   and exposes the Dmn_Io<T> interface (write methods). Clients push items
 *   into their dedicated source instances.
 * - The tee-pipe runs a dedicated conveyor thread (via Dmn_Proc) that waits
 *   until at least one item is available from every source in the current
 *   set of sources (m_buffers). When that condition is met it:
 *     1. Reads one item from each source.
 *     2. Optionally calls the PostProcessingTask on the collected vector of
 *        items to allow reordering or batch-processing semantics.
 *     3. Forwards the (possibly reordered) items into the outbound pipe by
 *        calling Dmn_Pipe<T>::write for each element in sequence.
 * - The pipeline is designed to process one element per source per conveyor
 *   cycle; therefore the order in which items from different sources appear on
 *   the outbound pipe is determined by the PostProcessingTask (if provided).
 *
 * Threading and synchronization
 * -----------------------------
 * - Synchronization between writers, source management, and the conveyor is
 *   implemented with a pthread mutex and condition variables:
 *     * m_mutex protects shared state (m_buffers and m_fill_buffer_count).
 *     * m_cond is signalled whenever a source becomes available or data is
 *       pushed into a source.
 *     * m_empty_cond is signalled when the conveyor consumes data and a
 *       waiting thread (e.g. removeDmn_TeePipeSource) can proceed.
 * - m_fill_buffer_count tracks the total number of items that have been
 *   written to source buffers and not yet consumed by the conveyor. It is
 *   incremented by Dmn_TeePipeSource::write and decremented when the conveyor
 *   reads from each source.
 *
 * Lifecycle notes
 * ----------------
 * - addDmn_TeePipeSource creates a new source with capacity 1 (a minimal
 *   bounded buffer) and returns a shared_ptr to it. The caller keeps this
 *   shared_ptr to write data; when the application releases the shared_ptr
 *   (i.e. its use_count drops to 1), wait() can be used to block until all
 *   open sources are closed and all buffered data is drained.
 * - removeDmn_TeePipeSource removes a particular source from m_buffers. It
 *   blocks until the source's internal buffer is empty before erasing it to
 *   ensure no data is lost.
 * - The destructor stops the conveyor thread (m_conveyor reset) before
 *   destroying the synchronization primitives to avoid waking/joining a thread
 *   that would use destroyed synchronization objects.
 *
 * Type aliases
 * ------------
 * - Task: callable invoked by the base Dmn_Pipe<T> to process outbound items
 *         (inherited behavior).
 * - PostProcessingTask: optional callable taking a vector<T>&; it receives one
 *         item from each source and can reorder or mutate them before they are
 *         forwarded to the outbound pipe.
 *
 * Example usage (sketch)
 * ----------------------
 * - Create Dmn_TeePipe<T> pipe("name", outboundTask, postProcessingFn).
 * - auto s1 = pipe.addDmn_TeePipeSource(); auto s2 =
 * pipe.addDmn_TeePipeSource();
 * - s1->write(item1); s2->write(item2);
 * - The conveyor will collect item1 and item2, call postProcessingFn if set,
 *   then forward each result to the outboundTask in order.
 */

#ifndef DMN_TEEPIPE_HPP_

#define DMN_TEEPIPE_HPP_

#include <algorithm>
#include <cassert>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include "dmn-io.hpp"
#include "dmn-limit-buffer.hpp"
#include "dmn-pipe.hpp"
#include "dmn-proc.hpp"

namespace dmn {

template <typename T> class Dmn_TeePipe : private Dmn_Pipe<T> {
  using Task = std::function<void(T)>;
  using PostProcessingTask = std::function<void(std::vector<T> &)>;

  /**
   * Dmn_TeePipeSource
   *
   * A single input/source for the tee-pipe. It owns a bounded buffer and
   * implements Dmn_Io<T> so callers can push data into this source. The
   * conveyor thread reads from each active Dmn_TeePipeSource via the private
   * read() method.
   */
  class Dmn_TeePipeSource : private Dmn_LimitBuffer<T>, public Dmn_Io<T> {
    friend class Dmn_TeePipe<T>;

  public:
    Dmn_TeePipeSource(size_t capacity, Dmn_TeePipe *tp);
    virtual ~Dmn_TeePipeSource() = default;

    /**
     * Copy the item into the source buffer.
     *
     * @param item Data to copy into the source buffer.
     */
    void write(T &item) override;

    /**
     * Move the item into the source buffer when possible.
     *
     * @param item Data to move into the source buffer.
     */
    void write(T &&item) override;

    /**
     * Convenience overload which chooses to move or copy depending on the
     * 'move' flag.
     *
     * @param item Data to push into the source buffer.
     * @param move If true, perform a move; otherwise copy.
     */
    void write(T &item, bool move);

  private:
    /**
     * Pop one element from the underlying Dmn_LimitBuffer. Returns empty
     * optional if buffer is empty.
     */
    std::optional<T> read() override;

    Dmn_TeePipe *m_teepipe{};
  }; // class Dmn_TeePipeSource

public:
  explicit Dmn_TeePipe(std::string_view name, Dmn_TeePipe::Task fn = {},
                       Dmn_TeePipe::PostProcessingTask pfn = {});

  virtual ~Dmn_TeePipe() noexcept;

  Dmn_TeePipe(const Dmn_TeePipe<T> &obj) = delete;
  const Dmn_TeePipe<T> &operator=(const Dmn_TeePipe<T> &obj) = delete;
  Dmn_TeePipe(const Dmn_TeePipe<T> &&obj) = delete;
  Dmn_TeePipe<T> &operator=(Dmn_TeePipe<T> &&obj) = delete;

  /**
   * Create and return a new Dmn_TeePipeSource. The returned shared_ptr should
   * be held by the caller for as long as the source is intended to be used.
   *
   * @return shared_ptr to newly created source.
   */
  std::shared_ptr<Dmn_TeePipeSource> addDmn_TeePipeSource();

  /**
   * Remove a previously-added source. This blocks until the source buffer is
   * empty, then erases it from the active source list. After removal, the
   * provided shared_ptr is reset.
   *
   * @param tps shared_ptr to the source to remove (must be non-null and
   *            belong to this Dmn_TeePipe).
   */
  void removeDmn_TeePipeSource(std::shared_ptr<Dmn_TeePipeSource> &tps);

  /**
   * Wait until all sources are closed (no external owners) and the internal
   * buffers are drained. This call returns once the tee-pipe has no open
   * sources and no pending items.
   */
  void wait();

  /**
   * Wait until all internal buffers (including outbound base pipe) are empty.
   * Returns the number of remaining items in the outbound pipe (inherited
   * behavior) after draining.
   */
  size_t waitForEmpty() override;

private:
  /**
   * Internal variant of wait() which optionally considers whether there are
   * any open sources still held by external owners.
   *
   * @param no_open_source If true, wait until there are no external owners of
   *                       any Dmn_TeePipeSource before draining.
   */
  size_t wait(bool no_open_source);

  /**
   * Move/copy the given item into the outbound Dmn_Pipe<T>.
   *
   * @param item Item to forward to outbound pipe.
   */
  void write(T &item) override;

  /**
   * The conveyor routine: repeatedly waits until at least one item from each
   * active source is available, collects a vector<T> of those items, calls
   * the optional m_post_processing_task_fn on the vector, then forwards each
   * element to the outbound pipe.
   */
  void runConveyorExec();

  /**
   * Data members for constructor to instantiate the object.
   */
  std::unique_ptr<Dmn_Proc> m_conveyor{};
  Dmn_TeePipe::PostProcessingTask m_post_processing_task_fn{};

  /**
   * Synchronization primitives and state for coordinating writers and the
   * conveyor thread.
   */
  pthread_mutex_t m_mutex{};
  pthread_cond_t m_cond{};
  pthread_cond_t m_empty_cond{};
  size_t m_fill_buffer_count{}; // number of items written to sources but not
                                // yet consumed
  std::vector<std::shared_ptr<Dmn_TeePipeSource>> m_buffers{};
}; // class Dmn_TeePipe

template <typename T>
Dmn_TeePipe<T>::Dmn_TeePipeSource::Dmn_TeePipeSource(size_t capacity,
                                                     Dmn_TeePipe *tp)
    : Dmn_LimitBuffer<T>{capacity}, m_teepipe(tp) {}

template <typename T> void Dmn_TeePipe<T>::Dmn_TeePipeSource::write(T &item) {
  write(item, false);
}

template <typename T> void Dmn_TeePipe<T>::Dmn_TeePipeSource::write(T &&item) {
  T moved_item = std::move_if_noexcept(item);

  write(moved_item, true);
}

template <typename T>
void Dmn_TeePipe<T>::Dmn_TeePipeSource::write(T &item, bool move) {
  assert(m_teepipe);

  Dmn_LimitBuffer<T>::push(item, move);

  int err = pthread_mutex_lock(&(m_teepipe->m_mutex));
  if (err) {
    throw std::runtime_error(strerror(err));
  }

  DMN_PROC_ENTER_PTHREAD_MUTEX_CLEANUP(&(m_teepipe->m_mutex));

  pthread_testcancel();

  m_teepipe->m_fill_buffer_count++;

  err = pthread_cond_signal(&(m_teepipe->m_cond));
  if (err) {
    pthread_mutex_unlock(&(m_teepipe->m_mutex));

    throw std::runtime_error(strerror(err));
  }

  DMN_PROC_EXIT_PTHREAD_MUTEX_CLEANUP();

  err = pthread_mutex_unlock(&(m_teepipe->m_mutex));
  if (err) {
    throw std::runtime_error(strerror(err));
  }
}

template <typename T>
std::optional<T> Dmn_TeePipe<T>::Dmn_TeePipeSource::read() {
  return Dmn_LimitBuffer<T>::pop();
}

template <typename T>
Dmn_TeePipe<T>::Dmn_TeePipe(std::string_view name, Dmn_TeePipe::Task fn,
                            Dmn_TeePipe::PostProcessingTask pfn)
    : Dmn_Pipe<T>{name, fn},
      m_conveyor{std::make_unique<Dmn_Proc>(std::string(name) + "-conveyor")},
      m_post_processing_task_fn{pfn} {
  int err{};

  err = pthread_mutex_init(&m_mutex, NULL);
  if (err) {
    throw std::runtime_error(strerror(err));
  }

  err = pthread_cond_init(&m_cond, NULL);
  if (err) {
    throw std::runtime_error(strerror(err));
  }

  err = pthread_cond_init(&m_empty_cond, NULL);
  if (err) {
    throw std::runtime_error(strerror(err));
  }

  runConveyorExec();
}

template <typename T> Dmn_TeePipe<T>::~Dmn_TeePipe() noexcept try {
  // this is important as the conveyor thread uses the conditional variable
  // and mutex, so we need to stop the thread before destroying both objects
  // at the end of life of the Dmn_TeePipe object, as otherwise, those threads
  // will fail to be awaken from the conditional variable state, and leak to
  // system wide leak of available thread resource.
  m_conveyor = {};

  pthread_cond_destroy(&m_cond);
  pthread_cond_destroy(&m_empty_cond);
  pthread_mutex_destroy(&m_mutex);
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

template <typename T>
std::shared_ptr<typename Dmn_TeePipe<T>::Dmn_TeePipeSource>
Dmn_TeePipe<T>::addDmn_TeePipeSource() {
  std::shared_ptr<Dmn_TeePipeSource> sp_tpSource{
      std::make_shared<Dmn_TeePipeSource>(1, this)};

  int err = pthread_mutex_lock(&m_mutex);
  if (err) {
    throw std::runtime_error(strerror(err));
  }

  DMN_PROC_ENTER_PTHREAD_MUTEX_CLEANUP(&m_mutex);

  pthread_testcancel();

  m_buffers.push_back(sp_tpSource);

  err = pthread_cond_signal(&m_cond);
  if (err) {
    pthread_mutex_unlock(&m_mutex);

    throw std::runtime_error(strerror(err));
  }

  DMN_PROC_EXIT_PTHREAD_MUTEX_CLEANUP();

  err = pthread_mutex_unlock(&m_mutex);
  if (err) {
    throw std::runtime_error(strerror(err));
  }

  return sp_tpSource;
}

template <typename T>
void Dmn_TeePipe<T>::removeDmn_TeePipeSource(
    std::shared_ptr<typename Dmn_TeePipe<T>::Dmn_TeePipeSource> &tps) {
  assert(nullptr != tps);
  assert(this == tps->m_teepipe);

  int err = pthread_mutex_lock(&m_mutex);
  if (err) {
    throw std::runtime_error(strerror(err));
  }

  DMN_PROC_ENTER_PTHREAD_MUTEX_CLEANUP(&m_mutex);

  pthread_testcancel();

  auto iter =
      std::find_if(m_buffers.begin(), m_buffers.end(),
                   [tps](std::shared_ptr<Dmn_TeePipeSource> sp_iterTps) {
                     return tps.get() == sp_iterTps.get();
                   });

  if (iter != m_buffers.end()) {
    while (tps->size() > 0) {
      err = pthread_cond_wait(&m_empty_cond, &m_mutex);
      if (err) {
        throw std::runtime_error(strerror(err));
      }

      pthread_testcancel();
    }

    m_buffers.erase(iter);
    tps = {};
  }

  err = pthread_cond_signal(&m_cond);
  if (err) {
    pthread_mutex_unlock(&m_mutex);

    throw std::runtime_error(strerror(err));
  }

  DMN_PROC_EXIT_PTHREAD_MUTEX_CLEANUP();

  err = pthread_mutex_unlock(&m_mutex);
  if (err) {
    throw std::runtime_error(strerror(err));
  }
}

template <typename T> void Dmn_TeePipe<T>::wait() { wait(true); }

template <typename T> size_t Dmn_TeePipe<T>::waitForEmpty() {
  return wait(false);
}

template <typename T> size_t Dmn_TeePipe<T>::wait(bool no_open_source) {
  int err = pthread_mutex_lock(&m_mutex);
  if (err) {
    throw std::runtime_error(strerror(err));
  }

  DMN_PROC_ENTER_PTHREAD_MUTEX_CLEANUP(&m_mutex);

  pthread_testcancel();

  // only returns if no other object owns the Dmn_TeePipeSource than
  // Dmn_TeePipe
  while (no_open_source && m_buffers.size() > 0 &&
         std::count_if(m_buffers.begin(), m_buffers.end(),
                       [](auto &item) { return item.use_count() > 1; }) > 0) {
    err = pthread_cond_wait(&m_cond, &m_mutex);
    if (err) {
      throw std::runtime_error(strerror(err));
    }

    pthread_testcancel();
  }

  // only returns if no data in buffer from Dmn_TeePipeSource to conveyor
  while (m_fill_buffer_count > 0) {
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

  return Dmn_Pipe<T>::waitForEmpty();
}

template <typename T> void Dmn_TeePipe<T>::write(T &item) {
  Dmn_Pipe<T>::write(item);
}

template <typename T> void Dmn_TeePipe<T>::runConveyorExec() {
  m_conveyor->exec([this]() {
    while (true) {
      int err{};

      err = pthread_mutex_lock(&m_mutex);
      if (err) {
        throw std::runtime_error(strerror(err));
      }

      DMN_PROC_ENTER_PTHREAD_MUTEX_CLEANUP(&m_mutex);

      pthread_testcancel();

      while (m_buffers.empty() || m_fill_buffer_count < m_buffers.size()) {
        err = pthread_cond_wait(&m_cond, &m_mutex);
        if (err) {
          throw std::runtime_error(strerror(err));
        }

        pthread_testcancel();
      }

      std::vector<T> post_processing_buffers{};

      for (auto tps : m_buffers) {
        m_fill_buffer_count--;

        auto data = tps->read();
        assert(data);

        post_processing_buffers.push_back(std::move_if_noexcept(*data));
      }

      if (m_post_processing_task_fn != nullptr) {
        m_post_processing_task_fn(post_processing_buffers);
      }

      for (auto &data : post_processing_buffers) {
        write(data);
      }

      post_processing_buffers.clear();

      err = pthread_cond_signal(&m_empty_cond);
      if (err) {
        pthread_mutex_unlock(&m_mutex);

        throw std::runtime_error(strerror(err));
      }

      DMN_PROC_EXIT_PTHREAD_MUTEX_CLEANUP();

      err = pthread_mutex_unlock(&m_mutex);
      if (err) {
        throw std::runtime_error(strerror(err));
      }
    }
  });
}

} // namespace dmn

#endif // DMN_TEEPIPE_HPP_
