; This Source Code Form is subject to the terms of the Mozilla Public
; License, v. 2.0. If a copy of the MPL was not distributed with this
; file, You can obtain one at https://mozilla.org/MPL/2.0/.
;
; Inno Setup script for the kvit-dictate per-user Windows installer (release-plan.md §3).
;
; Driven by packaging/windows/build-release.ps1, which builds, stages and passes the version.
; By hand:
;   iscc /DKvitVersion=1.0.0 /DStageDir=<stage> /DOutputDir=<dist> kvit-dictate.iss
;
; ─── Why per-user, and why that is not a detail ─────────────────────────────────────────────
;
; PrivilegesRequired=lowest installs into %LOCALAPPDATA%\Programs with no UAC prompt. That is a
; deliberate first impression: the first thing this application does after installation is
; register a global low-level keyboard hook, and an elevation prompt on top of that is precisely
; the wrong opening move for a tool asking to be trusted with every keystroke. plan.md §1.7 also
; makes an elevated *target* an unsupported state — the app refuses to type into one — so a
; per-machine, elevated install would be inconsistent with the program's own posture.
;
; ─── Not signed, and the consequence is stated rather than hidden ───────────────────────────
;
; release-plan.md §4 calls signing the largest risk in the release, and no certificate exists
; yet. An unsigned installer that lays down a program which hooks the keyboard and synthesises
; input will earn a SmartScreen "Windows protected your PC" screen, and may be quarantined
; outright. Nothing in this script pretends otherwise: there is no SignTool directive, and the
; first wizard page says what the program does before an antivirus gets to say it (§4 item 5).

#ifndef KvitVersion
  #error KvitVersion must be defined (pass /DKvitVersion=...)
