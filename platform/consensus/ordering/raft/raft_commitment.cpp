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

#include "platform/consensus/ordering/raft/raft_commitment.h"

#include <glog/logging.h>

#include <algorithm>
#include <chrono>

#include "platform/consensus/ordering/common/transaction_utils.h"

namespace resdb {
namespace raft {

RaftCommitment::RaftCommitment(const ResDBConfig& config, RaftLog* log,
                               ElectionManager* election_mgr,
                               ReplicaCommunicator* rc, SystemInfo* system_info,
                               SignatureVerifier* verifier,
                               std::unique_ptr<TransactionManager> txn_manager)
    : config_(config),
      log_(log),
      election_mgr_(election_mgr),
      rc_(rc),
      system_info_(system_info),
      verifier_(verifier) {
  global_stats_ = Stats::GetGlobalStats();

  executor_ = std::make_unique<TransactionExecutor>(
      config_,
      [this](std::unique_ptr<Request> request,
             std::unique_ptr<BatchUserResponse> resp) {
        // Send the response back to the originating proxy.
        if (resp == nullptr) return;
        auto response = NewRequest(Request::TYPE_RESPONSE, Request(),
                                   config_.GetSelfInfo().id(),
                                   config_.GetSelfShard());
        resp->SerializeToString(response->mutable_data());
        response->set_seq(resp->seq());
        response->set_proxy_id(resp->proxy_id());
        rc_->SendMessage(*response, resp->proxy_id());
        LOG(INFO) << "[RAFT] Sent TYPE_RESPONSE to proxy=" << resp->proxy_id()
                  << " seq=" << resp->seq();
      },
      system_info_,
      std::move(txn_manager));

  response_thread_ = std::thread(&RaftCommitment::ResponseLoop, this);
}

RaftCommitment::~RaftCommitment() {
  stop_ = true;
  if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
  if (response_thread_.joinable()) response_thread_.join();
}

void RaftCommitment::OnBecomeLeader() {
  LOG(INFO) << "[RAFT] OnBecomeLeader: initializing leader state";
  {
    std::lock_guard<std::mutex> lk(leader_mu_);
    uint64_t next = log_->LastIndex() + 1;
    for (const auto& replica : config_.GetReplicaInfos(config_.GetSelfShard())) {
      next_index_[replica.id()] = next;
      match_index_[replica.id()] = 0;
    }
    // Count self as having the full log.
    match_index_[config_.GetSelfInfo().id()] = log_->LastIndex();
  }

  stop_ = false;
  if (!heartbeat_thread_.joinable()) {
    heartbeat_thread_ = std::thread(&RaftCommitment::HeartbeatLoop, this);
  }
}

void RaftCommitment::OnStepDown() {
  LOG(INFO) << "[RAFT] OnStepDown: stopping leader work";
  // Heartbeat loop checks role; it will idle until we become leader again.
}

// ─── Leader: new client batch ─────────────────────────────────────────────────

int RaftCommitment::ProcessNewRequest(std::unique_ptr<Context> context,
                                      std::unique_ptr<Request> request) {
  if (election_mgr_->GetRole() != RaftRole::LEADER) {
    int32_t leader = election_mgr_->GetLeaderId();
    LOG(INFO) << "[RAFT] Not leader, forwarding to leader=" << leader;
    if (leader >= 0) rc_->SendMessage(*request, leader);
    return 0;
  }

  LOG(INFO) << "[RAFT] Leader processing new request proxy=" << request->proxy_id();

  RaftLogEntry entry;
  entry.set_term(election_mgr_->GetCurrentTerm());
  entry.set_proxy_id(request->proxy_id());
  request->SerializeToString(entry.mutable_data());
  entry.set_hash(request->hash());

  uint64_t index = log_->Append(election_mgr_->GetCurrentTerm(), entry);

  {
    std::lock_guard<std::mutex> lk(leader_mu_);
    match_index_[config_.GetSelfInfo().id()] = index;
  }

  // Replicate to all followers.
  for (const auto& replica : config_.GetReplicaInfos(config_.GetSelfShard())) {
    if (replica.id() == config_.GetSelfInfo().id()) continue;
    SendAppendEntries(replica.id());
  }

  return 0;
}

// ─── Leader: send AppendEntries to one peer ───────────────────────────────────

void RaftCommitment::SendAppendEntries(int32_t peer_id) {
  uint64_t ni;
  {
    std::lock_guard<std::mutex> lk(leader_mu_);
    ni = next_index_.count(peer_id) ? next_index_[peer_id] : 1;
  }

  uint64_t prev_log_index = (ni > 1) ? ni - 1 : 0;
  uint64_t prev_log_term = log_->TermAt(prev_log_index);

  RaftAppendEntries ae;
  ae.set_term(election_mgr_->GetCurrentTerm());
  ae.set_leader_id(config_.GetSelfInfo().id());
  ae.set_prev_log_index(prev_log_index);
  ae.set_prev_log_term(prev_log_term);
  ae.set_leader_commit(commit_index_);

  for (const auto& e : log_->EntriesAfter(prev_log_index)) {
    *ae.add_entries() = e;
  }

  auto msg = std::make_unique<Request>();
  msg->set_type(Request::TYPE_RAFT_APPEND_ENTRIES);
  msg->set_sender_id(config_.GetSelfInfo().id());
  msg->set_sender_shard_id(config_.GetSelfShard());
  msg->set_current_view(election_mgr_->GetCurrentTerm());
  ae.SerializeToString(msg->mutable_data());

  rc_->SendMessage(*msg, peer_id);
}

void RaftCommitment::SendHeartbeats() {
  for (const auto& replica : config_.GetReplicaInfos(config_.GetSelfShard())) {
    if (replica.id() == config_.GetSelfInfo().id()) continue;
    SendAppendEntries(replica.id());
  }
}

void RaftCommitment::HeartbeatLoop() {
  while (!stop_) {
    if (election_mgr_->GetRole() == RaftRole::LEADER) {
      SendHeartbeats();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

// ─── Follower: receive AppendEntries ─────────────────────────────────────────

int RaftCommitment::ProcessAppendEntries(std::unique_ptr<Context> context,
                                         std::unique_ptr<Request> request) {
  RaftAppendEntries ae;
  if (!ae.ParseFromString(request->data())) {
    LOG(ERROR) << "[RAFT] Failed to parse RaftAppendEntries";
    return -2;
  }

  // If leader has a higher term, step down (no-op in happy path since we
  // don't have competing leaders, but kept for correctness).
  election_mgr_->MaybeStepDown(ae.term());
  election_mgr_->ResetElectionTimer();
  election_mgr_->SetLeaderId(ae.leader_id());

  // Update system_info so other layers know who the primary is.
  system_info_->SetPrimary(ae.leader_id());
  system_info_->SetCurrentView(ae.term());

  bool success = true;

  // Consistency check on prev_log_index / prev_log_term.
  if (ae.prev_log_index() > 0) {
    uint64_t local_term = log_->TermAt(ae.prev_log_index());
    if (local_term == 0 || local_term != ae.prev_log_term()) {
      LOG(WARNING) << "[RAFT] Log inconsistency at prev_log_index="
                   << ae.prev_log_index();
      success = false;
    }
  }

  uint64_t match = log_->LastIndex();

  if (success) {
    for (const auto& entry : ae.entries()) {
      uint64_t idx = entry.seq();
      uint64_t existing_term = log_->TermAt(idx);
      if (existing_term != 0 && existing_term != entry.term()) {
        log_->TruncateAfter(idx - 1);
      }
      if (log_->TermAt(idx) == 0) {
        log_->Append(entry.term(), entry);
      }
    }
    match = log_->LastIndex();

    // Advance commit_index and apply.
    if (ae.leader_commit() > commit_index_) {
      commit_index_ = std::min(ae.leader_commit(), log_->LastIndex());
      ApplyCommitted();
    }
  }

  // Send response back to leader.
  RaftAppendEntriesResponse resp;
  resp.set_term(election_mgr_->GetCurrentTerm());
  resp.set_success(success);
  resp.set_match_index(match);
  resp.set_sender_id(config_.GetSelfInfo().id());

  auto response = std::make_unique<Request>();
  response->set_type(Request::TYPE_RAFT_APPEND_ENTRIES_RESPONSE);
  response->set_sender_id(config_.GetSelfInfo().id());
  response->set_sender_shard_id(config_.GetSelfShard());
  response->set_current_view(election_mgr_->GetCurrentTerm());
  resp.SerializeToString(response->mutable_data());

  rc_->SendMessage(*response, ae.leader_id());
  return 0;
}

// ─── Leader: receive AppendEntries responses ─────────────────────────────────

int RaftCommitment::ProcessAppendEntriesResponse(
    std::unique_ptr<Context> context, std::unique_ptr<Request> request) {
  RaftAppendEntriesResponse resp;
  if (!resp.ParseFromString(request->data())) {
    LOG(ERROR) << "[RAFT] Failed to parse RaftAppendEntriesResponse";
    return -2;
  }

  if (election_mgr_->MaybeStepDown(resp.term())) return 0;
  if (election_mgr_->GetRole() != RaftRole::LEADER) return 0;

  int32_t peer = resp.sender_id();

  if (resp.success()) {
    std::lock_guard<std::mutex> lk(leader_mu_);
    if (resp.match_index() > match_index_[peer]) {
      match_index_[peer] = resp.match_index();
      next_index_[peer] = resp.match_index() + 1;
    }
  } else {
    // Retry with an earlier index (log inconsistency — rare in happy path).
    std::lock_guard<std::mutex> lk(leader_mu_);
    if (next_index_[peer] > 1) next_index_[peer]--;
    SendAppendEntries(peer);
    return 0;
  }

  MaybeAdvanceCommitIndex();
  return 0;
}

// ─── Leader: advance commit_index ────────────────────────────────────────────

void RaftCommitment::MaybeAdvanceCommitIndex() {
  // Find the highest N such that a majority of match_index_[i] >= N
  // and log_.TermAt(N) == current_term.
  uint64_t current_term = election_mgr_->GetCurrentTerm();
  uint64_t new_commit = commit_index_.load();

  std::lock_guard<std::mutex> lk(leader_mu_);
  uint64_t last = log_->LastIndex();
  for (uint64_t n = last; n > commit_index_; n--) {
    if (log_->TermAt(n) != current_term) continue;
    int count = 0;
    for (const auto& [peer, mi] : match_index_) {
      if (mi >= n) count++;
    }
    int quorum = config_.GetReplicaNumInSelfShard() / 2 + 1;
    if (count >= quorum) {
      new_commit = n;
      break;
    }
  }

  if (new_commit > commit_index_) {
    commit_index_ = new_commit;
    ApplyCommitted();
  }
}

// ─── Apply committed entries ──────────────────────────────────────────────────

void RaftCommitment::ApplyCommitted() {
  uint64_t applied = last_applied_.load();
  uint64_t commit = commit_index_.load();

  for (uint64_t i = applied + 1; i <= commit; i++) {
    RaftLogEntry entry;
    if (!log_->GetEntry(i, &entry)) {
      LOG(ERROR) << "[RAFT] Missing log entry at index=" << i;
      continue;
    }

    // Reconstruct the original Request that holds the BatchUserRequest.
    auto req = std::make_unique<Request>();
    if (!req->ParseFromString(entry.data())) {
      LOG(ERROR) << "[RAFT] Failed to parse request from log entry " << i;
      continue;
    }
    req->set_seq(i);

    LOG(INFO) << "[RAFT] Committing log entry index=" << i
              << " proxy=" << req->proxy_id();

    executor_->Commit(std::move(req));
    last_applied_ = i;
  }
}

// ─── Response delivery thread ─────────────────────────────────────────────────

void RaftCommitment::ResponseLoop() {
  // The TransactionExecutor's PostExecuteFunc already sends the TYPE_RESPONSE
  // directly via the rc_ callback set in the constructor. This thread can be
  // used for any additional async response work if needed.
  while (!stop_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

}  // namespace raft
}  // namespace resdb
