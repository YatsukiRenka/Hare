// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Yatsuki Renka

#pragma once

#include <windows.h>

// Where the cloud sync configuration lives, and the two settings the scheduler
// needs.
//
// The deployer owns this key: it reads the whole configuration and the settings
// panel writes it. The server only asks three questions - is sync configured,
// how often should it run, and should it run at startup - so those answers live
// here rather than being duplicated in both projects.
//
// The key is deliberately outside the Rime user directory. That directory is
// itself synchronised, so a credential stored there would be uploaded, and
// reaching the cloud would depend on having already reached it.

namespace hare {

constexpr const wchar_t* kCloudSyncKey = L"Software\\Rime\\Hare\\CloudSync";

constexpr const wchar_t* kBackendValue = L"Backend";
constexpr const wchar_t* kIntervalMinutesValue = L"SyncIntervalMinutes";
constexpr const wchar_t* kSyncOnStartupValue = L"SyncOnStartup";

constexpr DWORD kDefaultIntervalMinutes = 60;

// An interval of zero means scheduled synchronisation is off; the tray menu
// entry and the settings panel still sync on demand.
constexpr DWORD kMaxIntervalMinutes = 24 * 60;

inline DWORD ReadCloudSyncDword(const wchar_t* value, DWORD fallback) {
  DWORD data = 0;
  DWORD size = sizeof(data);
  if (RegGetValueW(HKEY_CURRENT_USER, kCloudSyncKey, value, RRF_RT_REG_DWORD,
                   nullptr, &data, &size) != ERROR_SUCCESS) {
    return fallback;
  }
  return data;
}

inline bool CloudSyncConfigured() {
  wchar_t backend[32] = {0};
  DWORD size = sizeof(backend);
  if (RegGetValueW(HKEY_CURRENT_USER, kCloudSyncKey, kBackendValue,
                   RRF_RT_REG_SZ, nullptr, backend, &size) != ERROR_SUCCESS) {
    return false;
  }
  return backend[0] != L'\0' && wcscmp(backend, L"none") != 0;
}

inline DWORD SyncIntervalMinutes() {
  const DWORD minutes =
      ReadCloudSyncDword(kIntervalMinutesValue, kDefaultIntervalMinutes);
  return minutes > kMaxIntervalMinutes ? kMaxIntervalMinutes : minutes;
}

inline bool SyncOnStartup() {
  return ReadCloudSyncDword(kSyncOnStartupValue, 1) != 0;
}

}  // namespace hare
