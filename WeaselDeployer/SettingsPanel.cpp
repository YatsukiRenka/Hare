// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Yatsuki Renka

#include "stdafx.h"
#include "SettingsPanel.h"

#include <HareCloudSync.h>
#include <WeaselUtility.h>

#include <shlobj.h>
#include <wrl.h>

#include <WebView2.h>

#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "CloudCrypto.h"
#include "CloudHttp.h"
#include "CloudSync.h"
#include "Configurator.h"
#include "WeaselDeployer.h"

#pragma comment(lib, "WebView2LoaderStatic.lib")

#ifndef IDS_STR_SETTINGS_PANEL_FAILED
#define IDS_STR_SETTINGS_PANEL_FAILED 160
#endif

namespace hare {

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

// Host and page exchange unit-separated key=value pairs, values percent-encoded
// the way encodeURIComponent produces them. The payload is a flat record of
// short strings, so a JSON parser would earn nothing here beyond another
// dependency to carry through every upstream merge.
constexpr wchar_t kSeparator = L'\x1f';

// Posted by a worker thread once its operation has finished. wParam is unused,
// lParam owns a TaskResult.
constexpr UINT kTaskDone = WM_APP + 1;

// User-facing wording lives in the page, so the host reports outcomes as codes.
// This also keeps every non-ASCII string out of the C++ sources, where the
// compiler's source encoding would otherwise have to be pinned down.
struct TaskResult {
  const wchar_t* code = L"";
  bool ok = false;
  // The configuration changed, so the page needs the stored state again: which
  // credentials now exist, whether a data key is cached.
  bool refresh = false;
  bool clear_password = false;
};

using Fields = std::map<std::wstring, std::wstring>;

std::wstring Field(const Fields& fields, const wchar_t* name) {
  const auto it = fields.find(name);
  return it == fields.end() ? std::wstring() : it->second;
}

// Percent-encoded payloads are ASCII by construction, so anything outside it
// arrived malformed and is replaced rather than silently truncated to a byte.
std::string NarrowAscii(const std::wstring& text) {
  std::string out;
  out.reserve(text.size());
  for (wchar_t c : text)
    out.push_back(c > 0 && c < 128 ? static_cast<char>(c) : '?');
  return out;
}

std::string PercentDecode(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] != '%' || i + 2 >= value.size()) {
      out.push_back(value[i]);
      continue;
    }
    const auto digit = [](char c) -> int {
      if (c >= '0' && c <= '9')
        return c - '0';
      if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
      if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
      return -1;
    };
    const int high = digit(value[i + 1]);
    const int low = digit(value[i + 2]);
    if (high < 0 || low < 0) {
      out.push_back(value[i]);
      continue;
    }
    out.push_back(static_cast<char>((high << 4) | low));
    i += 2;
  }
  return out;
}

std::wstring Encode(const std::wstring& verb, const Fields& fields) {
  std::wstring message = verb;
  for (const auto& [key, value] : fields) {
    const std::string encoded = UriEncode(wtou8(value), true);
    message += kSeparator;
    message += key;
    message += L'=';
    message += std::wstring(encoded.begin(), encoded.end());
  }
  return message;
}

std::wstring Decode(const std::wstring& message, Fields* fields) {
  size_t begin = message.find(kSeparator);
  const std::wstring verb = message.substr(0, begin);
  while (begin != std::wstring::npos) {
    const size_t end = message.find(kSeparator, begin + 1);
    const std::wstring pair =
        message.substr(begin + 1, end == std::wstring::npos ? std::wstring::npos
                                                            : end - begin - 1);
    const size_t split = pair.find(L'=');
    if (split != std::wstring::npos && split > 0) {
      (*fields)[pair.substr(0, split)] =
          u8tow(PercentDecode(NarrowAscii(pair.substr(split + 1))));
    }
    begin = end;
  }
  return verb;
}

