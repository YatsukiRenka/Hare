#pragma once

#include "resource.h"
#include <resource.h>
#include <WeaselIPC.h>
#include <WeaselUI.h>
#include <RimeWithWeasel.h>
#include <WeaselUtility.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <winsparkle.h>

#include "WeaselTrayIcon.h"

namespace fs = std::filesystem;

// Hare's own update feeds. They must never point at rime/weasel: an update
// check that resolved there would offer the official product as an upgrade to
// this one.
constexpr const char* kReleaseFeedUrl =
    "https://yatsukirenka.github.io/Hare/release/appcast.xml";
constexpr const char* kTestingFeedUrl =
    "https://yatsukirenka.github.io/Hare/testing/appcast.xml";

class WeaselServerApp {
 public:
  static bool execute(const fs::path& cmd, const std::wstring& args) {
    return (uintptr_t)ShellExecuteW(NULL, NULL, cmd.c_str(), args.c_str(), NULL,
                                    SW_SHOWNORMAL) > 32;
  }

  static bool explore(const fs::path& path) {
    std::wstring quoted_path(L"\"" + path.wstring() + L"\"");
    return (uintptr_t)ShellExecuteW(NULL, L"explore", quoted_path.c_str(), NULL,
                                    NULL, SW_SHOWNORMAL) > 32;
  }

  static bool open(const fs::path& path) {
    return (uintptr_t)ShellExecuteW(NULL, L"open", path.c_str(), NULL, NULL,
                                    SW_SHOWNORMAL) > 32;
  }

  static bool check_update() {
    // The APPCAST resources inherited from upstream still describe the official
    // Weasel releases, so they are bypassed entirely: consulting them would let
    // Hare offer, and install, a different product over itself. Until Hare
    // publishes a signed feed of its own these addresses simply fail to
    // resolve, which leaves the check harmlessly unsuccessful.
    std::string feed_url = kReleaseFeedUrl;
    std::wstring channel{};
    auto ret = RegGetStringValue(HKEY_CURRENT_USER, L"Software\\Rime\\Hare",
                                 L"UpdateChannel", channel);
    if (!ret && channel == L"testing") {
      feed_url = kTestingFeedUrl;
    }
    win_sparkle_set_appcast_url(feed_url.c_str());
    win_sparkle_check_update_with_ui();
    return true;
  }

  static fs::path install_dir() {
    WCHAR exe_path[MAX_PATH] = {0};
    GetModuleFileNameW(GetModuleHandle(NULL), exe_path, _countof(exe_path));
    return fs::path(exe_path).remove_filename();
  }

 public:
  WeaselServerApp();
  ~WeaselServerApp();
  int Run();

 protected:
  void SetupMenuHandlers();

  weasel::Server m_server;
  weasel::UI m_ui;
  WeaselTrayIcon tray_icon;
  std::unique_ptr<RimeWithWeaselHandler> m_handler;
};
