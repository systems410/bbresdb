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

#include "platform/consensus/ordering/paxos/query.h"

#include <glog/logging.h>
#include <unistd.h>

namespace resdb {
namespace paxos { 

Query::Query(const ResDBConfig& config, std::unique_ptr<CustomQuery> executor)
    : config_(config),
      custom_query_executor_(std::move(executor)) {}

Query::~Query() {}

int Query::ProcessGetReplicaState(std::unique_ptr<Context> context,
                                  std::unique_ptr<Request> request) {
  ReplicaState replica_state;

  *replica_state.mutable_replica_config() = config_.GetConfigData();

  if (context != nullptr && context->client != nullptr) {
    int ret = context->client->SendRawMessage(replica_state);
    if (ret) {
      LOG(ERROR) << "send resp" << replica_state.DebugString()
                 << " fail ret:" << ret;
    }
  }
  return 0;
}

int Query::ProcessCustomQuery(std::unique_ptr<Context> context,
                              std::unique_ptr<Request> request) {
  if (custom_query_executor_ == nullptr) {
    LOG(ERROR) << "no custom executor";
    return -1;
  }

  std::unique_ptr<std::string> resp_str =
      custom_query_executor_->Query(request->data());

  CustomQueryResponse response;
  if (resp_str != nullptr) {
    response.set_resp_str(*resp_str);
  }

  if (context != nullptr && context->client != nullptr) {
    int ret = context->client->SendRawMessage(response);
    if (ret) {
      LOG(ERROR) << "send resp fail ret:" << ret;
    }
  }
  return 0;
}

} // namespace paxos
}  // namespace resdb
