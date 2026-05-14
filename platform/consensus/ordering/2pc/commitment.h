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

#include "platform/common/queue/batch_queue.h"
#include "platform/config/resdb_config.h"
#include "platform/consensus/execution/duplicate_manager.h"
#include "platform/consensus/ordering/2pc/message_manager.h"
#include "platform/networkstrate/replica_communicator.h"
#include "platform/statistic/stats.h"

namespace resdb {

namespace twopc 
{

class Commitment {
 public:
  Commitment(const ResDBConfig& config, 
             MessageManager* message_manager,
             ReplicaCommunicator* replica_communicator,
             SignatureVerifier* verifier,  
             SystemInfo* info);
  virtual ~Commitment();

  virtual int ProcessNewRequest(std::unique_ptr<Context> context,
                                std::unique_ptr<Request> user_request);

  virtual int ProcessPrepareMsg(std::unique_ptr<Context> context,
                                std::unique_ptr<Request> request);
  virtual int ProcessCommitMsg(std::unique_ptr<Context> context,
                               std::unique_ptr<Request> request);


  /// @brief Intended for the primary only. Process a vote message, adding a message to the 
  ///        vote count of this transaction. 
  /// @param context 
  /// @param request 
  /// @return 
  int ProcessVoteMsg(std::unique_ptr<Context> context,
                             std::unique_ptr<Request> request);

  int ProcessCommitAckMsg(std::unique_ptr<Context> context, std::unique_ptr<Request> request);

  std::mutex rc_mutex_;

  DuplicateManager* GetDuplicateManager();

 protected:
  std::set<uint32_t> GetShardPrimaryIds(); 

 protected:
 
  ResDBConfig config_;
  SignatureVerifier* verifier_;
  MessageManager* message_manager_;
  SystemInfo* system_info_; 
  ReplicaCommunicator* replica_communicator_;

  Stats* global_stats_;

  std::unique_ptr<DuplicateManager> duplicate_manager_;
};

} // namespace 2pc 
}  // namespace resdb
