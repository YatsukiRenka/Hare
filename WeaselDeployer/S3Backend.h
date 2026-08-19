// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Yatsuki Renka

#pragma once

#include <map>
#include <string>
#include <vector>

#include "CloudSync.h"

namespace hare {

struct S3Settings {
  std::string endpoint;  // https://<account>.r2.cloudflarestorage.com
  std::string host;      // <account>.r2.cloudflarestorage.com
  std::string bucket;
  std::string prefix;  // object key prefix, e.g. "hare/"
  std::string access_key;
  std::string secret_key;

  bool valid() const {
    return !endpoint.empty() && !host.empty() && !bucket.empty() &&
           !access_key.empty() && !secret_key.empty();
  }
};

// Talks to any S3-compatible storage. Cloudflare R2 is the intended target,
// but nothing here is R2-specific beyond the default region name, so MinIO,
// Backblaze B2 and the like work with a different endpoint.
class S3Backend : public SyncBackend {
 public:
  explicit S3Backend(S3Settings settings);

  bool List(std::vector<std::string>* names) override;
  FetchResult Get(const std::string& name, std::vector<uint8_t>* out) override;
  bool Put(const std::string& name, const std::vector<uint8_t>& data) override;
  PutIfAbsentResult PutIfAbsent(const std::string& name,
                                const std::vector<uint8_t>& data) override;
  std::wstring Describe() const override;

 private:
  std::map<std::wstring, std::wstring> SignedHeaders(
      const std::string& method,
      const std::string& canonical_uri,
      const std::string& canonical_query,
      const std::string& payload,
      bool if_none_match) const;

  std::string ObjectUrl(const std::string& key) const;
  std::string CanonicalUri(const std::string& key) const;

  S3Settings settings_;
};

}  // namespace hare
