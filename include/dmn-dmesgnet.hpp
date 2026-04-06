/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-dmesgnet.hpp
 * @brief Dmn_DMesgNet — network-aware DMesg extension.
 *
 * This header defines Dmn_DMesgNet, an extension of Dmn_DMesg that serializes
 * DMesgPb messages for network I/O and deserializes inbound messages into the
 * local DMesg publish/subscribe flow.
 *
 * Responsibilities:
 * - Serialize and send DMesgPb objects to an output Dmn_Io<std::string>.
 * - Read serialized DMesgPb strings from an input Dmn_Io<std::string> and
 *   publish them locally to Dmn_DMesg subscribers.
 * - Maintain light-weight cluster membership and a cooperative master election
 *   protocol using periodic heartbeats.
 *
 * Design summary (master election and heartbeat):
 * - Each node periodically broadcasts a heartbeat that includes:
 *   - the node identifier (creation timestamp, process id, ip),
 *   - the node's current master identifier (if any),
 *   - the node's known neighbor list (including itself).
 *
 * - Node states (high-level):
 *   1. Initialized: node starts, sends heartbeats and waits to learn of a
 *      master from neighbors or time out and self-elect.
 *   2. MasterPending: node expects a master's heartbeat; receiving one moves
 *      the node to Ready; timeout may cause self-proclamation as master.
 *   3. Ready: normal operation where a node follows the elected master. If it
 *      receives a relinquish message from the master it may participate in a
 *      re-election.
 *   4. Destroyed: final state (optionally persist last state).
 *
 * - Election/co-election rules (summary):
 *   - When a master relinquishes or is absent, nodes choose a master by
 *     selecting the node with the earliest creation timestamp from their
 *     current neighbor list (including themselves).
 *   - If all nodes share the same neighbor list, they will elect the same
 *     master and converge immediately.
 *   - In race conditions where neighbor lists differ, nodes reconcile by
 *     exchanging heartbeats and converging toward the node with the earliest
 *     creation timestamp once they observe the same candidate.
 *
 * Examples (brief):
 * - If nodes A and B boot and exchange heartbeats, each records the other as a
 *   neighbor. After initialization timeouts, they will deterministically elect
 *   the node with the earlier creation time as master.
 * - When a new node C joins later, it learns the cluster through received
 *   heartbeats and follows the elected master.
 * - When the master shuts down, it sends a final heartbeat that relinquishes
 *   leadership; remaining nodes remove it from neighbor lists and re-elect.
 *
 * Implementation notes:
 * - Heartbeats are periodic (see DMN_DMESGNET_HEARTBEAT_IN_NS).
 * - Nodes prune neighbors when heartbeats are absent for a configurable period.
 * - The class reconciliates local DMesgPb state with remote DMesgPb messages
 *   to maintain consistent view of master and membership.
 */

#ifndef DMN_DMESGNET_HPP_
#define DMN_DMESGNET_HPP_

#include "dmn-debug.hpp"
#include "dmn-dmesg-pb-util.hpp"
#include "dmn-dmesg.hpp"
#include "dmn-io.hpp"
#include "dmn-timer.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <sys/time.h>
#include <unordered_map>

namespace dmn {

/** @brief Heartbeat period in nanoseconds (1 second). */
#define DMN_DMESGNET_HEARTBEAT_IN_NS (1000000000)

/** @brief Maximum number of consecutive heartbeat cycles to wait for a master
 *         acknowledgement before declaring self as master. */
#define DMN_DMESGNET_MASTERPENDING_MAX_COUNTER (3)

/** @brief Maximum number of consecutive heartbeat cycles to wait for a master
 *         synchronisation confirmation before triggering a re-election. */
#define DMN_DMESGNET_MASTERSYNC_MAX_COUNTER (5)

class Dmn_DMesgNet : public Dmn_DMesg {
public:
  /**
   * @brief Dmn_DMesgNet constructor.
   *
   * Create a network-aware DMesg instance that can read/write serialized
   * DMesgPb messages through Dmn_Io handlers.
   *
   * @param name           Identification name for this DMesgNet instance.
   * @param input_handler  Optional input handler to receive inbound serialized
   *                       DMesgPb messages (std::string). If nullptr, no
   *                       input processing is started.
   * @param output_handler Optional output handler to send outbound serialized
   *                       DMesgPb messages (std::string). If nullptr, no
   *                       network sends are performed.
   */
  explicit Dmn_DMesgNet(
      std::string_view name,
      std::shared_ptr<Dmn_Io<std::string>> input_handler = nullptr,
      std::shared_ptr<Dmn_Io<std::string>> output_handler = nullptr);

