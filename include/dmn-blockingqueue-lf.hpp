/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-blockingqueue-lf.hpp
 * @brief Lock-free multi-producer/multi-consumer FIFO queue with optional
 * blocking pop. This modules implements the Michael-Scott queue algorithm.
 *
 * @details
 * This header provides dmn::Dmn_BlockingQueue_Lf, a thread-safe FIFO queue
 * that:
 * - supports multiple concurrent producers and consumers (MPMC),
 * - uses lock-free algorithms for push/pop (no mutexes in the fast path),
 * - provides blocking behavior for @ref pop() by spinning/yielding until an
 *   item becomes available (or until shutdown),
 * - uses an epoch-based reclamation mechanism (via @ref
 *   dmn::Dmn_Inflight_Guard) to safely reclaim removed nodes without
 *   use-after-free.
 *
 * ### Blocking model
 * The queue does not use condition variables. "Blocking" means active waiting
 * with yielding (and cancellation checks where applicable). This is suitable
 * for scenarios where latency is prioritized and threads are expected to wake
 * soon, but it may be inefficient for long idle waits.
 *
 * ### Shutdown behavior
 * Destruction triggers shutdown. After shutdown begins:
 * - push operations fail (throw),
 * - blocking pops are interrupted and will throw,
 * - non-blocking pops return empty.
 *
 * @note Memory reclamation is integrated with the inflight-guard interface;
 *       callers do not need to manage hazard pointers/epochs directly.
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
#include <utility>
#include <vector>

#include "dmn-proc.hpp"
#include "dmn-util.hpp"

#include "dmn-blockingqueue-interface.hpp"
#include "dmn-inflight-guard.hpp"

