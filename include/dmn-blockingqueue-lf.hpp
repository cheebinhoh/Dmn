/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-blockingqueue-lf.hpp
 * @brief Thread safe and lock free FIFO queue where pop can be optionally
 * block.
 */

#ifndef DMN_BLOCKINGQUEUE_LF_HPP_
#define DMN_BLOCKINGQUEUE_LF_HPP_

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "dmn-proc.hpp"
#include "dmn-util.hpp"

#include "dmn-blockingqueue-interface.hpp"

namespace dmn {

/**
 * @brief Thread-safe FIFO queue.
 *
 * Template parameter T is the stored item type.
 */
template <typename T = std::string>
class Dmn_BlockingQueue_Lf : public Dmn_BlockingQueue_Interface<T> {
  /**
   * Epoch based Reclamation logic
   *
   * The following parameters control Epoch based reclamation logic
   * for the pop out node with InFlightGuard. Each pop or push call is
   * guard via InFlightGuard.
   *
   * The queue maintains global epoch data in m_epochData which contains
   * the epoch id and the last timepoint where epoch id is updated. Instead
   * of using system time (which will be a hot path) as last time point,
   * we use the number of pop or push call as timepoint reference, if the
   * number of such api call is s_epochTimeScale difference from the last
   * value in m_epochData's m_in_flight_total, both the m_in_flight_total
   * and m_id is moved forward (aka the epoch is moved).
   *
   * Each InFlightGuard entered will derive the m_epochIndex based on
   * current m_epochData.m_id, and each m_id is grouped by s_epochIdScale,
   * for example if s_epochIdScale is , then m_id 0, 1 is one group, 2, 3
   * is another group, then the (m_id / s_epochIdScale) is modular divided
   * by s_epochDataSize (m_id / s_epochIdScale) % s_epochDataSize, and the
   * value is an m_epochIndex in the api call's InFlightGuard that will
   * be used to reference to the entry in the queue's global m_epochReclaimNode
   * and m_epochInFlightCount.
   *
   * The m_epochReclaimNode maintains a list of retired nodes parked under
   * the epoch and waiting to be deleted when the number of in flight api call
   * assigned to the epoch is zero and the epoch is not longer active global
   * epoch.
   *
   * Note that the m_epochData.m_id is a running counter, but multiple
   * consecutive m_id will be derived into a same m_epochIndex that is used to
   * reference the entry in the m_epochReclaimNode and m_epochInFlightCount.
   *
   * Such design allows us to have figure in time (s_epochTimeSccale) and space
   * (s_epochIdScale) when deriving the epochIndex where multiple calls in short
   * proximity in time are same epoch, but uses the space factor to make sure
   * that we do not have a large number of retired node waiting to be free.
   */
  static constexpr uint64_t s_epochTimeScale{500};
  static constexpr uint64_t s_epochIdScale{2};
  static constexpr uint32_t s_epochDataSize{50};

  struct alignas(16) EpochData {
    uint64_t m_in_flight_total{};
    uint64_t m_id{};
  };

  struct Node {
    T m_data{};
    std::atomic<Node *> m_next{nullptr};
    std::atomic<Node *> m_retired_next{nullptr};
  };

  struct InFlightGuard {
    Dmn_BlockingQueue_Lf *m_q{};
    bool m_entered{false};
    uint64_t m_epochIndex{};

