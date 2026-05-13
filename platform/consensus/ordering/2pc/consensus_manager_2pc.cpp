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

#include "platform/consensus/ordering/2pc/consensus_manager_2pc.h"

#include <glog/logging.h>
#include <unistd.h>

#include "common/crypto/signature_verifier.h"

namespace resdb {

namespace twopc {
  
ConsensusManager2PC::ConsensusManager2PC(const ResDBConfig& config, ReplicaCommunicator* communicator, 
                                         SystemInfo* info) 
    : ConsensusManager(config, true),
      system_info_(info),
      message_manager_(std::make_unique<MessageManager>(
          config, system_info_)),
      commitment_(std::make_unique<Commitment>(config_, message_manager_.get(),
                                               communicator, system_info_)),
      performance_manager_(config_.IsPerformanceRunning()
                               ? std::make_unique<PerformanceManager>(
                                     config_, GetBroadCastClient(),
                                     system_info_, GetSignatureVerifier())
                               : nullptr) {
  LOG(INFO) << "is running is performance mode:"
            << config_.IsPerformanceRunning();
  global_stats_ = Stats::GetGlobalStats();
}

void ConsensusManager2PC::SetCommitCallback(std::function<void(std::unique_ptr<Request>, std::unique_ptr<Context>)> commit_callback) { 
  message_manager_->SetConsensusCallback(std::move(commit_callback));
}

void ConsensusManager2PC::Start() {
  LOG(ERROR) << " ======= start";
  ConsensusManager::Start();
}

std::vector<ReplicaInfo> ConsensusManager2PC::GetReplicas() {
  return message_manager_->GetReplicas();
}

uint32_t ConsensusManager2PC::GetPrimary() {
  return system_info_->GetPrimaryId();
}

uint32_t ConsensusManager2PC::GetVersion() {
  return system_info_->GetCurrentView();
}

void ConsensusManager2PC::SetPrimary(uint32_t primary) {
  system_info_->SetPrimary(primary);
}

int ConsensusManager2PC::ConsensusCommit(std::unique_ptr<Context> context,
                                          std::unique_ptr<Request> request) {
  LOG(INFO) << "recv impl type:" << request->type() << " "
            << "sender id:" << request->sender_id()
            << " primary:" << system_info_->GetPrimaryId();

  int ret = InternalConsensusCommit(std::move(context), std::move(request));
  return ret;
}

int ConsensusManager2PC::InternalConsensusCommit(
    std::unique_ptr<Context> context, std::unique_ptr<Request> request) {
  LOG(ERROR) << "recv impl type:" << request->type() << " "
             << "sender id:" << request->sender_id()
             << " seq:" << request->seq()
             << " primary:" << system_info_->GetPrimaryId()
             << " is convery:" << request->is_recovery();

  switch (request->type()) {
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
  }
  return 0;
}

void ConsensusManager2PC::SetupPerformanceDataFunc(
    std::function<std::string()> func) {
  performance_manager_->SetDataFunc(func);
}


}// namespace 2pc 

}  // namespace resdb
