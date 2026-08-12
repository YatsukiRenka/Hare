// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Yatsuki Renka

#pragma once

#include <map>
#include <string>
#include <vector>

#include "CloudSync.h"

namespace hare {

struct WorkerSettings {
  std::string url;    // https://hare-sync.<subdomain>.workers.dev
  std::string token;  // shared secret configured on the Worker

  bool valid() const { return !url.empty() && !token.empty(); }
};

// Talks to the Worker in `worker/`, which holds the R2 binding and the storage
// credentials on Cloudflare's side. This exists so that setting up sync means
// pasting a URL and a token instead of an endpoint, a bucket name and a
// key pair; the snapshots are already encrypted before they get here, so the
// Worker never sees anything readable either.
class WorkerBackend : public SyncBackend {
 public:
  explicit WorkerBackend(WorkerSettings settings);

  bool List(std::vector<std::string>* names) override;
  FetchResult Get(const std::string& name, std::vector<uint8_t>* out) override;
  bool Put(const std::string& name, const std::vector<uint8_t>& data) override;
  std::wstring Describe() const override;

 private:
  std::map<std::wstring, std::wstring> AuthHeaders() const;

  WorkerSettings settings_;
};

}  // namespace hare
