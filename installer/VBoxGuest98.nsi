; VBoxGuest98 installer for Windows 98/ME (NSIS 2.46).
;
; Build from this folder with "makensis VBoxGuest98.nsi" after a free build
; (build.cmd does it).  Define OBJDIR=objchk to package a checked build.
;
; Windows 98/ME can't install a driver programmatically from 32-bit code, so the
; installer preinstalls it, as the Oney WDM book suggests for these systems:
;  - copies VBoxGst.sys, VBoxTray.exe and VBoxMou.vxd to their final places;
;  - copies an INF without CopyFiles lines to INF\OTHER under the name the
;    Add Hardware wizard would use (<Provider><file name>);
;  - sets ConfigFlags to CONFIGFLAG_REINSTALL on every VirtualBox Guest Device
;    under HKLM\Enum\PCI.
; At the next boot Windows (re)installs the device from that INF without asking
; anything; the INF's AddReg also creates the Run entry for VBoxTray.
;
; This file is in the Windows-1252 code page (NSIS 2 has no UTF-8 support).

!include LogicLib.nsh
!include WinVer.nsh
!include WinMessages.nsh
!include FileFunc.nsh
!include MUI2.nsh

!ifndef OBJDIR
  !define OBJDIR "objfre"
!endif

; VBoxGuest98's own version from the shared header, e.g. 0.1.0.
!searchparse /file "..\inc\VBoxGuest98.h" "#define VBOXGUEST98_VERSION_STRING" VBG_PAD '"' VBG_VERSION '"'

; The installer's INF is the repository INF without its CopyFiles lines: with
; them, Windows asks for the installation disk although the files are in place.
!system 'if not exist obj mkdir obj' = 0
!system 'findstr /v /b /c:"CopyFiles=" "..\VBoxGst\VBoxGst.inf" > "obj\VBoxGst.inf"' = 0
!system 'findstr /b /c:"CopyFiles=" "obj\VBoxGst.inf" > nul' = 1

!define PRODUCT         "VBoxGuest98"
!define UNINST_KEY      "Software\Microsoft\Windows\CurrentVersion\Uninstall\VBoxGuest98"
!define RUN_KEY         "Software\Microsoft\Windows\CurrentVersion\Run"
!define CLASS_KEY       "System\CurrentControlSet\Services\Class\System"
!define PCI_KEY         "Enum\PCI"
!define DEVICE_PREFIX   "VEN_80EE&DEV_CAFE"
!define VENDOR_PREFIX   "VEN_80EE&"
!define INF_NAME        "VBoxGuest98VBoxGst.inf"
!define UNINSTALLER     "VBoxGuest98Uninst.exe"
!define TRAY_WND_CLASS  "VBoxTrayToolWndClass"
!define TRAY_MUTEX      "VBoxTray98SingleInstance"

Name "${PRODUCT} ${VBG_VERSION}"
!if "${OBJDIR}" == "objchk"
  OutFile "obj\${PRODUCT}-${VBG_VERSION}-checked.exe"
!else
  OutFile "obj\${PRODUCT}-${VBG_VERSION}.exe"
!endif
SetCompressor /SOLID lzma
InstallDir "$SYSDIR"
ShowInstDetails show
ShowUninstDetails show
BrandingText "${PRODUCT} ${VBG_VERSION}"

VIProductVersion "${VBG_VERSION}.0"
VIAddVersionKey /LANG=1033 "ProductName" "${PRODUCT}"
VIAddVersionKey /LANG=1033 "FileDescription" "VirtualBox Guest Additions for Windows 98/ME setup"
VIAddVersionKey /LANG=1033 "FileVersion" "${VBG_VERSION}"
VIAddVersionKey /LANG=1033 "ProductVersion" "${VBG_VERSION}"
VIAddVersionKey /LANG=1033 "LegalCopyright" "GPLv3"


;--------------------------------------------------------------------------------------------------
; Pages and languages
;--------------------------------------------------------------------------------------------------

!define MUI_ICON "..\VBoxTray\VBoxTray.ico"
!define MUI_UNICON "..\VBoxTray\VBoxTray.ico"
!define MUI_ABORTWARNING

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

; The first language is the fallback; NSIS picks the one matching Windows.
!insertmacro MUI_LANGUAGE "PortugueseBR"
!insertmacro MUI_LANGUAGE "English"

