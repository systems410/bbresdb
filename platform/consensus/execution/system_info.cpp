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

#include "platform/consensus/execution/system_info.h"

#include <glog/logging.h>

namespace resdb {

SystemInfo::SystemInfo() : primary_id_(1), view_(1) {}

SystemInfo::SystemInfo(const ResDBConfig& config)
    : self_shard_(config.GetSelfShard()), view_(1) {

  if (self_shard_ == 0) { 
    primary_id_ = 0; 
  } else {
    primary_id_ = (config.GetReplicaInfos(self_shard_)[0].id());
  }

  for (uint32_t shard_id : config.GetShardIds()) {
      shard_primary_ids_[shard_id] = config.GetReplicaInfos(shard_id)[0].id();
  }

  SetReplicas(config.GetReplicaInfos(self_shard_));
  LOG(ERROR) << "get primary id:" << primary_id_;
}

void SystemInfo::SetCrossShardPrimaryId(uint32_t id) { 
  cross_shard_primary_id_ = id; 
}

uint32_t SystemInfo::GetCrossShardPrimaryId() const { 
  return cross_shard_primary_id_;
}

uint32_t SystemInfo::GetPrimaryIdOfShard(uint32_t shard_id) {
  std::lock_guard<std::mutex> lock(shard_primary_ids_mut_);
  auto it = shard_primary_ids_.find(shard_id);
  if (it == shard_primary_ids_.end()) { 
    return 0; 
  }
  return it->second; 
}

std::set<uint32_t> SystemInfo::GetAllShardPrimaryIds() { 
  std::lock_guard<std::mutex> lock(shard_primary_ids_mut_);
  std::set<uint32_t> all_primaries; 
  for (const auto& [shard_id, replica_id] : shard_primary_ids_) { 
    all_primaries.insert(replica_id);
  }
  return all_primaries; 
}

void SystemInfo::SetPrimaryOfShard(uint32_t shard_id, uint32_t id) {
  std::lock_guard<std::mutex> lock(shard_primary_ids_mut_);
  shard_primary_ids_[shard_id] = id;  
}

uint32_t SystemInfo::GetPrimaryId() const { return primary_id_; }

void SystemInfo::SetPrimary(uint32_t id) { 
  primary_id_ = id; 
  std::lock_guard<std::mutex> lock(shard_primary_ids_mut_);
  shard_primary_ids_[self_shard_] = id;  
}

uint64_t SystemInfo::GetCurrentView() const { return view_; }

void SystemInfo::SetCurrentView(uint64_t view_id) { view_ = view_id; }

std::vector<ReplicaInfo> SystemInfo::GetReplicas() const { return replicas_; }

void SystemInfo::SetReplicas(const std::vector<ReplicaInfo>& replicas) {
  replicas_ = replicas;
}

void SystemInfo::AddReplica(const ReplicaInfo& replica) {
  if (replica.id() == 0 || replica.ip().empty() || replica.port() == 0) {
    return;
  }
  for (const auto& cur_replica : replicas_) {
    if (cur_replica.id() == replica.id()) {
      LOG(ERROR) << " replica exist:" << replica.id();
      return;
    }
  }
  LOG(ERROR) << "add new replica:" << replica.DebugString();
  replicas_.push_back(replica);
}

void SystemInfo::ProcessRequest(const SystemInfoRequest& request) {
  switch (request.type()) {
    case SystemInfoRequest::ADD_REPLICA: {
      NewReplicaRequest info;
      if (info.ParseFromString(request.request())) {
        AddReplica(info.replica_info());
      }
    } break;
    default:
      break;
  }
}

}  // namespace resdb
