#ifndef ReleaseVersion
  #error Build with /DReleaseVersion=MAJOR.MINOR.PATCH
#endif

[Setup]
AppId={{11EB6105-3F94-413F-B20A-BDD02761F6C3}
AppName=Scarlett Gen 2 Tool
AppVersion={#ReleaseVersion}
AppPublisherURL=https://github.com/bgunn9in/scarlett-gen2-tool
AppSupportURL=https://github.com/bgunn9in/scarlett-gen2-tool/issues
AppUpdatesURL=https://github.com/bgunn9in/scarlett-gen2-tool/releases
DefaultDirName={localappdata}\Programs\Scarlett Gen 2 Tool
DefaultGroupName=Scarlett Gen 2 Tool
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
MinVersion=10.0
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
WizardStyle=modern
UninstallDisplayIcon={app}\scarlett-gen2.exe
OutputDir=build
OutputBaseFilename=Scarlett-Gen2-Tool-Setup
Compression=lzma2
SolidCompression=yes
CloseApplicationsFilter=scarlett-gen2.exe
RestartApplications=no

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"

[Files]
Source: "build\scarlett-gen2.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\8202-1051.bin"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\8202-1116.bin"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Scarlett Gen 2 Tool"; Filename: "{app}\scarlett-gen2.exe"; WorkingDir: "{app}"

[Run]
Filename: "{app}\scarlett-gen2.exe"; Description: "{cm:LaunchProgram,Scarlett Gen 2 Tool}"; Flags: nowait postinstall skipifsilent
