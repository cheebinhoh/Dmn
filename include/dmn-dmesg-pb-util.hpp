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

#include "proto/dmn-dmesg.pb.h"

#include <cassert>
#include <sys/time.h>

/**
 * @defgroup dmesg_pb_timestamp Timestamp setters
 * @brief Macros for setting timestamp fields on @c dmn::Timestamp protobuf
 *        objects.
 * @{
 */

/**
 * @def DMESG_PB_SET_TIMESTAMP_SECONDS
 * @brief Set the seconds field of a timestamp protobuf pointer.
 *
 * @param ts  Pointer to the @c dmn::Timestamp protobuf object.
 * @param val Integer value to assign to the seconds field.
 */
#define DMESG_PB_SET_TIMESTAMP_SECONDS(ts, val) (ts)->set_seconds(val)

/**
 * @def DMESG_PB_SET_TIMESTAMP_NANOS
 * @brief Set the nanoseconds field of a timestamp protobuf pointer.
 *
 * @param ts  Pointer to the @c dmn::Timestamp protobuf object.
 * @param val Integer value to assign to the nanos field.
 */
#define DMESG_PB_SET_TIMESTAMP_NANOS(ts, val) (ts)->set_nanos(val)

/**
 * @def DMESG_PB_SET_TIMESTAMP_FROM_TV
 * @brief Set both seconds and nanoseconds of a timestamp from a
 *        @c struct @c timeval.
 *
 * @param ts Pointer to the @c dmn::Timestamp protobuf object.
 * @param tv A @c struct @c timeval whose @c tv_sec and @c tv_usec fields are
 *           used.
 */
#define DMESG_PB_SET_TIMESTAMP_FROM_TV(ts, tv)                                 \
  do {                                                                         \
    DMESG_PB_SET_TIMESTAMP_SECONDS(ts, (tv).tv_sec);                           \
    DMESG_PB_SET_TIMESTAMP_NANOS(ts, (tv).tv_usec * 1000);                     \
  } while (false)

/// @}  // group dmesg_pb_timestamp

/**
 * @defgroup dmesg_pb_msg DMesgPb message field setters
 * @brief Macros for setting top-level fields on @c dmn::DMesgPb protobuf
 *        objects.
 * @{
 */

/**
 * @def DMESG_PB_SET_MSG_TIMESTAMP_FROM_TV
 * @brief Set the timestamp of a @c DMesgPb message from a @c struct timeval.
 *
 * @param pb The @c dmn::DMesgPb message object (by reference).
 * @param tv A @c struct timeval providing seconds and microseconds.
 */
#define DMESG_PB_SET_MSG_TIMESTAMP_FROM_TV(pb, tv)                             \
  DMESG_PB_SET_TIMESTAMP_FROM_TV((pb).mutable_timestamp(), tv)

/**
 * @def DMESG_PB_SET_MSG_TOPIC
 * @brief Set the topic field of a @c DMesgPb message.
 * @param pb  The @c dmn::DMesgPb message object.
 * @param val The topic string value.
 */
#define DMESG_PB_SET_MSG_TOPIC(pb, val) ((pb).set_topic((val)))

/**
 * @def DMESG_PB_SET_MSG_CONFLICT
 * @brief Set the conflict flag of a @c DMesgPb message.
 * @param pb  The @c dmn::DMesgPb message object.
 * @param val Boolean conflict value.
 */
#define DMESG_PB_SET_MSG_CONFLICT(pb, val) ((pb).set_conflict((val)))

/**
 * @def DMESG_PB_SET_MSG_RUNNINGCOUNTER
 * @brief Set the running counter field of a @c DMesgPb message.
 * @param pb  The @c dmn::DMesgPb message object.
 * @param val The counter value.
 */
#define DMESG_PB_SET_MSG_RUNNINGCOUNTER(pb, val)                               \
  ((pb).set_runningcounter((val)))

/**
 * @def DMESG_PB_SET_MSG_SOURCEIDENTIFIER
 * @brief Set the source identifier field of a @c DMesgPb message.
 * @param pb  The @c dmn::DMesgPb message object.
 * @param val The source identifier string.
 */
#define DMESG_PB_SET_MSG_SOURCEIDENTIFIER(pb, val)                             \
  ((pb).set_sourceidentifier((val)))

