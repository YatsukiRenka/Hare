// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Yatsuki Renka

#include "stdafx.h"
#include "CloudHttp.h"

#include <bcrypt.h>
#include <dpapi.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <limits>
#include <memory>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace hare {

namespace {

// Keep deployer operations bounded when DNS or a remote peer stalls.
constexpr int kResolveTimeoutMs = 15 * 1000;
constexpr int kConnectTimeoutMs = 15 * 1000;
constexpr int kSendTimeoutMs = 60 * 1000;
constexpr int kReceiveTimeoutMs = 60 * 1000;

thread_local HttpFailure g_last_http_failure = HttpFailure::kNone;

class HttpFailureRecorder {
 public:
  explicit HttpFailureRecorder(const HttpResponse* response)
      : response_(response) {}
  ~HttpFailureRecorder() { g_last_http_failure = response_->failure; }

 private:
  const HttpResponse* response_;
};

struct WinHttpHandleDeleter {
  void operator()(void* h) const {
    if (h)
      WinHttpCloseHandle(h);
  }
};
using WinHttpHandle = std::unique_ptr<void, WinHttpHandleDeleter>;

struct AlgProviderDeleter {
  void operator()(BCRYPT_ALG_HANDLE h) const {
    if (h)
      BCryptCloseAlgorithmProvider(h, 0);
  }
};

// One hashing routine serves both SHA-256 and HMAC-SHA-256; the only
// difference is whether the provider is opened with the HMAC flag and a key.
std::vector<uint8_t> HashWith(const std::vector<uint8_t>* key,
                              const std::string& data) {
  if (data.size() > (std::numeric_limits<ULONG>::max)() ||
      (key && key->size() > (std::numeric_limits<ULONG>::max)())) {
    return {};
  }

  BCRYPT_ALG_HANDLE raw_alg = nullptr;
  const ULONG flags = key ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0;
  if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
          &raw_alg, BCRYPT_SHA256_ALGORITHM, nullptr, flags))) {
    return {};
  }
  std::unique_ptr<void, AlgProviderDeleter> alg(raw_alg);

  DWORD hash_length = 0;
  DWORD copied = 0;
  if (!BCRYPT_SUCCESS(BCryptGetProperty(alg.get(), BCRYPT_HASH_LENGTH,
                                        reinterpret_cast<PUCHAR>(&hash_length),
                                        sizeof(hash_length), &copied, 0))) {
    return {};
  }

  BCRYPT_HASH_HANDLE hash = nullptr;
  PUCHAR key_bytes =
      key && !key->empty() ? const_cast<PUCHAR>(key->data()) : nullptr;
  const ULONG key_size = key ? static_cast<ULONG>(key->size()) : 0;
  if (!BCRYPT_SUCCESS(BCryptCreateHash(alg.get(), &hash, nullptr, 0, key_bytes,
                                       key_size, 0))) {
    return {};
  }

  std::vector<uint8_t> digest(hash_length);
  bool ok = BCRYPT_SUCCESS(BCryptHashData(
      hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
      static_cast<ULONG>(data.size()), 0));
  ok = ok &&
       BCRYPT_SUCCESS(BCryptFinishHash(hash, digest.data(), hash_length, 0));
  BCryptDestroyHash(hash);

  if (!ok)
    return {};
  return digest;
}

}  // namespace

HttpFailure LastHttpFailure() {
  return g_last_http_failure;
}

std::vector<uint8_t> Sha256(const std::string& data) {
  return HashWith(nullptr, data);
}

std::vector<uint8_t> HmacSha256(const std::vector<uint8_t>& key,
                                const std::string& data) {
  return HashWith(&key, data);
}

std::string ToHex(const std::vector<uint8_t>& bytes) {
  static const char* kDigits = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (uint8_t b : bytes) {
    out.push_back(kDigits[b >> 4]);
    out.push_back(kDigits[b & 0x0f]);
  }
  return out;
}

std::string UriEncode(const std::string& value, bool encode_slash) {
  static const char* kDigits = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : value) {
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                            c == '.' || c == '~';
    if (unreserved || (c == '/' && !encode_slash)) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kDigits[c >> 4]);
      out.push_back(kDigits[c & 0x0f]);
    }
  }
  return out;
}

std::string Base64(const std::string& data) {
  DWORD length = 0;
  if (!CryptBinaryToStringA(reinterpret_cast<const BYTE*>(data.data()),
                            static_cast<DWORD>(data.size()),
                            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr,
                            &length)) {
    return {};
  }
  std::string out(length, '\0');
  if (!CryptBinaryToStringA(reinterpret_cast<const BYTE*>(data.data()),
                            static_cast<DWORD>(data.size()),
                            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                            out.data(), &length)) {
    return {};
  }
  out.resize(length);
  return out;
}

