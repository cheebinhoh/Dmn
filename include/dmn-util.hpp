/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file include/dmn-util.hpp
 * @brief Small, header-only utility helpers used across the Dmn project.
 *
 * This header provides a couple of lightweight, commonly-used helpers:
 *  - incrementByOne<T>(T) : increment an integer value by one while
 *    ensuring the returned value is never less than 1.
 *  - stringCompare(...)   : compare two strings with optional case-insensitive
 *    mode.
 *
 * Implementation notes and guarantees:
 *  - These utilities are intentionally minimal and header-only for easy reuse.
 *  - incrementByOne supports integral types other than bool. At the maximum
 *    signed value it saturates; at the maximum unsigned value it wraps and
 *    returns 1.
 *  - stringCompare uses ICU's locale-independent Unicode case folding and
 *    canonical normalization when case-insensitive comparison is requested.
 *    The input must then be valid UTF-8. Case-sensitive comparison is a
 *    byte-wise comparison and does not validate UTF-8.
 *
 * Complexity:
 *  - incrementByOne: O(1)
 *  - stringCompare: O(N) where N is the length of the longer input string
 *
 * Examples:
 *  - incrementByOne<int>(3) -> 4
 *  - incrementByOne<int>(std::numeric_limits<int>::max()) -> same max value
 *  - incrementByOne<unsigned>(std::numeric_limits<unsigned>::max()) -> 1 (wraps
 *    modulo)
 *  - stringCompare("Hello", "hello", true) -> true
 *  - stringCompare("Foo", "Bar", false) -> false
 */

#ifndef DMN_UTIL_HPP_
#define DMN_UTIL_HPP_

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <unicode/unorm2.h>
#include <unicode/ustring.h>

namespace dmn {

namespace detail {

/**
 * @brief Decode UTF-8 text into ICU UTF-16 code units.
 *
 * @param value UTF-8 encoded input.
 * @return UTF-16 code units without a trailing NUL.
 * @throws std::length_error if the input exceeds ICU's length limit.
 * @throws std::invalid_argument if the input is malformed UTF-8.
 */
inline std::vector<UChar> stringToUnicode(const std::string_view value) {
  if (value.size() > static_cast<std::size_t>(INT32_MAX)) {
    throw std::length_error{"stringCompare input exceeds ICU's size limit"};
  }

  const auto *utf8 = value.empty() ? "" : value.data();
  const auto length = static_cast<int32_t>(value.size());
  UErrorCode status = U_ZERO_ERROR;
  int32_t unicodeLength = 0;

  u_strFromUTF8(nullptr, 0, &unicodeLength, utf8, length, &status);
  if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status)) {
    throw std::invalid_argument{"stringCompare input is not valid UTF-8"};
  }

  status = U_ZERO_ERROR;
  std::vector<UChar> result(static_cast<std::size_t>(unicodeLength) + 1);

  u_strFromUTF8(result.data(), static_cast<int32_t>(result.size()),
                &unicodeLength, utf8, length, &status);
  if (U_FAILURE(status)) {
    throw std::invalid_argument{"stringCompare input is not valid UTF-8"};
  }

  result.resize(static_cast<std::size_t>(unicodeLength));

  return result;
}

/**
 * @brief Normalize UTF-16 code units to canonical decomposition form.
 *
 * @param value UTF-16 input.
 * @param normalizer ICU normalizer instance.
 * @return The normalized UTF-16 code units without a trailing NUL.
 * @throws std::runtime_error if ICU normalization fails.
 */
