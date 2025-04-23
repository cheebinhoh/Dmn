/**
 * Copyright ©  2025 Chee Bin HOH. All rights reserved.
 */

#ifndef DMN_DEBUG_HPP_

#define DMN_DEBUG_HPP_

#include <iostream>

#ifdef NDEBUG
#define DMN_DEBUG_PRINT(printstmt)                                             \
  do {                                                                         \
    ;                                                                          \
  } while (false)
#else
#define DMN_DEBUG_PRINT(printstmt)                                             \
  do {                                                                         \
    (printstmt);                                                               \
  } while (false)
#endif

#endif // DMN_DEBUG_HPP_
