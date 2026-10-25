; GunMetal-DX Windows installer (from Slipstream's) (NSIS 3, built on Linux by scripts/wincross/make_installer.sh).
;
; Per-user install (no admin prompt) to %LOCALAPPDATA%\Programs\GunMetal-DX. Installing a
; newer build over an older one replaces the program files in place. The uninstaller
; removes exactly the files this installer put there (uninstall_list.nsh, generated from
; the staged package) and keeps %APPDATA%\GunMetalDX (settings, chat logs) unless
; the user ticks the option to remove it.
;
; Defines (makensis -D...): VERSION (e.g. 0.12.0.123), DISPLAY_VERSION (e.g. 0.12.0 Beta 5),
; SRCDIR (staged package), OUTFILE, ASSETS (this directory), LISTS (dir with the generated
; install_list.nsh / uninstall_list.nsh), APP_NAME (optional, default GunMetal-DX).

Unicode true
SetCompressor /SOLID lzma
ManifestDPIAware true
RequestExecutionLevel user

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"

!ifndef APP_NAME
  !define APP_NAME    "GunMetal-DX"   ; <GMDX>
!endif
!define APP_EXE       "GunMetalDX.exe"         ; <GMDX>
!define APP_DATA      "GunMetalDX"
!define PUBLISHER     "GunMetal-DX"
!define WEBSITE       "https://github.com/squeedledorf/GunMetal-DX"
!define UNINST_EXE    "Uninstall GunMetal-DX.exe"
!define UNINST_KEY    "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}"
!define APP_KEY       "Software\${APP_NAME}"

Name "${APP_NAME} ${DISPLAY_VERSION}"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\${APP_NAME}"
InstallDirRegKey HKCU "${APP_KEY}" "InstallDir"
BrandingText "${APP_NAME} ${DISPLAY_VERSION}"

VIProductVersion "${VERSION}"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "ProductVersion" "${DISPLAY_VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "${APP_NAME} installer"
VIAddVersionKey "CompanyName" "${PUBLISHER}"
VIAddVersionKey "LegalCopyright" "LGPL 2.1; see licenses.txt"

!define MUI_ICON   "${ASSETS}/install_icon.ico"
!define MUI_UNICON "${ASSETS}/uninstall_icon.ico"
!define MUI_ABORTWARNING
!define MUI_COMPONENTSPAGE_SMALLDESC
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Start ${APP_NAME} now"
!define MUI_FINISHPAGE_LINK "${WEBSITE}"
!define MUI_FINISHPAGE_LINK_LOCATION "${WEBSITE}"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_COMPONENTS
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; Refuse to run over a copy that is running (files in use would half-update it).
; "tasklist | find" exits 0 when the process is listed.
!macro CheckNotRunning
    ${Do}
        nsExec::ExecToStack 'cmd /c tasklist /FI "IMAGENAME eq ${APP_EXE}" /NH | find /I "${APP_EXE}"'
        Pop $0
        Pop $1
        ${If} $0 != 0
            ${Break}
        ${EndIf}
        MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "${APP_NAME} is running. Close it, then click Retry." /SD IDCANCEL IDRETRY +2
        Abort
    ${Loop}
!macroend

Section "${APP_NAME} (required)" SecMain
    SectionIn RO
    !insertmacro CheckNotRunning

    SetOutPath "$INSTDIR"
    ; Remove the program files of the version being replaced (only what an installer of
    ; ours put here, recorded at install time), then lay down the new ones.
    IfFileExists "$INSTDIR\uninstall_list.nsh.txt" 0 +2
        Call RemovePreviousProgramFiles
    !include "${LISTS}/install_list.nsh"
    File "/oname=$INSTDIR\uninstall_list.nsh.txt" "${LISTS}/uninstall_list.txt"

    WriteUninstaller "$INSTDIR\${UNINST_EXE}"
    CreateShortCut "$SMPROGRAMS\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0

    WriteRegStr HKCU "${APP_KEY}" "InstallDir" "$INSTDIR"
    WriteRegStr HKCU "${APP_KEY}" "Version" "${DISPLAY_VERSION}"
    WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "${APP_NAME}"
    WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${DISPLAY_VERSION}"
    WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "${PUBLISHER}"
    WriteRegStr HKCU "${UNINST_KEY}" "URLInfoAbout" "${WEBSITE}"
    WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\${APP_EXE}"
    WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\${UNINST_EXE}"'
    WriteRegStr HKCU "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\${UNINST_EXE}" /S'
    WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
    WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    IntFmt $0 "0x%08X" $0
    WriteRegDWORD HKCU "${UNINST_KEY}" "EstimatedSize" "$0"
SectionEnd

Section "Desktop shortcut" SecDesktop
    CreateShortCut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0
SectionEnd

!macro RegisterScheme scheme
    WriteRegStr HKCU "Software\Classes\${scheme}" "" "URL:Second Life"
    WriteRegStr HKCU "Software\Classes\${scheme}" "URL Protocol" ""
    WriteRegStr HKCU "Software\Classes\${scheme}\DefaultIcon" "" '"$INSTDIR\${APP_EXE}"'
    WriteRegStr HKCU "Software\Classes\${scheme}\shell\open\command" "" '"$INSTDIR\${APP_EXE}" -url "%1"'
!macroend

Section /o "Open secondlife:// links with ${APP_NAME}" SecSLURL
    !insertmacro RegisterScheme "secondlife"
    !insertmacro RegisterScheme "x-grid-location-info"
SectionEnd

; Only remove a scheme handler that still points at this install (another viewer may have
; taken it over since).
!macro UnregisterScheme scheme
    ReadRegStr $0 HKCU "Software\Classes\${scheme}\shell\open\command" ""
    ${If} $0 == '"$INSTDIR\${APP_EXE}" -url "%1"'
        DeleteRegKey HKCU "Software\Classes\${scheme}"
    ${EndIf}
!macroend

Function RemovePreviousProgramFiles
    ; uninstall_list.nsh.txt holds one relative path per line: "D name" or "F name".
    ClearErrors
    FileOpen $0 "$INSTDIR\uninstall_list.nsh.txt" r
    ${Do}
        FileRead $0 $1
        ${If} ${Errors}
            ${Break}
        ${EndIf}
        ; strip CR/LF
        ${Do}
            StrCpy $2 $1 1 -1
            ${If} $2 == "$\n"
            ${OrIf} $2 == "$\r"
                StrCpy $1 $1 -1
            ${Else}
                ${Break}
            ${EndIf}
        ${Loop}
        StrCpy $3 $1 2
        StrCpy $4 $1 "" 2
        ${If} $4 == ""
            ${Continue}
        ${EndIf}
        ${If} $3 == "D "
            RMDir /r "$INSTDIR\$4"
        ${ElseIf} $3 == "F "
            Delete "$INSTDIR\$4"
        ${EndIf}
    ${Loop}
    FileClose $0
FunctionEnd

LangString DESC_SecMain ${LANG_ENGLISH} "The viewer itself, with a Start menu shortcut and an uninstaller."
LangString DESC_SecDesktop ${LANG_ENGLISH} "Put a ${APP_NAME} shortcut on the desktop."
LangString DESC_SecSLURL ${LANG_ENGLISH} "Make secondlife:// links in your web browser open ${APP_NAME}. Leave it off to keep them opening your current viewer."
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
    !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} $(DESC_SecMain)
    !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} $(DESC_SecDesktop)
    !insertmacro MUI_DESCRIPTION_TEXT ${SecSLURL} $(DESC_SecSLURL)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ---------------------------------------------------------------------------- uninstall

