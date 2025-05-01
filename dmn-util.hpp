/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 */

#ifndef DMN_UTIL_HPP_

#define DMN_UTIL_HPP_

#include <algorithm>

namespace dmn {

/**
 * @brief The method increases the value by 1 and wrap it around to 1 if
 *        the increased number wraps around to zero or nagative value.
 *
 * @param value The value to be plus 1 and returned
 *
 * @return The value plus 1 or minimum value 1
 */
template <typename T> inline T incrementByOne(T value) {
  return std::max<T>(1, value + 1);
}

} // namespace dmn

#endif // DMN_UTIL_HPP_
