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

#include <map>
#include <mutex>

#include "platform/proto/raft_message.pb.h"

namespace resdb {
namespace raft {

// Thread-safe append-only Raft log. Indices start at 1.
class RaftLog {
 public:
  RaftLog();

  // Append an entry to the log. Returns the index of the appended entry.
  uint64_t Append(uint64_t term, const RaftLogEntry& entry);

  // Truncate all entries with index > last_index (used on log conflict).
  void TruncateAfter(uint64_t last_index);

  uint64_t LastIndex() const;
  uint64_t LastTerm() const;

  // Returns the term of the entry at index, or 0 if index is out of range.
  uint64_t TermAt(uint64_t index) const;

  // Returns a copy of the entry at index. Returns false if not present.
  bool GetEntry(uint64_t index, RaftLogEntry* out) const;

  // Returns all entries with index > after_index.
  std::vector<RaftLogEntry> EntriesAfter(uint64_t after_index) const;

 private:
  mutable std::mutex mu_;
  // 1-indexed: log_[i] is entry at log position i
  std::map<uint64_t, RaftLogEntry> entries_;
};

}  // namespace raft
}  // namespace resdb