namespace dmn {

using Inflight_Guard_Ticket =
    std::unique_ptr<Dmn_Inflight_Guard<uint64_t>::Ticket>;

/**
 * @brief Thread-safe FIFO queue.
 *
 * Template parameter T is the stored item type.
 */
template <typename T = std::string>
class Dmn_BlockingQueue_Lf : public Dmn_BlockingQueue_Interface<T>,
                             private Dmn_Inflight_Guard<uint64_t> {
  /**
   * @name Epoch-based reclamation (implementation notes)
   *
   * @rationale
   * Popped nodes cannot be immediately deleted because other threads may still
   * be reading pointers obtained during concurrent operations. This
   * implementation uses an epoch-based scheme coordinated by @ref
   * dmn::Dmn_Inflight_Guard:
   *
   * - Each push/pop enters an in-flight region and is assigned an epoch index.
   * - Removed nodes are placed onto a per-epoch retired list (not deleted yet).
   * - When an epoch bucket is no longer active and its in-flight count drops to
   *   zero, its retired nodes can be safely freed.
   *
   * The mapping from a monotonically increasing epoch id to a bounded number of
   * epoch buckets is controlled by:
   * - @ref s_epochTimeScale: how frequently the global epoch advances as a
   *   function of total in-flight API calls.
   * - @ref s_epochIdScale: how many consecutive epoch ids map to the same
   * bucket (coarser grouping reduces churn).
   * - @ref s_epochDataSize: number of buckets (upper bound on parked retired
   * lists).
   *
   * This bounded-bucket design prevents unbounded growth of deferred nodes
   * while still allowing safe reclamation under concurrency.
   *
   * @detail
   * The following parameters control Epoch based reclamation logic
   * for the pop out node with Dmn_Inflight_Guard. Each pop or push call is
   * guarded by Dmn_Inflight_Guard.
   *
   * The queue maintains global epoch data in m_epochData which contains
   * the epoch id and the last timepoint where epoch id is updated. Instead
   * of using system time (which will be a hot path) as last time point,
   * we use the number of pop or push call as timepoint reference, if the
   * number of such api call is s_epochTimeScale difference from the last
   * value in m_epochData's m_in_flight_total, both the m_in_flight_total
   * and m_id is moved forward (aka the epoch is moved).
   *
   * Each Dmn_Inflight_Guard entered will derive the value (which is served
   * as epochIndex) based on current m_epochData.m_id, and each m_id is grouped
   * by s_epochIdScale, for example if s_epochIdScale is 2, then m_id 0, 1 is
   * one group, 2, 3 is another group, then the (m_id / s_epochIdScale) is
   * modular divided by s_epochDataSize, i.e. (m_id / s_epochIdScale) %
   * s_epochDataSize, and the value is an epochIndex in the api call's
   * Dmn_Inflight_Guard that will be used to reference to the entry in the
   * queue's global m_epochReclaimNode and m_epochInFlightCount.
   *
   * The m_epochReclaimNode maintains a list of retired nodes parked under
   * the epoch and waiting to be deleted when the number of in-flight API calls
   * assigned to the epoch is zero and the epoch is no longer the active global
   * epoch.
   *
   * Note that the m_epochData.m_id is a running counter, but multiple
   * consecutive m_id will be derived into a same epochIndex that is used to
   * reference the entry in the m_epochReclaimNode and m_epochInFlightCount.
   *
   * Such design allows us to have configuration in time (s_epochTimeScale) and
   * space (s_epochIdScale) when deriving the epochIndex where multiple calls in
   * short proximity in time are same epoch, but uses the space factor to make
   * sure that we do not have a large number of retired nodes waiting to be
   * freed.
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

public:
  using Dmn_BlockingQueue_Interface<T>::pop;
  using Dmn_BlockingQueue_Interface<T>::push;

  Dmn_BlockingQueue_Lf();
  Dmn_BlockingQueue_Lf(std::initializer_list<T> list);

  virtual ~Dmn_BlockingQueue_Lf() noexcept;

  Dmn_BlockingQueue_Lf(const Dmn_BlockingQueue_Lf<T> &obj) = delete;
  Dmn_BlockingQueue_Lf<T> &
  operator=(const Dmn_BlockingQueue_Lf<T> &obj) = delete;
  Dmn_BlockingQueue_Lf(Dmn_BlockingQueue_Lf<T> &&obj) = delete;
  Dmn_BlockingQueue_Lf<T> &operator=(Dmn_BlockingQueue_Lf<T> &&obj) = delete;

  /**
   * @brief Pop up to @p count items, optionally bounded by a timeout.
   *
   * @details
   * Semantics:
   * - @p count must be > 0 (asserted).
   * - If @p timeout == 0, this call keeps attempting to dequeue until exactly
   *   @p count items have been returned, or until shutdown begins.
   * - If @p timeout > 0, this call keeps attempting to dequeue until either:
   *   - @p count items have been collected, or
   *   - the timeout duration (in microseconds) elapses, or
   *   - shutdown begins.
   *
   * If the timeout elapses (or shutdown begins) and at least one item was
   * dequeued, the returned vector contains the items obtained so far.
   * If no item was dequeued before timeout/shutdown, the returned vector is
   * empty.
   *
   * @warning The returned items are not guaranteed to be consecutive relative
   * to other concurrent consumers. This function only returns items
   *          successfully dequeued by the calling thread.
   *
   * @param count   Maximum number of items to return (must be > 0).
   * @param timeout Timeout in microseconds. 0 means wait indefinitely.
   * @return Vector of dequeued items (size in [0, count]).
   */
  virtual auto pop(size_t count, long timeout = 0) -> std::vector<T> override;

  /**
   * @brief Busy-wait until the queue becomes empty.
   *
   * @details
   * This method repeatedly checks the queue state until it observes an empty
   * condition. It is intended to be used by a single owning thread to wait for
   * draining, typically as part of a shutdown protocol external to this queue.
   *
   * @warning This method is not protected by the inflight/epoch guard and
   * should not be used concurrently with arbitrary producers/consumers unless
   *          the caller can ensure safe coordination.
   *
   * @return Total number of successful pushes observed by this queue since
   *         construction.
   */
  virtual auto waitForEmpty() -> uint64_t override;

protected:
  /**
   * @brief Wrapper to internal popOptional() that requires inflight guard
   * object.
   */
  virtual auto popOptional(bool wait) -> std::optional<T> override;

  /**
   * @brief Pop the head data, and wait for it if empty and wait is true.
   *
   * @param wait True will block the caller if the queue is empty.
   * @param inflightTicket The inflight ticket that keeps track of epochIndex
   *                       for epoch to store deleted node.
   * @return An optional containing the data from the head of the queue, or
   *         std::nullopt if the queue is empty and wait is false.
   */
  virtual auto popOptional(bool wait,
                           const Inflight_Guard_Ticket &inflightTicket)
      -> std::optional<T>;

  /**
   * @brief Wrapper call to pushImpl to copy and enqueue the item into the
   *        queue.
   *
   * @param item The item to be enqueued.
   * @throws std::runtime_error if the queue is shutting down when the push
   *         operation is attempted.
   */
  void pushCopy(const T &item) override;

  /**
   * @brief Wrapper call to pushImpl to move and enqueue the item into the
   *        queue.
   *
   * @param item The item to be enqueued.
   * @throws std::runtime_error if the queue is shutting down when the push
   *         operation is attempted.
   */
  void pushMove(T &&item) override;

  /**
   * @brief Push the item into the tail of the queue (move or copy semantics).
   *
   * @param item The item to be pushed into tail of the queue
   */
  template <class U> void pushImpl(U &&item);

  /**
   * @brief Signal all waiting threads to wake up and return.
   *
   * Sets the m_shutdown flag and gradually exit all inflight threads.
   */
  virtual void stop() override;

  /**
   * Delegation methods integrate the queue into Dmn_InflightGuard interface.
   */
  virtual auto enterInflightGuardFnc() -> uint64_t override;
  virtual auto isInflightGuardClosed() -> bool override;
  virtual void leaveInflightGuardFnc(const uint64_t &) noexcept override;

private:
  static void cleanup_thunk_inflight(void *arg);

  /**
   * @brief Derive the epoch index (base of epoch id) to reference into queue's
   *        epoch blocks.
   *
   * @param epochId The epoch id
   * @return The derived epoch index into queue's epoch blocks.
   */
  static uint64_t calc_epoch_index(uint64_t epochId);

  /**
   * @brief Free the chain of nodes based on templatized next field, and
   *        return total number of nodes freed.
   *
   * @param head Pointer to chain of nodes to be freed.
   * @return Total number of nodes freed.
   */
  template <std::atomic<Node *> Node::*NextField>
  static auto freeNodeChain(Node *head) -> uint64_t {
    uint64_t cnt = 0;

    while (head != nullptr) {
      Node *next = (head->*NextField).load(std::memory_order_relaxed);

      delete head;

      head = next;
      ++cnt;
    }

    return cnt;
  }

  /**
   * @brief The method frees a chain of nodes.
   *
   * @param head The head pointer to a chain of nodes.
   */
  auto freeNodeList(Node *head) -> uint64_t;

  /**
   * @brief The method frees a chain of retired nodes.
   *
   * @param head The head pointer to a chain of retired nodes.
   */
  auto freeRetiredNodeList(Node *head) -> uint64_t;

  /**
   * @brief The method returns a node into epoch based reclamation
   *        blocks, and free it later.
   *
   * @param epochIndex Index to the epoch block to retire the node.
   * @param node Pointer to the node to be free.
   */
  void retireNode(uint64_t epochIndex, Node *node);

  // a linked list of nodes that are maintained in FIFO and contains
  // the data pushed into and popped out of the queue.
  std::atomic<Node *> m_head{};
  std::atomic<Node *> m_tail{};

  std::atomic<std::uint64_t> m_total_push_count{};

  std::atomic<EpochData> m_epochData{};
  std::array<std::atomic<uint64_t>, s_epochDataSize> m_epochInFlightCount{};
  std::array<std::atomic<Node *>, s_epochDataSize> m_epochReclaimNode{};

  std::atomic<std::uint64_t> m_in_flight_total{0};
  std::atomic_flag m_shutdown_flag{};
}; // class Dmn_BlockingQueue_Lf

template <typename T> Dmn_BlockingQueue_Lf<T>::Dmn_BlockingQueue_Lf() {
  EpochData ep{.m_in_flight_total = 0, .m_id = 0};
  m_epochData.store(ep);

  for (uint64_t i = 0; i < s_epochDataSize; i++) {
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
  for (auto &data : list) {
    this->pushImpl(data); // use pushImpl and skip inflight check
  }
}

template <typename T>
Dmn_BlockingQueue_Lf<T>::~Dmn_BlockingQueue_Lf() noexcept try {
  stop();

  Node *ptr = m_head.load(std::memory_order_acquire);
  freeNodeList(ptr);

  auto ep = m_epochData.load(std::memory_order_acquire);
  auto globalEpochIndex = calc_epoch_index(ep.m_id);

  uint64_t index{};
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
auto Dmn_BlockingQueue_Lf<T>::calc_epoch_index(uint64_t epochId) -> uint64_t {
  return (epochId / s_epochIdScale) % s_epochDataSize;
}

template <typename T>
void Dmn_BlockingQueue_Lf<T>::cleanup_thunk_inflight(void *arg) {
  auto *ticket_sp = static_cast<Inflight_Guard_Ticket *>(arg);

  (*ticket_sp).reset();
}

template <typename T>
auto dmn::Dmn_BlockingQueue_Lf<T>::freeNodeList(Node *head) -> uint64_t {
  return freeNodeChain<&Node::m_next>(head);
}

template <typename T>
auto dmn::Dmn_BlockingQueue_Lf<T>::freeRetiredNodeList(Node *head) -> uint64_t {
  return freeNodeChain<&Node::m_retired_next>(head);
}

template <typename T>
auto Dmn_BlockingQueue_Lf<T>::pop(size_t count, long timeout)
    -> std::vector<T> {
  assert(count > 0);

  auto inflightTicket = this->enterInflightGate();

  std::vector<T> res{};

  auto end = std::chrono::high_resolution_clock::now() +
             std::chrono::microseconds(timeout);

  DMN_PROC_CLEANUP_PUSH(&Dmn_BlockingQueue_Lf<T>::cleanup_thunk_inflight,
                        &inflightTicket);

  do {
    auto data = popOptional(false, inflightTicket);
    if (data) {
      res.push_back(std::move(*data));
    } else {
      dmn::Dmn_Proc::yield();
    }
  } while (false == m_shutdown_flag.test(std::memory_order_acquire) &&
           res.size() < count &&
           (0 == timeout || std::chrono::high_resolution_clock::now() < end));

  DMN_PROC_CLEANUP_POP(0);

  return res;
}

template <typename T>
auto Dmn_BlockingQueue_Lf<T>::popOptional(bool wait) -> std::optional<T> {
  std::optional<T> data{};

  auto inflightTicket = this->enterInflightGate();

  DMN_PROC_CLEANUP_PUSH(&Dmn_BlockingQueue_Lf<T>::cleanup_thunk_inflight,
                        &inflightTicket);

  data = popOptional(wait, inflightTicket);

  DMN_PROC_CLEANUP_POP(0);

  return data;
}

template <typename T>
auto Dmn_BlockingQueue_Lf<T>::popOptional(
    bool wait, const Inflight_Guard_Ticket &inflightTicket)
    -> std::optional<T> {
  std::optional<T> res{};

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

          retireNode(inflightTicket->getValue(), first);

          break;
        }
      }
    }
  }

  return res;
}