/**
 * @def DMESG_PB_SET_MSG_SOURCEWRITEHANDLERIDENTIFIER
 * @brief Set the source write-handler identifier field of a @c DMesgPb
 *        message.
 * @param pb  The @c dmn::DMesgPb message object.
 * @param val The write-handler identifier string.
 */
#define DMESG_PB_SET_MSG_SOURCEWRITEHANDLERIDENTIFIER(pb, val)                 \
  ((pb).set_sourcewritehandleridentifier((val)))

/**
 * @def DMESG_PB_SET_MSG_TYPE
 * @brief Set the type field of a @c DMesgPb message.
 * @param pb  The @c dmn::DMesgPb message object.
 * @param val The @c dmn::DMesgTypePb enum value.
 */
#define DMESG_PB_SET_MSG_TYPE(pb, val) ((pb).set_type((val)))

/**
 * @def DMESG_PB_SET_MSG_PLAYBACK
 * @brief Set the playback flag of a @c DMesgPb message.
 * @param pb  The @c dmn::DMesgPb message object.
 * @param val Boolean playback flag value.
 */
#define DMESG_PB_SET_MSG_PLAYBACK(pb, val) ((pb).set_playback((val)))

/**
 * @def DMESG_PB_SET_MSG_FORCE
 * @brief Set the force flag of a @c DMesgPb message.
 * @param pb  The @c dmn::DMesgPb message object.
 * @param val Boolean force flag value.
 */
#define DMESG_PB_SET_MSG_FORCE(pb, val) ((pb).set_force((val)))

/// @}  // group dmesg_pb_msg

/**
 * @defgroup dmesg_pb_body DMesgPb message body setters
 * @brief Macros for setting body sub-message fields on @c dmn::DMesgPb objects.
 * @{
 */

/**
 * @def DMESG_PB_MSG_SET_MESSAGE
 * @brief Set the text message body of a @c DMesgPb whose type is
 *        @c dmn::DMesgTypePb::message.
 *
 * Asserts at runtime that @c (pb).type() == @c dmn::DMesgTypePb::message
 * before setting the field.
 *
 * @param pb  The @c dmn::DMesgPb message object.
 * @param val The message body string.
 */
#define DMESG_PB_MSG_SET_MESSAGE(pb, val)                                      \
  do {                                                                         \
    assert((pb).type() == dmn::DMesgTypePb::message);                          \
    ((pb).mutable_body()->set_message((val)));                                 \
  } while (false)

/// @}  // group dmesg_pb_body

/**
 * @defgroup dmesg_pb_sys DMesgPb sys sub-message setters
 * @brief Macros for setting fields on the @c sys sub-message and its node
 *        list entries inside a @c dmn::DMesgPb object.
 * @{
 */

/**
 * @def DMESG_PB_SYS_SET_TIMESTAMP_FROM_TV
 * @brief Set the sys sub-message timestamp from a @c struct timeval.
 * @param pb The @c dmn::DMesgPb message object.
 * @param tv A @c struct timeval.
 */
#define DMESG_PB_SYS_SET_TIMESTAMP_FROM_TV(pb, tv)                             \
  DMESG_PB_SET_TIMESTAMP_FROM_TV(                                              \
      (pb).mutable_body()->mutable_sys()->mutable_timestamp(), tv)

/**
 * @def DMESG_PB_SYS_NODE_SET_IDENTIFIER
 * @brief Set the identifier field on a sys node pointer.
 * @param node Pointer to a @c dmn::SysNodePb protobuf object.
 * @param val  The identifier string.
 */
#define DMESG_PB_SYS_NODE_SET_IDENTIFIER(node, val) (node)->set_identifier(val)

/**
 * @def DMESG_PB_SYS_NODE_SET_MASTERIDENTIFIER
 * @brief Set the master-identifier field on a sys node pointer.
 * @param node Pointer to a @c dmn::SysNodePb protobuf object.
 * @param val  The master identifier string.
 */
#define DMESG_PB_SYS_NODE_SET_MASTERIDENTIFIER(node, val)                      \
  (node)->set_masteridentifier(val)

