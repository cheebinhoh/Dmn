/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-dmesg-pb-util.hpp
 * @brief Utility macros to populate fields of the DMesg protobuf messages.
 *
 * This header provides a collection of small, focused macros that make it
 * convenient to set timestamps and other common fields in the generated
 * protobuf types defined in proto/dmn-dmesg.pb.h.
 *
 * Key points:
 *  - The macros are thin wrappers around the protobuf setters (e.g.
 *    set_seconds, set_nanos, set_topic) and are intended to reduce
 *    boilerplate when constructing DMesg messages.
 *  - Timestamp helpers accept either explicit seconds/nanoseconds values or
 *    struct timeval (from <sys/time.h>) via DMESG_PB_SET_TIMESTAMP_FROM_TV.
 *  - Some macros assert expected message types before mutating variant fields
 *    (see DMESG_PB_MSG_SET_MESSAGE).
 *  - Macros use expression or do/while(false) forms to behave like statements;
 *    pass lvalues where required.
 *
 * Usage notes:
 *  - Include this header after including the generated protobuf header
 *    "proto/dmn-dmesg.pb.h".
 *  - Be careful with side effects in macro arguments (they may be evaluated
 *    more than once).
 *  - These macros do not perform extensive runtime validation beyond simple
 *    asserts; callers are responsible for ensuring message structure and
 *    semantic correctness.
 *
 * Thread-safety:
 *  - The macros themselves are just expansions that call protobuf mutators.
 *    Any thread-safety guarantees depend on the underlying protobuf objects
 *    and how they are synchronized by the application.
 *
 * Example:
 *  dmn::DMesgPb msg;
 *  struct timeval tv;
 *  gettimeofday(&tv, nullptr);
 *  DMESG_PB_SET_MSG_TIMESTAMP_FROM_TV(msg, tv);
 *  DMESG_PB_SET_MSG_TOPIC(msg, "my/topic");
 *
 */

#ifndef DMN_DMESG_PB_UTIL_HPP_
#define DMN_DMESG_PB_UTIL_HPP_

#include <sys/time.h>

#include <cassert>

#include "proto/dmn-dmesg.pb.h"

#define DMESG_PB_SET_TIMESTAMP_SECONDS(ts, val) (ts)->set_seconds(val)

#define DMESG_PB_SET_TIMESTAMP_NANOS(ts, val) (ts)->set_nanos(val)

#define DMESG_PB_SET_TIMESTAMP_FROM_TV(ts, tv)                                 \
  do {                                                                         \
    DMESG_PB_SET_TIMESTAMP_SECONDS(ts, (tv).tv_sec);                           \
    DMESG_PB_SET_TIMESTAMP_NANOS(ts, (tv).tv_usec * 1000);                     \
  } while (false)

/**
 * DMesgPb Message setters
 */
#define DMESG_PB_SET_MSG_TIMESTAMP_FROM_TV(pb, tv)                             \
  DMESG_PB_SET_TIMESTAMP_FROM_TV((pb).mutable_timestamp(), tv)

#define DMESG_PB_SET_MSG_TOPIC(pb, val) ((pb).set_topic((val)))

#define DMESG_PB_SET_MSG_CONFLICT(pb, val) ((pb).set_conflict((val)))

#define DMESG_PB_SET_MSG_RUNNINGCOUNTER(pb, val)                               \
  ((pb).set_runningcounter((val)))

#define DMESG_PB_SET_MSG_SOURCEIDENTIFIER(pb, val)                             \
  ((pb).set_sourceidentifier((val)))

#define DMESG_PB_SET_MSG_SOURCEWRITEHANDLERIDENTIFIER(pb, val)                 \
  ((pb).set_sourcewritehandleridentifier((val)))

#define DMESG_PB_SET_MSG_TYPE(pb, val) ((pb).set_type((val)))

#define DMESG_PB_SET_MSG_PLAYBACK(pb, val) ((pb).set_playback((val)))

#define DMESG_PB_SET_MSG_FORCE(pb, val) ((pb).set_force((val)))

