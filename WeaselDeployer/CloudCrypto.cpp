// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Yatsuki Renka

#include "stdafx.h"
#include "CloudCrypto.h"

#include <bcrypt.h>
#include <dpapi.h>

#include <cstring>
#include <memory>

#include "CloudHttp.h"

extern "C" {
#include <argon2.h>
}

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace hare {

namespace {

constexpr const wchar_t* kConfigKey = L"Software\\Rime\\Hare\\CloudSync";
constexpr const wchar_t* kCachedKeyValue = L"DataKey";

// Marks the wrapped key format so a future change can be told apart from a
// corrupt blob.
constexpr const char kWrapMagic[] = "HARE1";
constexpr size_t kWrapMagicLength = 5;

struct AlgDeleter {
  void operator()(BCRYPT_ALG_HANDLE h) const {
    if (h)
      BCryptCloseAlgorithmProvider(h, 0);
  }
};
using AlgHandle = std::unique_ptr<void, AlgDeleter>;

struct KeyDeleter {
  void operator()(BCRYPT_KEY_HANDLE h) const {
    if (h)
      BCryptDestroyKey(h);
  }
};
using KeyHandle = std::unique_ptr<void, KeyDeleter>;

AlgHandle OpenAesGcm() {
  BCRYPT_ALG_HANDLE raw = nullptr;
  if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&raw, BCRYPT_AES_ALGORITHM,
                                                  nullptr, 0))) {
    return nullptr;
  }
  AlgHandle alg(raw);
  if (!BCRYPT_SUCCESS(BCryptSetProperty(
          alg.get(), BCRYPT_CHAINING_MODE,
          reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
          sizeof(BCRYPT_CHAIN_MODE_GCM), 0))) {
    return nullptr;
  }
  return alg;
}

bool WriteRegBinary(const wchar_t* value, const std::vector<uint8_t>& data) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kConfigKey, 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return false;
  }
  const LSTATUS status =
      RegSetValueExW(key, value, 0, REG_BINARY, data.data(),
                     static_cast<DWORD>(data.size()));
  RegCloseKey(key);
  return status == ERROR_SUCCESS;
}

std::vector<uint8_t> ReadRegBinary(const wchar_t* value) {
  DWORD size = 0;
  if (RegGetValueW(HKEY_CURRENT_USER, kConfigKey, value, RRF_RT_REG_BINARY,
                   nullptr, nullptr, &size) != ERROR_SUCCESS ||
      size == 0) {
    return {};
  }
  std::vector<uint8_t> data(size);
  if (RegGetValueW(HKEY_CURRENT_USER, kConfigKey, value, RRF_RT_REG_BINARY,
                   nullptr, data.data(), &size) != ERROR_SUCCESS) {
    return {};
  }
  data.resize(size);
  return data;
}

std::vector<uint8_t> ProtectBytes(const std::vector<uint8_t>& plain) {
  DATA_BLOB in = {};
  in.cbData = static_cast<DWORD>(plain.size());
  in.pbData = const_cast<BYTE*>(plain.data());

  DATA_BLOB out = {};
  if (!CryptProtectData(&in, L"Hare cloud sync data key", nullptr, nullptr,
                        nullptr, 0, &out)) {
    return {};
  }
  std::vector<uint8_t> result(out.pbData, out.pbData + out.cbData);
  LocalFree(out.pbData);
  return result;
}

std::vector<uint8_t> UnprotectBytes(const std::vector<uint8_t>& sealed) {
  DATA_BLOB in = {};
  in.cbData = static_cast<DWORD>(sealed.size());
  in.pbData = const_cast<BYTE*>(sealed.data());

  DATA_BLOB out = {};
  if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out))
    return {};

  std::vector<uint8_t> result(out.pbData, out.pbData + out.cbData);
  SecureZeroMemory(out.pbData, out.cbData);
  LocalFree(out.pbData);
  return result;
}

}  // namespace

std::vector<uint8_t> RandomBytes(size_t count) {
  std::vector<uint8_t> buffer(count);
  if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, buffer.data(),
                                      static_cast<ULONG>(buffer.size()),
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
    return {};
  }
  return buffer;
}

std::vector<uint8_t> AesGcmEncrypt(const std::vector<uint8_t>& key,
                                   const std::string& plaintext) {
  AlgHandle alg = OpenAesGcm();
  if (!alg || key.size() != kKeyLength)
    return {};

  BCRYPT_KEY_HANDLE raw_key = nullptr;
  if (!BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(
          alg.get(), &raw_key, nullptr, 0, const_cast<PUCHAR>(key.data()),
          static_cast<ULONG>(key.size()), 0))) {
    return {};
  }
  KeyHandle key_handle(raw_key);

  const std::vector<uint8_t> nonce = RandomBytes(kNonceLength);
  if (nonce.empty())
    return {};

  std::vector<uint8_t> tag(kTagLength);
  std::vector<uint8_t> ciphertext(plaintext.size());

  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
  BCRYPT_INIT_AUTH_MODE_INFO(info);
  info.pbNonce = const_cast<PUCHAR>(nonce.data());
  info.cbNonce = static_cast<ULONG>(nonce.size());
  info.pbTag = tag.data();
  info.cbTag = static_cast<ULONG>(tag.size());

  ULONG written = 0;
  if (!BCRYPT_SUCCESS(BCryptEncrypt(
          key_handle.get(),
          reinterpret_cast<PUCHAR>(const_cast<char*>(plaintext.data())),
          static_cast<ULONG>(plaintext.size()), &info, nullptr, 0,
          ciphertext.empty() ? nullptr : ciphertext.data(),
          static_cast<ULONG>(ciphertext.size()), &written, 0))) {
    return {};
  }
  ciphertext.resize(written);

  std::vector<uint8_t> sealed;
  sealed.reserve(nonce.size() + ciphertext.size() + tag.size());
  sealed.insert(sealed.end(), nonce.begin(), nonce.end());
  sealed.insert(sealed.end(), ciphertext.begin(), ciphertext.end());
  sealed.insert(sealed.end(), tag.begin(), tag.end());
  return sealed;
}

