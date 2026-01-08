/**
 * Copyright ©  2025 Chee Bin HOH. All rights reserved.
 *
 * @file include/dmn-debug.hpp
 * @brief Lightweight debug-print macro controlled by the preprocessor.
 *
 * This header provides the DMN_DEBUG_PRINT(print_stmt) macro which is
 * enabled when NDEBUG is not defined and compiled out when NDEBUG is
 * defined (typically for release builds). The macro expands to the
 * provided statement inside a safe do/while(false) wrapper:
 *
 *  - When NDEBUG is NOT defined:
 *      DMN_DEBUG_PRINT(print_stmt)  -> expands to (print_stmt);
 *
 *  - When NDEBUG IS defined:
 *      DMN_DEBUG_PRINT(print_stmt)  -> expands to an empty statement.
 *
 * Usage example:
 *   DMN_DEBUG_PRINT(std::cerr << "value: " << x << '\n');
 *
 * Important notes:
 *  - The argument must be a complete statement (for example a stream
 *    insertion expression followed by a semicolon when used inside the
 *    macro).
 *  - The macro is intentionally wrapped in a do { ... } while (false)
 *    to make it safe to use inside control-flow constructs (if/else).
 *  - When NDEBUG is defined the print expression is not evaluated, so
 *    do not rely on the expression for side effects in release builds.
 *  - This header includes <iostream> because typical use is to write to
 *    std::cerr or std::cout; adapt if you prefer a different logging sink.
 */

#ifndef DMN_DEBUG_HPP_
#define DMN_DEBUG_HPP_

#include <iostream>

#ifdef NDEBUG
#define DMN_DEBUG_PRINT(print_stmt)                                            \
  do {                                                                         \
  } while (false)
#else
#define DMN_DEBUG_PRINT(print_stmt)                                            \
  do {                                                                         \
    (print_stmt);                                                              \
  } while (false)
#endif

#endif // DMN_DEBUG_HPP_
