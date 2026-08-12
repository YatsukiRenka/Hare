// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Yatsuki Renka

#pragma once

#include <map>
#include <string>
#include <vector>

#include "CloudSync.h"

namespace hare {

struct WebDavSettings {
  std::string url;       // https://dav.example.com/dav/hare
  std::string username;
  std::string password;  // an application password where the service offers one

  bool valid() const {
    return !url.empty() && !username.empty() && !password.empty();
  }
};

// WebDAV reaches the services people already pay for, Nextcloud and Jianguoyun
// among them. The protocol is thinner than S3: collections are created with
// MKCOL, listings come from PROPFIND, and authentication is HTTP Basic over
// TLS.
class WebDavBackend : public SyncBackend {
 public:
  explicit WebDavBackend(WebDavSettings settings);

  bool List(std::vector<std::string>* names) override;
  FetchResult Get(const std::string& name, std::vector<uint8_t>* out) override;
  bool Put(const std::string& name, const std::vector<uint8_t>& data) override;
  std::wstring Describe() const override;

 private:
  std::map<std::wstring, std::wstring> AuthHeaders() const;
  std::wstring ResourceUrl(const std::string& name) const;

  // Depth 1 listing of one collection. Names come back relative to the root.
  bool ListCollection(const std::string& relative,
                      std::vector<std::string>* collections,
                      std::vector<std::string>* files) const;

  // Servers reject a PUT whose parent collection is missing, and MKCOL on an
  // existing collection is harmless, so it is issued unconditionally.
  void EnsureCollection(const std::string& relative) const;

  WebDavSettings settings_;
  std::string root_path_;  // path component of the configured URL
};

}  // namespace hare
