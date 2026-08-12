; hare installation script
!include FileFunc.nsh
!include LogicLib.nsh
!include MUI2.nsh
!include x64.nsh
!include winVer.nsh

Unicode true

;--------------------------------
; General

!ifndef WEASEL_VERSION
!define WEASEL_VERSION 0.1.0
!endif

!ifndef WEASEL_BUILD
!define WEASEL_BUILD 0
!endif

!define WEASEL_ROOT $INSTDIR\hare-${WEASEL_VERSION}
!define REG_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Hare"

; The name of the installer
Name "紫毫 ${WEASEL_VERSION}"

; The file to write
OutFile "archives\hare-${PRODUCT_VERSION}-installer.exe"

VIProductVersion "${WEASEL_VERSION}.${WEASEL_BUILD}"
VIAddVersionKey /LANG=2052 "ProductName" "紫毫"
VIAddVersionKey /LANG=2052 "Comments" "Powered by RIME | 中州韻輸入法引擎"
VIAddVersionKey /LANG=2052 "CompanyName" "式恕堂"
VIAddVersionKey /LANG=2052 "LegalCopyright" "Copyleft RIME Developers"
VIAddVersionKey /LANG=2052 "FileDescription" "紫毫輸入法"
VIAddVersionKey /LANG=2052 "FileVersion" "${WEASEL_VERSION}"

!define MUI_ICON ..\resource\weasel.ico
SetCompressor /SOLID lzma


; Request application privileges for Windows Vista
RequestExecutionLevel admin

;--------------------------------

; Pages

!insertmacro MUI_PAGE_LICENSE "LICENSE.txt"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

;--------------------------------

; Languages

!insertmacro MUI_LANGUAGE "TradChinese"
LangString DISPLAYNAME ${LANG_TRADCHINESE} "紫毫輸入法"
LangString LNKFORMANUAL ${LANG_TRADCHINESE} "【紫毫】說明書"
LangString LNKFORSETTING ${LANG_TRADCHINESE} "【紫毫】輸入法設定"
LangString LNKFORDICT ${LANG_TRADCHINESE} "【紫毫】用戶詞典管理"
LangString LNKFORSYNC ${LANG_TRADCHINESE} "【紫毫】用戶資料同步"
LangString LNKFORDEPLOY ${LANG_TRADCHINESE} "【紫毫】重新部署"
LangString LNKFORSERVER ${LANG_TRADCHINESE} "紫毫算法服務"
LangString LNKFORUSERFOLDER ${LANG_TRADCHINESE} "【紫毫】用戶文件夾"
LangString LNKFORAPPFOLDER ${LANG_TRADCHINESE} "【紫毫】程序文件夾"
LangString LNKFORUPDATER ${LANG_TRADCHINESE} "【紫毫】檢查新版本"
LangString LNKFORSETUP ${LANG_TRADCHINESE} "【紫毫】安裝選項"
LangString LNKFORUNINSTALL ${LANG_TRADCHINESE} "卸載紫毫"
LangString CONFIRMATION ${LANG_TRADCHINESE} "安裝前，請先卸載舊版本的紫毫。$\n$\n按下「確定」移除舊版本，按下「取消」放棄本次安裝。"
LangString SYSTEMVERSIONNOTOK ${LANG_TRADCHINESE} "您的系统不被支持，最低系統要求:Windows 8.1!"
LangString AUTOCHKUPDATE ${LANG_TRADCHINESE} "自動檢查版本更新？"

