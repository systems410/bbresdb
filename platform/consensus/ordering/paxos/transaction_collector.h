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

#include <bitset>

#include "platform/consensus/execution/transaction_executor.h"
#include "platform/networkstrate/server_comm.h"
#include "platform/proto/resdb.pb.h"
#include "platform/statistic/stats.h"

namespace resdb {
namespace paxos {


enum TransactionStatue {
  None = 0,
  READY_PREPARE = 1,
  READY_EXECUTE = 2,
  EXECUTED = 3,
  PROMISED = 4, 
  PROMISES_RECEIVED = 5,
  ACCEPTED = 6,
};

struct RequestInfo {
  std::unique_ptr<Request> request;
  SignatureInfo signature;
};

template <typename T>
class AtomicUniquePtr {
 public:
  AtomicUniquePtr() : v_(0) {}
  bool Set(std::unique_ptr<T>& new_ptr) {
    int old_v = 0;
    bool res = v_.compare_exchange_strong(old_v, 1, std::memory_order_acq_rel,
                                          std::memory_order_acq_rel);
    if (!res) {
      return false;
    }
    ptr_ = std::move(new_ptr);
    v_ = 2;
    return true;
  }

  T* Reference() {
    int v = v_.load(std::memory_order_acq_rel);
    if (v <= 1) {
      return nullptr;
    }
    return ptr_.get();
  }

  void Clear() {
    v_ = 0;
    ptr_ = nullptr;
  }

 private:
  std::unique_ptr<T> ptr_;
  std::atomic<int> v_;
};

class TransactionCollector {
 public:
  TransactionCollector(uint64_t seq, TransactionExecutor* executor)
      : seq_(seq),
        executor_(executor),
        acceptor_status_(TransactionStatue::None), 
        proposer_status_(TransactionStatue::None) {}

  ~TransactionCollector() = default;

  // TODO split the context list.
  // context contains the client channel used for sending back the response.
  int SetContextList(uint64_t seq,
                     std::vector<std::unique_ptr<Context>> context);
  bool HasClientContextList(uint64_t seq) const;
  std::vector<std::unique_ptr<Context>> FetchContextList(uint64_t seq);

  typedef std::list<std::unique_ptr<RequestInfo>> CollectorDataType;

  struct PaxosStatus { 
    std::atomic<TransactionStatue>* learner; 
    std::atomic<TransactionStatue>* acceptor; 
    std::atomic<TransactionStatue>* proposer; 
    std::atomic<TransactionStatue>* proxy; 
  };

  // Add a message and count by its hash value.
  // After it is done call_back will be triggered.
  int AddRequest(
      std::unique_ptr<Request> request, const SignatureInfo& signature,
      bool is_main_request,
      std::function<void(const Request&, int received_count,
                         CollectorDataType* data,
                         PaxosStatus& status, 
                         bool has_promised_higher)>
          call_back);

  std::vector<RequestInfo> GetPreparedProof();
  TransactionStatue GetProposerStatus() const;
  TransactionStatue GetAcceptorStatus() const; 
  TransactionStatue GetLearnerStatus() const; 

  Request* GetMainRequest() { 
    if (atomic_main_request_.Reference() == nullptr) { 
      return nullptr;
    }
    return atomic_main_request_.Reference()->request.get(); 
  }; 

  bool HasAccepted() const { return has_accepted_; } 

  std::pair<int32_t, int32_t> GetAcceptedIds() const { return { highest_promise_id_, highest_promise_node_id_}; }

  uint64_t Seq();

  bool IsPrepared();

  std::vector<std::string> GetAllStoredHash();

 private:
  int Commit();

 private:
  uint64_t seq_;
  TransactionExecutor* executor_;
  std::atomic<bool> is_committed_ = false;
  std::atomic<bool> is_prepared_ = false;
  std::vector<std::unique_ptr<Context>> context_list_;
  std::map<std::string, std::list<std::unique_ptr<RequestInfo>>>
      data_[Request::NUM_OF_TYPE];
  std::vector<std::unique_ptr<RequestInfo>> prepared_proof_;
  AtomicUniquePtr<RequestInfo> atomic_main_request_;
  std::atomic<TransactionStatue> acceptor_status_ = TransactionStatue::None;
  std::atomic<TransactionStatue> proposer_status_ = TransactionStatue::None;
  std::atomic<TransactionStatue> learner_status_ = TransactionStatue::None;
  std::atomic<TransactionStatue> proxy_status_ = TransactionStatue::None; 
  std::mutex mutex_;
  std::vector<SignatureInfo> commit_certs_;
  std::map<std::string, std::bitset<128>> senders_[Request::NUM_OF_TYPE];
  std::map<int32_t, uint32_t> num_of_promises_; 
  std::set<std::unique_ptr<RequestInfo>> other_main_request_;

  // Holds highest promise id state
  std::atomic<int32_t> highest_promise_id_ = -1; 
  std::atomic<int32_t> highest_promise_node_id_ = -1; 
  // Holds highest id of a promise response that has allready accepted 
  std::atomic<int32_t> highest_accepted_id_ = -1; 
  std::atomic<int32_t> highest_accepted_node_id_ = -1; 
  // If we have accepted from an accept request 
  std::atomic<bool> has_accepted_ = false; 
};
} // namespace paxos
}  // namespace resdb
