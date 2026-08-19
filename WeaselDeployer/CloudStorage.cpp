// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Yatsuki Renka

#include "stdafx.h"
#include "CloudStorage.h"

#include <utility>

namespace fs = std::filesystem;

namespace hare {

namespace {

struct StagedFile {
  fs::path target;
  fs::path temporary;
  fs::path backup;
  bool target_existed = false;
  bool committed = false;
};

volatile LONG g_next_file_id = 0;

fs::path UniqueSibling(const fs::path& path, const wchar_t* label) {
  fs::path sibling = path;
  sibling += L".hare.";
  sibling += label;
  sibling += L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
             std::to_wstring(GetCurrentThreadId()) + L"." +
             std::to_wstring(InterlockedIncrement(&g_next_file_id));
  return sibling;
}

bool PathExists(const fs::path& path, bool* exists) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES) {
    *exists = true;
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
  }
  const DWORD error = GetLastError();
  if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
    *exists = false;
    return true;
  }
  return false;
}

bool WriteHandle(HANDLE file, const std::string& data) {
  size_t offset = 0;
  while (offset < data.size()) {
    const size_t remaining = data.size() - offset;
    const DWORD requested = remaining > static_cast<size_t>(MAXDWORD)
                                ? MAXDWORD
                                : static_cast<DWORD>(remaining);
    DWORD written = 0;
    if (!WriteFile(file, data.data() + offset, requested, &written, nullptr) ||
        written == 0) {
      return false;
    }
    offset += written;
  }
  return FlushFileBuffers(file) != FALSE;
}

bool StageFile(const fs::path& target,
               const std::string& data,
               StagedFile* staged) {
  *staged = {};
  staged->target = target;

  std::error_code ec;
  fs::create_directories(target.parent_path(), ec);
  if (ec)
    return false;

  HANDLE file = INVALID_HANDLE_VALUE;
  fs::path temporary;
  for (int attempt = 0; attempt < 64; ++attempt) {
    temporary = UniqueSibling(target, L"tmp");
    file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE)
      break;
    const DWORD error = GetLastError();
    if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
      return false;
  }
  if (file == INVALID_HANDLE_VALUE)
    return false;
  staged->temporary = temporary;

  const bool wrote = WriteHandle(file, data);
  const bool closed = CloseHandle(file) != FALSE;
  if (!wrote || !closed)
    return false;

  bool target_existed = false;
  if (!PathExists(target, &target_existed))
    return false;
  staged->target_existed = target_existed;
  return true;
}

bool CommitFile(StagedFile* staged) {
  if (!staged->target_existed) {
    if (!MoveFileExW(staged->temporary.c_str(), staged->target.c_str(),
                     MOVEFILE_WRITE_THROUGH)) {
      return false;
    }
    staged->temporary.clear();
    staged->committed = true;
    return true;
  }

  for (int attempt = 0; attempt < 64; ++attempt) {
    staged->backup = UniqueSibling(staged->target, L"bak");
    if (ReplaceFileW(staged->target.c_str(), staged->temporary.c_str(),
                     staged->backup.c_str(), REPLACEFILE_WRITE_THROUGH, nullptr,
                     nullptr)) {
      staged->temporary.clear();
      staged->committed = true;
      return true;
    }
    const DWORD error = GetLastError();
    staged->backup.clear();
    if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
      return false;
  }
  return false;
}