!insertmacro MUI_LANGUAGE "SimpChinese"
LangString DISPLAYNAME ${LANG_SIMPCHINESE} "紫毫输入法"
LangString LNKFORMANUAL ${LANG_SIMPCHINESE} "【紫毫】说明书"
LangString LNKFORSETTING ${LANG_SIMPCHINESE} "【紫毫】输入法设定"
LangString LNKFORDICT ${LANG_SIMPCHINESE} "【紫毫】用户词典管理"
LangString LNKFORSYNC ${LANG_SIMPCHINESE} "【紫毫】用户资料同步"
LangString LNKFORDEPLOY ${LANG_SIMPCHINESE} "【紫毫】重新部署"
LangString LNKFORSERVER ${LANG_SIMPCHINESE} "紫毫算法服务"
LangString LNKFORUSERFOLDER ${LANG_SIMPCHINESE} "【紫毫】用户文件夹"
LangString LNKFORAPPFOLDER ${LANG_SIMPCHINESE} "【紫毫】程序文件夹"
LangString LNKFORUPDATER ${LANG_SIMPCHINESE} "【紫毫】检查新版本"
LangString LNKFORSETUP ${LANG_SIMPCHINESE} "【紫毫】安装选项"
LangString LNKFORUNINSTALL ${LANG_SIMPCHINESE} "卸载紫毫"
LangString CONFIRMATION ${LANG_SIMPCHINESE} '安装前，请先卸载旧版本的紫毫。$\n$\n点击 "确定" 移除旧版本，或点击 "取消" 放弃本次安装。'
LangString SYSTEMVERSIONNOTOK ${LANG_SIMPCHINESE} "您的系統不被支持，最低系统要求:Windows 8.1!"
LangString AUTOCHKUPDATE ${LANG_SIMPCHINESE} "自动检查版本更新？"

!insertmacro MUI_LANGUAGE "English"
LangString DISPLAYNAME ${LANG_ENGLISH} "Hare"
LangString LNKFORMANUAL ${LANG_ENGLISH} "Hare Manual"
LangString LNKFORSETTING ${LANG_ENGLISH} "Hare Settings"
LangString LNKFORDICT ${LANG_ENGLISH} "Hare Dictionary Manager"
LangString LNKFORSYNC ${LANG_ENGLISH} "Hare Sync User Profile"
LangString LNKFORDEPLOY ${LANG_ENGLISH} "Hare Deploy"
LangString LNKFORSERVER ${LANG_ENGLISH} "Hare Server"
LangString LNKFORUSERFOLDER ${LANG_ENGLISH} "Hare User Folder"
LangString LNKFORAPPFOLDER ${LANG_ENGLISH} "Hare App Folder"
LangString LNKFORUPDATER ${LANG_ENGLISH} "Hare Check for Updates"
LangString LNKFORSETUP ${LANG_ENGLISH} "Hare Installation Preference"
LangString LNKFORUNINSTALL ${LANG_ENGLISH} "Uninstall Hare"
LangString CONFIRMATION ${LANG_ENGLISH} "Before installation, please uninstall the old version of Hare.$\n$\nPress 'OK' to remove the old version, or 'Cancel' to abort installation."
LangString SYSTEMVERSIONNOTOK ${LANG_ENGLISH} "Your system not supported, minimium system required: Windows 8.1!"
LangString AUTOCHKUPDATE ${LANG_ENGLISH} "Automatically check for updates?"

;--------------------------------

Function .onInit
  ; if not version >= 8.1, quit and MessageBox(if not silent)
  ${IfNot} ${AtLeastWin8.1}
    IfSilent toquit
    MessageBox MB_OK '$(SYSTEMVERSIONNOTOK)'
toquit:
    Quit
  ${EndIf}

  ReadRegStr $R0 HKLM "Software\Rime\Hare" "InstallDir"
  StrCmp $R0 "" 0 skip
  ; The default installation directory
  ; install x64 build for NativeARM64_WINDOWS11 and NativeAMD64_WINDOWS11
  ${If} ${AtLeastWin11} ; Windows 11 and above
    ${If} ${IsNativeARM64}
      StrCpy $INSTDIR "$PROGRAMFILES64\Rime"
    ${ElseIf} ${IsNativeAMD64}
      StrCpy $INSTDIR "$PROGRAMFILES64\Rime"
    ${Else}
      StrCpy $INSTDIR "$PROGRAMFILES\Rime"
    ${Endif}
  ; install x64 build for NativeAMD64_BELLOW_WINDOWS11
  ${Else} ; Windows 10 or bellow
    ${If} ${IsNativeAMD64}
      StrCpy $INSTDIR "$PROGRAMFILES64\Rime"
    ${Else}
      StrCpy $INSTDIR "$PROGRAMFILES\Rime"
    ${Endif}
  ${Endif}
skip:
  ReadRegStr $R0 HKLM \
  "Software\Microsoft\Windows\CurrentVersion\Uninstall\Hare" \
  "UninstallString"
  StrCmp $R0 "" done

  StrCpy $0 "Upgrade"
  IfSilent uninst 0
  MessageBox MB_OKCANCEL|MB_ICONINFORMATION "$(CONFIRMATION)" IDOK uninst
  Abort

