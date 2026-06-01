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

#include "platform/consensus/ordering/raft/consensus_manager_raft.h"

#include <glog/logging.h>

namespace resdb {
namespace raft {

ConsensusManagerRaft::ConsensusManagerRaft(
    const ResDBConfig& config, std::unique_ptr<TransactionManager> executor,
    const CrossProtocolResources& resources,
    std::unique_ptr<CustomQuery> query_executor)
    : ConsensusManager(config, resources),
      log_(std::make_unique<RaftLog>()),
      response_manager_(config_.IsPerformanceRunning()
                            ? nullptr
                            : std::make_unique<ResponseManager>(
                                  config_, replica_communicator_,
                                  system_info_, GetSignatureVerifier())) {

  election_mgr_ = std::make_unique<ElectionManager>(
      config_, log_.get(), replica_communicator_,
      /* on_become_leader */
      [this]() {
        LOG(INFO) << "[RAFT] Became leader, term=" << election_mgr_->GetCurrentTerm();
        system_info_->SetPrimary(config_.GetSelfInfo().id());
        system_info_->SetCurrentView(election_mgr_->GetCurrentTerm());
        commitment_->OnBecomeLeader();
      },
      /* on_step_down */
      [this](uint64_t term) {
        system_info_->SetCurrentView(term);
        commitment_->OnStepDown();
      });

  commitment_ = std::make_unique<RaftCommitment>(
      config_, log_.get(), election_mgr_.get(), replica_communicator_,
      system_info_, GetSignatureVerifier(), std::move(executor));
}

ConsensusManagerRaft::ConsensusManagerRaft(
    const ResDBConfig& config, std::unique_ptr<TransactionManager> executor,
    std::unique_ptr<CustomQuery> query_executor)
    : ConsensusManagerRaft(config, std::move(executor), CrossProtocolResources{},
                           std::move(query_executor)) {}

void ConsensusManagerRaft::Start() {
  ConsensusManager::Start();
  election_mgr_->Start();
}

std::vector<ReplicaInfo> ConsensusManagerRaft::GetReplicas() {
  return config_.GetReplicaInfos(config_.GetSelfShard());
}

uint32_t ConsensusManagerRaft::GetPrimary() {
  return system_info_->GetPrimaryId();
}

uint32_t ConsensusManagerRaft::GetVersion() {
  return system_info_->GetCurrentView();
}

void ConsensusManagerRaft::SetPrimary(uint32_t primary, uint64_t version) {
  if (version > system_info_->GetCurrentView()) {
    system_info_->SetCurrentView(version);
    system_info_->SetPrimary(primary);
  }
}

void ConsensusManagerRaft::SetupPerformanceDataFunc(
    std::function<std::string()> func) {
  // No performance manager in Raft; ignore.
  (void)func;
}

int ConsensusManagerRaft::ConsensusCommit(std::unique_ptr<Context> context,
                                          std::unique_ptr<Request> request) {
  LOG(INFO) << "[RAFT] ConsensusCommit type=" << request->type()
            << " sender=" << request->sender_id();
  return InternalConsensusCommit(std::move(context), std::move(request));
}

int ConsensusManagerRaft::InternalConsensusCommit(
    std::unique_ptr<Context> context, std::unique_ptr<Request> request) {
  switch (request->type()) {
    // ── Client-side messages (handled by ResponseManager on proxy) ───────────
    case Request::TYPE_CLIENT_REQUEST:
      if (response_manager_) {
        return response_manager_->NewUserRequest(std::move(context),
                                                 std::move(request));
      }
      return 0;

    case Request::TYPE_RESPONSE:
      if (response_manager_) {
        return response_manager_->ProcessResponseMsg(std::move(context),
                                                     std::move(request));
      }
      return 0;

    // ── Raft replication messages ─────────────────────────────────────────────
    case Request::TYPE_NEW_TXNS:
      return commitment_->ProcessNewRequest(std::move(context),
                                            std::move(request));

    case Request::TYPE_RAFT_APPEND_ENTRIES:
      return commitment_->ProcessAppendEntries(std::move(context),
                                               std::move(request));

    case Request::TYPE_RAFT_APPEND_ENTRIES_RESPONSE:
      return commitment_->ProcessAppendEntriesResponse(std::move(context),
                                                       std::move(request));

    // ── Leader election messages ──────────────────────────────────────────────
    case Request::TYPE_RAFT_VOTE_REQUEST:
      return election_mgr_->ProcessVoteRequest(std::move(context),
                                               std::move(request));

    case Request::TYPE_RAFT_VOTE_RESPONSE:
      return election_mgr_->ProcessVoteResponse(std::move(context),
                                                std::move(request));

    default:
      LOG(WARNING) << "[RAFT] Unknown message type=" << request->type();
      return 0;
  }
}

}  // namespace raft
}  // namespace resdb
