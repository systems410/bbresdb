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

#include "platform/consensus/ordering/paxos/consensus_manager_paxos.h"

#include <glog/logging.h>
#include <unistd.h>

#include "common/crypto/signature_verifier.h"

namespace resdb {

namespace paxos {

ConsensusManagerPaxos::ConsensusManagerPaxos(
    const ResDBConfig& config, std::unique_ptr<TransactionManager> executor,
    const CrossProtocolResources& resources, std::unique_ptr<CustomQuery> query_executor) 

    : ConsensusManager(config, resources),
      message_manager_(std::make_unique<MessageManager>(
          config, std::move(executor), system_info_)),
      commitment_(std::make_unique<Commitment>(config_, message_manager_.get(),
                                               replica_communicator_,
                                               GetSignatureVerifier(), system_info_)),
      response_manager_(config_.IsPerformanceRunning()
                            ? nullptr
                            : std::make_unique<ResponseManager>(
                                  config_, replica_communicator_,
                                  system_info_, GetSignatureVerifier())),
      performance_manager_(config_.IsPerformanceRunning()
                               ? std::make_unique<PerformanceManager>(
                                     config_, replica_communicator_,
                                     system_info_, GetSignatureVerifier())
                               : nullptr),
      query_(std::make_unique<Query>(config_, std::move(query_executor))) {
  LOG(INFO) << "is running is performance mode:"
            << config_.IsPerformanceRunning();
  global_stats_ = Stats::GetGlobalStats();

} 

ConsensusManagerPaxos::ConsensusManagerPaxos(
    const ResDBConfig& config, std::unique_ptr<TransactionManager> executor,
    std::unique_ptr<CustomQuery> query_executor)
    : ConsensusManagerPaxos(config, std::move(executor), CrossProtocolResources(), std::move(query_executor)) {}

void ConsensusManagerPaxos::SetNeedCommitQC(bool need_qc) {
  commitment_->SetNeedCommitQC(need_qc);
}

void ConsensusManagerPaxos::Start() {
  LOG(ERROR) << " ======= start";
  ConsensusManager::Start();
}

std::vector<ReplicaInfo> ConsensusManagerPaxos::GetReplicas() {
  return message_manager_->GetReplicas();
}

uint32_t ConsensusManagerPaxos::GetPrimary() {
  return system_info_->GetPrimaryId();
}

uint32_t ConsensusManagerPaxos::GetVersion() {
  return system_info_->GetCurrentView();
}

void ConsensusManagerPaxos::SetPrimary(uint32_t primary, uint64_t version) {
  system_info_->SetPrimary(primary);
}


// The implementation of Paxos.
int ConsensusManagerPaxos::ConsensusCommit(std::unique_ptr<Context> context,
                                          std::unique_ptr<Request> request) {
  LOG(INFO) << "recv impl type:" << request->type() << " "
            << "sender id:" << request->sender_id()
            << " primary:" << system_info_->GetPrimaryId();
  int ret = InternalConsensusCommit(std::move(context), std::move(request));
  return ret;
}

int ConsensusManagerPaxos::InternalConsensusCommit(
    std::unique_ptr<Context> context, std::unique_ptr<Request> request) {
  LOG(ERROR) << "recv impl type:" << request->type() << " "
             << "sender id:" << request->sender_id()
             << " seq:" << request->seq()
             << " primary:" << system_info_->GetPrimaryId();

  switch (request->type()) {
    case Request::TYPE_CLIENT_REQUEST:
      LOG(ERROR) << "[PAXOS] Received client request from " 
                 << request->sender_id();
      if (config_.IsPerformanceRunning()) {
        return performance_manager_->StartEval();
      }
      return response_manager_->NewUserRequest(std::move(context),
                                               std::move(request));
    case Request::TYPE_RESPONSE:
      LOG(ERROR) << "[PAXOS] Received response from " 
                 << request->sender_id() << " with shard id " << request->sender_shard_id();
      if (config_.IsPerformanceRunning()) {
        return performance_manager_->ProcessResponseMsg(std::move(context),
                                                        std::move(request));
      }
      return response_manager_->ProcessResponseMsg(std::move(context),
                                                   std::move(request));
    case Request::TYPE_NEW_TXNS: {
      LOG(ERROR) << "[PAXOS] Received new txns from " 
                 << request->sender_id() << " with shard id " << request->sender_shard_id();
      uint64_t proxy_id = request->proxy_id();
      std::string hash = request->hash();
      int ret = commitment_->ProcessNewRequest(std::move(context),
                                               std::move(request));
      if (ret == -3) {
        LOG(ERROR) << "BAD RETURN";
      }
      return ret;
    }

    case Request::TYPE_PAXOS_ACCEPT_REQUEST:
      LOG(ERROR) << "[PAXOS] Received accept request from " 
                 << request->sender_id() << " with shard id " << request->sender_shard_id();
      return commitment_->ProcessAcceptRequestMsg(std::move(context),
                                                  std::move(request));

    case Request::TYPE_PAXOS_ACCEPT:
      LOG(ERROR) << "[PAXOS] Received accept from " 
                 << request->sender_id() << " with shard id " << request->sender_shard_id();
      return commitment_->ProcessAcceptMsg(std::move(context),
                                           std::move(request));

    case Request::TYPE_PAXOS_LEARN: 
      return commitment_->ProcessCommitMsg(std::move(context), 
                                           std::move(request));


    case Request::TYPE_REPLICA_STATE:
      return query_->ProcessGetReplicaState(std::move(context),
                                            std::move(request));
    case Request::TYPE_CUSTOM_QUERY:
      return query_->ProcessCustomQuery(std::move(context), std::move(request));

  }
  return 0;
}

void ConsensusManagerPaxos::SetupPerformanceDataFunc(
    std::function<std::string()> func) {
  performance_manager_->SetDataFunc(func);
}

void ConsensusManagerPaxos::SetPreVerifyFunc(
    std::function<bool(const Request&)> func) {
  commitment_->SetPreVerifyFunc(func);
}


} // namespace paxos
}  // namespace resdb