    explicit InFlightGuard(Dmn_BlockingQueue_Lf *q) : m_q{q} {
      // fast reject
      if (m_q->m_shutdown_flag.test(std::memory_order_acquire)) {
        return;
      }

      m_q->m_in_flight.fetch_add(1, std::memory_order_acq_rel);
      m_entered = true;

      // close race: if destructor set shutdown concurrently, back out
      if (q->m_shutdown_flag.test(std::memory_order_acquire)) {
        m_q->m_in_flight.fetch_sub(1, std::memory_order_acq_rel);
        m_q->m_in_flight.notify_all();
        m_entered = false;
      } else {
        auto total =
            m_q->m_in_flight_total.fetch_add(1, std::memory_order_acq_rel);

        do {
          auto ep = m_q->m_epochData.load(std::memory_order_acquire);
          if ((total - ep.m_in_flight_total) >= s_epochTimeScale ||
              total < ep.m_in_flight_total) {
            EpochData epNew{.m_in_flight_total = total, .m_id = ep.m_id + 1};

            if (!m_q->m_epochData.compare_exchange_strong(
                    ep, epNew, std::memory_order_release,
                    std::memory_order_acquire)) {
              continue;
            }

            m_epochIndex = (epNew.m_id / s_epochIdScale) % s_epochDataSize;

            size_t index = 0;
            while (index < s_epochDataSize) {
              if (m_epochIndex != index) {
                auto count = m_q->m_epochInFlightCount[index].load(
                    std::memory_order_acquire);

                if (0 == count) {
                  auto ptr = m_q->m_epochReclaimNode[index].load(
                      std::memory_order_acquire);
                  if (nullptr != ptr &&
                      !m_q->m_epochReclaimNode[index].compare_exchange_strong(
                          ptr, nullptr, std::memory_order_release,
                          std::memory_order_acquire)) {
                    continue;
                  }

                  m_q->freeRetiredNodeList(ptr);
                }
              }

              index++;
            }
          } else {
            m_epochIndex = (ep.m_id / s_epochIdScale) % s_epochDataSize;
          }

          // acq_rel: acquire prevents subsequent operations in this critical
          // section from being reordered before we register this "in-flight"
          // entry; release pairs with decrements on the same counter so that
          // all work done under this guard happens-before leaving the epoch.
          m_q->m_epochInFlightCount[m_epochIndex].fetch_add(
              1, std::memory_order_acq_rel);

          break;
        } while (true);
      }
    }

    ~InFlightGuard() {
      if (!m_entered) {
        return;
      }

      // acq_rel: acquire synchronizes with the registration fetch_add so that
      // shared state is visible; release ensures all operations performed under
      // this guard are visible to any thread that observes the count reaching
      // 0.
      if (1 == m_q->m_epochInFlightCount[m_epochIndex].fetch_sub(
                   1, std::memory_order_acq_rel)) {
        auto ep = m_q->m_epochData.load(std::memory_order_acquire);
        auto globalEpochIndex = (ep.m_id / s_epochIdScale) % s_epochDataSize;

        if (globalEpochIndex != m_epochIndex) {
          do {
            auto ptr = m_q->m_epochReclaimNode[m_epochIndex].load(
                std::memory_order_acquire);
            if (nullptr != ptr &&
                !m_q->m_epochReclaimNode[m_epochIndex].compare_exchange_strong(
                    ptr, nullptr, std::memory_order_release,
                    std::memory_order_acquire)) {
              continue;
            }

            m_q->freeRetiredNodeList(ptr);
            break;

          } while (true);
        }
      }

      m_q->m_in_flight.fetch_sub(1, std::memory_order_acq_rel);
      m_q->m_in_flight.notify_all();
    }

    explicit operator bool() const noexcept { return m_entered; }
  }; // InFlightGuard

public:
  Dmn_BlockingQueue_Lf();
  Dmn_BlockingQueue_Lf(std::initializer_list<T> list);
  virtual ~Dmn_BlockingQueue_Lf() noexcept;

  Dmn_BlockingQueue_Lf(const Dmn_BlockingQueue_Lf<T> &obj) = delete;
  const Dmn_BlockingQueue_Lf<T> &
  operator=(const Dmn_BlockingQueue_Lf<T> &obj) = delete;
  Dmn_BlockingQueue_Lf(const Dmn_BlockingQueue_Lf<T> &&obj) = delete;
  Dmn_BlockingQueue_Lf<T> &operator=(Dmn_BlockingQueue_Lf<T> &&obj) = delete;

  /**
   * @brief Remove and return the front item from the queue, blocking if
   * empty, it throws exception if the queue is destroyed while the caller pop
   * calls block waiting for item.
   *
   * @return The front item.
   */
  virtual auto pop() -> T;

  /**
   * @brief Pop multiple items from the queue with optional timeout semantics.
   *
   * @warning The method does not guarantee that returned items are
   * consecutive and next to each other, but ordering of the returning items
   * in case that multiple threads are doing pop at the same time.
   *
   * Detailed semantics:
   * - count > 0 is required (asserted).
   * - If the queue has >= count items, this returns exactly count items.
   * - If the queue is empty or less than count items:
   *   - timeout == 0: wait indefinitely for count items (return exactly
   *     count).
   *   - timeout > 0: wait up to timeout microseconds for items.
   *     * If timeout expires and there is at least one item, return 1..count
   *       items (the current queue size).
   *     * If timeout expires and the queue is still empty, the function
   *       returns no item.
   *
   * The returned vector contains moved items removed from the queue.
   *
   * @param count   Number of desired items (must be > 0).
   * @param timeout Timeout in microseconds for waiting for the full count.
   *                A value of 0 means wait forever for the count items.
   * @return Vector of items (size == count on success without timeout, or
   *         between 1 and count if a timeout occurred after at least one item
   *         was produced).
   */
  virtual auto pop(size_t count, long timeout = 0) -> std::vector<T>;