template <typename T> void Dmn_BlockingQueue_Lf<T>::pushCopy(const T &item) {
  auto inflightTicket = this->enterInflightGate();

  DMN_PROC_CLEANUP_PUSH(&Dmn_BlockingQueue_Lf<T>::cleanup_thunk_inflight,
                        &inflightTicket);

  pushImpl(item); // copy

  DMN_PROC_CLEANUP_POP(0);
}

template <typename T> void Dmn_BlockingQueue_Lf<T>::pushMove(T &&item) {
  auto inflightTicket = this->enterInflightGate();

  DMN_PROC_CLEANUP_PUSH(&Dmn_BlockingQueue_Lf<T>::cleanup_thunk_inflight,
                        &inflightTicket);

  pushImpl(std::move(item)); // move if noexcept, else copy

  DMN_PROC_CLEANUP_POP(0);
}

template <typename T>
template <class U>
void Dmn_BlockingQueue_Lf<T>::pushImpl(U &&item) {
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
}

template <typename T> auto Dmn_BlockingQueue_Lf<T>::waitForEmpty() -> uint64_t {
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
          break; // empty!
        }
      }
    }

    dmn::Dmn_Proc::testcancel();
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
  //    Release: establishes synchronization with threads that acquire m_tail in
  //    pushImpl/popOptional, ensuring they observe the shutdown state.
  m_tail.store(nullptr, std::memory_order_release);
  m_tail.notify_all();

  // 3. wait for all threads that already "entered the epoch" to leave
  this->waitForEmptyInflight();
}

