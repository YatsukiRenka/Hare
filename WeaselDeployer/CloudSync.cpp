// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Yatsuki Renka

#include "stdafx.h"
#include "CloudSync.h"

#include <HareCloudSync.h>
#include <WeaselUtility.h>
#include <winhttp.h>

#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "CloudCrypto.h"
#include "CloudHttp.h"
#include "CloudSnapshot.h"
#include "CloudStorage.h"
#include "S3Backend.h"
#include "WebDavBackend.h"
#include "WorkerBackend.h"

namespace fs = std::filesystem;

namespace hare {

namespace {

constexpr const wchar_t* kConfigKey = kCloudSyncKey;

// Where the wrapped data key lives inside the storage. It is the one object
// every device reads before anything else.
constexpr const char* kDataKeyName = "keys/dek.bin";

// A valid object is at most 64 MiB, but a malicious listing could otherwise
// make the prepare-before-commit pull retain an unbounded number of them.
constexpr size_t kMaxPullBatchBytes = 256u * 1024u * 1024u;

// Push is deliberately coupled to the preceding pull in the same sync round:
// publishing after an uncertain pull could overwrite good remote snapshots.
thread_local bool g_pull_succeeded = false;
thread_local CloudSyncError g_last_sync_error = CloudSyncError::kNone;

void RecordBackendFailure(const SyncConfig& config) {
  if (config.backend != SyncConfig::Backend::kLocalDir &&
      LastHttpFailure() == HttpFailure::kPayloadTooLarge) {
    g_last_sync_error = CloudSyncError::kObjectTooLarge;
  }
}

std::wstring ReadRegString(const wchar_t* key, const wchar_t* value) {
  DWORD size = 0;
  if (RegGetValueW(HKEY_CURRENT_USER, key, value, RRF_RT_REG_SZ, nullptr,
                   nullptr, &size) != ERROR_SUCCESS ||
      size < sizeof(wchar_t) || size % sizeof(wchar_t) != 0) {
    return {};
  }

  std::vector<wchar_t> buffer(size / sizeof(wchar_t));
  if (RegGetValueW(HKEY_CURRENT_USER, key, value, RRF_RT_REG_SZ, nullptr,
                   buffer.data(), &size) != ERROR_SUCCESS ||
      size < sizeof(wchar_t) || size % sizeof(wchar_t) != 0) {
    return {};
  }

  size_t length = size / sizeof(wchar_t);
  while (length != 0 && buffer[length - 1] == L'\0')
    --length;
  return std::wstring(buffer.data(), length);
}

std::vector<uint8_t> ReadRegBinary(const wchar_t* key, const wchar_t* value) {
  DWORD size = 0;
  if (RegGetValueW(HKEY_CURRENT_USER, key, value, RRF_RT_REG_BINARY, nullptr,
                   nullptr, &size) != ERROR_SUCCESS ||
      size == 0) {
    return {};
  }
  std::vector<uint8_t> data(size);
  if (RegGetValueW(HKEY_CURRENT_USER, key, value, RRF_RT_REG_BINARY, nullptr,
                   data.data(), &size) != ERROR_SUCCESS) {
    return {};
  }
  data.resize(size);
  return data;
}

// Secrets live in the registry under DPAPI rather than in a file, and never in
// the Rime user directory: that directory is itself synchronised, so a
// credential placed there would be uploaded and would also make the
// configuration needed to reach the cloud depend on the cloud.
std::string ReadRegSecret(const wchar_t* key, const wchar_t* value) {
  return Unprotect(ReadRegBinary(key, value));
}

// The result is signed as the canonical host, so it has to match the Host
// header WinHTTP puts on the wire. A non-default port belongs there; a port
// that merely restates the scheme's default does not, because WinHTTP omits it.
std::string HostFromEndpoint(const std::string& endpoint) {
  const size_t scheme_end = endpoint.find("://");
  const std::string scheme =
      scheme_end == std::string::npos ? "" : endpoint.substr(0, scheme_end);
  const size_t begin = scheme_end == std::string::npos ? 0 : scheme_end + 3;
  const size_t slash = endpoint.find('/', begin);
  std::string host = endpoint.substr(
      begin, slash == std::string::npos ? std::string::npos : slash - begin);

  const size_t colon = host.rfind(':');
  if (colon != std::string::npos) {
    const std::string port = host.substr(colon + 1);
    if ((scheme == "https" && port == "443") ||
        (scheme == "http" && port == "80")) {
      host.resize(colon);
    }
  }
  for (char& ch : host) {
    if (ch >= 'A' && ch <= 'Z')
      ch += 'a' - 'A';
  }
  return host;
}

bool IsValidS3Endpoint(const std::string& endpoint) {
  if (endpoint.empty() || endpoint.find('?') != std::string::npos ||
      endpoint.find('#') != std::string::npos) {
    return false;
  }

  const size_t scheme_end = endpoint.find("://");
  if (scheme_end == std::string::npos)
    return false;
  const size_t authority_begin = scheme_end + 3;
  if (endpoint.find('\\', authority_begin) != std::string::npos)
    return false;
  const size_t authority_end = endpoint.find('/', authority_begin);
  if (endpoint
          .substr(authority_begin, authority_end == std::string::npos
                                       ? std::string::npos
                                       : authority_end - authority_begin)
          .find('@') != std::string::npos) {
    return false;
  }

  const std::wstring url = u8tow(endpoint);
  URL_COMPONENTS parts = {};
  parts.dwStructSize = sizeof(parts);
  parts.dwSchemeLength = static_cast<DWORD>(-1);
  parts.dwHostNameLength = static_cast<DWORD>(-1);
  parts.dwUserNameLength = static_cast<DWORD>(-1);
  parts.dwPasswordLength = static_cast<DWORD>(-1);
  parts.dwUrlPathLength = static_cast<DWORD>(-1);
  parts.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) ||
      parts.nScheme != INTERNET_SCHEME_HTTPS || parts.dwHostNameLength == 0 ||
      parts.dwUserNameLength != 0 || parts.dwPasswordLength != 0 ||
      parts.dwExtraInfoLength != 0) {
    return false;
  }
  return parts.dwUrlPathLength == 0 ||
         (parts.dwUrlPathLength == 1 && parts.lpszUrlPath[0] == L'/');
}

