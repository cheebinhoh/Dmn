/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * This class implements a singleton class that subclass can inherit
 * from and used to create a singleton instance with arguments specific
 * to the subclass constructor.
 */

#ifndef DMN_SINGLETON_HPP_

#define DMN_SINGLETON_HPP_

#include <memory>
#include <utility>

namespace dmn {

class Dmn_Singleton {
public:
  /**
   * @brief The static method called to create singleton instance of the
   *        template type, and the concrete type specific singleton instance
   *        creation is done by template type (T) which implements static
   *        createInstanceInternal() that creates and always return same
   *        instance.
   *
   * @param arg The varying forwarded arguments that depends on the template
   *            type' createInstanceInternal() method arguments
   */
  template <typename T, class... U>
  static std::shared_ptr<T> createInstance(U &&...arg);
};

template <typename T, class... U>
std::shared_ptr<T> Dmn_Singleton::createInstance(U &&...arg) {
  std::shared_ptr<T> new_instance =
      T::createInstanceInternal(std::forward<U>(arg)...);

  return new_instance;
}

} // namespace dmn

#endif // DMN_SINGLETON_HPP_
