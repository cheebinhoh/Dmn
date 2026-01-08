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
 * Dmn_Singleton itself does not implement singleton storage or thread-safety.
 * Instead it forwards the provided arguments to the concrete type T's
 * responsibility: T must implement a static method with the signature
 *
 *   static std::shared_ptr<T> createInstanceInternal(Args...);
 *
 * The concrete type's createInstanceInternal() is where the actual singleton
 * instance should be created/returned and where any necessary synchronization
 * (for example using std::call_once or a function-local static) must be done
 * to ensure exactly-one construction in multithreaded contexts.
 *
 * Rationale:
 * - Different singleton types may require different construction arguments.
 * - This helper centralizes the forwarding call while leaving lifetime and
 *   synchronization policies to the concrete type (T).
 *
 * Usage example:
 * ```
 * struct MySingleton : public dmn::Dmn_Singleton {
 *   static std::shared_ptr<MySingleton> createInstanceInternal(int x,
 * std::string s) { static std::shared_ptr<MySingleton> instance; static
 * std::once_flag flag; std::call_once(flag, [&]{ instance =
 * std::make_shared<MySingleton>(x, std::move(s));
 *     });
 *     return instance;
 *   }
 *
 *  private:
 *   MySingleton(int x, std::string s) { ... }
 * };
 *
 * // Create / retrieve the singleton:
 * auto p = dmn::Dmn_Singleton::createInstance<MySingleton>(42, "hello");
 * ```
 *
 * Notes:
 * - The returned type is std::shared_ptr<T>; ownership and lifetime semantics
 *   are therefore shared. If a different ownership model is desired, have T's
 *   createInstanceInternal return the appropriate smart pointer and adjust this
 *   helper accordingly.
 * - Dmn_Singleton contains only static helpers and is not intended to be
 *   instantiated.
 */

#ifndef DMN_SINGLETON_HPP_
#define DMN_SINGLETON_HPP_

#include <memory>
#include <utility>

namespace dmn {

class Dmn_Singleton {
public:
  /**
   * @brief Forwarding helper to create or retrieve a singleton instance for T.
   *
   * This method forwards the provided arguments to T::createInstanceInternal()
   * and returns the resulting std::shared_ptr<T>.
   *
   * Template parameters:
   *  - T : concrete singleton type that provides createInstanceInternal().
   *  - U... : parameter pack of argument types to be forwarded.
   *
   * Requirements on T:
   *  - Must provide a static method:
   *      static std::shared_ptr<T> createInstanceInternal(U...);
   *  - That method must guarantee singleton semantics (return the same
   *    instance on subsequent calls) and perform any required synchronization
   *    to be thread-safe.
   *
   * @param arg Arguments forwarded to T::createInstanceInternal().
   * @return std::shared_ptr<T> pointing to the singleton instance managed by T.
   */
  template <typename T, class... U>
  static std::shared_ptr<T> createInstance(U &&...arg);
};

template <typename T, class... U>
std::shared_ptr<T> Dmn_Singleton::createInstance(U &&...arg) {
  // NOTE that the template type class is supposed to handle
  // thread-safety (example through std::once_flag) to make
  // sure that multiple threads calling createInstance is
  // thread safe and always the singleton instance is returned.

  std::shared_ptr<T> new_instance =
      T::createInstanceInternal(std::forward<U>(arg)...);

  return new_instance;
}

} // namespace dmn

#endif // DMN_SINGLETON_HPP_
