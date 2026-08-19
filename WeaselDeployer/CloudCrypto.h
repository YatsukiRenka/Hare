// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Yatsuki Renka

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "CloudHttp.h"

// End-to-end encryption for the snapshots.
//
// Word frequency data records every word the user has typed, so it is treated
// as sensitive and never leaves the machine in the clear.
//
// Two keys are involved:
//
//   DEK  32 random bytes, shared by every device, encrypts the snapshots
//   KEK  derived from the master password with Argon2id, encrypts the DEK
//
// The wrapped DEK is stored remotely, so a new device only needs the password.
// Once unwrapped, the DEK is cached locally under DPAPI and the password is no
// longer needed, which is what allows unattended synchronisation at startup.
// The design permits password rotation by re-wrapping the DEK instead of
// re-encrypting history; the rotation operation itself is not implemented yet.

namespace hare {

// Argon2id parameters. Memory hardness is the point: a password-only defence
// against offline brute force has to make each guess expensive in RAM, not
// just in CPU cycles.
constexpr uint32_t kArgon2MemoryKiB = 64 * 1024;
constexpr uint32_t kArgon2Iterations = 3;
constexpr uint32_t kArgon2Parallelism = 1;

constexpr size_t kKeyLength = 32;
constexpr size_t kSaltLength = 16;
constexpr size_t kNonceLength = 12;
constexpr size_t kTagLength = 16;
constexpr size_t kAesGcmRawOverhead = kNonceLength + kTagLength;
constexpr size_t kSnapshotMagicLength = 8;
constexpr size_t kSnapshotHeaderLength = kSnapshotMagicLength + 1;
constexpr size_t kSnapshotEnvelopeOverhead =
    kSnapshotHeaderLength + kAesGcmRawOverhead;
static_assert(kMaxCloudObjectBytes > kSnapshotEnvelopeOverhead);
constexpr size_t kMaxSnapshotPlaintextBytes =
    kMaxCloudObjectBytes - kSnapshotEnvelopeOverhead;

// The master password must be at least this long. Character-class rules are
// deliberately absent: they push people towards predictable substitutions and
// lower the real entropy, which is also the current NIST guidance.
constexpr size_t kMinPasswordLength = 10;

std::vector<uint8_t> RandomBytes(size_t count);

// Version-1 snapshot envelope (all offsets are byte offsets):
//
//   0..7       ASCII "HARESNAP" format marker
//   8          version byte 0x01
//   9..20      12-byte AES-GCM nonce
//   21..N-17   ciphertext, the same length as the non-empty plaintext
//   N-16..N-1  16-byte AES-GCM authentication tag
//
// Only this version is emitted or accepted. Its GCM associated data is exactly:
//
//   ASCII "HARESNAP" || 0x01 || U32_BE(name_length) || name
//
// The marker contributes exactly eight bytes and no NUL terminator; the version
// contributes one byte. U32_BE contributes four bytes, most-significant byte
// first, and encodes the number of bytes in `name`. `name` is the exact UTF-8
// remote object name passed to SyncBackend::Get/Put, already normalized by the
// sync layer to "<installation>/<file>" with one forward slash. The length
// prefix makes its boundary unambiguous. The marker and version are repeated in
// the AAD deliberately, so the version and exact on-wire name are authenticated
// together with the ciphertext.
enum class SnapshotDecryptResult {
  kOk,
  kUnsupportedFormat,
  kAuthenticationFailed,
};

std::vector<uint8_t> EncryptSnapshot(const std::vector<uint8_t>& key,
                                     const std::string& remote_name,
                                     const std::string& plaintext);
SnapshotDecryptResult DecryptSnapshot(const std::vector<uint8_t>& key,
                                      const std::string& remote_name,
                                      const std::vector<uint8_t>& sealed,
                                      std::string* plaintext);

// Argon2id. An empty result means the derivation failed.
std::vector<uint8_t> DeriveKey(const std::string& password,
                               const std::vector<uint8_t>& salt);

// The wrapped DEK as it is stored remotely:
//   "HARE1" [16-byte salt][12-byte nonce][32-byte ciphertext][16-byte tag]
// It intentionally keeps this existing raw-GCM format instead of using the
// snapshot envelope: its fixed `keys/dek.bin` name is unique and no other
// object is read as a wrapped key, while changing it would make every user
// re-establish the data key for no security benefit.
std::vector<uint8_t> WrapDataKey(const std::vector<uint8_t>& dek,
                                 const std::string& password);
std::optional<std::vector<uint8_t>> UnwrapDataKey(
    const std::vector<uint8_t>& wrapped,
    const std::string& password);

// Resolves the object read back after publishing a newly generated key. The
// storage copy is authoritative because another device may have won the race.
std::optional<std::vector<uint8_t>> ResolvePublishedDataKey(
    const std::vector<uint8_t>& published,
    const std::string& password);

// The locally cached DEK, protected with DPAPI and bound to this Windows
// account. Absent until the key has been set up on this machine.
std::optional<std::vector<uint8_t>> LoadCachedDataKey();
bool CacheDataKey(const std::vector<uint8_t>& dek);

// Drops the cached copy. Called when the configuration starts pointing at a
// different storage, whose key is a different one; keeping the old key would
// encrypt snapshots that nothing there can read.
void ForgetCachedDataKey();

}  // namespace hare