/**
 * DMesgPb message body message setters
 */
#define DMESG_PB_MSG_SET_MESSAGE(pb, val)                                      \
  do {                                                                         \
    assert((pb).type() == dmn::DMesgTypePb::message);                          \
    ((pb).mutable_body()->set_message((val)));                                 \
  } while (false)

/**
 * DMesgPb message body sys setters
 */
#define DMESG_PB_SYS_SET_TIMESTAMP_FROM_TV(pb, tv)                             \
  DMESG_PB_SET_TIMESTAMP_FROM_TV(                                              \
      (pb).mutable_body()->mutable_sys()->mutable_timestamp(), tv)

#define DMESG_PB_SYS_NODE_SET_IDENTIFIER(node, val) (node)->set_identifier(val)

#define DMESG_PB_SYS_NODE_SET_MASTERIDENTIFIER(node, val)                      \
  (node)->set_masteridentifier(val)

#define DMESG_PB_SYS_NODE_SET_STATE(node, val) (node)->set_state(val)

#define DMESG_PB_SYS_NODE_SET_UPDATEDTIMESTAMP_FROM_TV(node, tv)               \
  DMESG_PB_SET_TIMESTAMP_FROM_TV((node)->mutable_updatedtimestamp(), tv)

#define DMESG_PB_SYS_NODE_SET_INITIALIZEDTIMESTAMP_FROM_TV(node, tv)           \
  DMESG_PB_SET_TIMESTAMP_FROM_TV((node)->mutable_initializedtimestamp(), tv)

#define DMESG_PB_SYS_SET_NODELIST_ELEM_IDENTIFIER(sys, index, val)             \
  DMESG_PB_SYS_NODE_SET_IDENTIFIER(                                            \
      ((sys).mutable_body()->mutable_sys()->mutable_nodelist((index))), val)

#define DMESG_PB_SYS_SET_NODELIST_ELEM_MASTERIDENTIFIER(sys, index, val)       \
  DMESG_PB_SYS_NODE_SET_MASTERIDENTIFIER(                                      \
      ((sys).mutable_body()->mutable_sys()->mutable_nodelist((index))), val)

#define DMESG_PB_SYS_SET_NODELIST_ELEM_STATE(sys, index, val)                  \
  DMESG_PB_SYS_NODE_SET_STATE(                                                 \
      ((sys).mutable_body()->mutable_sys()->mutable_nodelist((index))), val)

#define DMESG_PB_SYS_SET_NODELIST_ELEM_INITIALIZEDTIMESTAMP(sys, index, val)   \
  do {                                                                         \
    DMESG_PB_SET_TIMESTAMP_SECONDS(((sys)                                      \
                                        .mutable_body()                        \
                                        ->mutable_sys()                        \
                                        ->mutable_nodelist((index))            \
                                        ->mutable_initializedtimestamp()),     \
                                   (val).seconds());                           \
    DMESG_PB_SET_TIMESTAMP_NANOS(((sys)                                        \
                                      .mutable_body()                          \
                                      ->mutable_sys()                          \
                                      ->mutable_nodelist((index))              \
                                      ->mutable_initializedtimestamp()),       \
                                 (val).nanos());                               \
  } while (false)

#define DMESG_PB_SYS_SET_NODELIST_ELEM_UPDATEDTIMESTAMP(sys, index, val)       \
  do {                                                                         \
    DMESG_PB_SET_TIMESTAMP_SECONDS(((sys)                                      \
                                        .mutable_body()                        \
                                        ->mutable_sys()                        \
                                        ->mutable_nodelist((index))            \
                                        ->mutable_updatedtimestamp()),         \
                                   (val).seconds());                           \
    DMESG_PB_SET_TIMESTAMP_NANOS(((sys)                                        \
                                      .mutable_body()                          \
                                      ->mutable_sys()                          \
                                      ->mutable_nodelist((index))              \
                                      ->mutable_updatedtimestamp()),           \
                                 (val).nanos());                               \
  } while (false)

#endif // DMN_DMESG_PB_UTIL_HPP_
