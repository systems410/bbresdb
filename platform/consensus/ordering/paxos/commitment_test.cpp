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

#include "platform/consensus/ordering/paxos/commitment.h"

#include <glog/logging.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <future>

#include "common/crypto/mock_signature_verifier.h"
#include "common/test/test_macros.h"
#include "interface/rdbc/mock_net_channel.h"
#include "platform/config/resdb_config_utils.h"
#include "platform/consensus/ordering/paxos/checkpoint_manager.h"
#include "platform/consensus/ordering/paxos/message_manager.h"
#include "platform/networkstrate/mock_replica_communicator.h"

namespace resdb {
namespace {

using ::resdb::testing::EqualsProto;
using ::testing::Invoke;
using ::testing::Return;
using ::testing::Test;

ResDBConfig GenerateConfig() {
  ResConfigData data;
  data.set_duplicate_check_frequency_useconds(100000);
  return ResDBConfig({GenerateReplicaInfo(1, "127.0.0.1", 1234, 1),
                      GenerateReplicaInfo(2, "127.0.0.1", 1235, 1),
                      GenerateReplicaInfo(3, "127.0.0.1", 1236, 1),
                      GenerateReplicaInfo(4, "127.0.0.1", 1237, 1)},
                      GenerateReplicaInfo(1, "127.0.0.1", 1234, 1), data);
}

class CommitmentTest : public Test {
 public:
  CommitmentTest()
      :  // just set the monitor time to 1 second to return early.
        global_stats_(Stats::GetGlobalStats(1)),
        config_(GenerateConfig()),
        system_info_(config_),
        message_manager_(std::make_unique<paxos::MessageManager>(
            config_, nullptr, &system_info_)),
        commitment_(
            std::make_unique<paxos::Commitment>(config_, message_manager_.get(),
                                         &replica_communicator_, &verifier_, &system_info_)) {}

  std::unique_ptr<Context> GetContext() {
    auto context = std::make_unique<Context>();
    context->signature.set_signature("signature");
    return context;
  }

  int AddProposeMsg(int sender_id, int paxos_id, bool need_resp = false, int proxy_id = 1) {
    auto context = std::make_unique<Context>();
    context->signature.set_signature("signature");

    Request request;
    request.set_seq(1);
    request.set_current_view(1);
    request.set_type(Request::TYPE_PAXOS_PREPARE);
    request.set_sender_id(sender_id);
    request.set_paxos_id(paxos_id);
    request.set_sender_shard_id(1);
    request.set_need_response(need_resp);
    request.set_proxy_id(proxy_id);
    request.set_data(data_);

    return commitment_->ProcessPrepareMsg(std::move(context),
                                          std::make_unique<Request>(request));
  }

  int AddAcceptRequestMsg(int sender_id, int paxos_id, bool need_resp = false, int proxy_id = 1) {
    auto context = std::make_unique<Context>();
    context->signature.set_signature("signature");

    Request request;
    request.set_seq(1);
    request.set_current_view(1);
    request.set_type(Request::TYPE_PAXOS_ACCEPT_REQUEST);
    request.set_sender_id(sender_id);
    request.set_paxos_id(paxos_id);
    request.set_sender_shard_id(1);
    request.set_need_response(need_resp);
    request.set_proxy_id(proxy_id);
    request.set_data(data_);

    return commitment_->ProcessAcceptRequestMsg(std::move(context),
                                                std::make_unique<Request>(request));
  }



  int AddCommitMsg(int sender_id) {
    auto context = std::make_unique<Context>();
    context->signature.set_signature("signature");

    Request request;
    request.set_current_view(1);
    request.set_seq(1);
    request.set_type(Request::TYPE_COMMIT);
    request.set_sender_id(sender_id);
    request.set_sender_shard_id(1);
    return commitment_->ProcessCommitMsg(std::move(context),
                                         std::make_unique<Request>(request));
  }

 protected:
  Stats* global_stats_;
  ResDBConfig config_;
  SystemInfo system_info_;
  MockReplicaCommunicator replica_communicator_;
  MockSignatureVerifier verifier_;
  std::unique_ptr<paxos::MessageManager> message_manager_;
  std::unique_ptr<paxos::Commitment> commitment_;
  std::string data_;
};


TEST_F(CommitmentTest, ProcessPrepareMessage) { 
  EXPECT_EQ(AddProposeMsg(1, 2), 0);
}


TEST_F(CommitmentTest, ProcessPrepareMessageIgnore) { 
  std::promise<bool> done;
  std::future<bool> done_future = done.get_future();


  Request request;
  request.set_seq(1);
  request.set_current_view(1);
  request.set_type(Request::TYPE_PAXOS_PROMISE);
  request.set_sender_id(1);
  request.set_paxos_id(2);
  request.set_sender_shard_id(1);
  request.set_need_response(false);
  request.set_proxy_id(1);
  *request.mutable_data_signature() = {};
  request.set_data(data_);

  EXPECT_CALL(replica_communicator_, SendMessage(EqualsProto(request), 1))
      .WillOnce(Invoke(
          [&](const google::protobuf::Message& request, int64_t node_id) {
            done.set_value(true);
            return 0;
          }));

  EXPECT_EQ(AddProposeMsg(1, 2), 0);
  // Should ignore this propose 
  EXPECT_EQ(AddProposeMsg(1, 1), 0);
  // done_future.get();
} 

TEST_F(CommitmentTest, ProcessPromiseMsgAccepted) { 
  std::promise<bool> done;
  std::future<bool> done_future = done.get_future();

  Request request;
  request.set_seq(1);
  request.set_current_view(1);
  request.set_type(Request::TYPE_PAXOS_PROMISE);
  request.set_sender_id(1);
  request.set_paxos_id(3);
  request.set_accepted_paxos_id(2);
  request.set_accepted_node_id(1);
  request.set_sender_shard_id(1);
  request.set_need_response(false);
  request.set_proxy_id(1);
  *request.mutable_data_signature() = {};
  request.set_data(data_);

  EXPECT_CALL(replica_communicator_, SendMessage(EqualsProto(request), 1))
      .Times(::testing::AtLeast(1))
      .WillRepeatedly(Invoke(
          [&](const google::protobuf::Message& request, int64_t node_id) {
            // done.set_value(true);
            return 0;
          }));

  EXPECT_EQ(AddProposeMsg(1, 2), 0);
  EXPECT_EQ(AddAcceptRequestMsg(1, 2), 0);
  EXPECT_EQ(AddProposeMsg(1, 3), 0);

  // done_future.get();
} 



}  // namespace

}  // namespace resdb