/**
 * @def DMESG_PB_SYS_NODE_SET_STATE
 * @brief Set the state field on a sys node pointer.
 * @param node Pointer to a @c dmn::SysNodePb protobuf object.
 * @param val  The node state value.
 */
#define DMESG_PB_SYS_NODE_SET_STATE(node, val) (node)->set_state(val)

/**
 * @def DMESG_PB_SYS_NODE_SET_UPDATEDTIMESTAMP_FROM_TV
 * @brief Set the updated-timestamp on a sys node from a @c struct timeval.
 * @param node Pointer to a @c dmn::SysNodePb protobuf object.
 * @param tv   A @c struct timeval.
 */
#define DMESG_PB_SYS_NODE_SET_UPDATEDTIMESTAMP_FROM_TV(node, tv)               \
  DMESG_PB_SET_TIMESTAMP_FROM_TV((node)->mutable_updatedtimestamp(), tv)

/**
 * @def DMESG_PB_SYS_NODE_SET_INITIALIZEDTIMESTAMP_FROM_TV
 * @brief Set the initialized-timestamp on a sys node from a @c struct timeval.
 * @param node Pointer to a @c dmn::SysNodePb protobuf object.
 * @param tv   A @c struct timeval.
 */
#define DMESG_PB_SYS_NODE_SET_INITIALIZEDTIMESTAMP_FROM_TV(node, tv)           \
  DMESG_PB_SET_TIMESTAMP_FROM_TV((node)->mutable_initializedtimestamp(), tv)

/**
 * @def DMESG_PB_SYS_SET_NODELIST_ELEM_IDENTIFIER
 * @brief Set the identifier of a node-list entry in a @c DMesgPb sys message.
 * @param sys   The @c dmn::DMesgPb message object.
 * @param index Zero-based index into the node list.
 * @param val   The identifier string.
 */
#define DMESG_PB_SYS_SET_NODELIST_ELEM_IDENTIFIER(sys, index, val)             \
  DMESG_PB_SYS_NODE_SET_IDENTIFIER(                                            \
      ((sys).mutable_body()->mutable_sys()->mutable_nodelist((index))), val)

/**
 * @def DMESG_PB_SYS_SET_NODELIST_ELEM_MASTERIDENTIFIER
 * @brief Set the master identifier of a node-list entry in a @c DMesgPb sys
 *        message.
 * @param sys   The @c dmn::DMesgPb message object.
 * @param index Zero-based index into the node list.
 * @param val   The master identifier string.
 */
#define DMESG_PB_SYS_SET_NODELIST_ELEM_MASTERIDENTIFIER(sys, index, val)       \
  DMESG_PB_SYS_NODE_SET_MASTERIDENTIFIER(                                      \
      ((sys).mutable_body()->mutable_sys()->mutable_nodelist((index))), val)

/**
 * @def DMESG_PB_SYS_SET_NODELIST_ELEM_STATE
 * @brief Set the state of a node-list entry in a @c DMesgPb sys message.
 * @param sys   The @c dmn::DMesgPb message object.
 * @param index Zero-based index into the node list.
 * @param val   The node state value.
 */
#define DMESG_PB_SYS_SET_NODELIST_ELEM_STATE(sys, index, val)                  \
  DMESG_PB_SYS_NODE_SET_STATE(                                                 \
      ((sys).mutable_body()->mutable_sys()->mutable_nodelist((index))), val)

/**
 * @def DMESG_PB_SYS_SET_NODELIST_ELEM_INITIALIZEDTIMESTAMP
 * @brief Set the initialized-timestamp of a node-list entry from a
 *        @c dmn::Timestamp protobuf.
 * @param sys   The @c dmn::DMesgPb message object.
 * @param index Zero-based index into the node list.
 * @param val   A @c dmn::Timestamp protobuf value.
 */
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

/**
 * @def DMESG_PB_SYS_SET_NODELIST_ELEM_UPDATEDTIMESTAMP
 * @brief Set the updated-timestamp of a node-list entry from a
 *        @c dmn::Timestamp protobuf.
 * @param sys   The @c dmn::DMesgPb message object.
 * @param index Zero-based index into the node list.
 * @param val   A @c dmn::Timestamp protobuf value.
 */
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

/// @}  // group dmesg_pb_sys

#endif // DMN_DMESG_PB_UTIL_HPP_
