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

#include "platform/consensus/ordering/2pc/message_manager.h"

#include <glog/logging.h>

#include "common/utils/utils.h"

namespace resdb {

namespace twopc {

MessageManager::MessageManager(
    const ResDBConfig& config, SystemInfo* system_info, ConsensusManager* replica_cm)
    : config_(config),
      replica_cm_(replica_cm),
      queue_("executed"),
      system_info_(system_info),
      collector_pool_(std::make_unique<LockFreeCollectorPool>(
          "txn", config_.GetMaxProcessTxn())) {
  global_stats_ = Stats::GetGlobalStats();
}

MessageManager::~MessageManager() {}

std::unique_ptr<BatchUserResponse> MessageManager::GetResponseMsg() {
  return queue_.Pop();
}

int64_t MessageManager::GetCurrentPrimary(uint64_t seq) {
  return system_info_->GetCrossShardPrimaryId(seq);
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

void MessageManager::IncrementSequence() { 
  std::unique_lock<std::mutex> lk(seq_mutex_);
  next_seq_++;
}

absl::StatusOr<uint64_t> MessageManager::AssignNextSeq() {
  std::unique_lock<std::mutex> lk(seq_mutex_);
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

  return true;
}

bool MessageManager::MayConsensusChangeStatus(
    int type, int received_count, std::atomic<TransactionStatue>* status,
    bool ret, uint64_t seq) {
  switch (type) {
    // Have the participant switch directly to ready commit on the prepare msg as we assume no aborts 
    case Request::TYPE_2PC_PREPARE: 
      // the coordinator may also be a participant, so dont let them switch to ready commit until they have enough votes
      if (config_.GetSelfInfo().id() != GetCurrentPrimary(seq) && *status == TransactionStatue::None) { 
        TransactionStatue old_status = TransactionStatue::None;
        return status->compare_exchange_strong(
            old_status, TransactionStatue::READY_COMMIT,
            std::memory_order_acq_rel, std::memory_order_acq_rel);
      }
      break; 
    // Have the coordinator enter a READY PREPARE state on new txns 
    case Request::TYPE_NEW_TXNS: 
      if (*status == TransactionStatue::None) { 
        TransactionStatue old_status = TransactionStatue::None;
        return status->compare_exchange_strong(
            old_status, TransactionStatue::READY_PREPARE,
            std::memory_order_acq_rel, std::memory_order_acq_rel);
      }  
      break; 

    // Received a COMMIT VOTE, if we have received votes for all replicas, 
    // transition into READY COMMIT state  
    case Request::TYPE_2PC_VOTE_COMMIT: 
      if (*status == TransactionStatue::READY_PREPARE && config_.GetMinDataReceiveNum(system_info_->GetAllShardPrimaryIds().size()) <= received_count) {
        TransactionStatue old_status = TransactionStatue::READY_PREPARE;
        return status->compare_exchange_strong(
            old_status, TransactionStatue::READY_COMMIT,
            std::memory_order_acq_rel, std::memory_order_acq_rel);
      }
      break;

    // Received global decision to commit. If ready to commit, transition to READY EXECUTE 
    case Request::TYPE_2PC_COMMIT:
      if (*status == TransactionStatue::READY_COMMIT) { 
        TransactionStatue old_status = TransactionStatue::READY_COMMIT;
        return status->compare_exchange_strong(
            old_status, TransactionStatue::READY_EXECUTE,
            std::memory_order_acq_rel, std::memory_order_acq_rel);
      }
      break; 
  }
  return ret;
}

// Add commit messages and return the number of messages have been received.
// The commit messages only include post(pre-prepare), prepare and commit
// messages. Messages are handled by state (PREPARE,COMMIT,READY_EXECUTE).

// If there are enough messages and the state is changed after adding the
// message, return 1, otherwise return 0. Return -2 if the request is not valid.
CollectorResultCode MessageManager::AddConsensusMsg(
    std::unique_ptr<Context> context, std::unique_ptr<Request> request) {
  if (request == nullptr || !IsValidMsg(*request)) {
    LOG(ERROR) << " msg not invalid";
    return CollectorResultCode::INVALID;
  }

  int type = request->type();
  uint64_t seq = request->seq();
  int resp_received_count = 0;
  int proxy_id = request->proxy_id();

  int ret = collector_pool_->GetCollector(seq)->AddRequest(
      std::move(request), std::move(context), type == Request::TYPE_2PC_PREPARE,
      [&](const Request& request, int received_count,
          TransactionCollector::CollectorDataType* data,
          std::atomic<TransactionStatue>* status, bool force) {
        if (MayConsensusChangeStatus(type, received_count, status, force, seq)) {
          resp_received_count = 1;
        }
      }, replica_cm_);
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

void MessageManager::SendResponse(std::unique_ptr<Request> request) {
  std::unique_ptr<BatchUserResponse> response =
      std::make_unique<BatchUserResponse>();
  response->set_createtime(GetCurrentTime());
  // response->set_local_id(batch_request.local_id());
  response->set_hash(request->hash());
  response->set_proxy_id(request->proxy_id());
  response->set_seq(request->seq());
  response->set_current_view(GetCurrentView());
  response->set_primary_id(GetCurrentPrimary(request->seq()));
}

LockFreeCollectorPool* MessageManager::GetCollectorPool() {
  return collector_pool_.get();
}

} // namepsace 2pc 
}  // namespace resdb