LangString MsgNeedWin9x ${LANG_PORTUGUESEBR} "O ${PRODUCT} funciona somente no Windows 98 ou no Windows ME."
LangString MsgNeedWin9x ${LANG_ENGLISH}      "${PRODUCT} works only on Windows 98 or Windows ME."
LangString MsgNoDevice  ${LANG_PORTUGUESEBR} "Nenhum dispositivo do VirtualBox foi encontrado.$\r$\nExecute este programa dentro de uma máquina virtual do VirtualBox."
LangString MsgNoDevice  ${LANG_ENGLISH}      "No VirtualBox device was found.$\r$\nRun this program inside a VirtualBox virtual machine."
LangString TxtClosing   ${LANG_PORTUGUESEBR} "Fechando o VBoxTray..."
LangString TxtClosing   ${LANG_ENGLISH}      "Closing VBoxTray..."
LangString TxtStillRun  ${LANG_PORTUGUESEBR} "O VBoxTray ainda está em execução; os arquivos serão trocados ao reiniciar."
LangString TxtStillRun  ${LANG_ENGLISH}      "VBoxTray is still running; its files will be replaced at restart."
LangString TxtDevices   ${LANG_PORTUGUESEBR} "Driver preparado para instalação na próxima inicialização ($R9 dispositivo(s))."
LangString TxtDevices   ${LANG_ENGLISH}      "Driver prepared for installation at the next startup ($R9 device(s))."
LangString TxtNewDevice ${LANG_PORTUGUESEBR} "Driver preparado para instalação quando o Windows detectar o dispositivo na próxima inicialização."
LangString TxtNewDevice ${LANG_ENGLISH}      "Driver prepared for installation when Windows detects the device at the next startup."
LangString TxtRemoved   ${LANG_PORTUGUESEBR} "Registro do driver removido."
LangString TxtRemoved   ${LANG_ENGLISH}      "Driver registration removed."


;--------------------------------------------------------------------------------------------------
; Helpers
;--------------------------------------------------------------------------------------------------

; Installs a file; when the old one is in use, Windows replaces it at the next
; startup (WININIT.INI on Windows 9x).  TMPNAME is an 8.3 name for the copy.
!macro INSTALL_FILE SRC DESTDIR NAME TMPNAME
  SetOutPath "${DESTDIR}"
  ClearErrors
  SetOverwrite try
  File "/oname=${NAME}" "${SRC}"
  SetOverwrite on
  ${If} ${Errors}
    File "/oname=${TMPNAME}" "${SRC}"
    Rename /REBOOTOK "${DESTDIR}\${TMPNAME}" "${DESTDIR}\${NAME}"
  ${EndIf}
!macroend

; Asks VBoxTray to exit (WM_CLOSE) and waits for its process to go away: the
; single instance mutex lives until the process exits.
!macro CLOSE_VBOXTRAY_FUNC UN
Function ${UN}CloseVBoxTray
  FindWindow $0 "${TRAY_WND_CLASS}"
  ${If} $0 <> 0
    DetailPrint "$(TxtClosing)"
    SendMessage $0 ${WM_CLOSE} 0 0 /TIMEOUT=5000
  ${EndIf}

  ; SYNCHRONIZE access, which Windows 9x ignores.
  StrCpy $1 0
  ${Do}
    System::Call 'kernel32::OpenMutexA(i 0x100000, i 0, t "${TRAY_MUTEX}") i .r0'
    ${If} $0 = 0
      ${ExitDo}
    ${EndIf}
    System::Call 'kernel32::CloseHandle(i r0)'
    IntOp $1 $1 + 1
    ${If} $1 > 40
      DetailPrint "$(TxtStillRun)"
      ${ExitDo}
    ${EndIf}
    Sleep 250
  ${Loop}
FunctionEnd
!macroend
!insertmacro CLOSE_VBOXTRAY_FUNC ""
!insertmacro CLOSE_VBOXTRAY_FUNC "un."

; Counts the VirtualBox Guest Device instances under HKLM\Enum\PCI into $R9,
; marking them for reinstallation (ConfigFlags = CONFIGFLAG_REINSTALL) when $R8
; is 1, and all VirtualBox PCI devices into $R7.  The Guest Device has no entry
; when the Add New Hardware wizard was cancelled for it: Windows then treats it
; as new at every startup and installs it from our INF.
Function ScanDevices
  StrCpy $R9 0
  StrCpy $R7 0
  StrCpy $0 0
  ${Do}
    EnumRegKey $1 HKLM "${PCI_KEY}" $0
    ${If} $1 == ""
      ${ExitDo}
    ${EndIf}
    IntOp $0 $0 + 1
    StrCpy $2 $1 9
    ${If} $2 == "${VENDOR_PREFIX}"
      IntOp $R7 $R7 + 1
    ${EndIf}
    StrCpy $2 $1 17
    ${If} $2 == "${DEVICE_PREFIX}"
      StrCpy $3 0
      ${Do}
        EnumRegKey $4 HKLM "${PCI_KEY}\$1" $3
        ${If} $4 == ""
          ${ExitDo}
        ${EndIf}
        IntOp $3 $3 + 1
        IntOp $R9 $R9 + 1
        ${If} $R8 = 1
          WriteRegBin HKLM "${PCI_KEY}\$1\$4" "ConfigFlags" 20000000
        ${EndIf}
      ${Loop}
    ${EndIf}
  ${Loop}
