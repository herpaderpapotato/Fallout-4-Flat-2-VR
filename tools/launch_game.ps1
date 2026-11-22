# Launch Fallout 4 through Mod Organizer 2 (F4SE executable, profile TestProfileVR) so the VFS supplies the fo4flatvr and
# FRIK-flat mods, unless the game is already running, and wait for the plugin's XR session.
# Since 15 Nov 21:16 the plugins are MO2 mods only; the base Data folder must not contain them (refused below).
# The OpenXR runtime is whatever fo4flatvr.ini [OpenXR] sRuntimeJson says (empty = the system's active runtime).
# MO2 is never closed: with its window open the request is forwarded to it.
# Exit codes: 0 = launched and XR up, 2 = already running (nothing launched), 1 = failed/timeout/refused.
param([int]$TimeoutSec = 180)

$running = Get-Process Fallout4 -ErrorAction SilentlyContinue
if ($running) {
    Write-Output "Fallout4 already running (pid $($running.Id), started $($running.StartTime)) - not launching."
    exit 2
}
$game = 'C:\Program Files (x86)\Steam\steamapps\common\Fallout 4'
$mo2 = 'C:\Modding\MO2\ModOrganizer.exe'
$inst = "$env:LOCALAPPDATA\ModOrganizer\Fallout 4"
$modlist = "$inst\profiles\TestProfileVR\modlist.txt"
$log = "$env:USERPROFILE\Documents\My Games\Fallout4\F4SE\fo4flatvr.log"

# old deploy path: a base copy would load twice / shadow the MO2 mod
$dup = Get-ChildItem "$game\Data\F4SE\Plugins" -ErrorAction SilentlyContinue | Where-Object { $_.Name -in @('fo4flatvr.dll', 'FRIK.dll') }
if ($dup) {
    Write-Output "REFUSING: $($dup.Name -join ', ') in the base $game\Data\F4SE\Plugins - the plugins live in the MO2 mods only; move it out (rename) first."
    exit 1
}
# the mod list is the user's: only read it
$lines = Get-Content $modlist -ErrorAction SilentlyContinue
if (-not ($lines -contains '+fo4flatvr')) {
    Write-Output "REFUSING: MO2 mod fo4flatvr is not enabled in profile TestProfileVR ($modlist). Enable it in MO2."
    exit 1
}
if (-not ($lines -contains '+FRIK-flat')) { Write-Output "note: FRIK-flat is disabled in MO2 (no FRIK smooth movement / body)." }
if (Test-Path "$inst\overwrite\F4SE\Plugins\fo4flatvr.ini") {
    Write-Output "WARNING: MO2 overwrite holds fo4flatvr.ini ($inst\overwrite\F4SE\Plugins) - it overrides the mod folder's ini."
}

$start = Get-Date
try {
    Start-Process -FilePath $mo2 -ArgumentList @('-i', '"Fallout 4"', '-p', 'TestProfileVR', 'run', '-e', 'F4SE') -WorkingDirectory (Split-Path $mo2) -ErrorAction Stop
} catch {
    Write-Output "launch failed: $($_.Exception.Message)"
    exit 1
}
$t = 0
Start-Sleep 5
# only the new run's log counts: the plugin rewrites it at start, so its LastWriteTime must be after $start
function XrUp {
    $f = Get-Item $log -ErrorAction SilentlyContinue
    if (-not $f -or $f.LastWriteTime -lt $start) { return $false }
    return [bool](Select-String -Path $log -Pattern 'state 2 -> 5|OpenXR init failed' -Quiet -ErrorAction SilentlyContinue)
}
while ($t -lt $TimeoutSec -and -not (XrUp)) {
    Start-Sleep 2; $t += 2
}
if ($t -ge $TimeoutSec) { Write-Output "timed out waiting for XR session (MO2 may be showing a dialog)"; exit 1 }
$p = Get-Process Fallout4 -ErrorAction SilentlyContinue
Write-Output "launched via MO2 (pid $($p.Id)); XR session up after ~$($t + 5)s"
exit 0
