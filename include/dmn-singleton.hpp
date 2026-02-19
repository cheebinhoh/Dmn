/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-singleton.hpp
 * @brief Lightweight helper for implementing argumented singletons.
 *
 * This header provides a small utility class, dmn::Dmn_Singleton, which
 * exposes a single static helper method:
 *
 *   template <typename T, class... U>
 *   static std::shared_ptr<T> createInstance(U &&...arg);
 *
 * Usage example:
 * ```
 * struct MySingleton : public dmn::Dmn_Singleton<MySingleton> {
 *
 *  private:
 *   MySingleton(int x, std::string s) { ... }
 * };
 *
 * // Create / retrieve the singleton:
 * auto p = dmn::MySingleton::createInstance(42, "hello");
 * ```
 *
 * Notes:
 * - The returned type is std::shared_ptr<T>; ownership and lifetime semantics
 *   are therefore shared.
 * - The subclass of Singleton can implement static method that is called before
 *   the singleton instance is created to run some setup, the signature of the
 *   method is "static void T::runPriorToCreateInstance()" and must be public.
 */

#ifndef DMN_SINGLETON_HPP_
#define DMN_SINGLETON_HPP_

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

namespace dmn {

template <typename T>
concept HasStaticRunPriorToCreateInstance = requires {
  { T::runPriorToCreateInstance() } -> std::same_as<void>;
};

template <typename T> class Dmn_Singleton {
public:
  /**
   * @brief Forwarding helper to create or retrieve a singleton instance for T.
   *
   * This method forwards the provided arguments to T type' constructor
   * and returns the resulting std::shared_ptr<T>.
   *
   * Template parameters:
   *  - T : concrete singleton type that is created by createInstance().
   *  - U... : parameter pack of argument types to be forwarded.
   *
   * Requirements on T:
   *  - Optionally provide a static method that is run before T instance is
   * created static void runPriorToCreateInstance();
   *  - That method must guarantee singleton semantics (return the same
   *    instance on subsequent calls) and perform any required synchronization
   *    to be thread-safe.
   *
   * @param arg Arguments forwarded to T type constructor
   * @return std::shared_ptr<T> pointing to the singleton instance managed by T.
   */
  template <class... U> static std::shared_ptr<T> createInstance(U &&...arg);

private:
  static std::atomic<bool> s_allocated;
  static std::shared_ptr<T> s_instance;
  static std::once_flag s_init_once;
};

template <typename T> std::atomic<bool> Dmn_Singleton<T>::s_allocated{false};

template <typename T> std::once_flag Dmn_Singleton<T>::s_init_once{};

template <typename T> std::shared_ptr<T> Dmn_Singleton<T>::s_instance{};

template <typename T>
template <class... U>
std::shared_ptr<T> Dmn_Singleton<T>::createInstance(U &&...arg) {
  // NOTE that the template type class is supposed to handle
  // thread-safety (example through std::once_flag) to make
  // sure that multiple threads calling createInstance is
  // thread safe and always the singleton instance is returned.

  if (!s_allocated.load()) {
    std::call_once(
        s_init_once,
        [](U &&...arg) {
          if constexpr (HasStaticRunPriorToCreateInstance<T>) {
            T::runPriorToCreateInstance();
          }

          s_instance = std::make_shared<T>(std::forward<U>(arg)...);
        },
        std::forward<U>(arg)...);
  }

  return s_instance;
}

} // namespace dmn

#endif // DMN_SINGLETON_HPP_
