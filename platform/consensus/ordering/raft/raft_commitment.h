/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

#include "executor/common/transaction_manager.h"
#include "platform/config/resdb_config.h"
#include "platform/consensus/execution/system_info.h"
#include "platform/consensus/execution/transaction_executor.h"
#include "platform/consensus/ordering/raft/election_manager.h"
#include "platform/consensus/ordering/raft/raft_log.h"
#include "platform/networkstrate/replica_communicator.h"
#include "platform/proto/raft_message.pb.h"
#include "platform/proto/resdb.pb.h"
#include "platform/statistic/stats.h"

namespace resdb {
namespace raft {

// Implements the Raft AppendEntries protocol (log replication and heartbeats).
//
// On the leader side:
//   - ProcessNewRequest:           receives a new client batch (TYPE_NEW_TXNS)
//   - ProcessAppendEntriesResponse: counts ACKs, advances commit_index
//   - SendHeartbeats:              periodic empty AppendEntries to followers
//
// On the follower side:
//   - ProcessAppendEntries:        validates and appends log entries, replies
class RaftCommitment {
 public:
  RaftCommitment(const ResDBConfig& config, RaftLog* log,
                 ElectionManager* election_mgr, ReplicaCommunicator* rc,
                 SystemInfo* system_info, SignatureVerifier* verifier,
                 std::unique_ptr<TransactionManager> txn_manager);
  ~RaftCommitment();

  // TYPE_NEW_TXNS — leader receives a batched client request from a proxy.
  int ProcessNewRequest(std::unique_ptr<Context> context,
                        std::unique_ptr<Request> request);

  // TYPE_RAFT_APPEND_ENTRIES — follower receives AppendEntries (or heartbeat).
  int ProcessAppendEntries(std::unique_ptr<Context> context,
                           std::unique_ptr<Request> request);

  // TYPE_RAFT_APPEND_ENTRIES_RESPONSE — leader counts replication ACKs.
  int ProcessAppendEntriesResponse(std::unique_ptr<Context> context,
                                   std::unique_ptr<Request> request);

  // Called when we become leader: initialise per-peer tracking state.
  void OnBecomeLeader();

  // Called when we step down: cancel pending heartbeat work.
  void OnStepDown();

 private:
  void SendAppendEntries(int32_t peer_id);
  void SendHeartbeats();
  void HeartbeatLoop();

  // Advance commit_index to the highest index replicated on a majority.
  void MaybeAdvanceCommitIndex();

  // Apply log entries up to commit_index to the transaction executor.
  void ApplyCommitted();

  // Background thread that drains the executor response queue and sends
  // TYPE_RESPONSE to the originating proxy.
  void ResponseLoop();

  ResDBConfig config_;
  RaftLog* log_;
  ElectionManager* election_mgr_;
  ReplicaCommunicator* rc_;
  SystemInfo* system_info_;
  SignatureVerifier* verifier_;

  std::atomic<uint64_t> commit_index_{0};
  std::atomic<uint64_t> last_applied_{0};

  // Leader-side per-peer state (guarded by leader_mu_)
  std::mutex leader_mu_;
  std::map<int32_t, uint64_t> next_index_;   // peer → next log index to send
  std::map<int32_t, uint64_t> match_index_;  // peer → highest replicated index

  std::unique_ptr<TransactionExecutor> executor_;

  std::atomic<bool> stop_{false};
  std::thread heartbeat_thread_;
  std::thread response_thread_;

  Stats* global_stats_;
};

}  // namespace raft
}  // namespace resdb