  /**
   * @brief Attempt a non-blocking pop. Return std::nullopt if empty.
   *
   * @return optional item, or std::nullopt if the queue was empty.
   */
  virtual auto popNoWait() -> std::optional<T>;

  /**
   * @brief Push an rvalue into the queue (attempts move, with
   * move_if_noexcept or equivalent).
   *
   * @param item The value to push (rvalue reference).
   */
  virtual void push(T &&item);

  /**
   * @brief Push an lvalue into the queue, optionally moving it (using
   *        move_if_noexcept or equivalent).
   *
   * @param item The value to push (lvalue reference; may be moved-from if
   *             move is true).
   * @param move If true, attempt to move the value into the queue; otherwise
   *             copy it.
   */
  virtual void push(T &item, bool move = true);

  /**
   * @brief Wait until the queue becomes empty and return the total number of
   *        items that have passed through the queue.
   *
   * @return The total number of items that have been passed through the
   * queue.
   */
  virtual auto waitForEmpty() -> size_t;

protected:
  /**
   * @brief Signal all waiting threads to wake up and return.
   *
   * Sets the m_shutdown flag and gradually exit all inflight threads.
   */
  virtual void stop();

  virtual auto popOptional(bool wait) -> std::optional<T>;

  template <class U> void pushImpl(U &&item);

private:
  static void cleanup_thunk_inflight(void *arg);

  /**
   * @brief The method frees a chain of nodes.
   *
   * @param head The head pointer to a chain of nodes.
   */
  auto freeNodeList(Node *head) -> size_t;

  /**
   * @brief The method frees a chain of retired nodes.
   *
   * @param head The head pointer to a chain of retired nodes.
   */
  auto freeRetiredNodeList(Node *head) -> size_t;

  /**
   * @brief The method returns a node into epoch based reclamation
   *        blocks, and free it later.
   *
   * @param epochIndex Index to the epoch block to retire the node.
   * @param node Pointer to the node to be free.
   */
  void retireNode(uint64_t epochIndex, Node *node);

  // a linked list of nodes that are maintained in FIFO and contains
  // the data pushed into and pop out of queue.
  std::atomic<Node *> m_head{};
  std::atomic<Node *> m_tail{};

  std::atomic<std::size_t> m_total_push_count{};

  std::atomic<EpochData> m_epochData{};
  std::array<std::atomic<uint64_t>, s_epochDataSize> m_epochInFlightCount{};
  std::array<std::atomic<Node *>, s_epochDataSize> m_epochReclaimNode{};