SyncConfig ConfigFromFields(const Fields& fields) {
  SyncConfig config;
  const std::wstring backend = Field(fields, L"backend");
  if (backend == L"localdir")
    config.backend = SyncConfig::Backend::kLocalDir;
  else if (backend == L"s3")
    config.backend = SyncConfig::Backend::kS3;
  else if (backend == L"webdav")
    config.backend = SyncConfig::Backend::kWebDav;
  else if (backend == L"worker")
    config.backend = SyncConfig::Backend::kWorker;

  config.local_dir = Field(fields, L"localDir");

  config.endpoint = wtou8(Field(fields, L"endpoint"));
  config.bucket = wtou8(Field(fields, L"bucket"));
  config.prefix = wtou8(Field(fields, L"prefix"));
  if (config.prefix.empty())
    config.prefix = "hare/";
  config.access_key = wtou8(Field(fields, L"accessKey"));
  config.secret_key = wtou8(Field(fields, L"secretKey"));

  config.dav_url = wtou8(Field(fields, L"davUrl"));
  config.dav_username = wtou8(Field(fields, L"davUsername"));
  config.dav_password = wtou8(Field(fields, L"davPassword"));

  config.worker_url = wtou8(Field(fields, L"workerUrl"));
  config.worker_token = wtou8(Field(fields, L"workerToken"));
  return config;
}

// A blank credential field means "leave the stored one alone", which the save
// path honours. Testing a connection has to honour it too, or every test of an
// already-configured backend would fail for want of a credential that is in
// fact present.
void MergeStoredSecrets(SyncConfig* config) {
  const SyncConfig stored = SyncConfig::LoadForEditing();
  if (config->access_key.empty())
    config->access_key = stored.access_key;
  if (config->secret_key.empty())
    config->secret_key = stored.secret_key;
  if (config->dav_password.empty())
    config->dav_password = stored.dav_password;
  if (config->worker_token.empty())
    config->worker_token = stored.worker_token;
}

unsigned IntervalFromFields(const Fields& fields) {
  const std::wstring value = Field(fields, L"intervalMinutes");
  if (value.empty())
    return 0;
  try {
    const long minutes = std::stol(value);
    return minutes < 0 ? 0 : static_cast<unsigned>(minutes);
  } catch (const std::exception&) {
    return 0;
  }
}

const wchar_t* BackendName(SyncConfig::Backend backend) {
  switch (backend) {
    case SyncConfig::Backend::kLocalDir:
      return L"localdir";
    case SyncConfig::Backend::kS3:
      return L"s3";
    case SyncConfig::Backend::kWebDav:
      return L"webdav";
    case SyncConfig::Backend::kWorker:
      return L"worker";
    case SyncConfig::Backend::kNone:
    default:
      return L"none";
  }
}

const wchar_t* KeySetupCode(KeySetupResult result) {
  switch (result) {
    case KeySetupResult::kOk:
      return L"key_ok";
    case KeySetupResult::kPasswordTooShort:
      return L"key_short";
    case KeySetupResult::kNoBackend:
      return L"key_no_backend";
    case KeySetupResult::kWrongPassword:
      return L"key_wrong_password";
    case KeySetupResult::kKeyGenerationFailed:
      return L"key_generate_failed";
    case KeySetupResult::kPublishFailed:
      return L"key_publish_failed";
    case KeySetupResult::kCacheFailed:
      return L"key_cache_failed";
    case KeySetupResult::kStorageUnreachable:
    default:
      return L"key_unreachable";
  }
}

// WebView2 keeps a profile on disk. Left to itself it would put it next to the
// executable, which is the installation directory and not writable for a
// standard user.
std::wstring WebViewProfilePath() {
  PWSTR local = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
    return {};
  std::wstring path(local);
  CoTaskMemFree(local);
  path += L"\\Hare\\WebView2";
  return path;
}

std::wstring LoadPage() {
  HMODULE module = GetModuleHandleW(nullptr);
  HRSRC resource =
      FindResourceW(module, MAKEINTRESOURCEW(IDR_SETTINGS_PAGE), RT_RCDATA);
  if (!resource)
    return {};
  const DWORD size = SizeofResource(module, resource);
  HGLOBAL loaded = LoadResource(module, resource);
  const char* bytes = static_cast<const char*>(LockResource(loaded));
  if (!bytes || size == 0)
    return {};
  return u8tow(std::string(bytes, size));
}

