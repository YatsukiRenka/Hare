// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Yatsuki Renka

#include "stdafx.h"
#include "CloudSync.h"

#include <HareCloudSync.h>
#include <WeaselUtility.h>

#include <fstream>
#include <sstream>
#include <string>

#include "CloudCrypto.h"
#include "CloudHttp.h"
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

std::wstring ReadRegString(const wchar_t* key, const wchar_t* value) {
  std::wstring result;
  RegGetStringValue(HKEY_CURRENT_USER, key, value, result);
  return result;
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
  return host;
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
bool ReadFileBytes(const fs::path& path, std::vector<uint8_t>* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;
  std::ostringstream buffer;
  buffer << in.rdbuf();
  if (in.bad())
    return false;
  const std::string data = buffer.str();
  out->assign(data.begin(), data.end());
  return true;
}

bool WriteFileBytes(const fs::path& path, const std::string& data) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out)
    return false;
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  return out.good();
}

// Remote names become local paths, so each component has to be a plain file
// name. Without this a remote object called "../../evil.userdb.txt" would be
// written outside the sync directory, and the snapshot filter alone would not
// catch it because it only inspects the base name.
bool IsPlainComponent(const std::string& component) {
  if (component.empty() || component == "." || component == "..")
    return false;
  if (component.find_first_of("/\\:") != std::string::npos)
    return false;
  return true;
}

// Files land in a directory named after the machine that produced them, so a
// blob name always has exactly one slash.
bool SplitName(const std::string& name,
               std::string* installation,
               std::string* file) {
  const size_t slash = name.find('/');
  if (slash == std::string::npos)
    return false;
  *installation = name.substr(0, slash);
  *file = name.substr(slash + 1);
  return IsPlainComponent(*installation) && IsPlainComponent(*file);
}

class LocalDirBackend : public SyncBackend {
 public:
  explicit LocalDirBackend(std::wstring root) : root_(std::move(root)) {}

  bool List(std::vector<std::string>* names) override {
    std::error_code ec;
    if (!fs::exists(root_, ec))
      return true;
    for (const auto& entry : fs::recursive_directory_iterator(root_, ec)) {
      if (!entry.is_regular_file())
        continue;
      const fs::path relative = fs::relative(entry.path(), root_, ec);
      if (ec)
        continue;
      std::string name = wtou8(relative.generic_wstring());
      names->push_back(std::move(name));
    }
    return true;
  }

  FetchResult Get(const std::string& name,
                  std::vector<uint8_t>* out) override {
    const fs::path path = root_ / u8tow(name);
    std::error_code ec;
    if (!fs::exists(path, ec))
      return ec ? FetchResult::kError : FetchResult::kNotFound;
    return ReadFileBytes(path, out) ? FetchResult::kOk : FetchResult::kError;
  }

  bool Put(const std::string& name,
           const std::vector<uint8_t>& data) override {
    const fs::path path = root_ / u8tow(name);
    return WriteFileBytes(path, std::string(data.begin(), data.end()));
  }

  std::wstring Describe() const override {
    return L"local directory " + root_.wstring();
  }

 private:
  fs::path root_;
};

// Obtains the data key without a password by using the copy cached on this
// machine. Absent until SetUpDataKey has run here.
std::vector<uint8_t> CachedKeyOrEmpty() {
  const auto cached = LoadCachedDataKey();
  return cached ? *cached : std::vector<uint8_t>();
}

bool WriteRegString(const wchar_t* value, const std::wstring& data) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kConfigKey, 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return false;
  }
  const LSTATUS status = RegSetValueExW(
      key, value, 0, REG_SZ, reinterpret_cast<const BYTE*>(data.c_str()),
      static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  return status == ERROR_SUCCESS;
}

bool WriteRegDword(const wchar_t* value, DWORD data) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kConfigKey, 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return false;
  }
  const LSTATUS status =
      RegSetValueExW(key, value, 0, REG_DWORD,
                     reinterpret_cast<const BYTE*>(&data), sizeof(data));
  RegCloseKey(key);
  return status == ERROR_SUCCESS;
}

// An empty secret means "leave what is stored alone". The panel shows saved
// credentials as blank fields rather than sending them back to the page, so
// blank has to mean unchanged; clearing one happens by switching backends.
bool WriteRegSecret(const wchar_t* value, const std::string& secret) {
  if (secret.empty())
    return true;
  const std::vector<uint8_t> sealed = Protect(secret);
  if (sealed.empty())
    return false;

  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kConfigKey, 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return false;
  }
  const LSTATUS status =
      RegSetValueExW(key, value, 0, REG_BINARY, sealed.data(),
                     static_cast<DWORD>(sealed.size()));
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