uninst:
  ; Backup data directory from previous installation, user files may exist
  ReadRegStr $R1 HKLM SOFTWARE\Rime\Hare "WeaselRoot"
  StrCmp $R1 "" call_uninstaller
  IfFileExists $R1\data\*.* 0 call_uninstaller
  CreateDirectory $TEMP\hare-backup
  CopyFiles $R1\data\*.* $TEMP\hare-backup

call_uninstaller:
  ExecWait '"$R1\HareServer.exe" /quit'
  ExecWait '"$R1\HareSetup.exe" /u'
  ; Remove only this product's branch. NSIS deletes recursively, so removing
  ; the shared SOFTWARE\Rime parent would take the official Weasel keys with
  ; it; /ifempty leaves the parent alone while any other Rime distribution
  ; still lives there.
  DeleteRegKey HKLM SOFTWARE\Rime\Hare
  DeleteRegKey /ifempty HKLM SOFTWARE\Rime
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Hare"
  ; don't redirect on 64 bit system for auto run setting
  ${If} ${IsNativeARM64}
    SetRegView 64
  ${ElseIf} ${IsNativeAMD64}
    SetRegView 64
  ${Endif}
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "HareServer"
  ; recover back to 32bit view
  SetRegView 32
  ; Remove files and uninstaller
  Delete  "$R1\data\opencc\*.*"
  Delete  "$R1\data\preview\*.*"
  Delete  "$R1\data\*.*"
  Delete  "$R1\*.*"
  RMDir   "$R1\data\opencc"
  RMDir   "$R1\data\preview"
  RMDir   "$R1\data"
  RMDir   "$R1"
  SetShellVarContext all
  Delete  "$SMPROGRAMS\$(DISPLAYNAME)\*.*"
  RMDir  "$SMPROGRAMS\$(DISPLAYNAME)"
  ; Prompt reboot
  SetRebootFlag true
  Sleep 800

done:
FunctionEnd

; Registry key to check for directory (so if you install again, it will
; overwrite the old one automatically)
InstallDirRegKey HKLM "Software\Rime\Hare" "InstallDir"

; The stuff to install
Section "Hare"

  SectionIn RO

  ; Write the new installation path into the registry
  ; redirect on 64 bit system
  ; HKLM SOFTWARE\WOW6432Node\Rime\Hare "InstallDir" "$INSTDIR"
  WriteRegStr HKLM SOFTWARE\Rime\Hare "InstallDir" "$INSTDIR"

  ; Reset INSTDIR for the new version
  StrCpy $INSTDIR "${WEASEL_ROOT}"

  IfFileExists "$INSTDIR\HareServer.exe" 0 +2
  ExecWait '"$INSTDIR\HareServer.exe" /quit'

  SetOverwrite try
  ; Set output path to the installation directory.
  SetOutPath $INSTDIR

  IfFileExists $TEMP\hare-backup\*.* 0 program_files
  CreateDirectory $INSTDIR\data
  CopyFiles $TEMP\hare-backup\*.* $INSTDIR\data
  RMDir /r $TEMP\hare-backup

