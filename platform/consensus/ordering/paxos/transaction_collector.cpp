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

#include "platform/consensus/ordering/paxos/transaction_collector.h"

#include <glog/logging.h>

#include "common/crypto/signature_verifier.h"

namespace resdb {
namespace paxos { 

uint64_t TransactionCollector::Seq() { return seq_; }

bool TransactionCollector::IsPrepared() { return is_prepared_; }

TransactionStatue TransactionCollector::GetProposerStatus() const { return proposer_status_; }
TransactionStatue TransactionCollector::GetAcceptorStatus() const { return acceptor_status_; }
TransactionStatue TransactionCollector::GetLearnerStatus() const { return learner_status_ ; }

int TransactionCollector::SetContextList(
    uint64_t seq, std::vector<std::unique_ptr<Context>> context) {
  if (seq != seq_) {
    return -2;
  }
  context_list_ = std::move(context);
  return 0;
}

bool TransactionCollector::HasClientContextList(uint64_t seq) const {
  if (seq != seq_) {
    return false;
  }
  return !context_list_.empty();
}

std::vector<std::unique_ptr<Context>> TransactionCollector::FetchContextList(
    uint64_t seq) {
  if (seq != seq_) {
    return std::vector<std::unique_ptr<Context>>();
  }
  return std::move(context_list_);
}

std::vector<RequestInfo> TransactionCollector::GetPreparedProof() {
  std::vector<RequestInfo> prepared_info;
  for (const auto& proof : prepared_proof_) {
    RequestInfo info;
    info.signature = proof->signature;
    info.request = std::make_unique<Request>(*proof->request);
    prepared_info.push_back(std::move(info));
  }
  return prepared_info;
}

int TransactionCollector::AddRequest(
    std::unique_ptr<Request> request, const SignatureInfo& signature,
    bool is_main_request,
    std::function<void(const Request&, int received_count, CollectorDataType*,
                       PaxosStatus& status, bool has_promised_higher)>
        call_back) {
  if (request == nullptr) {
    LOG(ERROR) << "request empty";
    return -2;
  }

  int32_t sender_id = request->sender_id();
  std::string hash = request->hash();
  int type = request->type();
  uint64_t seq = request->seq();
  if (is_committed_) {
    return -2;
  }
  if (learner_status_.load() == EXECUTED) {
    return -2;
  }

  if (seq_ != static_cast<uint64_t>(request->seq())) {
    LOG(ERROR) << "data invalid, seq not the same:" << seq
               << " collect seq:" << seq_;
    return -2;
  }

  PaxosStatus status = {
    .learner = &learner_status_, 
    .acceptor = &acceptor_status_, 
    .proposer = &proposer_status_
  };

  bool promised_higher = false; 
  if (request->paxos_id() == highest_promise_id_) { 
    promised_higher = request->sender_id() < highest_promise_node_id_;  
  } else { 
    promised_higher = request->paxos_id() < highest_promise_id_; 
  }

  if (!promised_higher) { 
    highest_promise_id_ = request->paxos_id(); 
    highest_promise_node_id_ = request->sender_id(); 
  }

  if (request->type() == Request::TYPE_PAXOS_ACCEPT_REQUEST && !promised_higher) { 
    has_accepted_ = true; 
  }

  // We need to update the main request if the promiser accepted a higher id 
  // and this is the highest accepted id we have seen 
  if (request->type() == Request::TYPE_PAXOS_PROMISE && request->has_accepted_paxos_id()) { 
    if (request->accepted_paxos_id() == highest_accepted_id_) { 
      is_main_request = request->sender_id() < highest_accepted_node_id_;  
    } else { 
      is_main_request = request->paxos_id() < highest_accepted_id_; 
    }
  }

  if (is_main_request) {
    auto request_info = std::make_unique<RequestInfo>();
    request_info->signature = signature;
    request_info->request = std::move(request);
    int ret = atomic_main_request_.Set(request_info);
    if (!ret) {
      other_main_request_.insert(std::move(request_info));
      LOG(ERROR) << "set main request fail: data existed:" << seq
                 << " ret:" << ret;
      return -2;
    }
    auto main_request = atomic_main_request_.Reference();
    if (main_request->request == nullptr) {
      LOG(ERROR) << "set main request data fail";
      return -2;
    }
    call_back(*main_request->request.get(), 1, nullptr, status, promised_higher);
    return 0;
  } else {
    if (request->type() == Request::TYPE_COMMIT) {
      if (request->has_data_signature() &&
          request->data_signature().node_id() > 0) {
        std::lock_guard<std::mutex> lk(mutex_);
        LOG(ERROR) << "add qc signature";
        commit_certs_.push_back(request->data_signature());
      }
    }

    {
      std::lock_guard<std::mutex> lk(mutex_);
      uint32_t count = 1; 
      if (request->type() == Request::TYPE_PAXOS_PROMISE) { 
        num_of_promises_[request->paxos_id()]++; 
        count = num_of_promises_[request->paxos_id()];
      } else { 
        if (senders_[type].count(hash) == 0) {
          senders_[type].insert(std::make_pair(hash, std::bitset<128>()));
        }
        senders_[type][hash][sender_id] = 1;
        count = senders_[type][hash].count(); 
      }
      call_back(*request, count, nullptr, status, promised_higher);
    }

    if (learner_status_.load() == TransactionStatue::READY_EXECUTE) {
      Commit();
      return 1;
    }
  }
  return 0;
}

int TransactionCollector::Commit() {
  TransactionStatue old_status = TransactionStatue::READY_EXECUTE;
  bool res = learner_status_.compare_exchange_strong(
      old_status, TransactionStatue::EXECUTED, std::memory_order_acq_rel,
      std::memory_order_acq_rel);
  if (!res) {
    return -2;
  }

  auto main_request = atomic_main_request_.Reference();
  if (main_request == nullptr) {
    LOG(ERROR) << "no main:" << seq_;
    return -2;
  }

  is_committed_ = true;
  if (executor_ && main_request->request) {
    if (!commit_certs_.empty()) {
      for (const auto& sig : commit_certs_) {
        *main_request->request->mutable_committed_certs()
             ->add_committed_certs() = sig;
        // LOG(ERROR) << "add sig:" << sig.DebugString();
      }
    }
    executor_->Commit(std::move(main_request->request));
  }
  return 0;
}

std::vector<std::string> TransactionCollector::GetAllStoredHash() {
  std::vector<std::string> v;
  auto main_request = atomic_main_request_.Reference();
  if (main_request) {
    v.push_back(main_request->request->hash());
  }
  for (auto& info : other_main_request_) {
    v.push_back(info->request->hash());
  }
  return v;
}
} // namespace paxos
}  // namespace resdb
