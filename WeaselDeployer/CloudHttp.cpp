// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Yatsuki Renka

#include "stdafx.h"
#include "CloudHttp.h"

#include <bcrypt.h>
#include <dpapi.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <memory>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace hare {

namespace {

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
  BCRYPT_ALG_HANDLE raw_alg = nullptr;
  const ULONG flags = key ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0;
  if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&raw_alg, BCRYPT_SHA256_ALGORITHM,
                                                  nullptr, flags))) {
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
  PUCHAR key_bytes = key && !key->empty()
                         ? const_cast<PUCHAR>(key->data())
                         : nullptr;
  const ULONG key_size = key ? static_cast<ULONG>(key->size()) : 0;
  if (!BCRYPT_SUCCESS(BCryptCreateHash(alg.get(), &hash, nullptr, 0, key_bytes,
                                       key_size, 0))) {
    return {};
  }

  std::vector<uint8_t> digest(hash_length);
  bool ok = BCRYPT_SUCCESS(BCryptHashData(
      hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
      static_cast<ULONG>(data.size()), 0));
  ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest.data(), hash_length, 0));
  BCryptDestroyHash(hash);

  if (!ok)
    return {};
  return digest;
}

}  // namespace

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

  URL_COMPONENTS parts = {};
  parts.dwStructSize = sizeof(parts);
  wchar_t host[256] = {0};
  wchar_t path[2048] = {0};
  parts.lpszHostName = host;
  parts.dwHostNameLength = _countof(host);
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = _countof(path);
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts))
    return response;

  WinHttpHandle session(WinHttpOpen(L"Hare/1.0",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session)
    return response;

  WinHttpHandle connection(
      WinHttpConnect(session.get(), host, parts.nPort, 0));
  if (!connection)
    return response;

  // Credentials must never travel in the clear. Backends authenticate with a
  // Basic password or a bearer token, so a plain http:// endpoint would hand
  // them to anyone on the path; refuse rather than downgrade silently.
  const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
  if (!secure && headers.count(L"Authorization") != 0)
    return response;

  const DWORD request_flags = secure ? WINHTTP_FLAG_SECURE : 0;
  WinHttpHandle request(WinHttpOpenRequest(connection.get(), method.c_str(),
                                           path, nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           request_flags));
  if (!request)
    return response;

  for (const auto& [name, value] : headers) {
    const std::wstring line = name + L": " + value;
    WinHttpAddRequestHeaders(request.get(), line.c_str(),
                             static_cast<DWORD>(line.size()),
                             WINHTTP_ADDREQ_FLAG_ADD);
  }

  const bool sent = WinHttpSendRequest(
      request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
      body.empty() ? WINHTTP_NO_REQUEST_DATA
                   : const_cast<char*>(body.data()),
      static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
  if (!sent || !WinHttpReceiveResponse(request.get(), nullptr))
    return response;

  DWORD status = 0;
  DWORD status_size = sizeof(status);
  WinHttpQueryHeaders(request.get(),
                      WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                      WINHTTP_NO_HEADER_INDEX);
  response.status = status;

  for (;;) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request.get(), &available) || available == 0)
      break;
    const size_t offset = response.body.size();
    response.body.resize(offset + available);
    DWORD read = 0;
    if (!WinHttpReadData(request.get(), response.body.data() + offset,
                         available, &read)) {
      response.body.resize(offset);
      break;
    }
    response.body.resize(offset + read);
  }

  return response;
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
