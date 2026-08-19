// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Yatsuki Renka

#pragma once

#include <windows.h>
#include <dpapi.h>

#include <cwchar>
#include <vector>

#pragma comment(lib, "crypt32.lib")

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
constexpr const wchar_t* kCachedDataKeyValue = L"DataKey";
constexpr const wchar_t* kCachedDataKeyIdentityValue = L"DataKeyIdentity";

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

inline bool CloudSyncStringPresent(const wchar_t* value) {
  DWORD size = 0;
  return RegGetValueW(HKEY_CURRENT_USER, kCloudSyncKey, value, RRF_RT_REG_SZ,
                      nullptr, nullptr, &size) == ERROR_SUCCESS &&
         size > sizeof(wchar_t);
}

inline bool CloudSyncProtectedValuePresent(const wchar_t* value,
                                           DWORD expected_size = 0) {
  DWORD size = 0;
  if (RegGetValueW(HKEY_CURRENT_USER, kCloudSyncKey, value, RRF_RT_REG_BINARY,
                   nullptr, nullptr, &size) != ERROR_SUCCESS ||
      size == 0) {
    return false;
  }

  std::vector<BYTE> encrypted(size);
  if (RegGetValueW(HKEY_CURRENT_USER, kCloudSyncKey, value, RRF_RT_REG_BINARY,
                   nullptr, encrypted.data(), &size) != ERROR_SUCCESS) {
    return false;
  }

  DATA_BLOB input = {size, encrypted.data()};
  DATA_BLOB plain = {};
  if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, 0,
                          &plain))
    return false;
  const bool present = plain.cbData != 0 &&
                       (expected_size == 0 || plain.cbData == expected_size);
  SecureZeroMemory(plain.pbData, plain.cbData);
  LocalFree(plain.pbData);
  return present;
}

inline bool CloudSyncBinaryValuePresent(const wchar_t* value,
                                        DWORD expected_size) {
  DWORD size = 0;
  return RegGetValueW(HKEY_CURRENT_USER, kCloudSyncKey, value,
                      RRF_RT_REG_BINARY, nullptr, nullptr,
                      &size) == ERROR_SUCCESS &&
         size == expected_size;
}

// This is shared by the deployer and server so a half-entered backend has the
// same meaning in manual and scheduled sync.
inline bool CloudSyncConfigured() {
  wchar_t backend[32] = {0};
  DWORD size = sizeof(backend);
  if (RegGetValueW(HKEY_CURRENT_USER, kCloudSyncKey, kBackendValue,
                   RRF_RT_REG_SZ, nullptr, backend, &size) != ERROR_SUCCESS) {
    return false;
  }
  if (wcscmp(backend, L"localdir") == 0)
    return CloudSyncStringPresent(L"LocalDir");
  if (wcscmp(backend, L"s3") == 0) {
    return CloudSyncStringPresent(L"Endpoint") &&
           CloudSyncStringPresent(L"Bucket") &&
           CloudSyncProtectedValuePresent(L"AccessKeyId") &&
           CloudSyncProtectedValuePresent(L"SecretAccessKey");
  }
  if (wcscmp(backend, L"webdav") == 0) {
    return CloudSyncStringPresent(L"DavUrl") &&
           CloudSyncStringPresent(L"DavUsername") &&
           CloudSyncProtectedValuePresent(L"DavPassword");
  }
  if (wcscmp(backend, L"worker") == 0) {
    return CloudSyncStringPresent(L"WorkerUrl") &&
           CloudSyncProtectedValuePresent(L"WorkerToken");
  }
  return false;
}

inline bool CloudSyncReady() {
  return CloudSyncConfigured() &&
         CloudSyncProtectedValuePresent(kCachedDataKeyValue, 32) &&
         CloudSyncBinaryValuePresent(kCachedDataKeyIdentityValue, 32);
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
