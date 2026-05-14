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
#include "platform/consensus/ordering/2pc/transaction_utils.h"

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
      stop_(false),
      replica_communicator_(replica_communicator), 
      system_info_(info) {
  executed_thread_ = std::thread(&Commitment::PostProcessExecutedMsg, this);
  global_stats_ = Stats::GetGlobalStats();
  duplicate_manager_ = std::make_unique<DuplicateManager>(config);

  global_stats_->SetProps(
      config_.GetSelfInfo().id(), config_.GetSelfInfo().ip(),
      config_.GetSelfInfo().port(), config_.GetConfigData().enable_resview(),
      config_.GetConfigData().enable_faulty_switch());
  global_stats_->SetPrimaryId(message_manager_->GetCurrentPrimary());
}

Commitment::~Commitment() {
  stop_ = true;
  if (executed_thread_.joinable()) {
    executed_thread_.join();
  }
}

// Send a prepare request to each replica 
int Commitment::ProcessNewRequest(std::unique_ptr<Context> context,
                                  std::unique_ptr<Request> user_request) {
  if (context == nullptr || context->signature.signature().empty()) {
    LOG(ERROR) << "user request doesn't contain signature, reject";
    return -2;
  }

  if (uint64_t seq =
          duplicate_manager_->CheckIfExecuted(user_request->hash())) {
    LOG(ERROR) << "This request is already executed with seq: " << seq;
    user_request->set_seq(seq);
    message_manager_->SendResponse(std::move(user_request));
    return -2;
  }


  global_stats_->IncClientRequest();
  if (duplicate_manager_->CheckAndAddProposed(user_request->hash())) {
    return -2;
  }

  auto seq = message_manager_->AssignNextSeq();

  // Artificially make the primary stop proposing new trasactions.

  if (!seq.ok()) {
    LOG(ERROR) << " seq fail";
    duplicate_manager_->EraseProposed(user_request->hash());
    global_stats_->SeqFail();
    Request request;
    request.set_type(Request::TYPE_RESPONSE);
    request.set_sender_id(config_.GetSelfInfo().id());
    request.set_sender_shard_id(config_.GetSelfShard());
    request.set_proxy_id(user_request->proxy_id());
    request.set_ret(-2);
    request.set_hash(user_request->hash());

    replica_communicator_->SendMessage(request, request.proxy_id());
    return -2;
  }

  global_stats_->RecordStateTime("request");
  auto req_cpy = NewRequest(Request::TYPE_NEW_TXNS, *user_request, user_request->sender_id());
  req_cpy->set_current_view(message_manager_->GetCurrentView());
  req_cpy->set_seq(*seq);

  CollectorResultCode ret = message_manager_->AddConsensusMsg(std::move(context), std::move(req_cpy));
  if (ret == CollectorResultCode::INVALID) { 
    return -2; 
  }

  user_request->set_type(Request::TYPE_2PC_PREPARE);
  user_request->set_current_view(message_manager_->GetCurrentView());
  user_request->set_seq(*seq);
  user_request->set_sender_id(config_.GetSelfInfo().id());
  user_request->set_sender_shard_id(config_.GetSelfShard()); 
  user_request->set_primary_id(config_.GetSelfInfo().id());

  std::cout << "[2PC] Commitment::ProcessNewRequest: Broadcasting prepare message to cross shard primaries" << std::endl;
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
      Request::TYPE_2PC_COMMIT, *request, config_.GetSelfInfo().id() 
  );

  CollectorResultCode ret =
      message_manager_->AddConsensusMsg(std::move(context), std::move(request));

  if (ret == CollectorResultCode::STATE_CHANGED) {
    // We have received all the commits we need. broadcast the global descision 
    // Add request to message_manager.
    std::cout << "[2PC] Commitment::ProcessVoteMsg: Broadcasting global decision to cross shard primaries" << std::endl;
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
      Request::TYPE_2PC_VOTE_COMMIT, *request, config_.GetSelfInfo().id());


  CollectorResultCode ret = message_manager_->AddConsensusMsg(std::move(context), std::move(request));
  if (ret == CollectorResultCode::INVALID) { 
    return ret; 
  }

  global_stats_->RecordStateTime("prepare");

  // Send the vote back to the coordinator 
  std::cout << "[2PC] Commitment::ProcessPrepareMSg: Sending vote to commit to " << sender << std::endl;
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
                                        *request, config_.GetSelfInfo().id());
  CollectorResultCode ret =
      message_manager_->AddConsensusMsg(std::move(context), std::move(request));

  if (ret == CollectorResultCode::STATE_CHANGED) {
    global_stats_->RecordStateTime("commit");
    std::cout << "[2PC] Commitment::ProcessCommitMsg: Sending commit ack message to " << sender << std::endl;
    replica_communicator_->SendMessage(*ack, sender);
    // Because leaders change, we must keep up on the current sequence 
    message_manager_->IncrementSequence();
  }
  return ret == CollectorResultCode::INVALID ? -2 : 0;
}

// =========== private threads ===========================
// If the transaction is executed, send back to the proxy.
int Commitment::PostProcessExecutedMsg() {
  while (!stop_) {
    auto batch_resp = message_manager_->GetResponseMsg();
    if (batch_resp == nullptr) {
      continue;
    }
    global_stats_->SendSummary();
    Request request;
    request.set_hash(batch_resp->hash());
    request.set_seq(batch_resp->seq());
    request.set_type(Request::TYPE_RESPONSE);
    request.set_sender_id(config_.GetSelfInfo().id());
    request.set_sender_shard_id(config_.GetSelfShard());
    request.set_current_view(batch_resp->current_view());
    request.set_proxy_id(batch_resp->proxy_id());
    request.set_primary_id(batch_resp->primary_id());
    LOG(ERROR) << "send back to proxy:" << batch_resp->proxy_id();
    batch_resp->SerializeToString(request.mutable_data());
    replica_communicator_->SendMessage(request, request.proxy_id());
  }
  return 0;
}

DuplicateManager* Commitment::GetDuplicateManager() {
  return duplicate_manager_.get();
}
}

}  // namespace resdb
