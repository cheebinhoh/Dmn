/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn.hpp
 * @brief Convenience umbrella header for the Distributed Messaging Network
 * (DMN).
 *
 * This header aggregates the public headers that make up the DMN library and is
 * provided as a single include for convenience. Including this file will pull
 * in the full public API surface of the DMN library; for faster compilation
 * times and tighter dependency control prefer including only the specific
 * headers that your translation unit requires.
 *
 * @details
 * - Purpose: Provide a single, easy-to-use entry point to the DMN public API.
 * - Behavior: This file only forwards includes and does not introduce new
 *   symbols or definitions beyond those in the included headers.
 * - Include guard: The guard below prevents multiple inclusion across the
 * build.
 *
 * See the project README or the individual header documentation for more
 * information about each component.
 */

#ifndef DMN_HPP_
#define DMN_HPP_

#include "dmn-async.hpp"
#include "dmn-blockingqueue.hpp"
#include "dmn-debug.hpp"
#include "dmn-dmesg-pb-util.hpp"
#include "dmn-dmesg.hpp"
#include "dmn-io.hpp"
#include "dmn-limit-blockingqueue.hpp"
#include "dmn-pipe.hpp"
#include "dmn-proc.hpp"
#include "dmn-pub-sub.hpp"
#include "dmn-runtime.hpp"
#include "dmn-singleton.hpp"
#include "dmn-socket.hpp"
#include "dmn-teepipe.hpp"
#include "dmn-timer.hpp"
#include "dmn-util.hpp"

#include "kafka/dmn-dmesgnet-kafka.hpp"
#include "kafka/dmn-kafka-util.hpp"
#include "kafka/dmn-kafka.hpp"

#endif // DMN_HPP_