class SettingsPanel : public CWindowImpl<SettingsPanel> {
 public:
  DECLARE_WND_CLASS_EX(L"HareCloudSyncSettings",
                       CS_HREDRAW | CS_VREDRAW,
                       COLOR_WINDOW)

  explicit SettingsPanel(const std::function<int()>& run_sync)
      : run_sync_(run_sync) {}

  BEGIN_MSG_MAP(SettingsPanel)
  MESSAGE_HANDLER(WM_SIZE, OnSize)
  MESSAGE_HANDLER(WM_CLOSE, OnClose)
  MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
  MESSAGE_HANDLER(kTaskDone, OnTaskDone)
  END_MSG_MAP()

  bool Open();
  bool InitializationFailed() const { return initialization_failed_; }

 private:
  LRESULT OnSize(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnClose(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnTaskDone(UINT, WPARAM, LPARAM, BOOL&);

  void CreateWebView();
  void AttachWebView(ICoreWebView2Controller* controller);
  void FailInitialization();
  void FitWebView();

  void OnPageMessage(const std::wstring& message);
  void SendSettings();
  void SendStatus(const wchar_t* level,
                  const wchar_t* code,
                  bool clear_password);
  void RunAsync(const wchar_t* busy_code,
                TaskResult failure,
                std::function<TaskResult()> work);

  ComPtr<ICoreWebView2Controller> controller_;
  ComPtr<ICoreWebView2> webview_;
  std::function<int()> run_sync_;
  bool busy_ = false;
  bool close_pending_ = false;
  bool initialization_failed_ = false;
};

// The page lays itself out in CSS pixels and WebView2 scales those by the
// monitor's factor, so a window sized in raw device pixels would leave the page
// a viewport a third too narrow on a 150% display and the form would spill out
// of it. GetDpiForWindow is resolved at run time because the project targets a
// Windows version older than the one that introduced it.
UINT WindowDpi(HWND window) {
  using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
  static const auto get_dpi = reinterpret_cast<GetDpiForWindowFn>(
      GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
  if (get_dpi) {
    const UINT dpi = get_dpi(window);
    if (dpi != 0)
      return dpi;
  }
  HDC screen = GetDC(nullptr);
  if (!screen)
    return 96;
  const int dpi = GetDeviceCaps(screen, LOGPIXELSX);
  ReleaseDC(nullptr, screen);
  return dpi > 0 ? static_cast<UINT>(dpi) : 96;
}

bool SettingsPanel::Open() {
  constexpr int kWidthDip = 920;
  constexpr int kHeightDip = 760;

  RECT initial = {0, 0, kWidthDip, kHeightDip};
  if (!Create(nullptr, initial, nullptr, WS_OVERLAPPEDWINDOW))
    return false;

  // Sizing needs the window's own DPI, which is only knowable once it exists.
  const UINT dpi = WindowDpi(m_hWnd);

  RECT work = {0};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);

  // On a small screen at a high scaling factor the scaled window would not fit;
  // the page scrolls, so shrinking it is better than hanging off the edge.
  const int width = min(MulDiv(kWidthDip, dpi, 96), work.right - work.left);
  const int height = min(MulDiv(kHeightDip, dpi, 96), work.bottom - work.top);

  const int left = work.left + ((work.right - work.left) - width) / 2;
  const int top = work.top + ((work.bottom - work.top) - height) / 2;
  MoveWindow(left, top, width, height, FALSE);

  HICON icon = LoadIconW(GetModuleHandleW(nullptr),
                         MAKEINTRESOURCEW(IDI_WEASELDEPLOYER));
  if (icon) {
    SetIcon(icon, TRUE);
    SetIcon(icon, FALSE);
  }
  ShowWindow(SW_SHOW);
  UpdateWindow();
  CreateWebView();
  return true;
}

void SettingsPanel::CreateWebView() {
  const std::wstring profile = WebViewProfilePath();
  const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
      nullptr, profile.empty() ? nullptr : profile.c_str(), nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [this](HRESULT result,
                 ICoreWebView2Environment* environment) -> HRESULT {
            if (FAILED(result) || !environment) {
              FailInitialization();
              return result;
            }
            return environment->CreateCoreWebView2Controller(
                m_hWnd,
                Callback<
                    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [this](HRESULT result,
                           ICoreWebView2Controller* controller) -> HRESULT {
                      if (FAILED(result) || !controller) {
                        FailInitialization();
                        return result;
                      }
                      AttachWebView(controller);
                      return S_OK;
                    })
                    .Get());
          })
          .Get());
  if (FAILED(hr))
    FailInitialization();
}