// installation.yaml is written by Rime and is a flat mapping, so a line scan
// is enough; pulling in a YAML parser for two keys would not pay for itself.
std::wstring ReadInstallationSetting(const std::string& key) {
  const fs::path path = WeaselUserDataPath() / L"installation.yaml";
  std::ifstream in(path);
  if (!in)
    return {};

  const std::string prefix = key + ":";
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind(prefix, 0) != 0)
      continue;
    std::string value = line.substr(prefix.size());
    const size_t begin = value.find_first_not_of(" \t\"");
    if (begin == std::string::npos)
      return {};
    size_t end = value.find_last_not_of(" \t\r\"");
    return u8tow(value.substr(begin, end - begin + 1));
  }
  return {};
}

// An unreadable file and an empty one must not look alike. Returning an empty
// buffer for both would let a read failure pass for "no data key stored yet",
// and the replacement key would render every existing snapshot unreadable.
enum class FileReadResult { kOk, kError, kPayloadTooLarge };

FileReadResult ReadFileBytes(const fs::path& path,
                             size_t max_bytes,
                             std::vector<uint8_t>* out) {
  out->clear();
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return FileReadResult::kError;

  std::array<char, 64 * 1024> chunk;
  for (;;) {
    in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    const std::streamsize count = in.gcount();
    if (count < 0)
      return FileReadResult::kError;
    const size_t byte_count = static_cast<size_t>(count);
    if (out->size() > max_bytes || byte_count > max_bytes - out->size()) {
      out->clear();
      return FileReadResult::kPayloadTooLarge;
    }
    out->insert(out->end(), chunk.begin(), chunk.begin() + byte_count);
    if (in.eof())
      return FileReadResult::kOk;
    if (!in)
      return FileReadResult::kError;
  }
}

template <typename Integer>
bool IsInteger(std::string_view value) {
  Integer parsed = 0;
  const char* begin = value.data();
  const char* end = begin + value.size();
  const auto result = std::from_chars(begin, end, parsed);
  return result.ec == std::errc() && result.ptr == end;
}

bool IsFiniteNumber(std::string_view value) {
  double parsed = 0;
  const char* begin = value.data();
  const char* end = begin + value.size();
  const auto result =
      std::from_chars(begin, end, parsed, std::chars_format::general);
  return result.ec == std::errc() && result.ptr == end && std::isfinite(parsed);
}

// UserDbValue::Pack writes exactly "c=<int> d=<double> t=<uint64>". Parsing
// that structure catches a truncated row instead of relying on Rime's TSV
// reader, which deliberately skips malformed rows and continues.
bool IsUserDbValue(std::string_view value) {
  if (value.rfind("c=", 0) != 0)
    return false;
  const size_t dee = value.find(" d=", 2);
  if (dee == std::string_view::npos)
    return false;
  const size_t tick = value.find(" t=", dee + 3);
  if (tick == std::string_view::npos)
    return false;
  return IsInteger<int>(value.substr(2, dee - 2)) &&
         IsFiniteNumber(value.substr(dee + 3, tick - dee - 3)) &&
         IsInteger<uint64_t>(value.substr(tick + 3));
}

// Mirrors the plain-userdb format emitted by librime's TsvWriter and
// userdb_entry_formatter. Unknown metadata is allowed because schemes add their
// own fields; LF and CRLF are both accepted so snapshots remain cross-platform.
bool IsUserDictionarySnapshot(const std::string& data) {
  if (data.empty() || data.size() > kMaxSnapshotPlaintextBytes ||
      data.back() != '\n' || data.find('\0') != std::string::npos) {
    return false;
  }
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data.data(),
                          static_cast<int>(data.size()), nullptr, 0) == 0) {
    return false;
  }

  bool saw_header = false;
  bool saw_entry = false;
  bool saw_db_name = false;
  bool saw_db_type = false;
  bool saw_rime_version = false;
  bool saw_tick = false;
  bool saw_user_id = false;

  size_t offset = 0;
  while (offset < data.size()) {
    const size_t newline = data.find('\n', offset);
    if (newline == std::string::npos)
      return false;
    size_t end = newline;
    if (end != offset && data[end - 1] == '\r')
      --end;
    const std::string_view line(data.data() + offset, end - offset);
    if (line.find('\r') != std::string_view::npos)
      return false;
    offset = newline + 1;

    if (!saw_header) {
      if (line != "# Rime user dictionary")
        return false;
      saw_header = true;
      continue;
    }

    if (!saw_entry && line.rfind("#@/", 0) == 0) {
      const size_t tab = line.find('\t', 3);
      if (tab == std::string_view::npos || tab <= 3)
        return false;
      const std::string_view key = line.substr(2, tab - 2);
      const std::string_view value = line.substr(tab + 1);
      if (key == "/db_name") {
        if (saw_db_name || value.empty() ||
            value.find('\t') != std::string_view::npos) {
          return false;
        }
        saw_db_name = true;
      } else if (key == "/db_type") {
        if (saw_db_type || value != "userdb")
          return false;
        saw_db_type = true;
      } else if (key == "/rime_version") {
        if (saw_rime_version || value.empty() ||
            value.find('\t') != std::string_view::npos) {
          return false;
        }
        saw_rime_version = true;
      } else if (key == "/tick") {
        if (saw_tick || !IsInteger<uint64_t>(value))
          return false;
        saw_tick = true;
      } else if (key == "/user_id") {
        if (saw_user_id || value.empty() ||
            value.find('\t') != std::string_view::npos) {
          return false;
        }
        saw_user_id = true;
      }
      continue;
    }

    saw_entry = true;
    if (line.empty() || line.front() == '#')
      return false;
    const size_t first_tab = line.find('\t');
    if (first_tab == std::string_view::npos)
      return false;
    const size_t second_tab = line.find('\t', first_tab + 1);
    if (second_tab == std::string_view::npos ||
        line.find('\t', second_tab + 1) != std::string_view::npos) {
      return false;
    }
    const std::string_view code = line.substr(0, first_tab);
    const std::string_view phrase =
        line.substr(first_tab + 1, second_tab - first_tab - 1);
    const std::string_view value = line.substr(second_tab + 1);
    if (code.empty() || code.back() != ' ' || phrase.empty() ||
        !IsUserDbValue(value)) {
      return false;
    }
  }

  return saw_header && saw_db_name && saw_db_type && saw_rime_version &&
         saw_tick && saw_user_id;
}

