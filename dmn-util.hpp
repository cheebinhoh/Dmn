/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 */

#ifndef DMN_UTIL_HPP_

#define DMN_UTIL_HPP_

#include <algorithm>

namespace dmn {

template <typename T> inline T incrementByOne(T value) {
  return std::max<T>(1, value + 1);
}

} // namespace dmn

#endif // DMN_UTIL_HPP_
