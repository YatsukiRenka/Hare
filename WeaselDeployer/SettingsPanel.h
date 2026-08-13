// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Yatsuki Renka

#pragma once

#include <functional>

// The cloud sync settings panel.
//
// It exists to take the master password off the command line: passing it as
// `HareDeployer.exe /cloudkey:<password>` put it where every other process on
// the machine could read it. The panel also replaces hand-editing the registry
// as the way to choose a backend and enter its credentials.
//
// The page is HTML rendered by WebView2 and compiled into the executable as a
// resource. WebView2 belongs here and nowhere near the candidate window: the
// deployer is a one-shot process, so its startup cost is invisible, while the
// candidate window redraws on every keystroke.

namespace hare {

// Runs the panel until the user closes it. `run_sync` performs one
// synchronisation run and returns what Configurator::SyncUserData does - 0 for
// success, kCloudSyncFailed when only the cloud half failed - so the panel can
// say which half went wrong. It is passed in so this file needs to know nothing
// about Rime.
//
// Returns 0 once the panel has been shown and closed, 1 if it could not be
// shown at all - which on a machine without the WebView2 runtime is the
// expected outcome, and is reported to the user before returning.
int ShowSettingsPanel(const std::function<int()>& run_sync);

}  // namespace hare