inline std::vector<UChar> normalizeNfd(const std::vector<UChar> &value,
                                       const UNormalizer2 *normalizer) {
  const auto *source = value.empty() ? u"" : value.data();
  const auto length = static_cast<int32_t>(value.size());
  UErrorCode status = U_ZERO_ERROR;

  const auto resultLength =
      unorm2_normalize(normalizer, source, length, nullptr, 0, &status);
  if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status)) {
    throw std::runtime_error{"ICU failed to normalize a string"};
  }

  status = U_ZERO_ERROR;
  std::vector<UChar> result(static_cast<std::size_t>(resultLength) + 1);

  const auto actualLength =
      unorm2_normalize(normalizer, source, length, result.data(),
                       static_cast<int32_t>(result.size()), &status);
  if (U_FAILURE(status)) {
    throw std::runtime_error{"ICU failed to normalize a string"};
  }

  result.resize(static_cast<std::size_t>(actualLength));

  return result;
}

/**
 * @brief Apply ICU's default, locale-independent Unicode case folding.
 *
 * @param value UTF-16 input.
 * @return Case-folded UTF-16 code units without a trailing NUL.
 * @throws std::runtime_error if ICU case folding fails.
 */
inline std::vector<UChar> foldUnicodeCase(const std::vector<UChar> &value) {
  const auto *source = value.empty() ? u"" : value.data();
  const auto length = static_cast<int32_t>(value.size());
  UErrorCode status = U_ZERO_ERROR;

  const auto resultLength =
      u_strFoldCase(nullptr, 0, source, length, U_FOLD_CASE_DEFAULT, &status);
  if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status)) {
    throw std::runtime_error{"ICU failed to fold a string"};
  }

  status = U_ZERO_ERROR;
  std::vector<UChar> result(static_cast<std::size_t>(resultLength) + 1);
  const auto actualLength =
      u_strFoldCase(result.data(), static_cast<int32_t>(result.size()), source,
                    length, U_FOLD_CASE_DEFAULT, &status);
  if (U_FAILURE(status)) {
    throw std::runtime_error{"ICU failed to fold a string"};
  }

  result.resize(static_cast<std::size_t>(actualLength));

  return result;
}

/**
 * @brief Decode, canonically normalize, and case-fold UTF-8 input.
 *
 * @param value UTF-8 encoded input.
 * @return Case-folded text in canonical decomposition form.
 * @throws std::invalid_argument if the input is malformed UTF-8.
 * @throws std::length_error if the input exceeds ICU's length limit.
 * @throws std::runtime_error if ICU initialization or processing fails.
 */
inline std::vector<UChar> unicodeCaseFoldNfd(std::string_view value) {
  UErrorCode status = U_ZERO_ERROR;
  const auto *normalizer = unorm2_getNFDInstance(&status);
  if (U_FAILURE(status)) {
    throw std::runtime_error{"ICU failed to initialize Unicode normalization"};
  }

  const auto decoded = stringToUnicode(value);
  const auto normalized = normalizeNfd(decoded, normalizer);

  return normalizeNfd(foldUnicodeCase(normalized), normalizer);
}

} // namespace detail

/**
 * @brief Increment an integer-like value by one and ensure the result is at
 * least 1.
 *
 * The function returns @c max(1, value + 1). Signed integral types saturate at
 * their maximum representable value; unsigned integral types wrap according
 * to modular arithmetic, so the maximum value produces 1.
 *
 * Only integral types other than @c bool are supported.
 *
 * Example:
 *  - incrementByOne<int>(0)  == 1
 *  - incrementByOne<int>(1)  == 2
 *  - incrementByOne<int>(-5) == 1 (since max(1, -4) == 1)
 *
 * @tparam T An integral type other than @c bool.
 * @param value The input value to increment.
 * @return The incremented value, bounded below by 1, saturated at the signed
 * maximum, or wrapped to 1 for the maximum unsigned value.
 */
template <std::integral T>
  requires(!std::same_as<T, bool>)
inline T incrementByOne(T value) {
  if constexpr (std::numeric_limits<T>::is_signed) {
    if (value == std::numeric_limits<T>::max()) {
      return value;
    }
  }

  return std::max<T>(1, value + 1);
}

