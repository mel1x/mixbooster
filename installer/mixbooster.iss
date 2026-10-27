; MixBooster installer (Inno Setup 7). release.bat builds it:
;   build\MixBooster.exe  ->  build\MixBooster-Setup.exe
; The version comes from the exe (res\version.h, ProductVersion).
;
; It installs to Program Files\MixBooster, the folder the service runs its copy from, so the app and the service
; share one exe. The app's own updates run this setup silently: /update=1 starts the new version after it,
; /update=2 (the app was closing) doesn't.

#ifndef SourceExe
  #define SourceExe "..\build\MixBooster.exe"
#endif
#define AppVersion GetStringFileInfo(SourceExe, "ProductVersion")

[Setup]
AppId={{F07DFB38-BA6B-4FF0-BB9D-9BF2CBC53724}
AppName=MixBooster
AppVersion={#AppVersion}
AppVerName=MixBooster {#AppVersion}
AppPublisher=mel1x
AppPublisherURL=https://github.com/mel1x/mixbooster
AppSupportURL=https://github.com/mel1x/mixbooster/issues
AppUpdatesURL=https://github.com/mel1x/mixbooster/releases
DefaultDirName={autopf}\MixBooster
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir=..\build
OutputBaseFilename=MixBooster-Setup
SetupIconFile=..\res\mixbooster.ico
UninstallDisplayIcon={app}\MixBooster.exe
UninstallDisplayName=MixBooster
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
; We close the app and stop the service ourselves (PrepareToInstall, --uninstall).
CloseApplications=no
VersionInfoVersion={#AppVersion}
VersionInfoProductName=MixBooster

[Languages]
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
ru.DesktopIcon=Значок на рабочем столе
en.DesktopIcon=Desktop shortcut
ru.LaunchNow=Запустить MixBooster
en.LaunchNow=Launch MixBooster

[Tasks]
Name: "desktopicon"; Description: "{cm:DesktopIcon}"; Flags: unchecked

[Files]
Source: "{#SourceExe}"; DestDir: "{app}"; DestName: "MixBooster.exe"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\MixBooster"; Filename: "{app}\MixBooster.exe"
Name: "{autodesktop}\MixBooster"; Filename: "{app}\MixBooster.exe"; Tasks: desktopicon

[Run]
; The service was stopped for the new exe: it starts again (sc only fails when it isn't installed).
Filename: "{sys}\sc.exe"; Parameters: "start MixBoosterGpuAffinity"; Flags: runhidden waituntilterminated
; postinstall entries run as the original, non-elevated user by default; the app needs administrator rights.
Filename: "{app}\MixBooster.exe"; Description: "{cm:LaunchNow}"; Flags: postinstall nowait skipifsilent runascurrentuser
; An update from the app, by a click on its card: start the new version.
Filename: "{app}\MixBooster.exe"; Flags: nowait runascurrentuser; Check: IsRelaunch

[UninstallRun]
; Closes the app and removes the service. The settings the app made stay as they are; ProgramData\MixBooster stays.
Filename: "{app}\MixBooster.exe"; Parameters: "--uninstall"; Flags: runhidden waituntilterminated; RunOnceId: "MixBoosterUninstall"

[Code]
function IsRelaunch: Boolean;
begin
  Result := ExpandConstant('{param:update|0}') = '1';
end;

// Close the running app and stop the service so the exe can be replaced. The exe there may also be the service's
// copy from a portable MixBooster.
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Exe: String;
  Code: Integer;
begin
  Result := '';
  Exe := ExpandConstant('{app}\MixBooster.exe');
  if FileExists(Exe) then
    Exec(Exe, '--shutdown', '', SW_HIDE, ewWaitUntilTerminated, Code);
end;
