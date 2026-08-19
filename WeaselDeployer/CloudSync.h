// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Yatsuki Renka

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "CloudSnapshot.h"

// Cloud sync wraps Rime's own sync mechanism rather than replacing it.
//
// Rime exports each user dictionary to sync_dir/<installation_id>/*.userdb.txt
// and merges every snapshot it finds there. Word frequencies merge by taking
// the higher use count, which is commutative and idempotent, so no conflict
// resolution is needed. This module only moves those snapshots between the
// sync directory and remote storage:
//
//   Pull()  ->  rime->sync_user_data()  ->  Push()
//
// The order matters. Pulling after the merge would leave the other machines'
// entries out of this round.

namespace hare {

// "Absent" has to be distinguishable from "could not tell". Treating a network
// error as proof that the data key is missing would lead to publishing a new
// one over the old, and every snapshot encrypted with the old key would become
// unreadable. The size-contract failure is separate as well so callers can
// report it without mislabelling a deliberate local rejection as transport.
enum class FetchResult { kOk, kNotFound, kError, kPayloadTooLarge };

enum class PutIfAbsentResult {
  kCreated,
  kAlreadyExists,
  kError,
};

// Remote storage, reduced to named blobs. Directory walking and encryption
// live in one place above this interface so every backend behaves the same and
// no backend can accidentally upload plaintext.
class SyncBackend {
 public:
  virtual ~SyncBackend() = default;

  // Names are relative and use forward slashes: "<installation_id>/<file>".
  virtual bool List(std::vector<std::string>* names) = 0;
  virtual FetchResult Get(const std::string& name,
                          std::vector<uint8_t>* out) = 0;
  virtual bool Put(const std::string& name,
                   const std::vector<uint8_t>& data) = 0;
  // Atomic conditional creation. kAlreadyExists guarantees that the existing
  // object was not modified.
  virtual PutIfAbsentResult PutIfAbsent(const std::string& name,
                                        const std::vector<uint8_t>& data) = 0;

  virtual std::wstring Describe() const = 0;
};

struct SyncConfig {
  enum class Backend { kNone, kLocalDir, kS3, kWebDav, kWorker };

  Backend backend = Backend::kNone;

  // kLocalDir
  std::wstring local_dir;

  // kWebDav
  std::string dav_url;
  std::string dav_username;
  std::string dav_password;

  // kWorker
  std::string worker_url;
  std::string worker_token;

  // kS3. Credentials arrive DPAPI-encrypted and are decrypted on load, so they
  // exist in the clear only for the lifetime of one sync run.
  std::string endpoint;
  std::string bucket;
  std::string prefix;
  std::string access_key;
  std::string secret_key;

  // Reads HKCU\Software\Rime\Hare\CloudSync. Credentials are kept out of the
  // Rime user directory on purpose: that directory is itself synchronised, so
  // storing them there would upload them and create a bootstrap cycle.
  //
  // A configuration missing a field it needs reports itself as disabled, so a
  // half-entered backend never takes part in a sync.
  static SyncConfig Load();

  // The same values as Load(), except that the selected backend survives
  // missing fields. The settings panel edits what is stored, and resetting the
  // user's choice to "off" because one field is still blank would lose it.
  static SyncConfig LoadForEditing();

  // Stages the configuration, with credentials protected under DPAPI, for the
  // immediately following SaveSyncSchedule() call. That call commits both sets
  // of values together and restores their exact registry pre-image on failure.
  // An empty credential leaves the stored one untouched: the panel never sends
  // a saved secret back to the page, so "unchanged" arrives as empty.
  //
  // Pointing at a different storage forgets the locally cached data key. Every
  // storage publishes its own key, and keeping the previous one would only
  // produce snapshots the new storage cannot decrypt.
  bool Save() const;

  bool enabled() const { return backend != Backend::kNone; }
  bool valid() const;
};

// Scheduling lives next to the rest of the configuration but is read by the
// server, which triggers synchronisation; see include\HareCloudSync.h. When a
// SyncConfig::Save() is pending on this thread, this is the single commit point
// for both configuration and schedule.
bool SaveSyncSchedule(unsigned interval_minutes, bool on_startup);

// Reaches the storage with the given settings without touching the registry,
// so the panel can check credentials before saving them. "Not configured" and
// "unreachable" stay apart: they call for different corrections.
enum class BackendTestResult { kOk, kNotConfigured, kUnreachable };

BackendTestResult TestBackend(const SyncConfig& config);

std::unique_ptr<SyncBackend> MakeBackend(const SyncConfig& config);

// The directory Rime writes its snapshots to, honouring sync_dir in
// installation.yaml and falling back to <user dir>\sync.
std::filesystem::path SyncDirectory();

// This machine's installation_id, as recorded in installation.yaml. Empty if
// the file has not been written yet, which happens before the first deploy.
std::wstring InstallationId();

// The most recent pull/push result on this thread. Existing callers still use
// the boolean result, while diagnostics can distinguish malformed or oversized
// snapshots from an otherwise unspecified cloud failure.
enum class CloudSyncError {
  kNone,
  kFailed,
  // Host/page status code: "sync_unsupported_snapshot_format".
  kUnsupportedSnapshotFormat,
  kInvalidSnapshot,
  kObjectTooLarge,
  // A local batch left a .hare.bak recovery file beside a destination.
  kLocalRecoveryRequired,
};

CloudSyncError LastCloudSyncError();

// Establishes the shared data key on this machine: unwraps the one already
// published with `password`, or atomically creates one if the storage holds no
// key yet. The unwrapped key is cached under DPAPI, so this only has to run
// once per machine and unattended synchronisation needs no password afterwards.
//
// The failure is reported by stage because the causes call for different
// answers from the user: a wrong password, an unreachable service and a
// misconfigured backend look identical otherwise.
enum class KeySetupResult {
  kOk = 0,
  kPasswordTooShort = 1,
  kNoBackend = 2,
  kWrongPassword = 3,
  kKeyGenerationFailed = 4,
  kPublishFailed = 5,
  kCacheFailed = 6,
  kStorageUnreachable = 7,
};

KeySetupResult SetUpDataKey(const std::string& password);

// Both return true when there was nothing to do. Push refuses to publish unless
// the preceding pull in this thread succeeded, while the local Rime merge still
// runs and the failed cloud round remains visible to the caller.
bool PullBeforeSync();
bool PushAfterSync();

}  // namespace hare