class LocalDirBackend : public SyncBackend {
 public:
  explicit LocalDirBackend(std::wstring root) : root_(std::move(root)) {}

  bool List(std::vector<std::string>* names) override {
    std::error_code ec;
    const bool exists = fs::exists(root_, ec);
    if (ec)
      return false;
    if (!exists)
      return true;

    fs::recursive_directory_iterator entry(root_, ec);
    if (ec)
      return false;
    const fs::recursive_directory_iterator end;
    while (entry != end) {
      std::error_code entry_ec;
      const bool regular = entry->is_regular_file(entry_ec);
      if (entry_ec)
        return false;
      if (regular) {
        const fs::path relative = fs::relative(entry->path(), root_, entry_ec);
        if (entry_ec)
          return false;
        std::string name = wtou8(relative.generic_wstring());
        names->push_back(std::move(name));
      }
      entry.increment(ec);
      if (ec)
        return false;
    }
    return true;
  }

  FetchResult Get(const std::string& name, std::vector<uint8_t>* out) override {
    fs::path path;
    if (!ResolvePath(name, &path))
      return FetchResult::kError;
    std::error_code ec;
    const bool exists = fs::exists(path, ec);
    if (ec)
      return FetchResult::kError;
    if (!exists)
      return FetchResult::kNotFound;
    const uintmax_t size = fs::file_size(path, ec);
    if (ec)
      return FetchResult::kError;
    if (size > kMaxCloudObjectBytes)
      return FetchResult::kPayloadTooLarge;
    switch (ReadFileBytes(path, kMaxCloudObjectBytes, out)) {
      case FileReadResult::kOk:
        return FetchResult::kOk;
      case FileReadResult::kPayloadTooLarge:
        return FetchResult::kPayloadTooLarge;
      case FileReadResult::kError:
      default:
        return FetchResult::kError;
    }
  }

  bool Put(const std::string& name, const std::vector<uint8_t>& data) override {
    if (data.size() > kMaxCloudObjectBytes)
      return false;
    fs::path path;
    if (!ResolvePath(name, &path))
      return false;
    return WriteFileAtomically(path, std::string(data.begin(), data.end()));
  }

  PutIfAbsentResult PutIfAbsent(const std::string& name,
                                const std::vector<uint8_t>& data) override {
    if (data.size() > kMaxCloudObjectBytes)
      return PutIfAbsentResult::kError;
    fs::path path;
    if (!ResolvePath(name, &path))
      return PutIfAbsentResult::kError;
    switch (CreateFileAtomically(path, std::string(data.begin(), data.end()))) {
      case CreateFileResult::kCreated:
        return PutIfAbsentResult::kCreated;
      case CreateFileResult::kAlreadyExists:
        return PutIfAbsentResult::kAlreadyExists;
      case CreateFileResult::kError:
      default:
        return PutIfAbsentResult::kError;
    }
  }

  std::wstring Describe() const override {
    return L"local directory " + root_.wstring();
  }

 private:
  bool ResolvePath(const std::string& name, fs::path* path) const {
    std::string installation;
    std::string file;
    if (!SplitSnapshotName(name, &installation, &file))
      return false;

    std::error_code ec;
    const fs::path root = fs::weakly_canonical(root_, ec);
    if (ec)
      return false;
    const fs::path candidate =
        fs::weakly_canonical(root / u8tow(installation) / u8tow(file), ec);
    if (ec)
      return false;

    const fs::path relative = candidate.lexically_relative(root);
    if (relative.empty() || relative == L"." || relative.is_absolute())
      return false;
    for (const fs::path& component : relative) {
      if (component == L"..")
        return false;
    }
    *path = candidate;
    return true;
  }

  fs::path root_;
};

struct RegistryValueWrite {
  std::wstring name;
  DWORD type = REG_NONE;
  std::vector<uint8_t> data;
};

struct RegistryValueSnapshot : RegistryValueWrite {
  bool exists = false;
};

struct RegistrySnapshot {
  bool key_existed = false;
  std::vector<RegistryValueSnapshot> values;
};

struct PendingConfigSave {
  std::vector<RegistryValueWrite> values;
  std::string storage_identity;
};

thread_local std::optional<PendingConfigSave> g_pending_config_save;

constexpr const wchar_t* kTransactionalValueNames[] = {
    kBackendValue,
    L"LocalDir",
    L"Endpoint",
    L"Bucket",
    L"Prefix",
    L"AccessKeyId",
    L"SecretAccessKey",
    L"DavUrl",
    L"DavUsername",
    L"DavPassword",
    L"WorkerUrl",
    L"WorkerToken",
    kIntervalMinutesValue,
    kSyncOnStartupValue,
    kCachedDataKeyValue,
    kCachedDataKeyIdentityValue,
};

bool StageRegString(const wchar_t* name,
                    const std::wstring& data,
                    std::vector<RegistryValueWrite>* values) {
  const size_t max_characters = MAXDWORD / sizeof(wchar_t);
  if (data.size() >= max_characters)
    return false;

  RegistryValueWrite value;
  value.name = name;
  value.type = REG_SZ;
  const size_t byte_count = (data.size() + 1) * sizeof(wchar_t);
  const auto* bytes = reinterpret_cast<const uint8_t*>(data.c_str());
  value.data.assign(bytes, bytes + byte_count);
  values->push_back(std::move(value));
  return true;
}

