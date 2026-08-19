// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Yatsuki Renka

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace hare {

enum class CreateFileResult {
  kCreated,
  kAlreadyExists,
  kError,
};

struct FileWrite {
  std::filesystem::path path;
  std::string data;
};

enum class FileBatchResult {
  kCommitted,
  kRolledBack,
  // At least one destination could not be restored, or an old backup could not
  // be removed after commit. Its .hare.bak sibling is deliberately retained.
  kRecoveryRequired,
};

// A single replacement is staged beside its destination and renamed only after
// the bytes have reached disk. The batch variant stages every item first and
// restores earlier destinations if a later commit fails. If Windows refuses a
// rollback or backup cleanup, kRecoveryRequired keeps the backup beside its
// destination.
bool WriteFileAtomically(const std::filesystem::path& path,
                         const std::string& data);
CreateFileResult CreateFileAtomically(const std::filesystem::path& path,
                                      const std::string& data);
FileBatchResult WriteFilesAtomically(const std::vector<FileWrite>& writes);

std::string CanonicalS3Prefix(std::string prefix);

enum class ObjectPrefixResult {
  kObject,
  kPrefixMarker,
  kInvalid,
};

ObjectPrefixResult RemoveObjectPrefix(const std::string& key,
                                      const std::string& prefix,
                                      std::string* name);

}  // namespace hare
