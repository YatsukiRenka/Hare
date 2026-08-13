// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Yatsuki Renka

#include "stdafx.h"
#include "SyncScheduler.h"

#include <HareCloudSync.h>

#include <string>

namespace hare {

namespace {

// Long enough for the desktop to settle after logon, short enough that the
// first sync of a session still feels like part of starting up.
constexpr DWORD kStartupDelayMs = 20 * 1000;

// With scheduled sync switched off the thread still wakes up occasionally, so
// that switching it back on in the panel does not wait for a server restart.
constexpr DWORD kIdleRecheckMs = 5 * 60 * 1000;

// The deployer is spawned rather than the sync being done here: it already owns
// the whole procedure, including putting the server into maintenance mode, and
// it holds a mutex that keeps a scheduled run from colliding with a manual one.
void RunSync(const std::filesystem::path& deployer) {
  std::wstring command = L"\"" + deployer.wstring() + L"\" /sync";

  STARTUPINFOW startup = {sizeof(startup)};
  PROCESS_INFORMATION process = {0};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
    return;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
}

}  // namespace

SyncScheduler::~SyncScheduler() {
  Stop();
}

void SyncScheduler::Start(const std::filesystem::path& deployer) {
  if (thread_.joinable())
    return;
  stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!stop_)
    return;
  thread_ = std::thread(&SyncScheduler::Loop, this, deployer);
}

void SyncScheduler::Stop() {
  if (stop_)
    SetEvent(stop_);
  if (thread_.joinable())
    thread_.join();
  if (stop_) {
    CloseHandle(stop_);
    stop_ = nullptr;
  }
}

void SyncScheduler::Loop(std::filesystem::path deployer) {
  if (WaitForSingleObject(stop_, kStartupDelayMs) != WAIT_TIMEOUT)
    return;

  if (SyncOnStartup() && CloudSyncConfigured())
    RunSync(deployer);

  for (;;) {
    const DWORD minutes = SyncIntervalMinutes();
    const DWORD wait = minutes == 0 ? kIdleRecheckMs : minutes * 60 * 1000;
    if (WaitForSingleObject(stop_, wait) != WAIT_TIMEOUT)
      return;
    if (minutes != 0 && CloudSyncConfigured())
      RunSync(deployer);
  }
}

}  // namespace hare
