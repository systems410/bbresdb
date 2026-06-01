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

#include "platform/consensus/ordering/raft/raft_log.h"

namespace resdb {
namespace raft {

RaftLog::RaftLog() = default;

uint64_t RaftLog::Append(uint64_t term, const RaftLogEntry& entry) {
  std::lock_guard<std::mutex> lk(mu_);
  uint64_t index = entries_.empty() ? 1 : entries_.rbegin()->first + 1;
  RaftLogEntry e = entry;
  e.set_term(term);
  e.set_seq(index);
  entries_[index] = std::move(e);
  return index;
}

void RaftLog::TruncateAfter(uint64_t last_index) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = entries_.upper_bound(last_index);
  entries_.erase(it, entries_.end());
}

uint64_t RaftLog::LastIndex() const {
  std::lock_guard<std::mutex> lk(mu_);
  return entries_.empty() ? 0 : entries_.rbegin()->first;
}

uint64_t RaftLog::LastTerm() const {
  std::lock_guard<std::mutex> lk(mu_);
  return entries_.empty() ? 0 : entries_.rbegin()->second.term();
}

uint64_t RaftLog::TermAt(uint64_t index) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = entries_.find(index);
  return (it == entries_.end()) ? 0 : it->second.term();
}

bool RaftLog::GetEntry(uint64_t index, RaftLogEntry* out) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = entries_.find(index);
  if (it == entries_.end()) return false;
  *out = it->second;
  return true;
}

std::vector<RaftLogEntry> RaftLog::EntriesAfter(uint64_t after_index) const {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<RaftLogEntry> result;
  for (auto it = entries_.upper_bound(after_index); it != entries_.end(); ++it) {
    result.push_back(it->second);
  }
  return result;
}

}  // namespace raft
}  // namespace resdb