bool RollBackFile(StagedFile* staged) {
  if (!staged->committed)
    return true;

  bool restored = false;
  if (staged->target_existed) {
    restored = !staged->backup.empty() &&
               MoveFileExW(staged->backup.c_str(), staged->target.c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (restored)
      staged->backup.clear();
  } else {
    staged->temporary = UniqueSibling(staged->target, L"rollback");
    restored = MoveFileExW(staged->target.c_str(), staged->temporary.c_str(),
                           MOVEFILE_WRITE_THROUGH) != FALSE;
  }
  if (restored)
    staged->committed = false;
  return restored;
}

bool DeleteIfPresent(const fs::path& path) {
  if (path.empty())
    return true;
  for (int attempt = 0; attempt < 4; ++attempt) {
    if (DeleteFileW(path.c_str()))
      return true;
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
      return true;
    if (attempt != 3)
      Sleep(10);
  }
  return false;
}

bool CleanUncommittedFiles(StagedFile* staged) {
  if (staged->committed)
    return false;  // Preserve recovery material if rollback could not complete.
  bool cleaned = true;
  if (DeleteIfPresent(staged->temporary)) {
    staged->temporary.clear();
  } else {
    cleaned = false;
  }
  if (DeleteIfPresent(staged->backup)) {
    staged->backup.clear();
  } else {
    cleaned = false;
  }
  return cleaned;
}

}  // namespace

bool WriteFileAtomically(const fs::path& path, const std::string& data) {
  return WriteFilesAtomically({{path, data}}) == FileBatchResult::kCommitted;
}

CreateFileResult CreateFileAtomically(const fs::path& path,
                                       const std::string& data) {
  StagedFile staged;
  if (!StageFile(path, data, &staged)) {
    CleanUncommittedFiles(&staged);
    return CreateFileResult::kError;
  }

  if (MoveFileExW(staged.temporary.c_str(), path.c_str(),
                  MOVEFILE_WRITE_THROUGH)) {
    staged.temporary.clear();
    return CreateFileResult::kCreated;
  }

  const DWORD error = GetLastError();
  if (!CleanUncommittedFiles(&staged))
    return CreateFileResult::kError;
  if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
    return CreateFileResult::kAlreadyExists;
  return CreateFileResult::kError;
}

FileBatchResult WriteFilesAtomically(const std::vector<FileWrite>& writes) {
  std::vector<StagedFile> staged(writes.size());
  size_t prepared = 0;
  for (; prepared < writes.size(); ++prepared) {
    if (!StageFile(writes[prepared].path, writes[prepared].data,
                    &staged[prepared])) {
      bool cleaned = CleanUncommittedFiles(&staged[prepared]);
      for (size_t i = 0; i < prepared; ++i)
        cleaned = CleanUncommittedFiles(&staged[i]) && cleaned;
      return cleaned ? FileBatchResult::kRolledBack
                     : FileBatchResult::kRecoveryRequired;
    }
  }

  size_t committed = 0;
  for (; committed < staged.size(); ++committed) {
    if (!CommitFile(&staged[committed])) {
      bool restored = true;
      for (size_t i = committed; i != 0; --i)
        restored = RollBackFile(&staged[i - 1]) && restored;
      bool cleaned = true;
      for (StagedFile& file : staged)
        cleaned = CleanUncommittedFiles(&file) && cleaned;
      return restored && cleaned ? FileBatchResult::kRolledBack
                                 : FileBatchResult::kRecoveryRequired;
    }
  }

  bool cleaned = true;
  for (StagedFile& file : staged) {
    if (DeleteIfPresent(file.backup)) {
      file.backup.clear();
      file.committed = false;
    } else {
      cleaned = false;
    }
  }
  return cleaned ? FileBatchResult::kCommitted
                 : FileBatchResult::kRecoveryRequired;
}

std::string CanonicalS3Prefix(std::string prefix) {
  if (prefix.empty())
    return "hare/";
  if (prefix.back() != '/')
    prefix.push_back('/');
  return prefix;
}

ObjectPrefixResult RemoveObjectPrefix(const std::string& key,
                                      const std::string& prefix,
                                      std::string* name) {
  if (!name)
    return ObjectPrefixResult::kInvalid;
  name->clear();
  if (prefix.empty() || key.size() < prefix.size() ||
      key.compare(0, prefix.size(), prefix) != 0) {
    return ObjectPrefixResult::kInvalid;
  }
  if (key.size() == prefix.size())
    return ObjectPrefixResult::kPrefixMarker;
  *name = key.substr(prefix.size());
  return ObjectPrefixResult::kObject;
}

}  // namespace hare
