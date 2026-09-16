; Inno Setup script -- Breath of Fire IV deutsche Uebersetzung (Patch-Installer).
; Distributes ONLY diffs + our own files (no whole game DATs). Runs the bundled
; official Python embeddable on apply_inno.py -> no PyInstaller, no AV false-positive.
;   Compile:  "C:\Program Files\Inno Setup 7\ISCC.exe" BoF4_DE.iss

#define MyName "Breath of Fire IV - Deutsche Übersetzung"
#define MyVer  "1.1"

[Setup]
AppName={#MyName}
AppVersion={#MyVer}
AppPublisher=BoF4 DE
DefaultDirName={tmp}\bof4de
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=no
UninstallDisplayName={#MyName}
Uninstallable=no
OutputDir=dist
OutputBaseFilename=BoF4_DE_Installieren
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "de"; MessagesFile: "compiler:Languages\German.isl"

[Files]
; the whole apply payload -> extracted to {tmp}, removed after install
Source: "pyembed\*";   DestDir: "{tmp}\pyembed";  Flags: recursesubdirs ignoreversion deleteafterinstall
Source: "runtime\*";   DestDir: "{tmp}\runtime";  Flags: recursesubdirs ignoreversion deleteafterinstall
Source: "payload\*";   DestDir: "{tmp}\payload";  Flags: recursesubdirs ignoreversion deleteafterinstall
Source: "derived\*";   DestDir: "{tmp}\derived";  Flags: recursesubdirs ignoreversion deleteafterinstall
Source: "files\*";     DestDir: "{tmp}\files";    Flags: recursesubdirs ignoreversion deleteafterinstall
Source: "apply_inno.py"; DestDir: "{tmp}";        Flags: ignoreversion deleteafterinstall

[Code]
var
  GamePage: TInputDirWizardPage;
  TitlePage: TInputOptionWizardPage;

function HasDAT(Dir: String): Boolean;
begin
  Result := DirExists(Dir + '\english\DAT') or DirExists(Dir + '\DAT');
end;

// Try to locate the game: Steam (registry + common libraries) then GOG heuristics.
function DetectGame(): String;
var steam, p, dl: String; i: Integer;
begin
  Result := '';
  if RegQueryStringValue(HKCU, 'Software\Valve\Steam', 'SteamPath', steam) then
  begin
    p := steam + '\steamapps\common\4249150_BreathofFire4';
    if HasDAT(p) then begin Result := p; Exit; end;
  end;
  // scan drive letters for common Steam-library / GOG install locations
  for i := Ord('C') to Ord('L') do
  begin
    dl := Chr(i);
    p := dl + ':\SteamLibrary\steamapps\common\4249150_BreathofFire4';        if HasDAT(p) then begin Result := p; Exit; end;
    p := dl + ':\Program Files (x86)\Steam\steamapps\common\4249150_BreathofFire4'; if HasDAT(p) then begin Result := p; Exit; end;
    p := dl + ':\GOG Games\Breath of Fire IV';                                if HasDAT(p) then begin Result := p; Exit; end;
    p := dl + ':\GOG\Breath of Fire IV';                                      if HasDAT(p) then begin Result := p; Exit; end;
    p := dl + ':\Games\Breath of Fire IV';                                    if HasDAT(p) then begin Result := p; Exit; end;
  end;
end;

procedure InitializeWizard();
var found: String;
begin
  GamePage := CreateInputDirPage(wpWelcome,
    'Spielordner', 'Wo ist Breath of Fire IV installiert?',
    'Wähle den Ordner, in dem BOF4.exe liegt. Er wurde versucht automatisch zu finden.',
    False, '');
  GamePage.Add('');
  found := DetectGame();
  if found <> '' then GamePage.Values[0] := found;

  TitlePage := CreateInputOptionPage(GamePage.ID,
    'Titelbildschirm', 'Welchen Titlescreen möchtest du verwenden?',
    'Beides mit deutschen Menütexten. Später änderbar durch erneutes Ausführen.',
    True, False);
  TitlePage.Add('Amerikanischen (US)');
  TitlePage.Add('Japanischen');
  TitlePage.SelectedValueIndex := 0;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = GamePage.ID) then
  begin
    if not HasDAT(GamePage.Values[0]) then
    begin
      MsgBox('In diesem Ordner wurde kein "DAT"-Verzeichnis gefunden.' + #13#10 +
             'Bitte den Spielordner wählen (dort liegt BOF4.exe).', mbError, MB_OK);
      Result := False;
    end;
  end;
end;

// After the files are extracted, run the applier.
procedure CurStepChanged(CurStep: TSetupStep);
var rc: Integer; title, cmd: String;
begin
  if CurStep = ssPostInstall then
  begin
    if TitlePage.SelectedValueIndex = 1 then title := 'jp' else title := 'us';
    // call python.exe directly: params = "<apply_inno.py>" "<gamedir>" us|jp
    cmd := '"' + ExpandConstant('{tmp}\apply_inno.py') + '" "' + GamePage.Values[0] + '" ' + title;
    if not Exec(ExpandConstant('{tmp}\pyembed\python.exe'), cmd, ExpandConstant('{tmp}'),
                SW_HIDE, ewWaitUntilTerminated, rc) then
    begin
      MsgBox('Konnte den Installationsvorgang nicht starten.', mbError, MB_OK);
      Abort();
    end;
    if rc <> 0 then
    begin
      MsgBox('Die Übersetzung konnte nicht vollständig eingespielt werden ' +
             '(Fehlercode ' + IntToStr(rc) + ').' + #13#10 +
             'Sind die Spieldateien im Originalzustand? Ggf. bei Steam/GOG die ' +
             'Dateien überprüfen lassen und erneut versuchen.', mbError, MB_OK);
      Abort();
    end;
  end;
end;
