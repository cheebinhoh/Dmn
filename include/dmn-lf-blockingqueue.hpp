/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-lf-blockingqueue.hpp
 * @brief Thread-safe lock free FIFO blocking queue for passing items between
 *        threads.
 *
 * <<<< NOTE: work in progress >>>>
 */

#ifndef DMN_LF_BUFFER_HPP_
#define DMN_LF_BUFFER_HPP_

#include <iostream>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace dmn {

/**
 * @brief Thread-safe FIFO buffer.
 *
 * Template parameter T is the stored item type.
 */
template <typename T = std::string> class Dmn_Lf_BlockingQueue {
  struct Node {
    T data{};
    std::shared_ptr<Node> next{};
  };

public:
  Dmn_Lf_BlockingQueue();
  Dmn_Lf_BlockingQueue(std::initializer_list<T> list);
  virtual ~Dmn_Lf_BlockingQueue() noexcept;

  Dmn_Lf_BlockingQueue(const Dmn_Lf_BlockingQueue<T> &obj) = delete;
  const Dmn_Lf_BlockingQueue<T> &
  operator=(const Dmn_Lf_BlockingQueue<T> &obj) = delete;
  Dmn_Lf_BlockingQueue(const Dmn_Lf_BlockingQueue<T> &&obj) = delete;
  Dmn_Lf_BlockingQueue<T> &operator=(Dmn_Lf_BlockingQueue<T> &&obj) = delete;

  virtual void push(T &item, bool move = false);

  auto debugPrint() -> size_t;

protected:
  template <class U> void pushImpl(U &&item);

private:
  std::shared_ptr<Node> m_dummy{};

  std::atomic<Node *> m_head{};
  std::atomic<Node *> m_tail{};
}; // class Dmn_Lf_BlockingQueue

template <typename T> Dmn_Lf_BlockingQueue<T>::Dmn_Lf_BlockingQueue() {
  m_dummy = std::make_shared<Node>();
  m_head.store(m_dummy.get());
  m_tail.store(m_dummy.get());
}

template <typename T>
Dmn_Lf_BlockingQueue<T>::Dmn_Lf_BlockingQueue(std::initializer_list<T> list)
    : Dmn_Lf_BlockingQueue{} {}

template <typename T>
Dmn_Lf_BlockingQueue<T>::~Dmn_Lf_BlockingQueue() noexcept try {
} catch (...) {
  // Destructors must be noexcept: swallow exceptions.
  return;
}

template <typename T> void Dmn_Lf_BlockingQueue<T>::push(T &item, bool move) {
  if (move) {
    // Preserve the original preference for noexcept-move (otherwise copy).
    pushImpl(std::move_if_noexcept(item));
  } else {
    pushImpl(item); // copy
  }
}

template <typename T> auto Dmn_Lf_BlockingQueue<T>::debugPrint() -> size_t {
  size_t count{};

  Node *pointNull{};
  Node *last{};

  do {
    last = m_tail.load();
  } while ((nullptr == last) || !m_tail.compare_exchange_weak(last, pointNull));

  Node *first{};

  do {
    first = m_head.load();
  } while ((nullptr == first) ||
           !m_head.compare_exchange_weak(first, pointNull));

  Node *cur = first;
  while (nullptr != cur) {
    if (cur != m_dummy.get()) {
      count++;
    }

    cur = cur->next.get();
  }

  m_head.compare_exchange_strong(pointNull, first);
  m_tail.compare_exchange_strong(pointNull, last);

  return count;
}

template <typename T>
template <class U>
void Dmn_Lf_BlockingQueue<T>::pushImpl(U &&item) {
  Node *pointNull{};
  Node *last{};

  do {
    last = m_tail.load();
  } while ((nullptr == last) || !m_tail.compare_exchange_weak(last, pointNull));

  assert(nullptr != last);
  assert(nullptr == m_tail.load());

  auto *newNode = new Node;
  newNode->data = std::move(item);
  newNode->next = nullptr;

  last->next = std::shared_ptr<Node>(newNode);

  m_tail.compare_exchange_strong(pointNull, newNode);
}

} // namespace dmn

#endif // DMN_LF_BUFFER_HPP_
