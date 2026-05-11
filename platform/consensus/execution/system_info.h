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

#include <memory>

#include "platform/config/resdb_config.h"
#include "platform/proto/resdb.pb.h"

namespace resdb {

// SystemInfo managers the cluster information which
// has been agreed on, like the primary, the replicas,etc..
class SystemInfo {
 public:
  SystemInfo();
  SystemInfo(const ResDBConfig& config);
  virtual ~SystemInfo() = default;

  std::vector<ReplicaInfo> GetReplicas() const;
  void SetReplicas(const std::vector<ReplicaInfo>& replicas);
  void AddReplica(const ReplicaInfo& replica);

  void ProcessRequest(const SystemInfoRequest& request);

  uint32_t GetPrimaryId() const;
  uint32_t GetPrimaryIdOfShard(uint32_t shard_id); 
  void SetPrimary(uint32_t id);
  void SetPrimaryOfShard(uint32_t shard_id, uint32_t id);

  uint64_t GetCurrentView() const;
  void SetCurrentView(uint64_t);

 private:
  uint32_t self_shard_; 
  std::vector<ReplicaInfo> replicas_;

  // All shard primary ids 
  std::mutex shard_primary_ids_mut_; 
  std::unordered_map<uint32_t, uint32_t> shard_primary_ids_; 

  // Represents this shards primary ids 
  std::atomic<uint32_t> primary_id_;
  std::atomic<uint64_t> view_;
};
}  // namespace resdb
