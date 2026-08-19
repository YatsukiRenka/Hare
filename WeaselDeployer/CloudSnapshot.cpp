// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Yatsuki Renka

#include "stdafx.h"
#include "CloudSnapshot.h"

#include <WeaselUtility.h>

#include <utility>

namespace fs = std::filesystem;

namespace hare {

bool IsPlainComponent(const std::string& component) {
  if (component.empty() || component.size() > 255 || component == "." ||
      component == "..") {
    return false;
  }
  if (component.find_first_of("/\\:*?\"<>|") != std::string::npos)
    return false;
  if (component.back() == '.' || component.back() == ' ')
    return false;
  for (unsigned char ch : component) {
    if (ch < 0x20)
      return false;
  }

  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, component.data(),
                          static_cast<int>(component.size()), nullptr,
                          0) == 0) {
    return false;
  }

  std::string stem = component.substr(0, component.find('.'));
  for (char& ch : stem) {
    if (ch >= 'a' && ch <= 'z')
      ch -= 'a' - 'A';
  }
  if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL")
    return false;
  if (stem.size() == 4 && stem[3] >= '1' && stem[3] <= '9' &&
      (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0)) {
    return false;
  }
  return true;
}

bool SplitSnapshotName(const std::string& name,
                       std::string* installation,
                       std::string* file) {
  const size_t slash = name.find('/');
  if (slash == std::string::npos || slash != name.rfind('/'))
    return false;
  *installation = name.substr(0, slash);
  *file = name.substr(slash + 1);
  return IsPlainComponent(*installation) && IsPlainComponent(*file);
}

bool LooksLikeEncryptedSnapshot(const std::string& name) {
  const size_t slash = name.find('/');
  if (slash == std::string::npos || slash == 0 || slash != name.rfind('/') ||
      slash + 1 == name.size()) {
    return false;
  }
  const std::string file = name.substr(slash + 1);
  constexpr const char* kSuffix = ".userdb.txt";
  constexpr size_t kSuffixLength = 11;
  return file.size() > kSuffixLength &&
         file.compare(file.size() - kSuffixLength, kSuffixLength, kSuffix) ==
             0 &&
         file.rfind("replacer", 0) != 0;
}

bool IsSyncableSnapshot(const fs::path& file) {
  const std::wstring name = file.filename().wstring();
  constexpr size_t kSuffixLength = 11;  // ".userdb.txt"
  if (name.size() <= kSuffixLength)
    return false;
  if (name.rfind(L".userdb.txt") != name.size() - kSuffixLength)
    return false;
  return name.rfind(L"replacer", 0) != 0;
}

bool SelectRemoteSnapshots(const std::vector<std::string>& names,
                           std::vector<RemoteSnapshot>* snapshots) {
  for (const std::string& name : names) {
    std::string installation;
    std::string file;
    if (!SplitSnapshotName(name, &installation, &file))
      continue;
    if (!IsSyncableSnapshot(fs::path(u8tow(file))))
      continue;

    RemoteSnapshot snapshot;
    snapshot.name = name;
    snapshot.installation = u8tow(installation);
    snapshot.file = u8tow(file);
    snapshot.local_name = snapshot.installation + L"\\" + snapshot.file;
    for (const RemoteSnapshot& existing : *snapshots) {
      const int comparison =
          CompareStringOrdinal(snapshot.local_name.c_str(), -1,
                               existing.local_name.c_str(), -1, TRUE);
      if (comparison == 0 || comparison == CSTR_EQUAL)
        return false;
    }
    snapshots->push_back(std::move(snapshot));
  }
  return true;
}

}  // namespace hare