  std::atomic<std::uint64_t> m_in_flight{0};
  std::atomic<std::uint64_t> m_in_flight_total{0};
  std::atomic_flag m_shutdown_flag{};
}; // class Dmn_BlockingQueue_Lf

template <typename T> Dmn_BlockingQueue_Lf<T>::Dmn_BlockingQueue_Lf() {
  EpochData ep{.m_in_flight_total = 0, .m_id = 0};
  m_epochData.store(ep);

  for (size_t i = 0; i < s_epochDataSize; i++) {
    m_epochReclaimNode[i].store(nullptr);
    m_epochInFlightCount[i].store(0);
  }

  auto dummy = new Node;

  m_head.store(dummy);
  m_tail.store(dummy);
}

template <typename T>
Dmn_BlockingQueue_Lf<T>::Dmn_BlockingQueue_Lf(std::initializer_list<T> list)
    : Dmn_BlockingQueue_Lf{} {
  for (auto data : list) {
    this->push(data);
  }
}

template <typename T>
Dmn_BlockingQueue_Lf<T>::~Dmn_BlockingQueue_Lf() noexcept try {
  stop();

  Node *ptr = m_head.load(std::memory_order_acquire);
  freeNodeList(ptr);

  auto ep = m_epochData.load(std::memory_order_acquire);
  auto globalEpochIndex = (ep.m_id / s_epochIdScale) % s_epochDataSize;

  size_t index{};
  for (auto &epRN : m_epochReclaimNode) {
    Node *head = epRN.load(std::memory_order_acquire);

    assert(index == globalEpochIndex || nullptr == head);
    freeRetiredNodeList(head);

    index++;
  }
} catch (...) {
  // Destructors must be noexcept: swallow exceptions.
  return;
}

template <typename T>
void Dmn_BlockingQueue_Lf<T>::cleanup_thunk_inflight(void *arg) {
  auto ptr = static_cast<std::unique_ptr<InFlightGuard> *>(arg);

  (*ptr).reset();
}

template <typename T>
auto dmn::Dmn_BlockingQueue_Lf<T>::freeNodeList(Node *head) -> size_t {
  size_t cnt{};

  while (nullptr != head) {
    // Correct pointer-to-member access syntax
    Node *nextPtr = head->m_next.load(std::memory_order_relaxed);
    delete head;

    head = nextPtr;
    cnt++;
  }

  return cnt;
}

template <typename T>
auto dmn::Dmn_BlockingQueue_Lf<T>::freeRetiredNodeList(Node *head) -> size_t {
  size_t cnt{};

  while (nullptr != head) {
    // Correct pointer-to-member access syntax
    Node *nextPtr = head->m_retired_next.load(std::memory_order_relaxed);
    delete head;

    head = nextPtr;
    cnt++;
  }

  return cnt;
}

template <typename T> auto Dmn_BlockingQueue_Lf<T>::pop() -> T {
  if (m_shutdown_flag.test(std::memory_order_acquire)) {
    throw std::runtime_error("Dmn_BlockingQueue_Lf object is shutting down");
  }

  auto data = popOptional(true);
  if (!data) {
    throw std::runtime_error("pop is interrupted, and return without data");
  }

  return std::move(*data);
}

template <typename T>
auto Dmn_BlockingQueue_Lf<T>::pop(size_t count, long timeout)
    -> std::vector<T> {
  assert(count > 0);

  if (m_shutdown_flag.test(std::memory_order_acquire)) {
    throw std::runtime_error("Dmn_BlockingQueue_Lf object is shutting down");
  }

  std::vector<T> res{};

  auto end = std::chrono::high_resolution_clock::now() +
             std::chrono::microseconds(timeout);

  do {
    auto data = popOptional(false);
    if (data) {
      res.push_back(std::move(*data));
    } else {
      dmn::Dmn_Proc::yield();
    }
  } while (false == m_shutdown_flag.test(std::memory_order_acquire) &&
           res.size() < count &&
           (0 == timeout || std::chrono::high_resolution_clock::now() < end));

  return res;
}

template <typename T>
auto Dmn_BlockingQueue_Lf<T>::popOptional(bool wait) -> std::optional<T> {
  auto g = std::make_unique<InFlightGuard>(this);
  if (!(*g)) {
    // choose behavior: throw, return nullopt, etc.
    return std::nullopt;
  }

  std::optional<T> res{};

  DMN_PROC_CLEANUP_PUSH(&Dmn_BlockingQueue_Lf<T>::cleanup_thunk_inflight, &g);

  while (true) {
    Node *last = m_tail.load(std::memory_order_acquire);
    if (nullptr == last) {
      break;
    }

    Node *first = m_head.load(std::memory_order_acquire);
    Node *next = first->m_next.load(std::memory_order_acquire);

    if (first == m_head.load(std::memory_order_acquire)) {
      if (first == last) {
        if (next == nullptr) {
          if (!wait) {
            break;
          }

          while (last == m_tail.load(std::memory_order_acquire)) {
            dmn::Dmn_Proc::testcancel();
            dmn::Dmn_Proc::yield();
          }

          continue;
        }

        m_tail.compare_exchange_strong(
            last, next, std::memory_order_release,
            std::memory_order_relaxed); // Help move tail
      } else {
        if (m_head.compare_exchange_weak(first, next, std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
          res = std::move(next->m_data);

          retireNode(g->m_epochIndex, first);

          break;
        }
      }
    }
  }

  DMN_PROC_CLEANUP_POP(0);

  return res;
}

template <typename T>
auto Dmn_BlockingQueue_Lf<T>::popNoWait() -> std::optional<T> {
  if (m_shutdown_flag.test(std::memory_order_acquire)) {
    throw std::runtime_error("Dmn_BlockingQueue_Lf object is shutting down");
  }

  return popOptional(false);
}

template <typename T> void Dmn_BlockingQueue_Lf<T>::push(T &&item) {
  if (m_shutdown_flag.test(std::memory_order_acquire)) {
    throw std::runtime_error("Dmn_BlockingQueue_Lf object is shutting down");
  }

  // Preserve the original preference for noexcept-move (otherwise copy).
  pushImpl(std::move_if_noexcept(item));
}

template <typename T> void Dmn_BlockingQueue_Lf<T>::push(T &item, bool move) {
  if (m_shutdown_flag.test(std::memory_order_acquire)) {
    throw std::runtime_error("Dmn_BlockingQueue_Lf object is shutting down");
  }

  if (move) {
    // Preserve the original preference for noexcept-move (otherwise copy).
    pushImpl(std::move_if_noexcept(item));
  } else {
    pushImpl(item); // copy
  }
}

template <typename T>
template <class U>
void Dmn_BlockingQueue_Lf<T>::pushImpl(U &&item) {
  auto g = std::make_unique<InFlightGuard>(this);
  if (!(*g)) {
    return;
  }

  DMN_PROC_CLEANUP_PUSH(&Dmn_BlockingQueue_Lf<T>::cleanup_thunk_inflight, &g);

  Node *newNode = new Node;

  newNode->m_data = std::forward<U>(item);

  Node *last{};
  Node *next{};

  while (true) {
    last = m_tail.load(std::memory_order_acquire);
    if (nullptr == last) {
      delete newNode;
      newNode = nullptr;

      throw std::runtime_error(
          "Dmn_BlockingQueue_Lf: push attempted on shutdown queue");
    }

    next = last->m_next.load(std::memory_order_acquire);

    if (last ==
        m_tail.load(
            std::memory_order_acquire)) { // Are tail and next consistent?
      if (next == nullptr) {
        // Release: ensures newNode->m_data is visible to any consumer that
        // acquires this m_next pointer (publish-subscribe ordering).
        if (last->m_next.compare_exchange_strong(next, newNode,
                                                 std::memory_order_release,
                                                 std::memory_order_relaxed)) {
          break;
        }
      } else {
        // Help advance the tail past a lagging pointer; release so that
        // readers of m_tail see the node's data via the earlier m_next publish.
        m_tail.compare_exchange_strong(last, next, std::memory_order_release,
                                       std::memory_order_relaxed);
      }
    }
  }

  if (newNode) {
    // Release: publishes the newly linked node so consumers see it via m_tail.
    m_tail.compare_exchange_strong(last, newNode, std::memory_order_release,
                                   std::memory_order_relaxed);
    m_tail.notify_all();

    // Relaxed: m_total_push_count is an exact counter (used by waitForEmpty());
    // we only require atomicity for the increment, not additional ordering.
    m_total_push_count.fetch_add(1, std::memory_order_relaxed);
  }

  DMN_PROC_CLEANUP_POP(0);
}

template <typename T> auto Dmn_BlockingQueue_Lf<T>::waitForEmpty() -> size_t {
  while (true) {
    Node *last = m_tail.load(std::memory_order_acquire);
    if (nullptr == last) {
      break;
    }

    Node *first = m_head.load(std::memory_order_acquire);
    Node *next = first->m_next.load(std::memory_order_acquire);

    if (first == m_head.load(std::memory_order_acquire)) {
      if (first == last) {
        if (next == nullptr) {
          break;
        }
      }
    }

    dmn::Dmn_Proc::yield();
  }

  return m_total_push_count.load(std::memory_order_acquire);
}

template <typename T>
void Dmn_BlockingQueue_Lf<T>::retireNode(uint64_t epochIndex, Node *node) {
  assert(epochIndex < s_epochDataSize);

  do {
    auto first = m_epochReclaimNode[epochIndex].load(std::memory_order_acquire);
    // Relaxed: the subsequent CAS (release) will order this store.
    node->m_retired_next.store(first, std::memory_order_relaxed);

    if (!m_epochReclaimNode[epochIndex].compare_exchange_strong(
            first, node, std::memory_order_release,
            std::memory_order_acquire)) {
      continue;
    }

    break;
  } while (true);
}

template <typename T> void Dmn_BlockingQueue_Lf<T>::stop() {
  // 1. set shutdown flag
  m_shutdown_flag.test_and_set(std::memory_order_release);

  // 2. detach m_tail and mark data as empty.
  // Release: establishes synchronization with threads that acquire m_tail in
  // pushImpl/popOptional, ensuring they observe the shutdown state.
  m_tail.store(nullptr, std::memory_order_release);
  m_tail.notify_all();

  // 3) wait for all threads that already "entered the epoch" to leave
  std::uint64_t n = m_in_flight.load(std::memory_order_acquire);
  while (n != 0) {
    m_in_flight.wait(n, std::memory_order_acquire);
    n = m_in_flight.load(std::memory_order_acquire);
  }
}

} // namespace dmn

#endif // DMN_BLOCKINGQUEUE_LF_HPP_