Section "un.${APP_NAME}" UnSecMain
    SectionIn RO
    !insertmacro CheckNotRunning

    ; Exactly what the installer put here; RMDir without /r at the end so anything the
    ; user added to the folder keeps it.
    IfFileExists "$INSTDIR\uninstall_list.nsh.txt" 0 +2
        Call un.RemovePreviousProgramFiles
    Delete "$INSTDIR\uninstall_list.nsh.txt"
    Delete "$INSTDIR\${UNINST_EXE}"
    RMDir "$INSTDIR"

    Delete "$SMPROGRAMS\${APP_NAME}.lnk"
    Delete "$DESKTOP\${APP_NAME}.lnk"
    !insertmacro UnregisterScheme "secondlife"
    !insertmacro UnregisterScheme "x-grid-location-info"
    DeleteRegKey HKCU "${UNINST_KEY}"
    DeleteRegKey HKCU "${APP_KEY}"
SectionEnd

Section /o "un.Also remove my settings, chat logs and cache" UnSecData
    RMDir /r "$APPDATA\${APP_DATA}"
    RMDir /r "$LOCALAPPDATA\${APP_DATA}"
SectionEnd

Function un.RemovePreviousProgramFiles
    ClearErrors
    FileOpen $0 "$INSTDIR\uninstall_list.nsh.txt" r
    ${Do}
        FileRead $0 $1
        ${If} ${Errors}
            ${Break}
        ${EndIf}
        ; strip CR/LF
        ${Do}
            StrCpy $2 $1 1 -1
            ${If} $2 == "$\n"
            ${OrIf} $2 == "$\r"
                StrCpy $1 $1 -1
            ${Else}
                ${Break}
            ${EndIf}
        ${Loop}
        StrCpy $3 $1 2
        StrCpy $4 $1 "" 2
        ${If} $4 == ""
            ${Continue}
        ${EndIf}
        ${If} $3 == "D "
            RMDir /r "$INSTDIR\$4"
        ${ElseIf} $3 == "F "
            Delete "$INSTDIR\$4"
        ${EndIf}
    ${Loop}
    FileClose $0
FunctionEnd

LangString DESC_UnSecMain ${LANG_ENGLISH} "Remove the ${APP_NAME} program files, shortcuts and uninstall entry."
LangString DESC_UnSecData ${LANG_ENGLISH} "Also delete %APPDATA%\${APP_DATA} (settings, saved logins, chat transcripts) and the cache. Leave this off if you might reinstall."
!insertmacro MUI_UNFUNCTION_DESCRIPTION_BEGIN
    !insertmacro MUI_DESCRIPTION_TEXT ${UnSecMain} $(DESC_UnSecMain)
    !insertmacro MUI_DESCRIPTION_TEXT ${UnSecData} $(DESC_UnSecData)
!insertmacro MUI_UNFUNCTION_DESCRIPTION_END
