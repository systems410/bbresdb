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

#include "platform/consensus/ordering/paxos/message_manager.h"

#include <glog/logging.h>

#include "common/utils/utils.h"

namespace resdb {
namespace paxos {

MessageManager::MessageManager(
    const ResDBConfig& config,
    std::unique_ptr<TransactionManager> transaction_manager, 
    SystemInfo* system_info)
    : config_(config),
      queue_("executed"),
      system_info_(system_info),
      transaction_executor_(std::make_unique<TransactionExecutor>(
          config,
          [&](std::unique_ptr<Request> request,
              std::unique_ptr<BatchUserResponse> resp_msg) {
            resp_msg->set_proxy_id(request->proxy_id());
            resp_msg->set_seq(request->seq());
            resp_msg->set_current_view(request->current_view());
            resp_msg->set_primary_id(GetCurrentPrimary());
            if (transaction_executor_->NeedResponse() &&
                resp_msg->proxy_id() != 0) {
              queue_.Push(std::move(resp_msg));
            }
          },
          system_info_, std::move(transaction_manager))),
      collector_pool_(std::make_unique<LockFreeCollectorPool>(
          "txn", config_.GetMaxProcessTxn(), transaction_executor_.get(),
          config_.GetConfigData().enable_viewchange())) {
  global_stats_ = Stats::GetGlobalStats();
  transaction_executor_->SetSeqUpdateNotifyFunc(
      [&](uint64_t seq) { collector_pool_->Update(seq - 1); });
}

MessageManager::~MessageManager() {
  if (transaction_executor_) {
    transaction_executor_->Stop();
  }
}

std::unique_ptr<BatchUserResponse> MessageManager::GetResponseMsg() {
  return queue_.Pop();
}

int64_t MessageManager::GetCurrentPrimary() const {
  return system_info_->GetPrimaryId();
}

uint64_t MessageManager ::GetCurrentView() const {
  return system_info_->GetCurrentView();
}

void MessageManager::SetNextSeq(uint64_t seq) {
  LOG(ERROR) << "set next old seq:" << next_seq_;
  next_seq_ = seq;
  LOG(ERROR) << "set next seq:" << next_seq_;
}

int64_t MessageManager::GetNextSeq() { return next_seq_; }

absl::StatusOr<uint64_t> MessageManager::AssignNextSeq() {
  std::unique_lock<std::mutex> lk(seq_mutex_);
  uint32_t max_executed_seq = transaction_executor_->GetMaxPendingExecutedSeq();
  global_stats_->SeqGap(next_seq_ - max_executed_seq);
  if (next_seq_ - max_executed_seq >
      static_cast<uint64_t>(config_.GetMaxProcessTxn())) {
    // LOG(ERROR) << "next_seq_: " << next_seq_ << " max_executed_seq: " <<
    // max_executed_seq;
    return absl::InvalidArgumentError("Seq has been used up.");
  }
  return next_seq_++;
}

std::vector<ReplicaInfo> MessageManager::GetReplicas() {
  return system_info_->GetReplicas();
}

// Check if the request is valid.
// 1. view is the same as the current view
// 2. seq is larger or equal than the next execute seq.
// 3. inside the water mark.
bool MessageManager::IsValidMsg(const Request& request) {
  if (request.type() == Request::TYPE_RESPONSE) {
    return true;
  }
  // view should be the same as the current one.
  if (static_cast<uint64_t>(request.current_view()) != GetCurrentView()) {
    LOG(ERROR) << "message view :[" << request.current_view()
               << "] is older than the cur view :[" << GetCurrentView() << "]";
    return false;
  }

  if (static_cast<uint64_t>(request.seq()) <
      transaction_executor_->GetMaxPendingExecutedSeq()) {
    return false;
  }

  return true;
}

Request* MessageManager::GetPromisedRequest(uint64_t seq) { 
  return collector_pool_->GetCollector(seq)->GetMainRequest(); 
}

bool MessageManager::MayConsensusChangeStatus(
    int type, int received_count, TransactionCollector::PaxosStatus& status,
    bool has_promised_higher) {

  if (*status.learner != TransactionStatue::None) { 
    return false;
  }
  switch (type) {
    case Request::TYPE_PAXOS_ACCEPT_REQUEST:
      if (*status.acceptor == TransactionStatue::None) {
        TransactionStatue old_status = TransactionStatue::None;
        return status.acceptor->compare_exchange_strong(
            old_status, TransactionStatue::ACCEPTED,
            std::memory_order_acq_rel, std::memory_order_acq_rel);
      }  
      if (*status.acceptor == TransactionStatue::ACCEPTED) {
        TransactionStatue old_status = TransactionStatue::ACCEPTED;
        return status.acceptor->compare_exchange_strong(
            old_status, TransactionStatue::ACCEPTED,
            std::memory_order_acq_rel, std::memory_order_acq_rel);
      }  
      break;
    // Received by the learner 
    case Request::TYPE_PAXOS_ACCEPT: 
      LOG(ERROR) << "[PAXOS] Received accept, have " << received_count << " need " << config_.GetMinDataReceiveNum(); 
      if (*status.proposer == TransactionStatue::None
        && config_.GetMinDataReceiveNum() <= received_count) {
        TransactionStatue old_status = TransactionStatue::None;
        return status.learner->compare_exchange_strong(
            old_status, TransactionStatue::READY_EXECUTE,
            std::memory_order_acq_rel, std::memory_order_acq_rel);
      }
      break;
    case Request::TYPE_PAXOS_LEARN: 
      LOG(ERROR) << "[PAXOS] Received accept, have " << received_count << " need " << config_.GetMinDataReceiveNum(); 
      if (*status.learner == TransactionStatue::None 
        && config_.GetMinDataReceiveNum() <= received_count) {
        TransactionStatue old_status = TransactionStatue::None;
        return status.learner->compare_exchange_strong(
            old_status, TransactionStatue::READY_EXECUTE,
            std::memory_order_acq_rel, std::memory_order_acq_rel);
      }
      break;
  }
  return false;
}

// Add commit messages and return the number of messages have been received.
// The commit messages only include post(pre-prepare), prepare and commit
// messages. Messages are handled by state (PREPARE,COMMIT,READY_EXECUTE).

// If there are enough messages and the state is changed after adding the
// message, return 1, otherwise return 0. Return -2 if the request is not valid.
CollectorResultCode MessageManager::AddConsensusMsg(
    const SignatureInfo& signature, std::unique_ptr<Request> request) {
  if (request == nullptr || !IsValidMsg(*request)) {
    LOG(ERROR) << " msg not invalid";
    return CollectorResultCode::INVALID;
  }

  int type = request->type();
  uint64_t seq = request->seq();
  int resp_received_count = 0;
  int proxy_id = request->proxy_id();
  // Main request only updated if nothing else has been accepted 
  bool has_accepted = collector_pool_->GetCollector(seq)->HasAccepted(); 
  int ret = collector_pool_->GetCollector(seq)->AddRequest(
      std::move(request), signature, type == Request::TYPE_PAXOS_ACCEPT_REQUEST,
      [&](const Request& request, int received_count,
          TransactionCollector::CollectorDataType* data,
          TransactionCollector::PaxosStatus& status, bool promised_higher) {
        if (MayConsensusChangeStatus(type, received_count, status, promised_higher)) {
          resp_received_count = 1;
        }
      });
  if (ret == 1) {
    SetLastCommittedTime(proxy_id);
  } else if (ret != 0) {
    LOG(ERROR) << " add request fail";
    return CollectorResultCode::INVALID;
  }
  if (resp_received_count > 0) {
    return CollectorResultCode::STATE_CHANGED;
  }
  return CollectorResultCode::OK;
}

std::vector<RequestInfo> MessageManager::GetPreparedProof(uint64_t seq) {
  return collector_pool_->GetCollector(seq)->GetPreparedProof();
}

int MessageManager::GetReplicaState(ReplicaState* state) {
  *state->mutable_replica_config() = config_.GetConfigData();
  return 0;
}

Storage* MessageManager::GetStorage() {
  return transaction_executor_->GetStorage();
}

void MessageManager::SetNextCommitSeq(int seq) {
  LOG(ERROR) << " set next commit seq:" << seq;
  SetNextSeq(seq);
  SetHighestPreparedSeq(seq);
  collector_pool_->Reset(seq);
  return transaction_executor_->SetPendingExecutedSeq(seq);
}

void MessageManager::SetLastCommittedTime(uint64_t proxy_id) {
  lct_lock_.lock();
  last_committed_time_[proxy_id] = GetCurrentTime();
  lct_lock_.unlock();
}

uint64_t MessageManager::GetLastCommittedTime(uint64_t proxy_id) {
  lct_lock_.lock();
  auto value = last_committed_time_[proxy_id];
  lct_lock_.unlock();
  return value;
}

bool MessageManager::IsPreapared(uint64_t seq) {
  return collector_pool_->GetCollector(seq)->IsPrepared();
}

bool MessageManager::HasAccepted(uint64_t seq) { 
  return collector_pool_->GetCollector(seq)->HasAccepted(); 
}

std::pair<int32_t, int32_t> MessageManager::GetAcceptedIds(uint64_t seq) const { 
  return collector_pool_->GetCollector(seq)->GetAcceptedIds(); 
}

uint64_t MessageManager::GetHighestPreparedSeq() {
  return 0; 
}

void MessageManager::SetHighestPreparedSeq(uint64_t seq) {
}

void MessageManager::SetDuplicateManager(DuplicateManager* manager) {
  transaction_executor_->SetDuplicateManager(manager);
}

void MessageManager::SendResponse(std::unique_ptr<Request> request) {
  std::unique_ptr<BatchUserResponse> response =
      std::make_unique<BatchUserResponse>();
  response->set_createtime(GetCurrentTime());
  // response->set_local_id(batch_request.local_id());
  response->set_hash(request->hash());
  response->set_proxy_id(request->proxy_id());
  response->set_seq(request->seq());
  response->set_current_view(GetCurrentView());
  response->set_primary_id(GetCurrentPrimary());
  if (transaction_executor_->NeedResponse() && response->proxy_id() != 0) {
    queue_.Push(std::move(response));
  }
}

LockFreeCollectorPool* MessageManager::GetCollectorPool() {
  return collector_pool_.get();
}
} // namespace paxos 
}  // namespace resdb