void StageRegDword(const wchar_t* name,
                   DWORD data,
                   std::vector<RegistryValueWrite>* values) {
  RegistryValueWrite value;
  value.name = name;
  value.type = REG_DWORD;
  const auto* bytes = reinterpret_cast<const uint8_t*>(&data);
  value.data.assign(bytes, bytes + sizeof(data));
  values->push_back(std::move(value));
}

bool StageRegSecret(const wchar_t* name,
                    const std::string& secret,
                    std::vector<RegistryValueWrite>* values) {
  // Blank means unchanged because saved secrets are never returned to the page.
  if (secret.empty())
    return true;
  if (secret.size() > MAXDWORD)
    return false;
  const std::vector<uint8_t> sealed = Protect(secret);
  if (sealed.empty())
    return false;

  RegistryValueWrite value;
  value.name = name;
  value.type = REG_BINARY;
  value.data = sealed;
  values->push_back(std::move(value));
  return true;
}

bool ReadRegistryValue(HKEY key,
                       const wchar_t* name,
                       RegistryValueSnapshot* snapshot) {
  snapshot->name = name;
  for (int attempt = 0; attempt < 4; ++attempt) {
    DWORD type = REG_NONE;
    DWORD size = 0;
    LSTATUS status =
        RegQueryValueExW(key, name, nullptr, &type, nullptr, &size);
    if (status == ERROR_FILE_NOT_FOUND) {
      snapshot->exists = false;
      snapshot->data.clear();
      snapshot->type = REG_NONE;
      return true;
    }
    if (status != ERROR_SUCCESS)
      return false;

    std::vector<uint8_t> data(size);
    DWORD read_size = size;
    DWORD read_type = REG_NONE;
    status = RegQueryValueExW(key, name, nullptr, &read_type,
                              data.empty() ? nullptr : data.data(), &read_size);
    if (status == ERROR_MORE_DATA)
      continue;
    if (status == ERROR_FILE_NOT_FOUND)
      continue;
    if (status != ERROR_SUCCESS)
      return false;

    data.resize(read_size);
    snapshot->exists = true;
    snapshot->type = read_type;
    snapshot->data = std::move(data);
    return true;
  }
  return false;
}

bool CaptureRegistrySnapshot(RegistrySnapshot* snapshot) {
  snapshot->values.clear();
  HKEY key = nullptr;
  const LSTATUS open =
      RegOpenKeyExW(HKEY_CURRENT_USER, kConfigKey, 0, KEY_QUERY_VALUE, &key);
  if (open == ERROR_FILE_NOT_FOUND) {
    snapshot->key_existed = false;
    for (const wchar_t* name : kTransactionalValueNames) {
      RegistryValueSnapshot value;
      value.name = name;
      snapshot->values.push_back(std::move(value));
    }
    return true;
  }
  if (open != ERROR_SUCCESS)
    return false;

  snapshot->key_existed = true;
  bool ok = true;
  for (const wchar_t* name : kTransactionalValueNames) {
    RegistryValueSnapshot value;
    if (!ReadRegistryValue(key, name, &value)) {
      ok = false;
      break;
    }
    snapshot->values.push_back(std::move(value));
  }
  RegCloseKey(key);
  return ok;
}

const RegistryValueSnapshot* FindSnapshotValue(const RegistrySnapshot& snapshot,
                                               const wchar_t* name) {
  for (const RegistryValueSnapshot& value : snapshot.values) {
    if (value.name == name)
      return &value;
  }
  return nullptr;
}

std::wstring SnapshotRegString(const RegistrySnapshot& snapshot,
                               const wchar_t* name) {
  const RegistryValueSnapshot* value = FindSnapshotValue(snapshot, name);
  if (!value || !value->exists || value->type != REG_SZ ||
      value->data.size() < sizeof(wchar_t) ||
      value->data.size() % sizeof(wchar_t) != 0) {
    return {};
  }

  std::wstring result(value->data.size() / sizeof(wchar_t), L'\0');
  std::memcpy(result.data(), value->data.data(), value->data.size());
  while (!result.empty() && result.back() == L'\0')
    result.pop_back();
  return result;
}

bool RestoreRegistrySnapshot(const RegistrySnapshot& snapshot) {
  if (!snapshot.key_existed) {
    const LSTATUS status = RegDeleteKeyW(HKEY_CURRENT_USER, kConfigKey);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
  }

  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kConfigKey, 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return false;
  }

  bool ok = true;
  for (const RegistryValueSnapshot& value : snapshot.values) {
    LSTATUS status = ERROR_SUCCESS;
    if (value.exists) {
      status = RegSetValueExW(key, value.name.c_str(), 0, value.type,
                              value.data.empty() ? nullptr : value.data.data(),
                              static_cast<DWORD>(value.data.size()));
    } else {
      status = RegDeleteValueW(key, value.name.c_str());
      if (status == ERROR_FILE_NOT_FOUND)
        status = ERROR_SUCCESS;
    }
    if (status != ERROR_SUCCESS)
      ok = false;
  }
  if (RegFlushKey(key) != ERROR_SUCCESS)
    ok = false;
  RegCloseKey(key);
  return ok;
}

bool SetRegistryValue(HKEY key, const RegistryValueWrite& value) {
  return RegSetValueExW(key, value.name.c_str(), 0, value.type,
                        value.data.empty() ? nullptr : value.data.data(),
                        static_cast<DWORD>(value.data.size())) == ERROR_SUCCESS;
}

bool WriteRegBinary(const wchar_t* value, const std::vector<uint8_t>& data) {
  if (data.empty() || data.size() > MAXDWORD)
    return false;
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kConfigKey, 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return false;
  }
  LSTATUS status = RegSetValueExW(key, value, 0, REG_BINARY, data.data(),
                                  static_cast<DWORD>(data.size()));
  if (status == ERROR_SUCCESS)
    status = RegFlushKey(key);
  RegCloseKey(key);
  return status == ERROR_SUCCESS;
}

std::wstring BackendName(SyncConfig::Backend backend) {
  switch (backend) {
    case SyncConfig::Backend::kLocalDir:
      return L"localdir";
    case SyncConfig::Backend::kS3:
      return L"s3";
    case SyncConfig::Backend::kWebDav:
      return L"webdav";
    case SyncConfig::Backend::kWorker:
      return L"worker";
    case SyncConfig::Backend::kNone:
    default:
      return L"none";
  }
}

