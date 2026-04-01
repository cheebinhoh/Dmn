/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-singleton.hpp
 * @brief Lightweight helper for implementing argumented singletons.
 *
 * Design pattern
 * Singleton - provides a global singleton instance of the class.
 * Factory method - provides an interface for creating concrete singleton
 *                  subclasses.
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

/**
 * @brief Concept that checks whether type @p T exposes a static
 *        @c runPriorToCreateInstance() method with a @c void return type.
 *
 * Satisfied when @p T declares:
 * @code
 *   static void runPriorToCreateInstance();
 * @endcode
 *
 * @tparam T The type to check.
 */
template <typename T>
concept HasStaticRunPriorToCreateInstance = requires {
  { T::runPriorToCreateInstance() } -> std::same_as<void>;
};

template <typename T> class Dmn_Singleton {
public:
  /**
   * @brief Create or retrieve the process-wide singleton instance of @p T.
   *
   * Forwards the provided arguments to the constructor of @p T and stores
   * the resulting object in a static @c std::shared_ptr<T>.  Subsequent
   * calls with any arguments return the same shared pointer without
   * constructing a new object.
   *
   * If @p T satisfies @ref HasStaticRunPriorToCreateInstance,
   * @c T::runPriorToCreateInstance() is invoked exactly once, before the
   * instance is constructed.
   *
   * Thread-safety is provided by @c std::call_once via a static
   * @c std::once_flag; it is therefore safe to call @c createInstance()
   * concurrently from multiple threads.
   *
   * @tparam T   Concrete singleton type whose instance is managed here.
   * @tparam U   Parameter pack of argument types forwarded to @p T's
   *             constructor.
   * @param  arg Arguments forwarded to @p T's constructor (only used on the
   *             first call).
   * @return @c std::shared_ptr<T> pointing to the singleton instance.
   */
  template <class... U> static std::shared_ptr<T> createInstance(U &&...arg);

private:
  static std::atomic<bool>
      s_allocated; ///< Flag set to @c true once the instance is allocated.
  static std::shared_ptr<T>
      s_instance; ///< Owning pointer to the singleton instance.
  static std::once_flag
      s_init_once; ///< Guards one-time initialization via std::call_once.
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
