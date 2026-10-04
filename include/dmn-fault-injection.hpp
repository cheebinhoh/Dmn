/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-fault-injection.hpp
 * @brief Fault injection helpers for Dmn.
 *
 * This header provides a thin wrapper around libfiu failure points.
 *
 * Fault points do not initialize libfiu themselves. The process entry point
 * must initialize libfiu before enabling points; fiu-run does this for tests.
 */

#ifndef DMN_FAULT_INJECTION_HPP_
#define DMN_FAULT_INJECTION_HPP_

#ifdef FIU_ENABLE
#include <fiu.h>
#else
#define fiu_fail(name) 0
#define fiu_failinfo() nullptr
#define fiu_do_on(name, action)
#define fiu_exit_on(name)
#define fiu_return_on(name, retval)
#endif

namespace dmn {

#define DMN_FI_TIMER_PIPE_PROC_PTHREAD_CREATE()                                \
  (fiu_fail("dmn/timer/pipe/proc/pthread_create") != 0)

#define DMN_FI_TIMER_RESCHEDULE_WRITE_AT()                                     \
  (fiu_fail("dmn/timer/reschedule/write_at") != 0)

} // namespace dmn

#endif // DMN_FAULT_INJECTION_HPP_
