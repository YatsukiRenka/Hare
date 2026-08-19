// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Yatsuki Renka

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace hare {

struct RemoteSnapshot {
  std::string name;
  std::wstring installation;
  std::wstring file;
  std::wstring local_name;
};

// Remote object names become local paths. Keep their validation and selection
// in one small module so every backend reaches the same path-safety boundary.
bool IsPlainComponent(const std::string& component);
bool SplitSnapshotName(const std::string& name,
                       std::string* installation,
                       std::string* file);
bool LooksLikeEncryptedSnapshot(const std::string& name);
bool IsSyncableSnapshot(const std::filesystem::path& file);
bool SelectRemoteSnapshots(const std::vector<std::string>& names,
                           std::vector<RemoteSnapshot>* snapshots);

}  // namespace hare
