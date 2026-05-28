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

#include "platform/consensus/ordering/2pc/commitment.h"

#include <glog/logging.h>
#include <unistd.h>

#include "common/utils/utils.h"
#include "platform/consensus/ordering/common/transaction_utils.h"

namespace resdb {

namespace twopc
{
Commitment::Commitment(const ResDBConfig& config,
                       MessageManager* message_manager,
                       ReplicaCommunicator* replica_communicator, 
                        SignatureVerifier* verifier,  
                       SystemInfo* info) 
    : config_(config),
      verifier_(verifier),
      message_manager_(message_manager),
      replica_communicator_(replica_communicator), 
      system_info_(info) {
  global_stats_ = Stats::GetGlobalStats();
  duplicate_manager_ = std::make_unique<DuplicateManager>(config);

  global_stats_->SetProps(
      config_.GetSelfInfo().id(), config_.GetSelfInfo().ip(),
      config_.GetSelfInfo().port(), config_.GetConfigData().enable_resview(),
      config_.GetConfigData().enable_faulty_switch());
  global_stats_->SetPrimaryId(1);
}

Commitment::~Commitment() {}

// Send a prepare request to each replica 
int Commitment::ProcessNewRequest(std::unique_ptr<Context> context,
                                  std::unique_ptr<Request> user_request) {
  LOG(ERROR) << "[FUCK] Made it here 1";
  if (context == nullptr || context->signature.signature().empty()) {
    LOG(ERROR) << "user request doesn't contain signature, reject";
    return -2;
  }

  LOG(ERROR) << "[FUCK] Made it here 2";
  if (uint64_t seq =
          duplicate_manager_->CheckIfExecuted(user_request->hash())) {
    LOG(ERROR) << "This request is already executed with seq: " << seq;
    user_request->set_seq(seq);
    message_manager_->SendResponse(std::move(user_request));
    return -2;
  }

  LOG(ERROR) << "[FUCK] Made it here 3";
  global_stats_->IncClientRequest();
  if (duplicate_manager_->CheckAndAddProposed(user_request->hash())) {
    return -2;
  }

  LOG(ERROR) << "[FUCK] Made it here 4";

  global_stats_->RecordStateTime("request");
  auto req_cpy = NewRequest(Request::TYPE_NEW_TXNS, *user_request, user_request->sender_id(), user_request->sender_shard_id());
  req_cpy->set_current_view(message_manager_->GetCurrentView());

  LOG(ERROR) << "[FUCK] Made it here 5";

  CollectorResultCode ret = message_manager_->AddConsensusMsg(std::move(context), std::move(req_cpy));
  if (ret == CollectorResultCode::INVALID) { 
    return -2; 
  }

  LOG(ERROR) << "[FUCK] Made it here 6";

  user_request->set_type(Request::TYPE_2PC_PREPARE);
  user_request->set_current_view(message_manager_->GetCurrentView());
  user_request->set_sender_id(config_.GetSelfInfo().id());
  user_request->set_sender_shard_id(config_.GetSelfShard()); 
  user_request->set_primary_id(config_.GetSelfInfo().id());

  LOG(ERROR) << "[2PC] Broadcasting prepare message to cross shard primaries";

  replica_communicator_->SendMessageTo(*user_request, GetShardPrimaryIds());

  return 0;
}

int Commitment::ProcessCommitAckMsg(std::unique_ptr<Context> context, std::unique_ptr<Request> request) { 
  return 0; 
}

std::set<uint32_t> Commitment::GetShardPrimaryIds() { 
  return system_info_->GetAllShardPrimaryIds(); 
}


int Commitment::ProcessVoteMsg(std::unique_ptr<Context> context,
                                  std::unique_ptr<Request> request) {
        

  if (context == nullptr || context->signature.signature().empty()) {
    LOG(ERROR) << "user request doesn't contain signature, reject"
               << " context:" << (context == nullptr);
    return -2;
  }
  uint64_t seq = request->seq();

  std::unique_ptr<Request> global_decision = NewRequest(
      Request::TYPE_2PC_COMMIT, *request, config_.GetSelfInfo().id(), config_.GetSelfShard()
  );

  CollectorResultCode ret =
      message_manager_->AddConsensusMsg(std::move(context), std::move(request));

  if (ret == CollectorResultCode::STATE_CHANGED) {
    LOG(ERROR) << "[2PC] Broadcasting global decision to commit to cross shard primaries";
    replica_communicator_->SendMessageTo(*global_decision, GetShardPrimaryIds());
  }
  return ret == CollectorResultCode::INVALID ? -2 : 0;
} 

// Just send a vote to commit message as we are assuming no aborts 
int Commitment::ProcessPrepareMsg(std::unique_ptr<Context> context,
                                  std::unique_ptr<Request> request) {
  if (context == nullptr || context->signature.signature().empty()) {
    LOG(ERROR) << "user request doesn't contain signature, reject";
    return -2;
  }
  int64_t sender = request->sender_id(); 

  std::unique_ptr<Request> commit_vote = NewRequest(
      Request::TYPE_2PC_VOTE_COMMIT, *request, config_.GetSelfInfo().id(), config_.GetSelfShard());


  CollectorResultCode ret = message_manager_->AddConsensusMsg(std::move(context), std::move(request));
  if (ret == CollectorResultCode::INVALID) { 
    return ret; 
  }

  global_stats_->RecordStateTime("prepare");

  // Send the vote back to the coordinator 
  LOG(ERROR) << "[2PC] Sending vote to commit to " << sender;
  replica_communicator_->SendMessage(*commit_vote, sender);

  return 1; 

}

int Commitment::ProcessCommitMsg(std::unique_ptr<Context> context,
                                 std::unique_ptr<Request> request) {
  if (context == nullptr || context->signature.signature().empty()) {
    LOG(ERROR) << "user request doesn't contain signature, reject"
               << " context:" << (context == nullptr);
    return -2;
  }
  uint64_t seq = request->seq();

  int64_t sender = request->sender_id(); 

  std::unique_ptr<Request> ack = NewRequest(Request::TYPE_2PC_COMMIT_ACK, 
                                        *request, config_.GetSelfInfo().id(), config_.GetSelfShard());
  CollectorResultCode ret =
      message_manager_->AddConsensusMsg(std::move(context), std::move(request));

  if (ret == CollectorResultCode::STATE_CHANGED) {
    global_stats_->RecordStateTime("commit");
    LOG(ERROR) << "[2PC] Sending commit ack message to " << sender;
    replica_communicator_->SendMessage(*ack, sender);
  }
  return ret == CollectorResultCode::INVALID ? -2 : 0;
}

DuplicateManager* Commitment::GetDuplicateManager() {
  return duplicate_manager_.get();
}
}

}  // namespace resdb
