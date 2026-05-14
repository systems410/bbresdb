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
#include "platform/consensus/ordering/2pc/checkpoint_manager.h"
#include "platform/consensus/ordering/2pc/commitment.h"
#include "platform/consensus/ordering/2pc/message_manager.h"
#include "platform/consensus/ordering/2pc/performance_manager.h"
#include "platform/consensus/ordering/2pc/query.h"
#include "platform/consensus/ordering/2pc/response_manager.h"
#include "platform/consensus/ordering/2pc/viewchange_manager.h"
#include "platform/consensus/recovery/recovery.h"
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
                                            replica_communicator_, system_info_, std::move(query_executor))), 
    message_manager_(std::make_unique<MessageManager>(
        config, system_info_, replica_cm_.get())),
    commitment_(std::make_unique<Commitment>(config_, message_manager_.get(),
                                            replica_communicator_, system_info_)),
    response_manager_(config_.IsPerformanceRunning()
                          ? nullptr
                          : std::make_unique<ResponseManager>(
                                config_, replica_communicator_,
                                system_info_, GetSignatureVerifier())),
    performance_manager_(config_.IsPerformanceRunning()
                            ? std::make_unique<PerformanceManager>(
                                    config_, replica_communicator_,
                                    system_info_, GetSignatureVerifier())
                            : nullptr) {
      LOG(INFO) << "is running is performance mode:"
                  << config_.IsPerformanceRunning();
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


  uint32_t GetPrimary() override {
    return system_info_->GetPrimaryId();
  }

  uint32_t GetVersion() override {
    return system_info_->GetCurrentView();
  }

  void SetPrimary(uint32_t primary) {
    system_info_->SetPrimary(primary);
  }

  std::vector<ReplicaInfo> GetReplicas() override {
    return config_.GetAllReplicas(); 
  }

  void Start() override {
    LOG(ERROR) << " ======= start";
    ConsensusManager::Start();
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

      case Request::TYPE_CLIENT_REQUEST:
        std::cout << "[2PC] ConsensusManager2PC::InternalConsensusCommit: Received client request" << std::endl;
        if (config_.IsPerformanceRunning()) {
          return performance_manager_->StartEval();
        }
        return response_manager_->NewUserRequest(std::move(context),
                                                std::move(request));

      // Received by the coordinator. Send the prepare messages to the replicas to get their vote 
      case Request::TYPE_2PC_NEW_TXNS: {
        std::cout << "[2PC] ConsensusManager2PC::InternalConsensusCommit: Received txns message" << std::endl;
        uint64_t proxy_id = request->proxy_id();
        std::string hash = request->hash();
        int ret = commitment_->ProcessNewRequest(std::move(context),
                                                std::move(request));
        if (ret == -3) {
          LOG(ERROR) << "TYPE BAD RETURN";
        }
        return ret;
      }

      // Received by the coordinator, used to count up the number of votes 
      case Request::TYPE_2PC_VOTE_COMMIT: 
        std::cout << "[2PC] ConsensusManager2PC::InternalConsensusCommit: Received vote to commit message" << std::endl;
        return commitment_->ProcessVoteMsg(std::move(context), 
                                          std::move(request));
      

      // Received by all participants. This is where they will respond with their vote 
      case Request::TYPE_2PC_PREPARE:
        std::cout << "[2PC] ConsensusManager2PC::InternalConsensusCommit: Received prepare message" << std::endl;
        return commitment_->ProcessPrepareMsg(std::move(context),
                                              std::move(request));

      // Received by all participants. This is the global descision to commit 
      case Request::TYPE_2PC_COMMIT:
        std::cout << "[2PC] ConsensusManager2PC::InternalConsensusCommit: Received commit message" << std::endl;
        return commitment_->ProcessCommitMsg(std::move(context),
                                            std::move(request));

      case Request::TYPE_2PC_COMMIT_ACK: 
        std::cout << "[2PC] ConsensusManager2PC::InternalConsensusCommit: Received commit ack message" << std::endl;
        return commitment_->ProcessCommitAckMsg(std::move(context), 
                                                std::move(request));

      default: 
        return replica_cm_->ConsensusCommit(std::move(context),
                                            std::move(request));
    }
    return 0;
  }

  void SetupPerformanceDataFunc(
      std::function<std::string()> func) {
    performance_manager_->SetDataFunc(func);
  }


 protected:

  std::unique_ptr<ReplicaCM> replica_cm_; 
  std::unique_ptr<MessageManager> message_manager_;
  std::unique_ptr<Commitment> commitment_;
  std::unique_ptr<ResponseManager> response_manager_;
  std::unique_ptr<PerformanceManager> performance_manager_;
  Stats* global_stats_;

};

} // namespace 2pc
}  // namespace resdb