std::optional<std::string> AesGcmDecrypt(const std::vector<uint8_t>& key,
                                         const std::vector<uint8_t>& sealed) {
  if (key.size() != kKeyLength || sealed.size() < kNonceLength + kTagLength)
    return std::nullopt;

  AlgHandle alg = OpenAesGcm();
  if (!alg)
    return std::nullopt;

  BCRYPT_KEY_HANDLE raw_key = nullptr;
  if (!BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(
          alg.get(), &raw_key, nullptr, 0, const_cast<PUCHAR>(key.data()),
          static_cast<ULONG>(key.size()), 0))) {
    return std::nullopt;
  }
  KeyHandle key_handle(raw_key);

  const size_t cipher_length = sealed.size() - kNonceLength - kTagLength;
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
  BCRYPT_INIT_AUTH_MODE_INFO(info);
  info.pbNonce = const_cast<PUCHAR>(sealed.data());
  info.cbNonce = kNonceLength;
  info.pbTag = const_cast<PUCHAR>(sealed.data() + kNonceLength + cipher_length);
  info.cbTag = kTagLength;

  std::string plaintext(cipher_length, '\0');
  ULONG written = 0;
  // A wrong key or tampered data fails here rather than returning garbage,
  // which is the reason for choosing an authenticated cipher.
  if (!BCRYPT_SUCCESS(BCryptDecrypt(
          key_handle.get(),
          const_cast<PUCHAR>(sealed.data() + kNonceLength),
          static_cast<ULONG>(cipher_length), &info, nullptr, 0,
          plaintext.empty() ? nullptr
                            : reinterpret_cast<PUCHAR>(plaintext.data()),
          static_cast<ULONG>(plaintext.size()), &written, 0))) {
    return std::nullopt;
  }
  plaintext.resize(written);
  return plaintext;
}

std::vector<uint8_t> DeriveKey(const std::string& password,
                               const std::vector<uint8_t>& salt) {
  std::vector<uint8_t> key(kKeyLength);
  const int result = argon2id_hash_raw(
      kArgon2Iterations, kArgon2MemoryKiB, kArgon2Parallelism, password.data(),
      password.size(), salt.data(), salt.size(), key.data(), key.size());
  if (result != ARGON2_OK)
    return {};
  return key;
}

std::vector<uint8_t> WrapDataKey(const std::vector<uint8_t>& dek,
                                 const std::string& password) {
  const std::vector<uint8_t> salt = RandomBytes(kSaltLength);
  if (salt.empty())
    return {};

  const std::vector<uint8_t> kek = DeriveKey(password, salt);
  if (kek.empty())
    return {};

  const std::vector<uint8_t> sealed =
      AesGcmEncrypt(kek, std::string(dek.begin(), dek.end()));
  if (sealed.empty())
    return {};

  std::vector<uint8_t> wrapped;
  wrapped.insert(wrapped.end(), kWrapMagic, kWrapMagic + kWrapMagicLength);
  wrapped.insert(wrapped.end(), salt.begin(), salt.end());
  wrapped.insert(wrapped.end(), sealed.begin(), sealed.end());
  return wrapped;
}

std::optional<std::vector<uint8_t>> UnwrapDataKey(
    const std::vector<uint8_t>& wrapped,
    const std::string& password) {
  if (wrapped.size() < kWrapMagicLength + kSaltLength)
    return std::nullopt;
  if (std::memcmp(wrapped.data(), kWrapMagic, kWrapMagicLength) != 0)
    return std::nullopt;

  const std::vector<uint8_t> salt(
      wrapped.begin() + kWrapMagicLength,
      wrapped.begin() + kWrapMagicLength + kSaltLength);
  const std::vector<uint8_t> kek = DeriveKey(password, salt);
  if (kek.empty())
    return std::nullopt;

  const std::vector<uint8_t> sealed(
      wrapped.begin() + kWrapMagicLength + kSaltLength, wrapped.end());
  const auto plaintext = AesGcmDecrypt(kek, sealed);
  if (!plaintext || plaintext->size() != kKeyLength)
    return std::nullopt;

  return std::vector<uint8_t>(plaintext->begin(), plaintext->end());
}

std::optional<std::vector<uint8_t>> LoadCachedDataKey() {
  const std::vector<uint8_t> sealed = ReadRegBinary(kCachedKeyValue);
  if (sealed.empty())
    return std::nullopt;
  std::vector<uint8_t> key = UnprotectBytes(sealed);
  if (key.size() != kKeyLength)
    return std::nullopt;
  return key;
}

bool CacheDataKey(const std::vector<uint8_t>& dek) {
  const std::vector<uint8_t> sealed = ProtectBytes(dek);
  if (sealed.empty())
    return false;
  return WriteRegBinary(kCachedKeyValue, sealed);
}

}  // namespace hare