void AppendIdentityField(const std::string& value, std::string* record) {
  const uint32_t length = static_cast<uint32_t>(value.size());
  record->push_back(static_cast<char>((length >> 24) & 0xff));
  record->push_back(static_cast<char>((length >> 16) & 0xff));
  record->push_back(static_cast<char>((length >> 8) & 0xff));
  record->push_back(static_cast<char>(length & 0xff));
  record->append(value);
}

// Credentials are deliberately omitted: rotating one does not change the
// storage. Length-prefixed UTF-8 fields keep distinct tuples distinct and are
// straightforward for tools\configure-sync.ps1 to reproduce.
std::string StorageIdentityRecord(const SyncConfig& config) {
  std::string record = "HARE-STORAGE-ID/1";
  record.push_back('\0');
  switch (config.backend) {
    case SyncConfig::Backend::kLocalDir:
      AppendIdentityField("localdir", &record);
      AppendIdentityField(wtou8(config.local_dir), &record);
      break;
    case SyncConfig::Backend::kS3:
      AppendIdentityField("s3", &record);
      AppendIdentityField(config.endpoint, &record);
      AppendIdentityField(config.bucket, &record);
      AppendIdentityField(CanonicalS3Prefix(config.prefix), &record);
      break;
    case SyncConfig::Backend::kWebDav:
      AppendIdentityField("webdav", &record);
      AppendIdentityField(config.dav_url, &record);
      AppendIdentityField(config.dav_username, &record);
      break;
    case SyncConfig::Backend::kWorker:
      AppendIdentityField("worker", &record);
      AppendIdentityField(config.worker_url, &record);
      break;
    case SyncConfig::Backend::kNone:
    default:
      AppendIdentityField("none", &record);
      break;
  }
  return record;
}

SyncConfig StorageIdentityConfig(const RegistrySnapshot& snapshot) {
  SyncConfig config;
  const std::wstring backend = SnapshotRegString(snapshot, kBackendValue);
  if (backend == L"localdir") {
    config.backend = SyncConfig::Backend::kLocalDir;
    config.local_dir = SnapshotRegString(snapshot, L"LocalDir");
  } else if (backend == L"s3") {
    config.backend = SyncConfig::Backend::kS3;
    config.endpoint = wtou8(SnapshotRegString(snapshot, L"Endpoint"));
    config.bucket = wtou8(SnapshotRegString(snapshot, L"Bucket"));
    config.prefix =
        CanonicalS3Prefix(wtou8(SnapshotRegString(snapshot, L"Prefix")));
  } else if (backend == L"webdav") {
    config.backend = SyncConfig::Backend::kWebDav;
    config.dav_url = wtou8(SnapshotRegString(snapshot, L"DavUrl"));
    config.dav_username = wtou8(SnapshotRegString(snapshot, L"DavUsername"));
  } else if (backend == L"worker") {
    config.backend = SyncConfig::Backend::kWorker;
    config.worker_url = wtou8(SnapshotRegString(snapshot, L"WorkerUrl"));
  }
  return config;
}

std::vector<uint8_t> StorageIdentityFingerprint(const SyncConfig& config) {
  return Sha256(StorageIdentityRecord(config));
}

bool InvalidateCachedDataKey() {
  HKEY key = nullptr;
  LSTATUS status =
      RegOpenKeyExW(HKEY_CURRENT_USER, kConfigKey, 0, KEY_SET_VALUE, &key);
  if (status == ERROR_FILE_NOT_FOUND)
    return true;
  if (status != ERROR_SUCCESS)
    return false;

  status = RegDeleteValueW(key, kCachedDataKeyIdentityValue);
  if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
    RegCloseKey(key);
    return false;
  }
  status = RegDeleteValueW(key, kCachedDataKeyValue);
  if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
    RegCloseKey(key);
    return false;
  }
  const bool flushed = RegFlushKey(key) == ERROR_SUCCESS;
  RegCloseKey(key);
  return flushed;
}

bool CommitStagedSettings(
    std::vector<RegistryValueWrite> values,
    const std::optional<std::string>& next_storage_identity) {
  RegistrySnapshot snapshot;
  if (!CaptureRegistrySnapshot(&snapshot))
    return false;

  const bool identity_changed =
      next_storage_identity &&
      StorageIdentityRecord(StorageIdentityConfig(snapshot)) !=
          *next_storage_identity;

  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kConfigKey, 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return false;
  }

  bool ok = true;
  if (identity_changed) {
    LSTATUS status = RegDeleteValueW(key, kCachedDataKeyIdentityValue);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
      ok = false;
    if (ok) {
      status = RegDeleteValueW(key, kCachedDataKeyValue);
      if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
        ok = false;
    }
    // Make the missing identity durable before any storage field changes. A
    // process interruption can then lose the cache, but cannot make an old DEK
    // look valid for the new storage.
    if (ok && RegFlushKey(key) != ERROR_SUCCESS)
      ok = false;
  }

  // Backend is the selector for all other fields, so publish it last. On a
  // backend switch, readers keep the previous selection until the next
  // backend's dependent fields have been written.
  if (ok) {
    for (const RegistryValueWrite& value : values) {
      if (value.name != kBackendValue && !SetRegistryValue(key, value)) {
        ok = false;
        break;
      }
    }
  }
  if (ok) {
    for (const RegistryValueWrite& value : values) {
      if (value.name == kBackendValue && !SetRegistryValue(key, value)) {
        ok = false;
        break;
      }
    }
  }
  if (ok && RegFlushKey(key) != ERROR_SUCCESS)
    ok = false;
  RegCloseKey(key);

  if (ok)
    return true;

  const bool restored = RestoreRegistrySnapshot(snapshot);
  if (!restored && identity_changed)
    InvalidateCachedDataKey();
  return false;
}