// What the data key belongs to. Credentials are left out on purpose: rotating
// an access key still addresses the same storage, and forgetting the key then
// would ask for the master password again for no reason.
std::wstring StorageIdentity(const SyncConfig& config) {
  switch (config.backend) {
    case SyncConfig::Backend::kLocalDir:
      return L"localdir|" + config.local_dir;
    case SyncConfig::Backend::kS3:
      return L"s3|" + u8tow(config.endpoint + "|" + config.bucket + "|" +
                            config.prefix);
    case SyncConfig::Backend::kWebDav:
      return L"webdav|" + u8tow(config.dav_url);
    case SyncConfig::Backend::kWorker:
      return L"worker|" + u8tow(config.worker_url);
    case SyncConfig::Backend::kNone:
    default:
      return L"none";
  }
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
    if (require_complete && config.local_dir.empty())
      config.backend = SyncConfig::Backend::kNone;
  } else if (backend == L"s3") {
    config.backend = SyncConfig::Backend::kS3;
    config.endpoint = wtou8(ReadRegString(kConfigKey, L"Endpoint"));
    config.bucket = wtou8(ReadRegString(kConfigKey, L"Bucket"));
    config.prefix = wtou8(ReadRegString(kConfigKey, L"Prefix"));
    if (config.prefix.empty())
      config.prefix = "hare/";
    config.access_key = ReadRegSecret(kConfigKey, L"AccessKeyId");
    config.secret_key = ReadRegSecret(kConfigKey, L"SecretAccessKey");
    if (require_complete &&
        (config.endpoint.empty() || config.bucket.empty() ||
         config.access_key.empty() || config.secret_key.empty())) {
      config.backend = SyncConfig::Backend::kNone;
    }
  } else if (backend == L"webdav") {
    config.backend = SyncConfig::Backend::kWebDav;
    config.dav_url = wtou8(ReadRegString(kConfigKey, L"DavUrl"));
    config.dav_username = wtou8(ReadRegString(kConfigKey, L"DavUsername"));
    config.dav_password = ReadRegSecret(kConfigKey, L"DavPassword");
    if (require_complete &&
        (config.dav_url.empty() || config.dav_username.empty() ||
         config.dav_password.empty())) {
      config.backend = SyncConfig::Backend::kNone;
    }
  } else if (backend == L"worker") {
    config.backend = SyncConfig::Backend::kWorker;
    config.worker_url = wtou8(ReadRegString(kConfigKey, L"WorkerUrl"));
    config.worker_token = ReadRegSecret(kConfigKey, L"WorkerToken");
    if (require_complete &&
        (config.worker_url.empty() || config.worker_token.empty())) {
      config.backend = SyncConfig::Backend::kNone;
    }
  }
  return config;
}

}  // namespace

bool IsSyncableSnapshot(const fs::path& file) {
  const std::wstring name = file.filename().wstring();
  constexpr size_t kSuffixLength = 11;  // ".userdb.txt"
  if (name.size() <= kSuffixLength)
    return false;
  if (name.rfind(L".userdb.txt") != name.size() - kSuffixLength)
    return false;
  return name.rfind(L"replacer", 0) != 0;
}

SyncConfig SyncConfig::Load() {
  return LoadConfig(true);
}

SyncConfig SyncConfig::LoadForEditing() {
  return LoadConfig(false);
}

bool SyncConfig::Save() const {
  const std::wstring previous = StorageIdentity(LoadConfig(false));

  bool ok = WriteRegString(L"Backend", BackendName(backend));
  switch (backend) {
    case Backend::kLocalDir:
      ok = WriteRegString(L"LocalDir", local_dir) && ok;
      break;
    case Backend::kS3:
      ok = WriteRegString(L"Endpoint", u8tow(endpoint)) && ok;
      ok = WriteRegString(L"Bucket", u8tow(bucket)) && ok;
      ok = WriteRegString(L"Prefix", u8tow(prefix)) && ok;
      ok = WriteRegSecret(L"AccessKeyId", access_key) && ok;
      ok = WriteRegSecret(L"SecretAccessKey", secret_key) && ok;
      break;
    case Backend::kWebDav:
      ok = WriteRegString(L"DavUrl", u8tow(dav_url)) && ok;
      ok = WriteRegString(L"DavUsername", u8tow(dav_username)) && ok;
      ok = WriteRegSecret(L"DavPassword", dav_password) && ok;
      break;
    case Backend::kWorker:
      ok = WriteRegString(L"WorkerUrl", u8tow(worker_url)) && ok;
      ok = WriteRegSecret(L"WorkerToken", worker_token) && ok;
      break;
    case Backend::kNone:
    default:
      break;
  }

  // Reading back what was actually stored, rather than trusting these fields,
  // keeps the comparison honest when a write only half succeeded.
  if (StorageIdentity(LoadConfig(false)) != previous)
    ForgetCachedDataKey();
  return ok;
}

