#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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
// Changing the password re-wraps the DEK instead of re-encrypting history.

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

// The master password must be at least this long. Character-class rules are
// deliberately absent: they push people towards predictable substitutions and
// lower the real entropy, which is also the current NIST guidance.
constexpr size_t kMinPasswordLength = 10;

std::vector<uint8_t> RandomBytes(size_t count);

// AES-256-GCM. The output carries its own nonce and tag:
//   [12-byte nonce][ciphertext][16-byte tag]
std::vector<uint8_t> AesGcmEncrypt(const std::vector<uint8_t>& key,
                                   const std::string& plaintext);
std::optional<std::string> AesGcmDecrypt(const std::vector<uint8_t>& key,
                                         const std::vector<uint8_t>& sealed);

// Argon2id. An empty result means the derivation failed.
std::vector<uint8_t> DeriveKey(const std::string& password,
                               const std::vector<uint8_t>& salt);

// The wrapped DEK as it is stored remotely:
//   "HARE1" [16-byte salt][12-byte nonce][32-byte ciphertext][16-byte tag]
std::vector<uint8_t> WrapDataKey(const std::vector<uint8_t>& dek,
                                 const std::string& password);
std::optional<std::vector<uint8_t>> UnwrapDataKey(
    const std::vector<uint8_t>& wrapped,
    const std::string& password);

// The locally cached DEK, protected with DPAPI and bound to this Windows
// account. Absent until the key has been set up on this machine.
std::optional<std::vector<uint8_t>> LoadCachedDataKey();
bool CacheDataKey(const std::vector<uint8_t>& dek);

}  // namespace hare
