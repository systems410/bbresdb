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

#include "platform/consensus/ordering/raft/election_manager.h"

#include <glog/logging.h>

#include <chrono>
#include <random>

#include "platform/consensus/ordering/common/transaction_utils.h"

namespace resdb {
namespace raft {

namespace {
// Randomized election timeout: [150ms, 300ms).
int RandomElectionTimeoutMs() {
  static thread_local std::mt19937 rng(std::random_device{}());
  std::uniform_int_distribution<int> dist(150, 300);
  return dist(rng);
}
}  // namespace

ElectionManager::ElectionManager(const ResDBConfig& config, RaftLog* log,
                                 ReplicaCommunicator* rc,
                                 std::function<void()> on_become_leader,
                                 std::function<void(uint64_t term)> on_step_down)
    : config_(config),
      log_(log),
      rc_(rc),
      on_become_leader_(std::move(on_become_leader)),
      on_step_down_(std::move(on_step_down)) {}

ElectionManager::~ElectionManager() { Stop(); }

void ElectionManager::Start() {
  stop_ = false;
  election_thread_ = std::thread(&ElectionManager::RunElectionLoop, this);
}

void ElectionManager::Stop() {
  stop_ = true;
  if (election_thread_.joinable()) election_thread_.join();
}

void ElectionManager::ResetElectionTimer() { timer_reset_ = true; }

RaftRole ElectionManager::GetRole() const { return role_.load(); }
uint64_t ElectionManager::GetCurrentTerm() const { return current_term_.load(); }
int32_t ElectionManager::GetLeaderId() const { return leader_id_.load(); }
void ElectionManager::SetLeaderId(int32_t id) { leader_id_ = id; }

bool ElectionManager::MaybeStepDown(uint64_t term) {
  if (term > current_term_.load()) {
    BecomeFollower(term);
    return true;
  }
  return false;
}

void ElectionManager::RunElectionLoop() {
  while (!stop_) {
    // Leaders do not participate in elections.
    if (role_ == RaftRole::LEADER) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }

    int timeout_ms = RandomElectionTimeoutMs();
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);

    timer_reset_ = false;

    while (std::chrono::steady_clock::now() < deadline) {
      if (stop_) return;
      if (timer_reset_.exchange(false)) {
        // Heartbeat received — restart the timer.
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // If the timer was reset we didn't time out — restart outer loop.
    if (timer_reset_.exchange(false)) continue;
    if (role_ == RaftRole::LEADER) continue;

    // Timed out without a heartbeat: start an election.
    StartElection();
  }
}

void ElectionManager::StartElection() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    current_term_++;
    role_ = RaftRole::CANDIDATE;
    int32_t self_id = config_.GetSelfInfo().id();
    voted_for_ = self_id;
    votes_received_ = 1;  // vote for self
    leader_id_ = -1;
  }

  LOG(INFO) << "[RAFT] Starting election for term " << current_term_
            << " candidate=" << config_.GetSelfInfo().id();

  RaftVoteRequest vote_req;
  vote_req.set_term(current_term_);
  vote_req.set_candidate_id(config_.GetSelfInfo().id());
  vote_req.set_last_log_index(log_->LastIndex());
  vote_req.set_last_log_term(log_->LastTerm());

  auto request = std::make_unique<Request>();
  request->set_type(Request::TYPE_RAFT_VOTE_REQUEST);
  request->set_sender_id(config_.GetSelfInfo().id());
  request->set_sender_shard_id(config_.GetSelfShard());
  request->set_current_view(current_term_);
  vote_req.SerializeToString(request->mutable_data());

  rc_->SendMessageToShard(*request, config_.GetSelfShard());
}

void ElectionManager::BecomeFollower(uint64_t term) {
  LOG(INFO) << "[RAFT] Stepping down to follower, term=" << term;
  std::lock_guard<std::mutex> lk(mu_);
  current_term_ = term;
  role_ = RaftRole::FOLLOWER;
  voted_for_ = -1;
  if (on_step_down_) on_step_down_(term);
}

void ElectionManager::BecomeLeader() {
  LOG(INFO) << "[RAFT] Becoming leader for term=" << current_term_;
  role_ = RaftRole::LEADER;
  leader_id_ = config_.GetSelfInfo().id();
  if (on_become_leader_) on_become_leader_();
}

int ElectionManager::ProcessVoteRequest(std::unique_ptr<Context> context,
                                        std::unique_ptr<Request> request) {
  RaftVoteRequest vote_req;
  if (!vote_req.ParseFromString(request->data())) {
    LOG(ERROR) << "[RAFT] Failed to parse RaftVoteRequest";
    return -2;
  }

  MaybeStepDown(vote_req.term());

  bool grant = false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    bool term_ok = (vote_req.term() >= current_term_);
    bool not_voted = (voted_for_ == -1 || voted_for_ == vote_req.candidate_id());

    // Raft log-up-to-date check.
    bool log_ok =
        (vote_req.last_log_term() > log_->LastTerm()) ||
        (vote_req.last_log_term() == log_->LastTerm() &&
         vote_req.last_log_index() >= log_->LastIndex());

    if (term_ok && not_voted && log_ok) {
      voted_for_ = vote_req.candidate_id();
      grant = true;
      timer_reset_ = true;  // treat granting a vote as a heartbeat
    }
  }

  LOG(INFO) << "[RAFT] Vote request from candidate=" << vote_req.candidate_id()
            << " term=" << vote_req.term() << " grant=" << grant;

  RaftVoteResponse resp;
  resp.set_term(current_term_);
  resp.set_vote_granted(grant);

  auto response = std::make_unique<Request>();
  response->set_type(Request::TYPE_RAFT_VOTE_RESPONSE);
  response->set_sender_id(config_.GetSelfInfo().id());
  response->set_sender_shard_id(config_.GetSelfShard());
  response->set_current_view(current_term_);
  resp.SerializeToString(response->mutable_data());

  rc_->SendMessage(*response, vote_req.candidate_id());
  return 0;
}

int ElectionManager::ProcessVoteResponse(std::unique_ptr<Context> context,
                                         std::unique_ptr<Request> request) {
  RaftVoteResponse resp;
  if (!resp.ParseFromString(request->data())) {
    LOG(ERROR) << "[RAFT] Failed to parse RaftVoteResponse";
    return -2;
  }

  if (MaybeStepDown(resp.term())) return 0;

  if (role_ != RaftRole::CANDIDATE) return 0;
  if (resp.term() != current_term_) return 0;

  bool became_leader = false;
  if (resp.vote_granted()) {
    std::lock_guard<std::mutex> lk(mu_);
    votes_received_++;
    int quorum = config_.GetReplicaNumInSelfShard() / 2 + 1;
    if (votes_received_ >= quorum && role_ == RaftRole::CANDIDATE) {
      BecomeLeader();
      became_leader = true;
    }
  }

  return 0;
}

}  // namespace raft
}  // namespace resdb
