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
#include <mutex>
#include <thread>

#include "platform/config/resdb_config.h"
#include "platform/consensus/ordering/raft/raft_log.h"
#include "platform/networkstrate/replica_communicator.h"
#include "platform/proto/raft_message.pb.h"
#include "platform/proto/resdb.pb.h"

namespace resdb {
namespace raft {

enum class RaftRole { FOLLOWER, CANDIDATE, LEADER };

// Manages Raft leader election: randomized election timers, RequestVote RPCs,
// and vote counting. Calls back into the consensus manager on role transitions.
class ElectionManager {
 public:
  ElectionManager(const ResDBConfig& config, RaftLog* log,
                  ReplicaCommunicator* rc,
                  std::function<void()> on_become_leader,
                  std::function<void(uint64_t term)> on_step_down);

  ~ElectionManager();

  void Start();
  void Stop();

  // Called by the commitment layer whenever a valid AppendEntries or heartbeat
  // arrives from the current leader. Resets the election timer.
  void ResetElectionTimer();

  // Called on receipt of a TYPE_RAFT_VOTE_REQUEST message.
  int ProcessVoteRequest(std::unique_ptr<Context> context,
                         std::unique_ptr<Request> request);

  // Called on receipt of a TYPE_RAFT_VOTE_RESPONSE message.
  int ProcessVoteResponse(std::unique_ptr<Context> context,
                          std::unique_ptr<Request> request);

  // If we receive any message with a higher term, step down immediately.
  // Returns true if we stepped down.
  bool MaybeStepDown(uint64_t term);

  RaftRole GetRole() const;
  uint64_t GetCurrentTerm() const;
  int32_t GetLeaderId() const;
  void SetLeaderId(int32_t id);

 private:
  void RunElectionLoop();
  void StartElection();
  void BecomeFollower(uint64_t term);
  void BecomeLeader();

  ResDBConfig config_;
  RaftLog* log_;
  ReplicaCommunicator* rc_;

  std::function<void()> on_become_leader_;
  std::function<void(uint64_t term)> on_step_down_;

  mutable std::mutex mu_;
  std::atomic<RaftRole> role_{RaftRole::FOLLOWER};
  std::atomic<uint64_t> current_term_{0};
  std::atomic<int32_t> voted_for_{-1};   // replica id voted for in current_term_
  std::atomic<int32_t> leader_id_{-1};
  int votes_received_ = 0;

  // Election timer state
  std::atomic<bool> timer_reset_{false};
  std::atomic<bool> stop_{false};
  std::thread election_thread_;
};

}  // namespace raft
}  // namespace resdb