bool CacheDataKeyForStorage(const std::vector<uint8_t>& dek,
                            const SyncConfig& config) {
  const std::vector<uint8_t> fingerprint = StorageIdentityFingerprint(config);
  if (fingerprint.size() != 32 || !InvalidateCachedDataKey())
    return false;
  if (!CacheDataKey(dek))
    return false;
  if (!WriteRegBinary(kCachedDataKeyIdentityValue, fingerprint)) {
    InvalidateCachedDataKey();
    return false;
  }
  return true;
}

// A DPAPI blob alone is bound only to the Windows account. The adjacent
// fingerprint also binds it to the exact storage selected by this round.
std::vector<uint8_t> CachedKeyOrEmpty(const SyncConfig& config) {
  const std::vector<uint8_t> expected = StorageIdentityFingerprint(config);
  const std::vector<uint8_t> stored =
      ReadRegBinary(kConfigKey, kCachedDataKeyIdentityValue);
  if (expected.size() != 32 || stored != expected)
    return {};
  const auto cached = LoadCachedDataKey();
  return cached ? *cached : std::vector<uint8_t>();
}

// `require_complete` decides what happens to a backend that is missing a field
// it needs: a sync has to treat it as switched off, while the panel has to keep
// showing it so the user can finish filling it in.
SyncConfig LoadConfig(bool require_complete) {
  SyncConfig config;
  const std::wstring backend = ReadRegString(kConfigKey, L"Backend");
  if (backend == L"localdir") {
    config.backend = SyncConfig::Backend::kLocalDir;
    config.local_dir = ReadRegString(kConfigKey, L"LocalDir");
  } else if (backend == L"s3") {
    config.backend = SyncConfig::Backend::kS3;
    config.endpoint = wtou8(ReadRegString(kConfigKey, L"Endpoint"));
    config.bucket = wtou8(ReadRegString(kConfigKey, L"Bucket"));
    config.prefix =
        CanonicalS3Prefix(wtou8(ReadRegString(kConfigKey, L"Prefix")));
    config.access_key = ReadRegSecret(kConfigKey, L"AccessKeyId");
    config.secret_key = ReadRegSecret(kConfigKey, L"SecretAccessKey");
  } else if (backend == L"webdav") {
    config.backend = SyncConfig::Backend::kWebDav;
    config.dav_url = wtou8(ReadRegString(kConfigKey, L"DavUrl"));
    config.dav_username = wtou8(ReadRegString(kConfigKey, L"DavUsername"));
    config.dav_password = ReadRegSecret(kConfigKey, L"DavPassword");
  } else if (backend == L"worker") {
    config.backend = SyncConfig::Backend::kWorker;
    config.worker_url = wtou8(ReadRegString(kConfigKey, L"WorkerUrl"));
    config.worker_token = ReadRegSecret(kConfigKey, L"WorkerToken");
  }
  if (require_complete && !config.valid())
    config.backend = SyncConfig::Backend::kNone;
  return config;
}

}  // namespace

CloudSyncError LastCloudSyncError() {
  return g_last_sync_error;
}

SyncConfig SyncConfig::Load() {
  return LoadConfig(true);
}

SyncConfig SyncConfig::LoadForEditing() {
  return LoadConfig(false);
}

bool SyncConfig::valid() const {
  switch (backend) {
    case Backend::kLocalDir:
      return !local_dir.empty();
    case Backend::kS3:
      return IsValidS3Endpoint(endpoint) && !bucket.empty() &&
             !access_key.empty() && !secret_key.empty();
    case Backend::kWebDav:
      return !dav_url.empty() && !dav_username.empty() && !dav_password.empty();
    case Backend::kWorker:
      return !worker_url.empty() && !worker_token.empty();
    case Backend::kNone:
    default:
      return false;
  }
}

bool SyncConfig::Save() const {
  // A failed or abandoned staging attempt must never leak into a later save.
  g_pending_config_save.reset();
  if (backend == Backend::kS3 && !IsValidS3Endpoint(endpoint))
    return false;

  PendingConfigSave pending;
  pending.storage_identity = StorageIdentityRecord(*this);
  switch (backend) {
    case Backend::kLocalDir:
      if (!StageRegString(L"LocalDir", local_dir, &pending.values))
        return false;
      break;
    case Backend::kS3: {
      const std::string canonical_prefix = CanonicalS3Prefix(prefix);
      if (!StageRegString(L"Endpoint", u8tow(endpoint), &pending.values) ||
          !StageRegString(L"Bucket", u8tow(bucket), &pending.values) ||
          !StageRegString(L"Prefix", u8tow(canonical_prefix),
                          &pending.values) ||
          !StageRegSecret(L"AccessKeyId", access_key, &pending.values) ||
          !StageRegSecret(L"SecretAccessKey", secret_key, &pending.values)) {
        return false;
      }
      break;
    }
    case Backend::kWebDav:
      if (!StageRegString(L"DavUrl", u8tow(dav_url), &pending.values) ||
          !StageRegString(L"DavUsername", u8tow(dav_username),
                          &pending.values) ||
          !StageRegSecret(L"DavPassword", dav_password, &pending.values)) {
        return false;
      }
      break;
    case Backend::kWorker:
      if (!StageRegString(L"WorkerUrl", u8tow(worker_url), &pending.values) ||
          !StageRegSecret(L"WorkerToken", worker_token, &pending.values)) {
        return false;
      }
      break;
    case Backend::kNone:
    default:
      break;
  }
  if (!StageRegString(kBackendValue, BackendName(backend), &pending.values))
    return false;
  g_pending_config_save = std::move(pending);
  return true;
}

bool SaveSyncSchedule(unsigned interval_minutes, bool on_startup) {
  const DWORD minutes = interval_minutes > kMaxIntervalMinutes
                            ? kMaxIntervalMinutes
                            : static_cast<DWORD>(interval_minutes);
  std::vector<RegistryValueWrite> values;
  std::optional<std::string> next_storage_identity;
  if (g_pending_config_save) {
    values = std::move(g_pending_config_save->values);
    next_storage_identity = std::move(g_pending_config_save->storage_identity);
    g_pending_config_save.reset();
  }
  StageRegDword(kIntervalMinutesValue, minutes, &values);
  StageRegDword(kSyncOnStartupValue, on_startup ? 1 : 0, &values);
  return CommitStagedSettings(std::move(values), next_storage_identity);
}

