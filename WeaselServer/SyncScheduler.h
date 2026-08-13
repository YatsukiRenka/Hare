// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Yatsuki Renka

#pragma once

#include <filesystem>
#include <thread>

// Runs cloud synchronisation on a timer.
//
// It lives in the server because the server is the process that stays running:
// the deployer is one-shot and could not hold a timer. Nothing is hooked to
// shutdown either - processes are killed at logoff, so a hook there would run
// on a good day and be missed on a bad one.
//
// The schedule is read from the registry on every tick rather than cached, so
// changing it in the settings panel takes effect on the next one instead of at
// the next server start.

namespace hare {

class SyncScheduler {
 public:
  ~SyncScheduler();

  // `deployer` is the full path of HareDeployer.exe. Starting twice does
  // nothing the second time.
  void Start(const std::filesystem::path& deployer);
  void Stop();

 private:
  void Loop(std::filesystem::path deployer);

  HANDLE stop_ = nullptr;
  std::thread thread_;
};

}  // namespace hare