#endif
#ifndef StageDir
  #error StageDir must be defined (pass /DStageDir=... the tree to install)
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
; A fixed AppId ties every version together as one product, so an upgrade replaces the previous
; install rather than sitting beside it, and the uninstaller can find what it installed.
; **Never change it.**
AppId={{4E1C9A67-2D3B-4F81-9A0E-7C6B5D48E210}
AppName=kvit-dictate
AppVersion={#KvitVersion}
AppVerName=kvit-dictate {#KvitVersion}
AppPublisher=Kvit
AppPublisherURL=https://github.com/kvit-s/kvit-dictate
AppSupportURL=https://github.com/kvit-s/kvit-dictate/issues
AppUpdatesURL=https://github.com/kvit-s/kvit-dictate/releases
DefaultDirName={autopf}\kvit-dictate
DefaultGroupName=kvit-dictate
UninstallDisplayIcon={app}\kvit-dictate.exe
UninstallDisplayName=kvit-dictate {#KvitVersion}
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
OutputDir={#OutputDir}
OutputBaseFilename=kvit-dictate-{#KvitVersion}-setup
SetupIconFile=kvit-dictate.ico
LicenseFile={#StageDir}\LICENSE
; Shown on the "ready to install" page, and on purpose: see §4 item 5. A tool that reads every
; keystroke to find its hotkey and types into other applications should say so itself, before
; the user's antivirus says something less accurate.
InfoBeforeFile=disclosure.txt
ChangesEnvironment=no
; force, not yes. The Restart Manager cannot ask kvit-dictate to close: its only window is a
; message-only one, which receives no WM_QUERYENDSESSION, so with `yes` an upgrade over a running
; copy waited 30 s, failed with "some applications could not be shut down" and rolled back. With
; `force` it ends the process after that wait instead (its child processes go with it — they are
; in a kill-on-close job). The in-app updater never gets that far: it quits first, and
; InitializeSetup below waits for it (/WAITPID).
CloseApplications=force
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
; Unchecked, like the desktop icon, and for the reason the [Registry] section gives: a dictation
; tool that added itself to startup uninvited would be making exactly the assumption this project
; is careful not to make. It was pre-ticked in the first draft, which a silent install then
; applied by default — verified by installing this and finding the Run key written without anyone
; having agreed to it.
Name: "startup"; Description: "Start kvit-dictate when I sign in"; GroupDescription: "Startup:"; Flags: unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\kvit-dictate"; Filename: "{app}\kvit-dictate.exe"
Name: "{group}\Setting up a server (docs)"; Filename: "{app}\docs\setup.md"
Name: "{group}\{cm:UninstallProgram,kvit-dictate}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\kvit-dictate"; Filename: "{app}\kvit-dictate.exe"; Tasks: desktopicon

[Registry]
; HKCU, to match the per-user install. Flagged so the uninstaller removes it, and only written
; when the task is ticked — a dictation tool that added itself to startup uninvited would be
; making exactly the assumption this project is careful not to make about the user's machine.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; \
    ValueName: "kvit-dictate"; ValueData: """{app}\kvit-dictate.exe"""; \
    Flags: uninsdeletevalue; Tasks: startup

[Run]
Filename: "{app}\kvit-dictate.exe"; Description: "{cm:LaunchProgram,kvit-dictate}"; \
    Flags: nowait postinstall skipifsilent
; The in-app updater (src/updater.h) runs this silently with /RELAUNCH=1, and the app it closed
; comes back when the install is done. As the original user, because an all-users install runs
; elevated and the app must not: plan.md §1.7 treats an elevated kvit-dictate as unsupported.
Filename: "{app}\kvit-dictate.exe"; Flags: nowait runasoriginaluser; Check: RelaunchAfterInstall

[UninstallDelete]
; Nothing here by default. Settings and logs live in %LOCALAPPDATA%\kvit-dictate and are removed
; only if the user asks, below — an uninstaller that silently deletes a configuration the user
; spent time on is one they cannot safely reinstall over.
Type: filesandordirs; Name: "{localappdata}\kvit-dictate"; Check: RemoveSettingsRequested

[Code]
const
  SYNCHRONIZE = $00100000;

var
  RemoveSettings: Boolean;
  Relaunched: Boolean;

function OpenProcess(dwDesiredAccess: Cardinal; bInheritHandle: Boolean;
  dwProcessId: Cardinal): THandle;
  external 'OpenProcess@kernel32.dll stdcall';
function WaitForSingleObject(hHandle: THandle; dwMilliseconds: Cardinal): Cardinal;
  external 'WaitForSingleObject@kernel32.dll stdcall';
function CloseHandle(hObject: THandle): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';

function RemoveSettingsRequested(): Boolean;
begin
  Result := RemoveSettings;
end;

// ─── The in-app updater's two parameters (src/updater.h) ────────────────────────────────────
//
//   /WAITPID=<pid>   the kvit-dictate that started this installer and is now quitting. Waited
//                    for before anything else happens, so no file is replaced while it runs.
//   /RELAUNCH=1      start kvit-dictate again afterwards — the new version when the install
//                    worked, the old one when it was cancelled or failed and rolled back.

function RelaunchRequested(): Boolean;
begin
  Result := ExpandConstant('{param:RELAUNCH|0}') = '1';
end;

function RelaunchAfterInstall(): Boolean;
begin
  Result := RelaunchRequested();
  if Result then Relaunched := True;
end;

function InitializeSetup(): Boolean;
var
  Pid: Integer;
  Process: THandle;
begin
  Result := True;
  Relaunched := False;
  Pid := StrToIntDef(ExpandConstant('{param:WAITPID|0}'), 0);
  if Pid > 0 then
  begin
    Process := OpenProcess(SYNCHRONIZE, False, Pid);
    if Process <> 0 then
    begin
      Log(Format('Waiting up to 30 s for kvit-dictate (pid %d) to exit', [Pid]));
      // Bounded: if it is somehow still there, CloseApplications=force deals with it.
      if WaitForSingleObject(Process, 30000) <> 0 then
        Log('It had not exited; leaving it to the Restart Manager');
      CloseHandle(Process);
    end;
  end;
end;

procedure DeinitializeSetup();
var
  Exe: String;
  ResultCode: Integer;
begin
  // Cancelled, or failed and rolled back: the updater already quit, so put the old version back
  // on the tray rather than leave the user with no dictation and no explanation.
  if RelaunchRequested() and not Relaunched then
  begin
    Exe := '';
    try
      Exe := ExpandConstant('{app}\kvit-dictate.exe');
    except
      Exe := '';
    end;
    if (Exe <> '') and FileExists(Exe) then
      ExecAsOriginalUser(Exe, '', '', SW_SHOWNORMAL, ewNoWait, ResultCode);
  end;
end;

function InitializeUninstall(): Boolean;
begin
  Result := True;
  RemoveSettings := False;
  // Asked, never assumed, and defaulting to No. The settings hold a server address the user
  // typed and per-application injection policies they tuned; both are cheap to keep and
  // annoying to reconstruct.
  if DirExists(ExpandConstant('{localappdata}\kvit-dictate')) then
  begin
    RemoveSettings := SuppressibleMsgBox(
      'Also remove your kvit-dictate settings and logs?' + #13#10#13#10 +
      'This deletes ' + ExpandConstant('{localappdata}\kvit-dictate') + ', including the server '
      + 'address you configured. Choose No if you plan to reinstall.',
      mbConfirmation, MB_YESNO or MB_DEFBUTTON2, IDNO) = IDYES;
  end;
end;