BackendTestResult TestBackend(const SyncConfig& config) {
  // A local directory that does not exist yet is not a failure - the first push
  // creates it - but a path that cannot be created is, and List() would report
  // an empty directory either way.
  if (config.backend == SyncConfig::Backend::kLocalDir) {
    if (config.local_dir.empty())
      return BackendTestResult::kNotConfigured;
    std::error_code ec;
    fs::create_directories(fs::path(config.local_dir), ec);
    if (!fs::is_directory(fs::path(config.local_dir), ec))
      return BackendTestResult::kUnreachable;
  }

  auto backend = MakeBackend(config);
  if (!backend)
    return BackendTestResult::kNotConfigured;

  std::vector<std::string> names;
  if (!backend->List(&names))
    return BackendTestResult::kUnreachable;
  return BackendTestResult::kOk;
}

std::unique_ptr<SyncBackend> MakeBackend(const SyncConfig& config) {
  if (!config.valid())
    return nullptr;
  switch (config.backend) {
    case SyncConfig::Backend::kLocalDir:
      return std::make_unique<LocalDirBackend>(config.local_dir);
    case SyncConfig::Backend::kS3: {
      S3Settings settings;
      settings.endpoint = config.endpoint;
      settings.host = HostFromEndpoint(config.endpoint);
      settings.bucket = config.bucket;
      settings.prefix = CanonicalS3Prefix(config.prefix);
      settings.access_key = config.access_key;
      settings.secret_key = config.secret_key;
      if (!settings.valid())
        return nullptr;
      return std::make_unique<S3Backend>(std::move(settings));
    }
    case SyncConfig::Backend::kWebDav: {
      WebDavSettings settings;
      settings.url = config.dav_url;
      settings.username = config.dav_username;
      settings.password = config.dav_password;
      if (!settings.valid())
        return nullptr;
      return std::make_unique<WebDavBackend>(std::move(settings));
    }
    case SyncConfig::Backend::kWorker: {
      WorkerSettings settings;
      settings.url = config.worker_url;
      settings.token = config.worker_token;
      if (!settings.valid())
        return nullptr;
      return std::make_unique<WorkerBackend>(std::move(settings));
    }
    case SyncConfig::Backend::kNone:
    default:
      return nullptr;
  }
}

fs::path SyncDirectory() {
  const std::wstring configured = ReadInstallationSetting("sync_dir");
  if (!configured.empty())
    return fs::path(configured);
  return WeaselUserDataPath() / L"sync";
}

std::wstring InstallationId() {
  return ReadInstallationSetting("installation_id");
}

KeySetupResult SetUpDataKey(const std::string& password) {
  if (password.size() < kMinPasswordLength)
    return KeySetupResult::kPasswordTooShort;

  const SyncConfig config = SyncConfig::Load();
  auto backend = MakeBackend(config);
  if (!backend)
    return KeySetupResult::kNoBackend;

  std::vector<uint8_t> wrapped;
  const FetchResult fetched = backend->Get(kDataKeyName, &wrapped);

  // Only a definite absence justifies creating a key. If the storage merely
  // could not be reached, publishing a fresh one would overwrite the key that
  // existing snapshots were encrypted with and lose them for good.
  if (fetched == FetchResult::kError ||
      fetched == FetchResult::kPayloadTooLarge) {
    return KeySetupResult::kStorageUnreachable;
  }

  // A stored key that reads back empty is a damaged object, not a missing one.
  // Replacing it would discard whatever the existing snapshots were encrypted
  // with, so this stops instead.
  if (fetched == FetchResult::kOk && wrapped.empty())
    return KeySetupResult::kStorageUnreachable;

  if (fetched == FetchResult::kOk) {
    // Another device published a key already; this one has to join it, and a
    // wrong password simply fails to unwrap.
    const auto dek = UnwrapDataKey(wrapped, password);
    if (!dek)
      return KeySetupResult::kWrongPassword;
    return CacheDataKeyForStorage(*dek, config) ? KeySetupResult::kOk
                                                : KeySetupResult::kCacheFailed;
  }

  std::vector<std::string> names;
  if (!backend->List(&names))
    return KeySetupResult::kStorageUnreachable;
  for (const std::string& name : names) {
    if (LooksLikeEncryptedSnapshot(name))
      return KeySetupResult::kStorageUnreachable;
  }

  const std::vector<uint8_t> dek = RandomBytes(kKeyLength);
  if (dek.empty())
    return KeySetupResult::kKeyGenerationFailed;
  const std::vector<uint8_t> to_publish = WrapDataKey(dek, password);
  if (to_publish.empty())
    return KeySetupResult::kKeyGenerationFailed;
  const PutIfAbsentResult created =
      backend->PutIfAbsent(kDataKeyName, to_publish);
  if (created == PutIfAbsentResult::kError)
    return KeySetupResult::kPublishFailed;

  // The conditional create makes the first published key immutable under
  // concurrent setup. Read the storage copy back whether this device created it
  // or lost the race, then cache only that authoritative value.
  std::vector<uint8_t> published;
  const FetchResult confirmed = backend->Get(kDataKeyName, &published);
  if (confirmed != FetchResult::kOk || published.empty())
    return KeySetupResult::kStorageUnreachable;
  const auto winner = ResolvePublishedDataKey(published, password);
  if (!winner)
    return KeySetupResult::kWrongPassword;
  return CacheDataKeyForStorage(*winner, config) ? KeySetupResult::kOk
                                                 : KeySetupResult::kCacheFailed;
}