void SettingsPanel::AttachWebView(ICoreWebView2Controller* controller) {
  controller_ = controller;
  controller_->get_CoreWebView2(&webview_);
  if (!webview_) {
    FailInitialization();
    return;
  }

  ComPtr<ICoreWebView2Settings> settings;
  if (SUCCEEDED(webview_->get_Settings(&settings)) && settings) {
    // The page is a settings form, not a browser: none of these belong in it,
    // and every one of them is a way to leave the page unexpectedly.
    settings->put_AreDevToolsEnabled(FALSE);
    settings->put_AreDefaultContextMenusEnabled(FALSE);
    settings->put_IsStatusBarEnabled(FALSE);
    settings->put_IsZoomControlEnabled(FALSE);
  }

  EventRegistrationToken token = {};
  webview_->add_WebMessageReceived(
      Callback<ICoreWebView2WebMessageReceivedEventHandler>(
          [this](ICoreWebView2*,
                 ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            LPWSTR raw = nullptr;
            if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw) {
              const std::wstring message(raw);
              CoTaskMemFree(raw);
              OnPageMessage(message);
            }
            return S_OK;
          })
          .Get(),
      &token);

  // The page owns its own title, which is where the caption comes from; that
  // keeps the user-facing wording in one file.
  webview_->add_DocumentTitleChanged(
      Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
          [this](ICoreWebView2* sender, IUnknown*) -> HRESULT {
            LPWSTR title = nullptr;
            if (SUCCEEDED(sender->get_DocumentTitle(&title)) && title) {
              ::SetWindowTextW(m_hWnd, title);
              CoTaskMemFree(title);
            }
            return S_OK;
          })
          .Get(),
      &token);

  FitWebView();

  const std::wstring page = LoadPage();
  if (page.empty()) {
    FailInitialization();
    return;
  }
  webview_->NavigateToString(page.c_str());
}

void SettingsPanel::FailInitialization() {
  initialization_failed_ = true;
  PostMessageW(WM_CLOSE);
}

void SettingsPanel::FitWebView() {
  if (!controller_)
    return;
  RECT bounds = {0};
  GetClientRect(&bounds);
  controller_->put_Bounds(bounds);
}

LRESULT SettingsPanel::OnSize(UINT, WPARAM, LPARAM, BOOL&) {
  FitWebView();
  return 0;
}

LRESULT SettingsPanel::OnClose(UINT, WPARAM, LPARAM, BOOL&) {
  // Closing while a key is being derived or a sync is running would leave the
  // worker posting to a window that no longer exists, and would hide whether
  // the operation succeeded. The window goes away as soon as it reports back.
  if (busy_) {
    close_pending_ = true;
    EnableWindow(FALSE);
    return 0;
  }
  DestroyWindow();
  return 0;
}

LRESULT SettingsPanel::OnDestroy(UINT, WPARAM, LPARAM, BOOL&) {
  if (controller_)
    controller_->Close();
  webview_.Reset();
  controller_.Reset();
  PostQuitMessage(0);
  return 0;
}

