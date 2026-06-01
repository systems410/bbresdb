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

#include "executor/common/custom_query.h"
#include "platform/config/resdb_config.h"
#include "platform/consensus/ordering/pbft/response_manager.h"
#include "platform/consensus/ordering/raft/election_manager.h"
#include "platform/consensus/ordering/raft/raft_commitment.h"
#include "platform/consensus/ordering/raft/raft_log.h"
#include "platform/networkstrate/consensus_manager.h"

namespace resdb {
namespace raft {

// Raft-based within-shard consensus manager.
//
// Replaces ConsensusManagerPBFT as the ReplicaCM template argument for
// ShardedConsensusManager2PC.  The external interface is identical:
//
//   ConsensusManagerRaft(config, executor, resources, query_executor)
//
// Internally it runs Raft (AppendEntries replication + leader election)
// instead of PBFT's three-phase commit.  No failure recovery, checkpointing,
// or view-change logic is included; only the happy-path is implemented.
class ConsensusManagerRaft : public ConsensusManager {
 public:
  ConsensusManagerRaft(const ResDBConfig& config,
                       std::unique_ptr<TransactionManager> executor,
                       const CrossProtocolResources& resources,
                       std::unique_ptr<CustomQuery> query_executor = nullptr);

  // Standalone constructor (for use outside 2PC).
  ConsensusManagerRaft(const ResDBConfig& config,
                       std::unique_ptr<TransactionManager> executor,
                       std::unique_ptr<CustomQuery> query_executor = nullptr);

  virtual ~ConsensusManagerRaft() = default;

  int ConsensusCommit(std::unique_ptr<Context> context,
                      std::unique_ptr<Request> request) override;

  std::vector<ReplicaInfo> GetReplicas() override;
  uint32_t GetPrimary() override;
  uint32_t GetVersion() override;
  void SetPrimary(uint32_t primary, uint64_t version) override;

  void Start() override;

  // Called by ShardedConsensusManager2PC to wire in performance data.
  void SetupPerformanceDataFunc(std::function<std::string()> func);

 private:
  int InternalConsensusCommit(std::unique_ptr<Context> context,
                              std::unique_ptr<Request> request);

  std::unique_ptr<RaftLog> log_;
  std::unique_ptr<ElectionManager> election_mgr_;
  std::unique_ptr<RaftCommitment> commitment_;
  std::unique_ptr<ResponseManager> response_manager_;
};

}  // namespace raft
}  // namespace resdb
