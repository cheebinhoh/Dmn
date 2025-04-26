/**
 * Copyright ©  2025 Chee Bin HOH. All rights reserved.
 */

#ifndef DMN_DEBUG_HPP_

#define DMN_DEBUG_HPP_

#include <iostream>

#ifdef NDEBUG
#define DMN_DEBUG_PRINT(print_stmt)                                            \
  do {                                                                         \
    ;                                                                          \
  } while (false)
#else
#define DMN_DEBUG_PRINT(print_stmt)                                            \
  do {                                                                         \
    (print_stmt);                                                              \
  } while (false)
#endif

#endif // DMN_DEBUG_HPP_
