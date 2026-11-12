# One launcher-style rollback match between two games on this PC over a simulated network, with the match stats each
# game writes (stats-<match>.json: what the server's match log gets) kept in a folder for tools\statsum.py. For comparing
# pacing, frame cap and netcode changes on what the screen got, not on the frames' own tops.
#
#   . tools\pace-local.ps1
#   Start-PaceRun -Name base -Extra "MK9_NET_LAG=45;MK9_NET_JITTER=15"                         # two even games
#   Start-PaceRun -Name hic  -Extra "MK9_NET_LAG=45;MK9_NET_JITTER=15" -ExtraB "MK9_TEST_HICCUP=4000:40"   # B stalls
#   python -I tools\statsum.py <Results> base hic
#
# -Extra / -ExtraB: environment for both games / for game B only ("A=1;B=2"). Useful variables (docs/GAME.md):
#   MK9_NET_LAG=<ms> MK9_NET_JITTER=<ms>   the simulated network (one way)
#   MK9_TEST_HICCUP=<every ms>:<busy ms>   the game's main thread stalls that long now and then in a fight
#   MK9_CAP_SLACK_MS=2                     the frame cap of 0.5.10 and before (17 since 0.5.11)
#   MK9_TEST_PACE_STREAK=0                 the lead hold of 0.5.10 and before (14 frames since 0.5.11)
#   MK9_FRAME_CAP=40                       a slower PC
# -Bin: a folder with another build (mk9.exe, rexruntime.dll, rexgpu-xenos.dll), so that a build can go on while a series
# runs. -Delay: the input delay (4 covers a 45 ms lag with 15 of jitter). -TraceScript: each game's script trace is kept
# too (python -I tools\scriptcmp.py <Results>\<name>.a.script.txt <Results>\<name>.b.script.txt: the fight, 0 frames differ).
# Each run takes about 4 minutes and shows two game windows; the run folders (saves, logs, 100s of MB) are deleted at the end.
function Start-PaceRun {
    param([Parameter(Mandatory = $true)][string]$Name, [string]$Extra = "", [string]$ExtraB = "", [int]$Delay = 4,
          [string]$Bin = "", [string]$Results = "", [switch]$AttackB, [switch]$Rematch, [switch]$TraceScript)
    $root = Split-Path $PSScriptRoot -Parent
    if (-not $Results) { $Results = Join-Path $env:TEMP "mk9-pace-results" }
    $work = Join-Path $env:TEMP "mk9-pace-run"
    $out = Join-Path $work $Name
    New-Item -ItemType Directory -Force $work, $Results | Out-Null
    if (Test-Path $out) { Remove-Item -Recurse -Force $out }
    . (Join-Path $root "tools\link-local.ps1")
    $args2 = @{ Out = $out; Netcode = "rollback"; Delay = $Delay; Extra = $Extra; ExtraB = $ExtraB; Snaps = "99:99" }
    if ($Bin) { $args2["Bin"] = $Bin }
    if ($AttackB) { $args2["AttackB"] = $true } else { $args2["Attack"] = $true }
    if ($Rematch) { $args2["Rematch"] = $true }
    if ($TraceScript) { $args2["TraceScript"] = $true }
    $log = Start-LinkLocal @args2 *>&1 | Out-String
    Set-Content -Path (Join-Path $Results "$Name.runner.txt") -Value $log
    $pids = @([regex]::Matches($log, "started pids ([\d, ]+)") | ForEach-Object { $_.Groups[1].Value -split ",\s*" } | Where-Object { $_ })
    Start-Sleep -Seconds 12   # the games write their stats every 10 s and at the result
    foreach ($d in "a", "b") {
        $dir = Join-Path $out $d
        Get-ChildItem (Join-Path $dir "stats-*.json") -ErrorAction SilentlyContinue | ForEach-Object {
            Copy-Item $_.FullName (Join-Path $Results "$Name.$d.$($_.Name)")
        }
        if ($TraceScript -and (Test-Path (Join-Path $dir "script.txt"))) { Copy-Item (Join-Path $dir "script.txt") (Join-Path $Results "$Name.$d.script.txt") }
        foreach ($f in "crash.txt", "hang.txt") {   # a game that crashed or stood still leaves one: judge a run by these first
            if (Test-Path (Join-Path $dir $f)) { Copy-Item (Join-Path $dir $f) (Join-Path $Results "$Name.$d.$f") }
        }
        $gl = Join-Path $dir "game.log"
        if (Test-Path $gl) {   # the pacing, rollback and frame rate lines only: the log is big
            Select-String -Path $gl -Pattern "mk9 net:|frames/s|\[error\]|\[warning\]" | ForEach-Object { $_.Line } |
                Select-Object -First 4000 | Set-Content (Join-Path $Results "$Name.$d.game-lines.txt")
        }
    }
    foreach ($p in $pids) { if ($p) { Stop-Process -Id ([int]$p) -Force -ErrorAction SilentlyContinue } }
    Start-Sleep -Seconds 3
    Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
    "done $Name (stats in $Results)"
}