HttpResponse HttpRequest(const std::wstring& method,
                         const std::wstring& url,
                         const std::map<std::wstring, std::wstring>& headers,
                         const std::string& body) {
  HttpResponse response;
  HttpFailureRecorder failure_recorder(&response);

  if (url.size() >= (std::numeric_limits<DWORD>::max)()) {
    return response;
  }
  if (body.size() > kMaxCloudObjectBytes) {
    response.failure = HttpFailure::kPayloadTooLarge;
    return response;
  }

  URL_COMPONENTS parts = {};
  parts.dwStructSize = sizeof(parts);
  const size_t component_buffer_size = url.size() + 1;
  const DWORD component_capacity = static_cast<DWORD>(component_buffer_size);
  std::vector<wchar_t> host(component_buffer_size);
  std::vector<wchar_t> path(component_buffer_size);
  std::vector<wchar_t> query(component_buffer_size);
  parts.lpszHostName = host.data();
  parts.dwHostNameLength = component_capacity;
  parts.lpszUrlPath = path.data();
  parts.dwUrlPathLength = component_capacity;
  parts.lpszExtraInfo = query.data();
  parts.dwExtraInfoLength = component_capacity;
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts))
    return response;

  // The query is requested separately and re-joined rather than relying on
  // where WinHttpCrackUrl leaves it when no buffer is supplied. SigV4 signs the
  // query, so a target that silently lost it would be rejected as a signature
  // mismatch, and that is not a failure worth making dependent on undocumented
  // behaviour.
  const std::wstring host_name(host.data(), parts.dwHostNameLength);
  const std::wstring target =
      std::wstring(path.data(), parts.dwUrlPathLength) +
      std::wstring(query.data(), parts.dwExtraInfoLength);

  WinHttpHandle session(
      WinHttpOpen(L"Hare/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session)
    return response;
  if (!WinHttpSetTimeouts(session.get(), kResolveTimeoutMs, kConnectTimeoutMs,
                          kSendTimeoutMs, kReceiveTimeoutMs)) {
    return response;
  }

  WinHttpHandle connection(
      WinHttpConnect(session.get(), host_name.c_str(), parts.nPort, 0));
  if (!connection)
    return response;

  // Credentials must never travel in the clear. Backends authenticate with a
  // Basic password or a bearer token, so a plain http:// endpoint would hand
  // them to anyone on the path; refuse rather than downgrade silently.
  const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
  if (!secure && headers.count(L"Authorization") != 0)
    return response;

  const DWORD request_flags = secure ? WINHTTP_FLAG_SECURE : 0;
  WinHttpHandle request(WinHttpOpenRequest(
      connection.get(), method.c_str(), target.c_str(), nullptr,
      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, request_flags));
  if (!request)
    return response;

  // Custom Authorization headers must not be replayed to a redirect target.
  DWORD disabled_features = WINHTTP_DISABLE_REDIRECTS;
  if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE,
                        &disabled_features, sizeof(disabled_features))) {
    return response;
  }

  for (const auto& [name, value] : headers) {
    const std::wstring line = name + L": " + value;
    if (line.size() >= (std::numeric_limits<DWORD>::max)() ||
        !WinHttpAddRequestHeaders(request.get(), line.c_str(),
                                  static_cast<DWORD>(line.size()),
                                  WINHTTP_ADDREQ_FLAG_ADD)) {
      return response;
    }
  }

  const bool sent = WinHttpSendRequest(
      request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
      body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
      static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
  if (!sent || !WinHttpReceiveResponse(request.get(), nullptr))
    return response;

  DWORD status = 0;
  DWORD status_size = sizeof(status);
  if (!WinHttpQueryHeaders(
          request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
          WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
          WINHTTP_NO_HEADER_INDEX)) {
    response.status = 0;
    response.body.clear();
    return response;
  }
  response.status = status;
  response.failure = HttpFailure::kNone;

  for (;;) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request.get(), &available)) {
      response.status = 0;
      response.failure = HttpFailure::kTransport;
      response.body.clear();
      return response;
    }
    if (available == 0)
      break;
    const size_t offset = response.body.size();
    if (offset > kMaxCloudObjectBytes ||
        available > kMaxCloudObjectBytes - offset) {
      response.status = 0;
      response.failure = HttpFailure::kPayloadTooLarge;
      response.body.clear();
      return response;
    }
    response.body.resize(offset + available);
    DWORD read = 0;
    if (!WinHttpReadData(request.get(), response.body.data() + offset,
                         available, &read)) {
      response.status = 0;
      response.failure = HttpFailure::kTransport;
      response.body.clear();
      return response;
    }
    response.body.resize(offset + read);
  }

  return response;
}

std::vector<uint8_t> Protect(const std::string& plain) {
  DATA_BLOB in = {};
  in.cbData = static_cast<DWORD>(plain.size());
  in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()));

  DATA_BLOB out = {};
  if (!CryptProtectData(&in, L"Hare cloud sync credential", nullptr, nullptr,
                        nullptr, 0, &out)) {
    return {};
  }
  std::vector<uint8_t> result(out.pbData, out.pbData + out.cbData);
  LocalFree(out.pbData);
  return result;
}

std::string Unprotect(const std::vector<uint8_t>& encrypted) {
  if (encrypted.empty())
    return {};

  DATA_BLOB in = {};
  in.cbData = static_cast<DWORD>(encrypted.size());
  in.pbData = const_cast<BYTE*>(encrypted.data());

  DATA_BLOB out = {};
  if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out))
    return {};

  std::string result(reinterpret_cast<char*>(out.pbData), out.cbData);
  SecureZeroMemory(out.pbData, out.cbData);
  LocalFree(out.pbData);
  return result;
}

}  // namespace hare