  virtual ~Dmn_DMesgNet() noexcept;

  Dmn_DMesgNet(const Dmn_DMesgNet &obj) = delete;
  Dmn_DMesgNet &operator=(const Dmn_DMesgNet &obj) = delete;
  Dmn_DMesgNet(Dmn_DMesgNet &&obj) = delete;
  Dmn_DMesgNet &operator=(Dmn_DMesgNet &&obj) = delete;

protected:
  /**
   * @brief Reconcile system-level information from a remote DMesgPb message.
   *
   * This method updates local view of the cluster (master id, neighbor list,
   * timestamps, etc.) based on information contained in dmesgpb_other. It is
   * responsible for applying election/co-election logic (for example, when a
   * remote node declares itself master or relinquishes mastership) and for
   * ensuring local state stays consistent with observed remote state.
   *
   * @param dmesgpb_other DMesgPb message received from a remote node.
   */
  void reconciliateDMesgPbSys(const dmn::DMesgPb &dmesgpb_other);

  /**
   * @brief Return the internal per-topic last-published message cache.
   *
   * Overrides @c Dmn_DMesg::getLastTopicCacheInternal() to return the
   * network layer's own cache (@c m_topic_last_dmesgpb) rather than the
   * base-class cache, so that heartbeat and reconciliation logic use the
   * correct view.
   *
   * @return Reference to the network layer's topic-to-last-message map.
   */
  auto getLastTopicCacheInternal()
      -> std::unordered_map<std::string, dmn::DMesgPb> & override;

private:
  /** @brief Create and start the background thread that reads from
   *         @c m_input_handler and republishes inbound messages locally. */
  void createInputHandlerProc();

  /** @brief Register the internal publish-subscriber handler that forwards
   *         outbound local messages to @c m_output_handler. */
  void createSubscriptHandler();

  /** @brief Create and start the periodic heartbeat timer thread. */
  void createTimerProc();

  /** @brief Drain and send any messages queued in the outbound pending
   *         buffer (@c m_topic_outbound_pending_dmesgpb). */
  void sendPendingOutboundQueueMessage();

  /**
   * Constructor-initialized members.
   */
  std::string m_name{};                                    ///< Instance name.
  std::shared_ptr<Dmn_Io<std::string>> m_input_handler{}; ///< Inbound I/O handler.
  std::shared_ptr<Dmn_Io<std::string>> m_output_handler{}; ///< Outbound I/O handler.

  /**
   * Internal runtime state and helper objects.
   */
  std::unique_ptr<dmn::Dmn_Proc> m_input_proc{};       ///< Thread reading from @c m_input_handler.
  Dmn_DMesg::HandlerType m_subscript_handler{};         ///< Subscriber handler for local messages.
  Dmn_DMesg::HandlerType m_write_handler{};             ///< Handler for write operations.
  Dmn_DMesg::HandlerType m_sys_handler{};               ///< Handler for system (heartbeat) messages.
  std::unique_ptr<dmn::Dmn_Timer<std::chrono::nanoseconds>> m_timer_proc{}; ///< Heartbeat timer.

  dmn::DMesgPb m_sys{};                    ///< Local system DMesgPb (node identity, neighbor list).
  long long m_master_pending_counter{};    ///< Cycles spent waiting for a master acknowledgement.
  long long m_master_sync_pending_counter{}; ///< Cycles spent waiting for master synchronisation.
  struct timeval m_last_remote_master_timestamp{}; ///< Timestamp of the last received master heartbeat.
  std::unordered_map<std::string, dmn::DMesgPb> m_topic_last_dmesgpb{};           ///< Last published message per topic.
  std::unordered_map<std::string, std::vector<dmn::DMesgPb>>
      m_topic_outbound_pending_dmesgpb{}; ///< Outbound messages waiting to be sent.

  std::atomic_flag m_ready{}; ///< Set once master election has completed and this node is ready.

  bool m_is_master{};              ///< True when this node is the elected master.
  long long m_number_of_neighbor{}; ///< Number of known neighbor nodes (excluding self).

  std::atomic<bool> m_shutdown{}; ///< True once shutdown has been requested.
}; // class Dmn_DMesgNet

} // namespace dmn

#endif // DMN_DMESGNET_HPP_