FunctionEnd


;--------------------------------------------------------------------------------------------------
; Installation
;--------------------------------------------------------------------------------------------------

Function .onInit
  ; NSIS itself only recognizes "/S" in upper case.
  ${GetParameters} $0
  ClearErrors
  ${GetOptions} $0 "/S" $1
  ${IfNot} ${Errors}
  ${AndIf} $1 == ""
    SetSilent silent
  ${EndIf}

  ${If} ${IsNT}
  ${OrIfNot} ${AtLeastWin98}
  ${OrIfNot} ${AtMostWinME}
    MessageBox MB_ICONSTOP|MB_OK "$(MsgNeedWin9x)" /SD IDOK
    Abort
  ${EndIf}

  StrCpy $R8 0
  Call ScanDevices
  ${If} $R7 = 0
    MessageBox MB_ICONSTOP|MB_OK "$(MsgNoDevice)" /SD IDOK
    Abort
  ${EndIf}
FunctionEnd

Section "${PRODUCT}" SecMain
  SectionIn RO

  Call CloseVBoxTray

  !insertmacro INSTALL_FILE "..\VBoxGst\${OBJDIR}\i386\VBoxGst.sys" "$WINDIR\SYSTEM32\DRIVERS" "VBoxGst.sys" "VBOXGST.NEW"
  !insertmacro INSTALL_FILE "..\VBoxTray\${OBJDIR}\i386\VBoxTray.exe" "$SYSDIR" "VBoxTray.exe" "VBOXTRAY.NEW"
  !insertmacro INSTALL_FILE "..\VBoxMou\${OBJDIR}\i386\VBoxMou.vxd" "$SYSDIR" "VBoxMou.vxd" "VBOXMOU.NEW"

  SetOutPath "$WINDIR\INF\OTHER"
  File "/oname=${INF_NAME}" "obj\VBoxGst.inf"

  StrCpy $R8 1
  Call ScanDevices
  ${If} $R9 = 0
    DetailPrint "$(TxtNewDevice)"
  ${Else}
    DetailPrint "$(TxtDevices)"
  ${EndIf}

  WriteUninstaller "$SYSDIR\${UNINSTALLER}"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "${PRODUCT} ${VBG_VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VBG_VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "${PRODUCT}"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$SYSDIR\VBoxTray.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$SYSDIR\${UNINSTALLER}"'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1

  ; The driver is (re)installed by Windows at the next startup.
  SetRebootFlag true
SectionEnd


;--------------------------------------------------------------------------------------------------
; Uninstallation
;--------------------------------------------------------------------------------------------------

Function un.onInit
  ; NSIS itself only recognizes "/S" in upper case.
  ${un.GetParameters} $0
  ClearErrors
  ${un.GetOptions} $0 "/S" $1
  ${IfNot} ${Errors}
  ${AndIf} $1 == ""
    SetSilent silent
  ${EndIf}
FunctionEnd

Section "Uninstall"
  Call un.CloseVBoxTray

  DeleteRegValue HKLM "${RUN_KEY}" "VBoxTray"
  Delete /REBOOTOK "$SYSDIR\VBoxTray.exe"
  Delete /REBOOTOK "$SYSDIR\VBoxMou.vxd"
  Delete /REBOOTOK "$WINDIR\SYSTEM32\DRIVERS\VBoxGst.sys"
  Delete "$WINDIR\INF\OTHER\${INF_NAME}"

  ; Driver keys (Class\System\NNNN) that load VBoxGst.sys.
  StrCpy $0 0
  ${Do}
    EnumRegKey $1 HKLM "${CLASS_KEY}" $0
    ${If} $1 == ""
      ${ExitDo}
    ${EndIf}
    ReadRegStr $2 HKLM "${CLASS_KEY}\$1" "NTMPDriver"
    ${If} $2 == "VBoxGst.sys"
      DeleteRegKey HKLM "${CLASS_KEY}\$1"
    ${Else}
      IntOp $0 $0 + 1
    ${EndIf}
  ${Loop}

  ; Device entries; Windows detects the device again at the next startup.
  StrCpy $0 0
  ${Do}
    EnumRegKey $1 HKLM "${PCI_KEY}" $0
    ${If} $1 == ""
      ${ExitDo}
    ${EndIf}
    StrCpy $2 $1 17
    ${If} $2 == "${DEVICE_PREFIX}"
      DeleteRegKey HKLM "${PCI_KEY}\$1"
    ${Else}
      IntOp $0 $0 + 1
    ${EndIf}
  ${Loop}
  DetailPrint "$(TxtRemoved)"

  DeleteRegKey HKLM "${UNINST_KEY}"
  Delete "$SYSDIR\${UNINSTALLER}"

  SetRebootFlag true
SectionEnd
