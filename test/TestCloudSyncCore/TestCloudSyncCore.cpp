// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Yatsuki Renka

#include <boost/detail/lightweight_test.hpp>
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../../WeaselDeployer/CloudCrypto.h"
#include "../../WeaselDeployer/CloudSnapshot.h"
#include "../../WeaselDeployer/CloudStorage.h"

namespace {

namespace fs = std::filesystem;

std::string ReadTextFile(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

fs::path FileFixtureDirectory() {
  wchar_t module_path[MAX_PATH] = {};
  const DWORD module_length = GetModuleFileNameW(
      nullptr, module_path, static_cast<DWORD>(std::size(module_path)));
  BOOST_TEST(module_length != 0);
  BOOST_TEST(module_length < std::size(module_path));
  const fs::path output_directory =
      module_length == 0 || module_length >= std::size(module_path)
          ? fs::current_path()
          : fs::path(module_path).parent_path();
  const fs::path directory = output_directory / L"TestCloudSyncCore.fixtures";
  std::error_code ec;
  fs::create_directories(directory, ec);
  BOOST_TEST(!ec);
  return directory;
}

bool ArchiveIfPresent(const fs::path& path) {
  fs::path archive = path;
  archive += L".previous";
  if (MoveFileExW(path.c_str(), archive.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return true;
  }
  const DWORD error = GetLastError();
  return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

bool HasNoTransactionArtifacts(const fs::path& directory) {
  std::error_code ec;
  for (fs::directory_iterator entry(directory, ec), end; !ec && entry != end;
       entry.increment(ec)) {
    if (entry->path().filename().wstring().find(L".hare.") !=
        std::wstring::npos) {
      return false;
    }
  }
  return !ec;
}

void TestSnapshotEnvelope() {
  std::vector<uint8_t> key(hare::kKeyLength);
  for (size_t i = 0; i < key.size(); ++i)
    key[i] = static_cast<uint8_t>(i);

  const std::string name = "device/wanxiang.userdb.txt";
  const std::string plaintext = "cloud sync regression fixture\n";
  std::vector<uint8_t> sealed = hare::EncryptSnapshot(key, name, plaintext);
  BOOST_TEST(!sealed.empty());

  std::string decrypted;
  BOOST_TEST(hare::DecryptSnapshot(key, name, sealed, &decrypted) ==
             hare::SnapshotDecryptResult::kOk);
  BOOST_TEST(decrypted == plaintext);

  decrypted = "must be cleared";
  BOOST_TEST(hare::DecryptSnapshot(key, "other/wanxiang.userdb.txt", sealed,
                                   &decrypted) ==
             hare::SnapshotDecryptResult::kAuthenticationFailed);
  BOOST_TEST(decrypted.empty());

  sealed.back() ^= 0x01;
  BOOST_TEST(hare::DecryptSnapshot(key, name, sealed, &decrypted) ==
             hare::SnapshotDecryptResult::kAuthenticationFailed);
  sealed[0] ^= 0x01;
  BOOST_TEST(hare::DecryptSnapshot(key, name, sealed, &decrypted) ==
             hare::SnapshotDecryptResult::kUnsupportedFormat);
}

void TestPublishedDataKeyResolution() {
  std::vector<uint8_t> remote(hare::kKeyLength, 0x22);
  const std::vector<uint8_t> published =
      hare::WrapDataKey(remote, "remote-password");
  BOOST_TEST(!published.empty());

  const auto winner =
      hare::ResolvePublishedDataKey(published, "different-password");
  BOOST_TEST(!winner);
}

void TestRemoteSnapshotSelection() {
  std::vector<hare::RemoteSnapshot> snapshots;
  BOOST_TEST(hare::SelectRemoteSnapshots(
      {"device/alpha.userdb.txt", "other/beta.userdb.txt"}, &snapshots));
  BOOST_TEST_EQ(snapshots.size(), 2u);

  snapshots.clear();
  BOOST_TEST(!hare::SelectRemoteSnapshots(
      {"device/alpha.userdb.txt", "device/alpha.userdb.txt"}, &snapshots));

  snapshots.clear();
  BOOST_TEST(!hare::SelectRemoteSnapshots(
      {"Device/alpha.userdb.txt", "device/ALPHA.userdb.txt"}, &snapshots));

  snapshots.clear();
  BOOST_TEST(hare::SelectRemoteSnapshots(
      {"../escape.userdb.txt", "device/replacer.userdb.txt",
       "device/valid.userdb.txt"},
      &snapshots));
  BOOST_TEST_EQ(snapshots.size(), 1u);
  BOOST_TEST(snapshots[0].name == "device/valid.userdb.txt");
}

void TestRemoteNameValidation() {
  BOOST_TEST(hare::IsPlainComponent("device"));
  BOOST_TEST(!hare::IsPlainComponent(".."));
  BOOST_TEST(!hare::IsPlainComponent("CON"));
  BOOST_TEST(!hare::IsPlainComponent("device/child"));
  BOOST_TEST(hare::LooksLikeEncryptedSnapshot("device/alpha.userdb.txt"));
  BOOST_TEST(!hare::LooksLikeEncryptedSnapshot("keys/dek.bin"));
  BOOST_TEST(!hare::LooksLikeEncryptedSnapshot("device/replacer.userdb.txt"));
}

void TestStorageNames() {
  BOOST_TEST(hare::CanonicalS3Prefix("") == "hare/");
  BOOST_TEST(hare::CanonicalS3Prefix("custom") == "custom/");
  BOOST_TEST(hare::CanonicalS3Prefix("custom/") == "custom/");

  std::string name;
  BOOST_TEST(hare::RemoveObjectPrefix("hare/device/alpha.userdb.txt", "hare/",
                                      &name) ==
             hare::ObjectPrefixResult::kObject);
  BOOST_TEST(name == "device/alpha.userdb.txt");
  BOOST_TEST(hare::RemoveObjectPrefix("hare/", "hare/", &name) ==
             hare::ObjectPrefixResult::kPrefixMarker);
  BOOST_TEST(hare::RemoveObjectPrefix("other/device/alpha.userdb.txt", "hare/",
                                      &name) ==
             hare::ObjectPrefixResult::kInvalid);
}

void TestAtomicFileCreate() {
  const fs::path directory = FileFixtureDirectory();
  const fs::path path = directory / L"create.bin";
  BOOST_TEST(ArchiveIfPresent(path));
  BOOST_TEST(hare::CreateFileAtomically(path, "first") ==
             hare::CreateFileResult::kCreated);
  BOOST_TEST(hare::CreateFileAtomically(path, "second") ==
             hare::CreateFileResult::kAlreadyExists);
  BOOST_TEST(ReadTextFile(path) == "first");
  BOOST_TEST(ArchiveIfPresent(path));
  BOOST_TEST(HasNoTransactionArtifacts(directory));
}

void TestAtomicFileBatchRollback() {
  const fs::path directory = FileFixtureDirectory();
  const fs::path first = directory / L"first.bin";
  const fs::path second = directory / L"second.bin";
  BOOST_TEST(hare::WriteFileAtomically(first, "original-first"));
  BOOST_TEST(hare::WriteFileAtomically(second, "original-second"));

  const HANDLE locked =
      CreateFileW(second.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL, nullptr);
  BOOST_TEST(locked != INVALID_HANDLE_VALUE);
  if (locked == INVALID_HANDLE_VALUE)
    return;

  const hare::FileBatchResult result = hare::WriteFilesAtomically(
      {{first, "replacement-first"}, {second, "replacement-second"}});
  CloseHandle(locked);

  BOOST_TEST(result == hare::FileBatchResult::kRolledBack);
  BOOST_TEST(ReadTextFile(first) == "original-first");
  BOOST_TEST(ReadTextFile(second) == "original-second");
  BOOST_TEST(HasNoTransactionArtifacts(directory));
}

void TestStageFailureCleanup() {
  const fs::path directory = FileFixtureDirectory();
  const fs::path directory_target = directory / L"stage-target-directory";
  std::error_code ec;
  fs::create_directories(directory_target, ec);
  BOOST_TEST(!ec);

  BOOST_TEST(hare::CreateFileAtomically(directory_target, "create") ==
             hare::CreateFileResult::kError);
  BOOST_TEST(fs::is_directory(directory_target, ec));
  BOOST_TEST(!ec);
  BOOST_TEST(HasNoTransactionArtifacts(directory));

  BOOST_TEST(hare::WriteFilesAtomically({{directory_target, "batch"}}) ==
             hare::FileBatchResult::kRolledBack);
  BOOST_TEST(fs::is_directory(directory_target, ec));
  BOOST_TEST(!ec);
  BOOST_TEST(HasNoTransactionArtifacts(directory));
}

}  // namespace

int main() {
  TestSnapshotEnvelope();
  TestPublishedDataKeyResolution();
  TestRemoteSnapshotSelection();
  TestRemoteNameValidation();
  TestStorageNames();
  TestAtomicFileCreate();
  TestAtomicFileBatchRollback();
  TestStageFailureCleanup();
  return boost::report_errors();
}