template <typename T>
auto Dmn_BlockingQueue_Lf<T>::enterInflightGuardFnc() -> uint64_t {
  uint64_t epochIndex = 0;
  auto total = m_in_flight_total.fetch_add(1, std::memory_order_acq_rel);

  do {
    auto ep = m_epochData.load(std::memory_order_acquire);
    if ((total - ep.m_in_flight_total) >= s_epochTimeScale ||
        total < ep.m_in_flight_total) {
      EpochData epNew{.m_in_flight_total = total, .m_id = ep.m_id + 1};

      if (!m_epochData.compare_exchange_strong(ep, epNew,
                                               std::memory_order_release,
                                               std::memory_order_acquire)) {
        continue;
      }

      epochIndex = calc_epoch_index(epNew.m_id);

      uint64_t index = 0;
      while (index < s_epochDataSize) {
        if (epochIndex != index) {
          auto count =
              m_epochInFlightCount[index].load(std::memory_order_acquire);

          if (0 == count) {
            auto ptr =
                m_epochReclaimNode[index].load(std::memory_order_acquire);
            if (nullptr != ptr &&
                !m_epochReclaimNode[index].compare_exchange_strong(
                    ptr, nullptr, std::memory_order_release,
                    std::memory_order_acquire)) {
              continue;
            }

            freeRetiredNodeList(ptr);
          }
        }

        index++;
      }
    } else {
      epochIndex = calc_epoch_index(ep.m_id);
    }

    // acq_rel: acquire prevents subsequent operations in this critical
    // section from being reordered before we register this "in-flight"
    // entry; release pairs with decrements on the same counter so that
    // all work done under this guard happens-before leaving the epoch.
    m_epochInFlightCount[epochIndex].fetch_add(1, std::memory_order_acq_rel);

    break;
  } while (true);

  return epochIndex;
}

