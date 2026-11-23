; iscc /DVersion=<x.y.z> /DPlatform=<x64|arm64> /DSource=<unzipped build, absolute> /O<output dir> livewdf.iss
#define Arch Platform == "arm64" ? "arm64" : "x64compatible"

[Setup]
AppId={{EDEF0651-B467-4FDC-8CD9-34A0D430678D}
AppName=LiveWDF
AppVersion={#Version}
AppPublisher=Yahia Kemari
AppPublisherURL=https://github.com/Yahiake/LiveWDF
DefaultDirName={autopf}\LiveWDF
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed={#Arch}
ArchitecturesInstallIn64BitMode={#Arch}
LicenseFile={#Source}\LICENSE
OutputBaseFilename=LiveWDF-{#Version}-windows-{#Platform}-setup
UninstallDisplayName=LiveWDF

[Files]
Source: "{#Source}\Standalone\*"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Source}\Editor\*"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Source}\LICENSE"; DestDir: "{app}"
Source: "{#Source}\THIRD-PARTY-NOTICES.md"; DestDir: "{app}"
Source: "{#Source}\VST3\LiveWDF.vst3\*"; DestDir: "{commoncf64}\VST3\LiveWDF.vst3"; Flags: ignoreversion recursesubdirs

[Icons]
Name: "{autoprograms}\LiveWDF"; Filename: "{app}\LiveWDF.exe"
Name: "{autoprograms}\LiveWDF Editor"; Filename: "{app}\LiveWDF Editor.exe"
