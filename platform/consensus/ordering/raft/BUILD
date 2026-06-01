#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#

package(default_visibility = ["//visibility:private"])

cc_library(
    name = "raft_log",
    srcs = ["raft_log.cpp"],
    hdrs = ["raft_log.h"],
    deps = [
        "//platform/proto:raft_message_cc_proto",
    ],
)

cc_library(
    name = "election_manager",
    srcs = ["election_manager.cpp"],
    hdrs = ["election_manager.h"],
    deps = [
        ":raft_log",
        "//platform/config:resdb_config",
        "//platform/consensus/ordering/common:transaction_utils",
        "//platform/networkstrate:replica_communicator",
        "//platform/proto:raft_message_cc_proto",
        "//platform/proto:resdb_cc_proto",
    ],
)

cc_library(
    name = "raft_commitment",
    srcs = ["raft_commitment.cpp"],
    hdrs = ["raft_commitment.h"],
    deps = [
        ":election_manager",
        ":raft_log",
        "//executor/common:transaction_manager",
        "//platform/config:resdb_config",
        "//platform/consensus/execution:system_info",
        "//platform/consensus/execution:transaction_executor",
        "//platform/consensus/ordering/common:transaction_utils",
        "//platform/networkstrate:replica_communicator",
        "//platform/proto:raft_message_cc_proto",
        "//platform/proto:resdb_cc_proto",
        "//platform/statistic:stats",
    ],
)

cc_library(
    name = "consensus_manager_raft",
    srcs = ["consensus_manager_raft.cpp"],
    hdrs = ["consensus_manager_raft.h"],
    visibility = [
        "//visibility:public",
    ],
    deps = [
        ":election_manager",
        ":raft_commitment",
        ":raft_log",
        "//executor/common:custom_query",
        "//platform/config:resdb_config",
        "//platform/consensus/ordering/pbft:response_manager",
        "//platform/networkstrate:consensus_manager",
    ],
)