program_files:
  File "LICENSE.txt"
  File "README.txt"
  File "7-zip-license.txt"
  File "7z.dll"
  File "7z.exe"
  File "COPYING-curl.txt"
  File "curl.exe"
  File "curl-ca-bundle.crt"
  File "rime-install.bat"
  File "rime-install-config.bat"
  File "start_service.bat"
  File "stop_service.bat"
  File "hare.dll"
  ${If} ${RunningX64}
    File "harex64.dll"
  ${EndIf}
  ${If} ${IsNativeARM64}
    File /nonfatal "hareARM.dll"
    File /nonfatal "hareARM64.dll"
    File /nonfatal "hareARM64X.dll"
  ${EndIf}
  ; install x64 build for NativeARM64_WINDOWS11 and NativeAMD64_WINDOWS11
  ${If} ${AtLeastWin11} ; Windows 11 and above
    ${If} ${IsNativeARM64}
      File "HareDeployer.exe"
      File "HareServer.exe"
      File "rime.dll"
      File "WinSparkle.dll"
    ${ElseIf} ${IsNativeAMD64}
      File "HareDeployer.exe"
      File "HareServer.exe"
      File "rime.dll"
      File "WinSparkle.dll"
    ${Else}
      File "Win32\HareDeployer.exe"
      File "Win32\HareServer.exe"
      File "Win32\rime.dll"
      File "Win32\WinSparkle.dll"
    ${Endif}
  ; install x64 build for NativeAMD64_BELLOW_WINDOWS11
  ${Else} ; Windows 10 or bellow
    ${If} ${IsNativeAMD64}
      File "HareDeployer.exe"
      File "HareServer.exe"
      File "rime.dll"
      File "WinSparkle.dll"
    ${Else}
      File "Win32\HareDeployer.exe"
      File "Win32\HareServer.exe"
      File "Win32\rime.dll"
      File "Win32\WinSparkle.dll"
    ${Endif}
  ${Endif}

  File "HareSetup.exe"
  ; shared data files
  SetOutPath $INSTDIR\data
  File "data\*.yaml"
  File /nonfatal "data\*.txt"
  File /nonfatal "data\*.gram"
  ; opencc data files
  SetOutPath $INSTDIR\data\opencc
  File "data\opencc\*.json"
  File "data\opencc\*.ocd*"
  ; images
  SetOutPath $INSTDIR\data\preview
  File "data\preview\*.png"

  SetOutPath $INSTDIR

  ; test /T flag for zh_TW locale
  StrCpy $R2 "/i"
  ${GetParameters} $R0
  ClearErrors
  ${GetOptions} $R0 "/S" $R1
  IfErrors +2 0
  StrCpy $R2 "/s"
  ${GetOptions} $R0 "/T" $R1
  IfErrors +2 0
  StrCpy $R2 "/t"

  ExecWait '"$INSTDIR\HareSetup.exe" $R2'

  ; Write the uninstall keys for Windows
  WriteRegStr HKLM "${REG_UNINST_KEY}" "DisplayName" "$(DISPLAYNAME)"
  WriteRegStr HKLM "${REG_UNINST_KEY}" "DisplayIcon" '"$INSTDIR\HareServer.exe"'
  WriteRegStr HKLM "${REG_UNINST_KEY}" "DisplayVersion" "${WEASEL_VERSION}.${WEASEL_BUILD}"
  WriteRegStr HKLM "${REG_UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${REG_UNINST_KEY}" "Publisher" "式恕堂"
  WriteRegStr HKLM "${REG_UNINST_KEY}" "URLInfoAbout" "https://rime.im/"
  WriteRegStr HKLM "${REG_UNINST_KEY}" "HelpLink" "https://rime.im/docs/"
  WriteRegDWORD HKLM "${REG_UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${REG_UNINST_KEY}" "NoRepair" 1
  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; run as user...
  IfSilent deploy_silently
  ExecWait "$INSTDIR\HareDeployer.exe /install"
  GoTo deploy_done

  deploy_silently:
  ExecWait "$INSTDIR\HareDeployer.exe /deploy"
  deploy_done:

  ; don't redirect on 64 bit system for auto run setting
  ${If} ${IsNativeARM64}
    SetRegView 64
  ${ElseIf} ${IsNativeAMD64}
    SetRegView 64
  ${Endif}
  ; Write autorun key
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "HareServer" "$INSTDIR\HareServer.exe"
  ; Start HareServer
  Exec "$INSTDIR\HareServer.exe"

  ; option CheckForUpdates
  IfSilent DisableAutoCheckUpdate
  MessageBox MB_YESNO|MB_ICONINFORMATION "$(AUTOCHKUPDATE)" IDYES EnableAutoCheckUpdate
  DisableAutoCheckUpdate:
  WriteRegStr HKCU "Software\Rime\Hare\Updates" "CheckForUpdates" "0"
  GoTo end
  EnableAutoCheckUpdate:
  WriteRegStr HKCU "Software\Rime\Hare\Updates" "CheckForUpdates" "1"
  end:

  ; Prompt reboot
  StrCmp $0 "Upgrade" 0 +2
  SetRebootFlag true

SectionEnd