LRESULT SettingsPanel::OnTaskDone(UINT, WPARAM, LPARAM lParam, BOOL&) {
  std::unique_ptr<TaskResult> result(reinterpret_cast<TaskResult*>(lParam));
  busy_ = false;
  if (result->refresh)
    SendSettings();
  SendStatus(result->ok ? L"ok" : L"error", result->code,
             result->clear_password);
  if (close_pending_) {
    EnableWindow(TRUE);
    DestroyWindow();
  }
  return 0;
}

void SettingsPanel::RunAsync(const wchar_t* busy_code,
                             TaskResult failure,
                             std::function<TaskResult()> work) {
  if (busy_)
    return;
  busy_ = true;
  SendStatus(L"busy", busy_code, false);

  const HWND window = m_hWnd;
  try {
    auto result = std::make_unique<TaskResult>(failure);
    std::thread([window, result = std::move(result),
                 work = std::move(work)]() mutable noexcept {
      try {
        *result = work();
      } catch (...) {
        // `result` already holds this operation's existing failure outcome.
      }
      TaskResult* posted = result.release();
      if (!::PostMessageW(window, kTaskDone, 0,
                          reinterpret_cast<LPARAM>(posted))) {
        delete posted;
      }
    }).detach();
  } catch (...) {
    // Construction failed on the UI thread; report the same prepared outcome
    // directly because no worker exists to post kTaskDone.
    busy_ = false;
    if (failure.refresh)
      SendSettings();
    SendStatus(L"error", failure.code, failure.clear_password);
  }
}

void SettingsPanel::SendSettings() {
  if (!webview_)
    return;

  const SyncConfig config = SyncConfig::LoadForEditing();
  Fields fields;
  fields[L"backend"] = BackendName(config.backend);
  fields[L"localDir"] = config.local_dir;
  fields[L"endpoint"] = u8tow(config.endpoint);
  fields[L"bucket"] = u8tow(config.bucket);
  fields[L"prefix"] = u8tow(config.prefix);
  fields[L"davUrl"] = u8tow(config.dav_url);
  fields[L"davUsername"] = u8tow(config.dav_username);
  fields[L"workerUrl"] = u8tow(config.worker_url);

  // Stored credentials stay in this process. The page is told only that one
  // exists, which is enough to explain why its field is empty.
  fields[L"has_accessKey"] = config.access_key.empty() ? L"0" : L"1";
  fields[L"has_secretKey"] = config.secret_key.empty() ? L"0" : L"1";
  fields[L"has_davPassword"] = config.dav_password.empty() ? L"0" : L"1";
  fields[L"has_workerToken"] = config.worker_token.empty() ? L"0" : L"1";
  fields[L"hasDataKey"] = LoadCachedDataKey() ? L"1" : L"0";

  fields[L"intervalMinutes"] = std::to_wstring(SyncIntervalMinutes());
  fields[L"syncOnStartup"] = SyncOnStartup() ? L"1" : L"0";
  fields[L"installationId"] = InstallationId();
  fields[L"syncDir"] = SyncDirectory().wstring();

  webview_->PostWebMessageAsString(Encode(L"settings", fields).c_str());
}

void SettingsPanel::SendStatus(const wchar_t* level,
                               const wchar_t* code,
                               bool clear_password) {
  if (!webview_)
    return;
  Fields fields;
  fields[L"level"] = level;
  fields[L"code"] = code;
  if (clear_password)
    fields[L"clear"] = L"1";
  webview_->PostWebMessageAsString(Encode(L"status", fields).c_str());
}