template <typename T>
auto Dmn_BlockingQueue_Lf<T>::isInflightGuardClosed() -> bool {
  return m_shutdown_flag.test(std::memory_order_acquire);
}

template <typename T>
void Dmn_BlockingQueue_Lf<T>::leaveInflightGuardFnc(
    const uint64_t &epochIndex) noexcept {
  // acq_rel: acquire synchronizes with the registration fetch_add so that
  // shared state is visible; release ensures all operations performed under
  // this guard are visible to any thread that observes the count reaching
  // 0.
  if (1 == m_epochInFlightCount[epochIndex].fetch_sub(
               1, std::memory_order_acq_rel)) {
    auto ep = m_epochData.load(std::memory_order_acquire);
    auto globalEpochIndex = calc_epoch_index(ep.m_id);

    if (globalEpochIndex != epochIndex) {
      do {
        auto ptr =
            m_epochReclaimNode[epochIndex].load(std::memory_order_acquire);
        if (nullptr != ptr &&
            !m_epochReclaimNode[epochIndex].compare_exchange_strong(
                ptr, nullptr, std::memory_order_release,
                std::memory_order_acquire)) {
          continue;
        }

        freeRetiredNodeList(ptr);

        break;
      } while (true);
    }
  }
}
} // namespace dmn

#endif // DMN_BLOCKINGQUEUE_LF_HPP_
