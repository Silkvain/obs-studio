; Siegfried - Windows installer (a fork of OBS Studio)
;
; Built by .github/workflows/multistream-windows.yaml from the packaged
; build. Installs alongside (not over) an official OBS Studio install.
;
; Defines passed by the workflow:
;   APPVERSION  version string, e.g. 33.0.2-3-g0ed30b4
;   SRCDIR      folder containing bin\, core\, data\
;   VCREDIST    path to vc_redist.x64.exe (optional)
;   REPOROOT    repository root (for icon and license)
;   OUTFILE     output installer path

Unicode true
ManifestDPIAware true
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"

!ifndef APPVERSION
  !define APPVERSION "0.0.0"
!endif
!ifndef OUTFILE
  !define OUTFILE "Siegfried-Setup.exe"
!endif

!define APPNAME "Siegfried"
!define SHORTNAME "Siegfried"
!define PUBLISHER "Silkvain"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Siegfried"
!define EXE "bin\64bit\obs64.exe"

Name "${APPNAME}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\${APPNAME}"
InstallDirRegKey HKLM "Software\${APPNAME}" "InstallDir"
RequestExecutionLevel admin
BrandingText "${APPNAME} ${APPVERSION}"

VIProductVersion "0.0.0.0"
VIAddVersionKey "ProductName" "${APPNAME}"
VIAddVersionKey "ProductVersion" "${APPVERSION}"
VIAddVersionKey "FileDescription" "${APPNAME} installer"
VIAddVersionKey "FileVersion" "${APPVERSION}"
VIAddVersionKey "CompanyName" "${PUBLISHER}"
VIAddVersionKey "LegalCopyright" "GPL-2.0-or-later"

!define MUI_ICON "${REPOROOT}\frontend\cmake\windows\obs-studio.ico"
!define MUI_UNICON "${REPOROOT}\frontend\cmake\windows\obs-studio.ico"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\${EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Launch ${SHORTNAME}"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${REPOROOT}\frontend\data\license\gplv2.txt"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "${APPNAME} needs 64-bit Windows."
    Abort
  ${EndIf}
  SetRegView 64
FunctionEnd

Function un.onInit
  SetRegView 64
FunctionEnd

Section "Install"
  SetShellVarContext all

  ; Replace any previous version's program files (settings live in
  ; %APPDATA%\obs-studio and are left alone)
  RMDir /r "$INSTDIR\bin"
  RMDir /r "$INSTDIR\data"
  RMDir /r "$INSTDIR\core"

  SetOutPath "$INSTDIR"
  File /r "${SRCDIR}\*.*"

!ifdef VCREDIST
  ; Microsoft Visual C++ runtime that OBS needs
  DetailPrint "Installing Microsoft Visual C++ runtime..."
  SetOutPath "$PLUGINSDIR"
  File "/oname=vc_redist.x64.exe" "${VCREDIST}"
  ExecWait '"$PLUGINSDIR\vc_redist.x64.exe" /install /quiet /norestart'
!endif

  ; Shortcuts start in bin\64bit so OBS finds its files
  SetOutPath "$INSTDIR\bin\64bit"
  CreateShortcut "$DESKTOP\${SHORTNAME}.lnk" "$INSTDIR\${EXE}"
  CreateDirectory "$SMPROGRAMS\${SHORTNAME}"
  CreateShortcut "$SMPROGRAMS\${SHORTNAME}\${SHORTNAME}.lnk" "$INSTDIR\${EXE}"
  CreateShortcut "$SMPROGRAMS\${SHORTNAME}\Uninstall ${SHORTNAME}.lnk" "$INSTDIR\uninstall.exe"

  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "Software\${APPNAME}" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayName" "${APPNAME}"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayVersion" "${APPVERSION}"
  WriteRegStr HKLM "${UNINSTKEY}" "Publisher" "${PUBLISHER}"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayIcon" "$INSTDIR\${EXE}"
  WriteRegStr HKLM "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTKEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  SetShellVarContext all
  Delete "$DESKTOP\${SHORTNAME}.lnk"
  RMDir /r "$SMPROGRAMS\${SHORTNAME}"

  ; Only remove what the installer put there, never the whole folder blindly
  RMDir /r "$INSTDIR\bin"
  RMDir /r "$INSTDIR\data"
  RMDir /r "$INSTDIR\core"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"

  DeleteRegKey HKLM "${UNINSTKEY}"
  DeleteRegKey HKLM "Software\${APPNAME}"
SectionEnd
