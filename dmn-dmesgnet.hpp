/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * The Dmn_DMesgNet stands for Distributed Messaging Network, it is an
 * extension to the Dmn_DMesg with support of serializing the DMesgPb
 * message and send to the output Dmn_Io and deserialize the data read
 * from input Dmn_Io and then publish it to the local Dmn_DMesg
 * subscribed handlers.
 */

#ifndef DMN_DMESGNET_HPP_

#define DMN_DMESGNET_HPP_

#include <sys/time.h>

#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include "dmn-debug.hpp"
#include "dmn-dmesg-pb-util.hpp"
#include "dmn-dmesg.hpp"
#include "dmn-io.hpp"
#include "dmn-timer.hpp"

namespace Dmn {

#define DMN_DMESGNET_HEARTBEAT_IN_NS (1000000000)
#define DMN_DMESGNET_MASTERPENDING_MAX_COUNTER (3)
#define DMN_DMESGNET_MASTERSYNC_MAX_COUNTER (5)

class Dmn_DMesgNet : public Dmn_DMesg {
public:
  /**
   * 1.    Initialized - [send heartbeat] -> MasterElectionPending
   * 2[1]. MasterPending - [receive master heartbeat] -> Ready
   * 2[2]. MasterPending - [timeout w/o master heartbeat, act as master]
   *       -> Ready
   * 3[1]. Ready - [receive master last heartbeat] -> MasterPending
   * 3[2]. Ready - [send last heartbeat, optionally relinquish master role]
   *       -> Destroyed
   * 4.    Destroyed [cache last state in file?]
   *
   * When a master is about to be destroyed, its last heartbeat message will
   * relinquish its master role, that will trigger 3[1] for all other nodes,
   * and instead of moving all other nodes into passive master election 2[1]
   * or 2[2], all other nodes can do co-election of master by:
   * - each node selects the earliest created timestamp node from the list of
   *   neighbor nodes (including itself).
   * - if all remaining nodes have same list of neighbor nodes, they will all
   *   elect the same master, and all are in sync.
   * - otherwise, different master will be elected by different nodes, but as
   *   those nodes are syncing their heartbeat, they will all agree on node
   *   with earliest created timestamp.
   *
   * for each heartbeat broadcast message from the node, it includes the
   * following information:
   * - the node identifier [timestamp_at_initialized.process_id.ip]
   * - the node's master identifier which
   * - the node's known list of networked node identifiers (include itself)
   *
   * If there are 3 nodes, A, B and C. And it boots up in the following orders:
   *
   * T1: A boot up, and send heartbeat [A, , []]
   * T2: B boot up, and send heartbeat [B, , []]
   * T3: A receives [B, , []], and add B to its neighbor list, but B is
   *     not master, so A remains in initialized (as not yet timeout),
   *     and its next pending heartbeat is [A, , [A, B]]
   * T4: B receives [A, , []], and add A to its neighbor list, but A is
   *     not master, so B remains in initialized (as not yet timeout),
   *     and its next pending heartbeat is [B, , [A, B]]
   * T5: A timeout at initialized state, and self-proclaim itself as
   *     master based on its list of neighbor nodes [A, B] as A
   *     has earliest timestamp at created.
   * T6: B timeout at initialized state, and select A as the master based
   *     on its list of neighbor nodes [A, B] and A has earliest timestamp
   *     at created.
   * T7: both A and B receives each other heartbeat, and then
   *     both agree that A is master as A has early timestamp on created,
   *     and so both will remain in ready state.
   *     heartbeat A = [A, A, [A, B]] B = [B, A, [A, B]].
   * T8: when C boots up, it is in initialized state, and send out
   *     heartbeat C = [C, , ,]
   * T9: A and B receives C heartbeat and both send out heartbeat
   *     A = [A, A, [A, B, C]] and B = [B, A, [A, B, C]], and C receives
   *     either of the message will declare A as master and into ready
   *     state, also keep a list of neighbor nodes [A, B, C].
   *
   * Tn: A is shutting down, and manage to send its last heartbeat,
   *     [A, ,[B, C]] to B and C, and both will notice that the message
   *     is from A but it relinquishes master position, and remove itself
   *     from the neighbor list, and B and C will elect new master
   *     which is B as it has earliest timestamp.
   *
   * The node will maintain its list of neighbor nodes, but if there is no
   * heartbeat from one of the neighbor for N seconds, it will remove it from
   * the list, if the master is not sending heartbeat, then all nodes will
   * participate in master selection by using it last known list of networed
   * nodes (include itself) and elect one with early timestamp as master, and
   * send out the next heartbeat, in good case, all active nodes will agree on
   * the same new master right away.
   *
   * In very race case, two nodes elect different master as one node does not
   * see one or few other nodes that are better candidate for master (early
   * timestamp), then it will surrender its master election result and agree on
   * one selected by its neighbor heartbeat message.
   */

  /**
   * @brief The constructor method for DMesgNet.
   *
   * @param name          The identification name for the DMesgNet object
   * @param inputHandler  The Dmn::Hal_Io object to receive inbound stringified
   *                      DMesgPb message
   * @param outputHandler The Dmn::Hal_Io object to send outbound stringified
   *                      DMesgPb message
   */
  Dmn_DMesgNet(std::string_view name,
               std::unique_ptr<Dmn_Io<std::string>> inputHandler = nullptr,
               std::unique_ptr<Dmn_Io<std::string>> outputHandler = nullptr);

  virtual ~Dmn_DMesgNet() noexcept;

  Dmn_DMesgNet(const Dmn_DMesgNet &obj) = delete;
  const Dmn_DMesgNet &operator=(const Dmn_DMesgNet &obj) = delete;
  Dmn_DMesgNet(Dmn_DMesgNet &&obj) = delete;
  Dmn_DMesgNet &operator=(Dmn_DMesgNet &&obj) = delete;

protected:
  /**
   * @brief The method reconciliate other node's DMesgPb with local node
   *        DMesgPb in regard about sys state, like master and list of neighbor
   *        nodes.
   *
   * @param dmesgPbOther The other node DMesgPb
   */
  void reconciliateDMesgPbSys(Dmn::DMesgPb dmesgPbOther);

private:
  /**
   * data members for constructor to instantiate the object.
   */
  std::string m_name{};
  std::unique_ptr<Dmn_Io<std::string>> m_inputHandler{};
  std::unique_ptr<Dmn_Io<std::string>> m_outputHandler{};

  /**
   * data members for internal logic.
   */
  std::unique_ptr<Dmn::Dmn_Proc> m_inputProc{};
  std::shared_ptr<Dmn_DMesgHandler> m_subscriptHandler{};
  std::shared_ptr<Dmn_DMesgHandler> m_sysHandler{};
  std::unique_ptr<Dmn::Dmn_Timer<std::chrono::nanoseconds>> m_timerProc{};

  Dmn::DMesgPb m_sys{};
  long long m_masterPendingCounter{};
  long long m_masterSyncPendingCounter{};
  struct timeval m_lastRemoteMasterTimestamp {};
  std::map<std::string, Dmn::DMesgPb> m_topicLastDMesgPb{};

  bool m_isMaster{};
  long long m_numberOfNeighbor{};
}; // class Dmn_DMesgNet

} // namespace Dmn

#endif // DMN_DMESGNET_HPP_