; Optional section (can be disabled by the user)
Section "Start Menu Shortcuts"
  SetShellVarContext all
  CreateDirectory "$SMPROGRAMS\$(DISPLAYNAME)"
  CreateShortCut "$SMPROGRAMS\$(DISPLAYNAME)\$(LNKFORMANUAL).lnk" "$INSTDIR\README.txt"
  CreateShortCut "$SMPROGRAMS\$(DISPLAYNAME)\$(LNKFORSETTING).lnk" "$INSTDIR\HareDeployer.exe" "" "$SYSDIR\shell32.dll" 21
  CreateShortCut "$SMPROGRAMS\$(DISPLAYNAME)\$(LNKFORDICT).lnk" "$INSTDIR\HareDeployer.exe" "/dict" "$SYSDIR\shell32.dll" 6
  CreateShortCut "$SMPROGRAMS\$(DISPLAYNAME)\$(LNKFORSYNC).lnk" "$INSTDIR\HareDeployer.exe" "/sync" "$SYSDIR\shell32.dll" 26
  CreateShortCut "$SMPROGRAMS\$(DISPLAYNAME)\$(LNKFORDEPLOY).lnk" "$INSTDIR\HareDeployer.exe" "/deploy" "$SYSDIR\shell32.dll" 144
  CreateShortCut "$SMPROGRAMS\$(DISPLAYNAME)\$(LNKFORSERVER).lnk" "$INSTDIR\HareServer.exe" "" "$INSTDIR\HareServer.exe" 0
  CreateShortCut "$SMPROGRAMS\$(DISPLAYNAME)\$(LNKFORUSERFOLDER).lnk" "$INSTDIR\HareServer.exe" "/userdir" "$SYSDIR\shell32.dll" 126
  CreateShortCut "$SMPROGRAMS\$(DISPLAYNAME)\$(LNKFORAPPFOLDER).lnk" "$INSTDIR\HareServer.exe" "/weaseldir" "$SYSDIR\shell32.dll" 19
  CreateShortCut "$SMPROGRAMS\$(DISPLAYNAME)\$(LNKFORUPDATER).lnk" "$INSTDIR\HareServer.exe" "/update" "$SYSDIR\shell32.dll" 13
  CreateShortCut "$SMPROGRAMS\$(DISPLAYNAME)\$(LNKFORSETUP).lnk" "$INSTDIR\HareSetup.exe" "" "$SYSDIR\shell32.dll" 162
  CreateShortCut "$SMPROGRAMS\$(DISPLAYNAME)\$(LNKFORUNINSTALL).lnk" "$INSTDIR\uninstall.exe" "" "$INSTDIR\uninstall.exe" 0

SectionEnd

;--------------------------------

; Uninstaller

Section "Uninstall"

  ExecWait '"$INSTDIR\HareServer.exe" /quit'

  ExecWait '"$INSTDIR\HareSetup.exe" /u'

  ; Remove only this product's branch. NSIS deletes recursively, so removing
  ; the shared SOFTWARE\Rime parent would take the official Weasel keys with
  ; it; /ifempty leaves the parent alone while any other Rime distribution
  ; still lives there.
  DeleteRegKey HKLM SOFTWARE\Rime\Hare
  DeleteRegKey /ifempty HKLM SOFTWARE\Rime
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Hare"
  ; don't redirect on 64 bit system for auto run setting
  ${If} ${IsNativeARM64}
    SetRegView 64
  ${ElseIf} ${IsNativeAMD64}
    SetRegView 64
  ${Endif}
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "HareServer"

  ; Remove files and uninstaller
  SetOutPath $TEMP
  Delete  "$INSTDIR\data\opencc\*.*"
  Delete  "$INSTDIR\data\preview\*.*"
  Delete  "$INSTDIR\data\*.*"
  Delete  "$INSTDIR\*.*"
  RMDir  "$INSTDIR\data\opencc"
  RMDir  "$INSTDIR\data\preview"
  RMDir  "$INSTDIR\data"
  RMDir  "$INSTDIR"
  SetShellVarContext all
  Delete  "$SMPROGRAMS\$(DISPLAYNAME)\*.*"
  RMDir  "$SMPROGRAMS\$(DISPLAYNAME)"

  ; Prompt reboot
  SetRebootFlag true

SectionEnd
