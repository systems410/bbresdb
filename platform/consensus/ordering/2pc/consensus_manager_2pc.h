/*
 * ;Licensed to the Apache Software Foundation (ASF) under one
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
#include "platform/consensus/ordering/2pc/commitment.h"
#include "platform/consensus/ordering/2pc/message_manager.h"
#include "platform/networkstrate/consensus_manager.h"

namespace resdb {

namespace twopc {

template <typename ReplicaCM>  
class ShardedConsensusManager2PC : public ConsensusManager {
 public:

  ShardedConsensusManager2PC(const ResDBConfig& config, std::unique_ptr<TransactionManager> executor, 
                             std::unique_ptr<CustomQuery> query_executor = nullptr) 
  : ConsensusManager(config),
    replica_cm_(std::make_unique<ReplicaCM>(config, std::move(executor),  
                                            CrossProtocolResources{ 
                                              .rc = replica_communicator_, 
                                              .system_info = system_info_, 
                                              .verifier = verifier_
                                            },
                                            std::move(query_executor))), 
    message_manager_(std::make_unique<MessageManager>(
        config, system_info_, replica_cm_.get())),
    commitment_(std::make_unique<Commitment>(config_, message_manager_.get(),
                                            replica_communicator_, GetSignatureVerifier(),
                                            system_info_)) {
      global_stats_ = Stats::GetGlobalStats();
    }

  virtual ~ShardedConsensusManager2PC() = default;

  int ConsensusCommit(std::unique_ptr<Context> context,
                      std::unique_ptr<Request> request) override {
    LOG(INFO) << "recv impl type:" << request->type() << " "
              << "sender id:" << request->sender_id()
              << " primary:" << system_info_->GetPrimaryId();

    int ret = InternalConsensusCommit(std::move(context), std::move(request));
    return ret;
  }

  void SetupPerformanceDataFunc(std::function<std::string()> func) { 
    replica_cm_->SetupPerformanceDataFunc(std::move(func));
  }

  uint32_t GetPrimary() override {
    return system_info_->GetCrossShardPrimaryId();
  }

  uint32_t GetVersion() override {
    return system_info_->GetCurrentView();
  }

  void SetPrimary(uint32_t primary) {
    system_info_->SetCrossShardPrimaryId(primary);
  }

  std::vector<ReplicaInfo> GetReplicas() override {
    return config_.GetAllReplicas(); 
  }

 protected:

  int InternalConsensusCommit(
      std::unique_ptr<Context> context, std::unique_ptr<Request> request) {
    LOG(ERROR) << "recv impl type:" << request->type() << " "
              << "sender id:" << request->sender_id()
              << " seq:" << request->seq()
              << " primary:" << system_info_->GetPrimaryId()
              << " is convery:" << request->is_recovery();

    switch (request->type()) {

      case Request::TYPE_NEW_TXNS: {
      LOG(ERROR) << "[2PC] Received new txns from " 
                 << request->sender_id() << " with shard id " << request->sender_shard_id();
        system_info_->SetCrossShardPrimaryId(config_.GetSelfInfo().id());
        int ret = commitment_->ProcessNewRequest(std::move(context),
                                                 std::move(request));
        if (ret == -3) {
          LOG(ERROR) << "TYPE BAD RETURN";
        }
        return ret;
      }

      // Received by the coordinator, used to count up the number of votes 
      case Request::TYPE_2PC_VOTE_COMMIT: 
      LOG(ERROR) << "[2PC] Received commit vote from " 
                 << request->sender_id() << " with shard id " << request->sender_shard_id();
        return commitment_->ProcessVoteMsg(std::move(context), 
                                           std::move(request));
      

      // Received by all participants. This is where they will respond with their vote 
      case Request::TYPE_2PC_PREPARE:
        LOG(ERROR) << "[2PC] Received prepare from " 
                   << request->sender_id() << " with shard id " << request->sender_shard_id();
        system_info_->SetCrossShardPrimaryId(request->sender_id());
        return commitment_->ProcessPrepareMsg(std::move(context),
                                              std::move(request));

      // Received by all participants. This is the global descision to commit 
      case Request::TYPE_2PC_COMMIT:
        LOG(ERROR) << "[2PC] Received commmit from " 
                   << request->sender_id() << " with shard id " << request->sender_shard_id();
        return commitment_->ProcessCommitMsg(std::move(context),
                                             std::move(request));

      case Request::TYPE_2PC_COMMIT_ACK: 
        LOG(ERROR) << "[2PC] Received commmit ack from " 
                   << request->sender_id() << " with shard id " << request->sender_shard_id();
        return commitment_->ProcessCommitAckMsg(std::move(context), 
                                                std::move(request));

      default: 
        return replica_cm_->ConsensusCommit(std::move(context),
                                            std::move(request));
    }
    return 0;
  }



 protected:

  std::unique_ptr<ReplicaCM> replica_cm_; 
  std::unique_ptr<MessageManager> message_manager_;
  std::unique_ptr<Commitment> commitment_;
  Stats* global_stats_;

};

} // namespace 2pc
}  // namespace resdb
