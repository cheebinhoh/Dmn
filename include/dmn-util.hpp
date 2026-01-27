/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file include/dmn-util.hpp
 * @brief Small, header-only utility helpers used across the Dmn project.
 *
 * This header provides a couple of lightweight, commonly-used helpers:
 *  - incrementByOne<T>(T) : increment an integral-like value by one while
 *    ensuring the returned value is never less than 1.
 *  - stringCompare(...)   : compare two strings with optional case-insensitive
 *    mode.
 *
 * Implementation notes and guarantees:
 *  - These utilities are intentionally minimal and header-only for easy reuse.
 *  - incrementByOne expects a type T that behaves like an integer (supports +,
 *    and comparison with integer literal 1). For signed integer types, adding 1
 *    to the maximum representable value is undefined behavior (signed
 *    overflow). For unsigned integer types overflow is well-defined (modular
 *    arithmetic) and the function will return 1 when wrapping occurs.
 *  - stringCompare makes copies of the provided string_views and applies a
 *    simple ASCII-based lowercasing via ::tolower when case-insensitive
 *    comparison is requested. This approach is locale-independent and does not
 *    perform full Unicode case folding — for locale-aware or Unicode-aware
 *    comparisons prefer using std::collate/std::locale or a dedicated Unicode
 *    library.
 *
 * Complexity:
 *  - incrementByOne: O(1)
 *  - stringCompare: O(N) where N is the length of the longer input string
 *
 * Examples:
 *  - incrementByOne<int>(3) -> 4
 *  - incrementByOne<unsigned>(std::numeric_limits<unsigned>::max()) -> 1 (wraps
 *    modulo)
 *  - stringCompare("Hello", "hello", true) -> true
 *  - stringCompare("Foo", "Bar", false) -> false
 */

#ifndef DMN_UTIL_HPP_
#define DMN_UTIL_HPP_

#include <algorithm>
#include <string>
#include <string_view>

namespace dmn {

/**
 * @brief Increment an integer-like value by one and ensure the result is at
 *        least 1.
 *
 * The function returns std::max<T>(1, value + 1).
 *
 * Requirements and behaviour:
 *  - T must be a type that supports operator+ with integer literal 1 and
 *    comparison with 1 (typically an integral type).
 *  - For signed integer types, adding 1 to the maximum representable value
 *    invokes undefined behaviour (signed overflow). Avoid passing such values.
 *  - For unsigned integer types, overflow is well-defined (modular arithmetic);
 *    if value is the maximum value and wraps to 0, this function will return 1.
 *
 * Example:
 *  - incrementByOne<int>(0)  == 1
 *  - incrementByOne<int>(1)  == 2
 *  - incrementByOne<int>(-5) == 1  (since max(1, -4) == 1)
 *
 * @tparam T An integer-like numeric type.
 * @param value The input value to increment.
 * @return value + 1, or 1 if value + 1 is less than 1.
 */
template <typename T> inline T incrementByOne(T value) {
  return std::max<T>(1, value + 1);
}

/**
 * @brief Compare two strings for equality, optionally in a case-insensitive
 * way.
 *
 * The function copies the provided string_views into std::string, and when
 * caseInsensitive is true it lowercases both copies using ::tolower prior to
 * comparison.
 *
 * Notes:
 *  - The lowercasing uses the C locale's tolower via ::tolower and therefore
 *    performs a simple ASCII/byte-wise conversion. It is not suitable for
 *    full Unicode case folding.
 *  - Because ::tolower takes an int (typically promoted unsigned char or EOF),
 *    converting a signed char directly may yield undefined behaviour on some
 *    platforms for negative char values; for a robust implementation cast to
 *    unsigned char before calling ::tolower.
 *
 * Complexity: O(N) where N is the length of the longer input string.
 *
 * @param str1 The first string to compare (string_view).
 * @param str2 The second string to compare (string_view).
 * @param caseInsensitive If true, comparison is performed case-insensitively.
 *                        Defaults to true.
 * @return true if the (possibly lowercased) strings are equal, false otherwise.
 */
inline bool stringCompare(const std::string_view str1,
                          const std::string_view str2,
                          bool caseInsensitive = true) {
  std::string strValue1{str1};
  std::string strValue2{str2};

  if (caseInsensitive) {
    std::transform(strValue1.begin(), strValue1.end(), strValue1.begin(),
                   ::tolower);
    std::transform(strValue2.begin(), strValue2.end(), strValue2.begin(),
                   ::tolower);
  }

  return strValue1 == strValue2;
}

} // namespace dmn

#endif // DMN_UTIL_HPP_
