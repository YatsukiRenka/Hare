// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Yatsuki Renka

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// HTTP and hashing for the cloud sync backends. WinHTTP and CNG both ship with
// Windows, which keeps the dependency surface of this fork unchanged.

namespace hare {

// Encrypted snapshots and every HTTP request/response body share this bound.
// Keeping one wire-size contract prevents uploading an object that a later GET
// would refuse to read.
constexpr size_t kMaxCloudObjectBytes = 64u * 1024u * 1024u;

enum class HttpFailure {
  kNone,
  kTransport,
  kPayloadTooLarge,
};

struct HttpResponse {
  // Zero means no complete response. `failure` preserves whether that was a
  // transport failure or the local payload-size contract rejecting the body.
  unsigned status = 0;
  HttpFailure failure = HttpFailure::kTransport;
  std::string body;

  bool ok() const { return status >= 200 && status < 300; }
};

// `url` is absolute. Header values are ASCII. An empty `body` sends no payload.
HttpResponse HttpRequest(const std::wstring& method,
                         const std::wstring& url,
                         const std::map<std::wstring, std::wstring>& headers,
                         const std::string& body);

// Preserves the category even when a backend reduces HttpResponse to its
// existing bool/FetchResult interface. Like HttpRequest, this is per-thread.
HttpFailure LastHttpFailure();

std::vector<uint8_t> Sha256(const std::string& data);
std::vector<uint8_t> HmacSha256(const std::vector<uint8_t>& key,
                                const std::string& data);
std::string ToHex(const std::vector<uint8_t>& bytes);

inline std::string Sha256Hex(const std::string& data) {
  return ToHex(Sha256(data));
}

// Percent-encoding as required for canonical URIs and query strings. Slashes
// stay literal in paths and are escaped in query values.
std::string UriEncode(const std::string& value, bool encode_slash);

// Single-line base64, as HTTP Basic authorisation expects.
std::string Base64(const std::string& data);

// DPAPI, bound to the current Windows account. Credentials never leave the
// machine in the clear and never enter the Rime user directory, which is
// itself synchronised.
//
// No entropy is passed, which matches what tools\configure-sync.ps1 produces:
// blobs written by either route have to be readable by the other.
std::vector<uint8_t> Protect(const std::string& plain);
std::string Unprotect(const std::vector<uint8_t>& encrypted);

}  // namespace hare