void SettingsPanel::OnPageMessage(const std::wstring& message) {
  Fields fields;
  const std::wstring verb = Decode(message, &fields);

  if (verb == L"load") {
    SendSettings();
    return;
  }

  if (verb == L"save") {
    const SyncConfig config = ConfigFromFields(fields);
    const bool saved =
        config.Save() &&
        SaveSyncSchedule(IntervalFromFields(fields),
                         Field(fields, L"syncOnStartup") == L"1");
    SendSettings();
    SendStatus(saved ? L"ok" : L"error", saved ? L"saved" : L"save_failed",
               false);
    return;
  }

  if (verb == L"test") {
    SyncConfig config = ConfigFromFields(fields);
    MergeStoredSecrets(&config);
    TaskResult failure;
    failure.code = L"test_unreachable";
    RunAsync(L"busy_test", failure, [config]() {
      TaskResult result;
      switch (TestBackend(config)) {
        case BackendTestResult::kOk:
          result.ok = true;
          result.code = L"test_ok";
          break;
        case BackendTestResult::kNotConfigured:
          result.code = L"test_incomplete";
          break;
        case BackendTestResult::kUnreachable:
        default:
          result.code = L"test_unreachable";
          break;
      }
      return result;
    });
    return;
  }

  if (verb == L"setkey") {
    const SyncConfig config = ConfigFromFields(fields);
    const unsigned interval = IntervalFromFields(fields);
    const bool on_startup = Field(fields, L"syncOnStartup") == L"1";
    std::string password = wtou8(Field(fields, L"password"));
    // The decoded message holds the password too; it is wiped here so only the
    // copy handed to the worker survives, and that one is wiped when it is
    // done.
    if (auto entry = fields.find(L"password"); entry != fields.end()) {
      SecureZeroMemory(entry->second.data(),
                       entry->second.size() * sizeof(wchar_t));
    }

    // Saving first is not a convenience: the data key belongs to whichever
    // storage is configured, so deriving one against settings that have not
    // been written would attach it to the previous backend.
    TaskResult failure;
    failure.code = L"key_unreachable";
    failure.refresh = true;
    failure.clear_password = true;
    RunAsync(L"busy_key", failure,
             [config, interval, on_startup,
              password = std::move(password)]() mutable {
               struct PasswordWiper {
                 std::string* value;
                 ~PasswordWiper() {
                   SecureZeroMemory(value->data(), value->size());
                 }
               } wipe{&password};
               TaskResult result;
               result.refresh = true;
               result.clear_password = true;
               if (!config.Save() || !SaveSyncSchedule(interval, on_startup)) {
                 result.code = L"save_failed";
               } else {
                 const KeySetupResult setup = SetUpDataKey(password);
                 result.ok = setup == KeySetupResult::kOk;
                 result.code = KeySetupCode(setup);
               }
               return result;
             });
    return;
  }

  if (verb == L"sync") {
    TaskResult failure;
    failure.code = L"sync_failed";
    failure.refresh = true;
    RunAsync(L"busy_sync", failure, [this]() {
      TaskResult result;
      result.refresh = true;
      const int outcome = run_sync_ ? run_sync_() : 1;
      result.ok = outcome == 0;
      // A cloud round that failed while the local merge succeeded is worth
      // saying out loud: the dictionaries are fine, the other machines are not
      // going to see this round.
      if (outcome == 0) {
        result.code = L"sync_ok";
      } else if (outcome == kCloudSyncFailed) {
        switch (LastCloudSyncError()) {
          case CloudSyncError::kUnsupportedSnapshotFormat:
            result.code = L"sync_unsupported_snapshot_format";
            break;
          case CloudSyncError::kLocalRecoveryRequired:
            result.code = L"sync_local_recovery_required";
            break;
          default:
            result.code = L"sync_cloud_failed";
            break;
        }
      } else {
        result.code = L"sync_failed";
      }
      return result;
    });
    return;
  }
}

}  // namespace

int ShowSettingsPanel(const std::function<int()>& run_sync) {
  LPWSTR version = nullptr;
  if (FAILED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) ||
      !version) {
    MSG_BY_IDS(IDS_STR_NO_WEBVIEW2, IDS_STR_WEASEL, MB_OK | MB_ICONERROR);
    return 1;
  }
  CoTaskMemFree(version);

  SettingsPanel panel(run_sync);
  if (!panel.Open())
    return 1;

  MSG message = {0};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  if (panel.InitializationFailed()) {
    MSG_BY_IDS(IDS_STR_SETTINGS_PANEL_FAILED, IDS_STR_WEASEL,
               MB_OK | MB_ICONERROR);
    return 1;
  }
  return 0;
}

}  // namespace hare
