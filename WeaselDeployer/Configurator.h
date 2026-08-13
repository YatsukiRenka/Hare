#pragma once

class UIStyleSettings;

// SyncUserData() tells the two halves of a sync apart. Rime's own merge failing
// is a different problem from the cloud round failing, and only the second one
// leaves the local dictionaries perfectly usable.
constexpr int kCloudSyncFailed = 2;

class Configurator {
 public:
  explicit Configurator();

  void Initialize();
  int Run(bool installing);
  int UpdateWorkspace(bool report_errors = false);
  int DictManagement();
  int SyncUserData();
};