bool SaveSyncSchedule(unsigned interval_minutes, bool on_startup) {
  const DWORD minutes = interval_minutes > kMaxIntervalMinutes
                            ? kMaxIntervalMinutes
                            : static_cast<DWORD>(interval_minutes);
  bool ok = WriteRegDword(kIntervalMinutesValue, minutes);
  ok = WriteRegDword(kSyncOnStartupValue, on_startup ? 1 : 0) && ok;
  return ok;
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
  switch (config.backend) {
    case SyncConfig::Backend::kLocalDir:
      return std::make_unique<LocalDirBackend>(config.local_dir);
    case SyncConfig::Backend::kS3: {
      S3Settings settings;
      settings.endpoint = config.endpoint;
      settings.host = HostFromEndpoint(config.endpoint);
      settings.bucket = config.bucket;
      settings.prefix = config.prefix;
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
  if (fetched == FetchResult::kError)
    return KeySetupResult::kStorageUnreachable;

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
    return CacheDataKey(*dek) ? KeySetupResult::kOk
                              : KeySetupResult::kCacheFailed;
  }

  const std::vector<uint8_t> dek = RandomBytes(kKeyLength);
  if (dek.empty())
    return KeySetupResult::kKeyGenerationFailed;
  const std::vector<uint8_t> to_publish = WrapDataKey(dek, password);
  if (to_publish.empty())
    return KeySetupResult::kKeyGenerationFailed;
  if (!backend->Put(kDataKeyName, to_publish))
    return KeySetupResult::kPublishFailed;

  // Two devices setting up at the same moment would both find no key, generate
  // different ones and overwrite each other, leaving one of them unable to read
  // what the other uploads. Reading the key back and adopting whatever is
  // actually stored makes both converge on the same one. No snapshots exist yet
  // at this point, so the discarded key protects nothing.
  std::vector<uint8_t> published;
  if (backend->Get(kDataKeyName, &published) == FetchResult::kOk &&
      !published.empty()) {
    const auto winner = UnwrapDataKey(published, password);
    if (winner) {
      return CacheDataKey(*winner) ? KeySetupResult::kOk
                                   : KeySetupResult::kCacheFailed;
    }
  }

  return CacheDataKey(dek) ? KeySetupResult::kOk
                           : KeySetupResult::kCacheFailed;
}

bool PullBeforeSync() {
  const SyncConfig config = SyncConfig::Load();
  if (!config.enabled())
    return true;

  auto backend = MakeBackend(config);
  if (!backend)
    return true;

  const std::vector<uint8_t> dek = CachedKeyOrEmpty();
  if (dek.empty())
    return false;  // key not set up on this machine yet

  std::vector<std::string> names;
  if (!backend->List(&names))
    return false;

  const fs::path sync_dir = SyncDirectory();
  std::error_code ec;
  fs::create_directories(sync_dir, ec);

  for (const std::string& name : names) {
    std::string installation;
    std::string file;
    if (!SplitName(name, &installation, &file))
      continue;  // the key blob and anything else unstructured
    if (!IsSyncableSnapshot(fs::path(u8tow(file))))
      continue;

    std::vector<uint8_t> sealed;
    if (backend->Get(name, &sealed) != FetchResult::kOk)
      return false;
    const auto plaintext = AesGcmDecrypt(dek, sealed);
    if (!plaintext)
      return false;  // wrong key or tampered data; stop rather than guess
    if (!WriteFileBytes(sync_dir / u8tow(installation) / u8tow(file),
                        *plaintext)) {
      return false;
    }
  }
  return true;
}

bool PushAfterSync() {
  const SyncConfig config = SyncConfig::Load();
  if (!config.enabled())
    return true;

  auto backend = MakeBackend(config);
  if (!backend)
    return true;

  const std::vector<uint8_t> dek = CachedKeyOrEmpty();
  if (dek.empty())
    return false;

  const std::wstring installation_id = InstallationId();
  if (installation_id.empty())
    return false;

  std::error_code ec;
  const fs::path dir = SyncDirectory() / installation_id;
  if (!fs::exists(dir, ec))
    return true;

  for (const auto& file : fs::directory_iterator(dir, ec)) {
    if (!file.is_regular_file() || !IsSyncableSnapshot(file.path()))
      continue;
    std::vector<uint8_t> raw;
    if (!ReadFileBytes(file.path(), &raw))
      return false;  // never overwrite a good remote snapshot with nothing
    const std::vector<uint8_t> sealed =
        AesGcmEncrypt(dek, std::string(raw.begin(), raw.end()));
    if (sealed.empty())
      return false;
    const std::string name = wtou8(installation_id) + "/" +
                             wtou8(file.path().filename().wstring());
    if (!backend->Put(name, sealed))
      return false;
  }
  return true;
}

}  // namespace hare