bool PullBeforeSync() {
  g_pull_succeeded = false;
  g_last_sync_error = CloudSyncError::kFailed;

  const SyncConfig config = SyncConfig::Load();
  if (!config.enabled()) {
    g_pull_succeeded = true;
    g_last_sync_error = CloudSyncError::kNone;
    return true;
  }

  auto backend = MakeBackend(config);
  if (!backend) {
    g_pull_succeeded = true;
    g_last_sync_error = CloudSyncError::kNone;
    return true;
  }

  std::vector<uint8_t> wrapped;
  const FetchResult wrapped_result = backend->Get(kDataKeyName, &wrapped);
  if (wrapped_result != FetchResult::kOk || wrapped.empty()) {
    if (wrapped_result == FetchResult::kPayloadTooLarge) {
      g_last_sync_error = CloudSyncError::kObjectTooLarge;
    } else if (wrapped_result == FetchResult::kError) {
      RecordBackendFailure(config);
    }
    return false;
  }

  const std::vector<uint8_t> dek = CachedKeyOrEmpty(config);
  if (dek.empty())
    return false;  // key not set up on this machine yet

  std::vector<std::string> names;
  if (!backend->List(&names)) {
    RecordBackendFailure(config);
    return false;
  }

  std::vector<RemoteSnapshot> snapshots;
  if (!SelectRemoteSnapshots(names, &snapshots))
    return false;

  const fs::path sync_dir = SyncDirectory();
  std::error_code ec;
  fs::create_directories(sync_dir, ec);
  if (ec)
    return false;
  const bool sync_dir_exists = fs::is_directory(sync_dir, ec);
  if (ec || !sync_dir_exists)
    return false;

  std::vector<FileWrite> pending_writes;
  size_t pending_bytes = 0;
  for (const RemoteSnapshot& snapshot : snapshots) {
    std::vector<uint8_t> sealed;
    const FetchResult result = backend->Get(snapshot.name, &sealed);
    if (result != FetchResult::kOk) {
      if (result == FetchResult::kPayloadTooLarge) {
        g_last_sync_error = CloudSyncError::kObjectTooLarge;
      } else if (result == FetchResult::kError) {
        RecordBackendFailure(config);
      }
      return false;
    }
    if (sealed.size() > kMaxCloudObjectBytes) {
      g_last_sync_error = CloudSyncError::kObjectTooLarge;
      return false;
    }
    std::string plaintext;
    const SnapshotDecryptResult decrypted =
        DecryptSnapshot(dek, snapshot.name, sealed, &plaintext);
    if (decrypted == SnapshotDecryptResult::kUnsupportedFormat) {
      g_last_sync_error = CloudSyncError::kUnsupportedSnapshotFormat;
      return false;
    }
    if (decrypted != SnapshotDecryptResult::kOk)
      return false;  // wrong key or tampered data; stop rather than guess
    if (!IsUserDictionarySnapshot(plaintext)) {
      g_last_sync_error = CloudSyncError::kInvalidSnapshot;
      return false;
    }
    if (plaintext.size() > kMaxPullBatchBytes - pending_bytes) {
      g_last_sync_error = CloudSyncError::kObjectTooLarge;
      return false;
    }
    pending_bytes += plaintext.size();
    pending_writes.push_back({sync_dir / snapshot.installation / snapshot.file,
                              std::move(plaintext)});
  }
  const FileBatchResult written = WriteFilesAtomically(pending_writes);
  if (written == FileBatchResult::kRecoveryRequired)
    g_last_sync_error = CloudSyncError::kLocalRecoveryRequired;
  if (written != FileBatchResult::kCommitted)
    return false;
  g_pull_succeeded = true;
  g_last_sync_error = CloudSyncError::kNone;
  return true;
}

bool PushAfterSync() {
  if (!g_pull_succeeded)
    return false;
  g_pull_succeeded = false;
  g_last_sync_error = CloudSyncError::kFailed;

  const SyncConfig config = SyncConfig::Load();
  if (!config.enabled()) {
    g_last_sync_error = CloudSyncError::kNone;
    return true;
  }

  auto backend = MakeBackend(config);
  if (!backend) {
    g_last_sync_error = CloudSyncError::kNone;
    return true;
  }

  const std::vector<uint8_t> dek = CachedKeyOrEmpty(config);
  if (dek.empty())
    return false;

  const std::wstring installation_id = InstallationId();
  const std::string installation = wtou8(installation_id);
  if (!IsPlainComponent(installation))
    return false;

  std::error_code ec;
  const fs::path dir = SyncDirectory() / installation_id;
  const bool exists = fs::exists(dir, ec);
  if (ec)
    return false;
  if (!exists) {
    g_last_sync_error = CloudSyncError::kNone;
    return true;
  }

  fs::directory_iterator file(dir, ec);
  if (ec)
    return false;
  const fs::directory_iterator end;
  while (file != end) {
    std::error_code file_ec;
    const bool regular = file->is_regular_file(file_ec);
    if (file_ec)
      return false;
    if (regular && IsSyncableSnapshot(file->path())) {
      const uintmax_t size = file->file_size(file_ec);
      if (file_ec)
        return false;
      if (size > kMaxSnapshotPlaintextBytes) {
        g_last_sync_error = CloudSyncError::kObjectTooLarge;
        return false;
      }
      const std::string file_name = wtou8(file->path().filename().wstring());
      if (!IsPlainComponent(file_name))
        return false;
      std::vector<uint8_t> raw;
      const FileReadResult read =
          ReadFileBytes(file->path(), kMaxSnapshotPlaintextBytes, &raw);
      if (read == FileReadResult::kPayloadTooLarge) {
        g_last_sync_error = CloudSyncError::kObjectTooLarge;
        return false;
      }
      if (read != FileReadResult::kOk)
        return false;
      const std::string plaintext(raw.begin(), raw.end());
      if (!IsUserDictionarySnapshot(plaintext)) {
        g_last_sync_error = CloudSyncError::kInvalidSnapshot;
        return false;
      }
      const std::string name = installation + "/" + file_name;
      const std::vector<uint8_t> sealed = EncryptSnapshot(dek, name, plaintext);
      if (sealed.empty())
        return false;
      if (!backend->Put(name, sealed)) {
        RecordBackendFailure(config);
        return false;
      }
    }
    file.increment(ec);
    if (ec)
      return false;
  }
  g_last_sync_error = CloudSyncError::kNone;
  return true;
}

}  // namespace hare