/**
 * @brief Compare two strings for equality, optionally in a case-insensitive
 * way.
 *
 * When @p caseInsensitive is true, the function decodes both inputs as UTF-8,
 * applies locale-independent Unicode default case folding and canonical
 * decomposition, then compares the results. Invalid UTF-8 input throws
 * @c std::invalid_argument. This is Unicode-aware caseless comparison; it does
 * not apply locale-specific casing rules.
 *
 * When @p caseInsensitive is false, the function compares the input bytes
 * directly without validating UTF-8.
 *
 * Complexity: O(N) where N is the length of the longer input string.
 *
 * @param str1 The first string to compare (string_view).
 * @param str2 The second string to compare (string_view).
 * @param caseInsensitive If true, comparison is performed case-insensitively.
 * Defaults to true.
 * @return true if the (possibly lowercased) strings are equal, false otherwise.
 * @throws std::invalid_argument if case-insensitive comparison receives
 * malformed UTF-8.
 * @throws std::length_error if an input exceeds ICU's length limit.
 * @throws std::runtime_error if ICU normalization or case folding fails.
 */
inline bool stringCompare(const std::string_view str1,
                          const std::string_view str2,
                          bool caseInsensitive = true) {
  if (!caseInsensitive) {
    return str1 == str2;
  }

  return detail::unicodeCaseFoldNfd(str1) == detail::unicodeCaseFoldNfd(str2);
}

/**
 * @brief Minimal scope guard with move transfer and explicit release.
 *
 * An armed guard calls its callable when it goes out of scope. Moving a guard
 * transfers that responsibility and disarms the source. Calling @c release()
 * disarms the guard. The callable must be nothrow-invocable.
 *
 * @tparam F A nothrow-invocable callable type.
 */
template <typename F>
  requires std::is_nothrow_invocable_v<F &>
class ScopeGuard {
public:
  /**
   * @brief Create an armed guard that owns the cleanup callable.
   *
   * @param callable Nothrow-invocable callable to run on scope exit.
   * @throws Any exception thrown while copying or moving the callable.
   */
  explicit ScopeGuard(F callable) noexcept(
      std::is_nothrow_move_constructible_v<F>)
      : m_callable(std::move(callable)) {}

  /// Copy construction is disabled to prevent duplicate cleanup.
  ScopeGuard(const ScopeGuard &) = delete;

  /// Copy assignment is disabled to prevent duplicate cleanup.
  auto operator=(const ScopeGuard &) -> ScopeGuard & = delete;

  /// Move assignment is disabled so an armed destination cannot lose cleanup.
  auto operator=(ScopeGuard &&) -> ScopeGuard & = delete;

  /**
   * @brief Transfer cleanup responsibility from another guard.
   *
   * @param other Guard to move from; it is disarmed by the move.
   * @throws Any exception thrown while moving the callable.
   */
  ScopeGuard(ScopeGuard &&other) noexcept(
      std::is_nothrow_move_constructible_v<F>)
      : m_callable(std::move(other.m_callable)),
        m_active(std::exchange(other.m_active, false)) {}

  /// @brief Invoke the cleanup callable if this guard remains armed.
  ~ScopeGuard() noexcept {
    if (m_active) {
      m_callable();
    }
  }

  /**
   * @brief Disarm the guard so its cleanup callable is not invoked.
   */
  void release() noexcept { m_active = false; }

private:
  F m_callable;
  bool m_active{true};
};

/**
 * @brief Construct a @c ScopeGuard with automatic callable-type deduction.
 *
 * @tparam F Callable type; deduced from the argument and stored by value.
 * @param f Nothrow-invocable callable to invoke when the armed guard is
 * destroyed.
 * @return A move-only guard that calls @p f on scope exit unless released.
 * @throws Any exception thrown while constructing the stored callable.
 */
template <typename F>
  requires(std::is_nothrow_invocable_v<std::decay_t<F> &> &&
           std::constructible_from<std::decay_t<F>, F>)
auto make_scope_guard(F &&f) -> ScopeGuard<std::decay_t<F>> {
  return ScopeGuard<std::decay_t<F>>{std::forward<F>(f)};
}

} // namespace dmn

#endif // DMN_UTIL_HPP_
