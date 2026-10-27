# Game instances driven the way the launcher drives them (cmd.txt / status.txt, docs/PROTOCOL.md
# section 3): two players (and with -Spectator a third game watching through a stand-in feed relay,
# tools/feedrelay.py) boot to the hold point, get their start lines and play an online match over
# loopback. With -Attack player 1 jabs through the fight so the match ends; then the result is
# checked, -Rematch plays a second match from the CONTINUE? menu, and -Park sends both back to the
# hold point at the end. For testing the game's side of the launcher link without the launcher.
#   . tools\link-local.ps1; Start-LinkLocal -Out <folder> [-Mode versus|tag|tyl] [-Netcode lockstep]
#     [-Picks cursor] [-Snaps "match:frame,..."] [-Spectator] [-Attack] [-Rematch] [-Park]
function Start-LinkLocal {
    param([Parameter(Mandatory = $true)][string]$Out, [string]$Mode = "versus", [string]$Netcode = "lockstep",
          [int]$Delay = 2, [string]$Picks = "cursor", [string]$Snaps = "2:300", [int]$TimeoutS = 240,
          [switch]$Spectator, [switch]$Attack, [switch]$Rematch, [switch]$Park, [string]$Extra = "")
    $root = Split-Path $PSScriptRoot -Parent
    if (-not $root) { $root = "C:\Users\Owner\source\repos\mk9-rollback" }
    $bin = Join-Path $root "mk9\out\build\win-amd64-release"
    $disc = Join-Path $root "game\disc"
    $inst = @(@{ dir = "a"; slot = 0; port = 7101; peer = "1@127.0.0.1:7102"; feed = "127.0.0.1:7201" },
              @{ dir = "b"; slot = 1; port = 7102; peer = "0@127.0.0.1:7101"; feed = "127.0.0.1:7201" })
    if ($Spectator) {
        $inst += @{ dir = "w"; slot = -1; feed = "127.0.0.1:7202" }
        New-Item -ItemType Directory -Force $Out | Out-Null
        $relay = Start-Process -FilePath "python" -ArgumentList "-I", (Join-Path $root "tools\feedrelay.py"), "--players-port", "7201", "--watch-port", "7202" `
            -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $Out "relay.log") -RedirectStandardError (Join-Path $Out "relay.err")
        "relay pid $($relay.Id)"
    }
    $procs = @()
    foreach ($i in $inst) {
        $d = Join-Path $Out $i.dir
        New-Item -ItemType Directory -Force $d | Out-Null
        $envs = @{ MK9_LINK = "1"; MK9_CMD = (Join-Path $d "cmd.txt"); MK9_STATUS = (Join-Path $d "status.txt");
                   MK9_NET_LOG = (Join-Path $d "net-{match}.log"); MK9_NET_SNAPS = $Snaps;
                   MK9_SNAP_DIR = $d; MK9_SNAP_WIDTH = "480" }
        if ($Attack) { $envs["MK9_AUTOPILOT_ATTACK"] = "1" }
        foreach ($e in ($Extra -split ";")) { if ($e -match "^(\w+)=(.*)$") { $envs[$Matches[1]] = $Matches[2] } }
        foreach ($k in $envs.Keys) { Set-Item "Env:$k" $envs[$k] }
        $procs += Start-Process -FilePath (Join-Path $bin "mk9.exe") -WorkingDirectory $bin -PassThru -ArgumentList `
            "--game_data_root=$disc", "--user_data_root=$(Join-Path $d 'user')", "--log_file=$(Join-Path $d 'game.log')"
        foreach ($k in $envs.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
    }
    "started pids $($procs.Id -join ', ')"
    function Status($i) { Get-Content (Join-Path (Join-Path $Out $i.dir) "status.txt") -ErrorAction SilentlyContinue | Select-Object -First 1 }
    function Send($i, $line) {
        $cmd = Join-Path (Join-Path $Out $i.dir) "cmd.txt"
        Set-Content -Path "$cmd.tmp" -Value $line -Encoding ascii -NoNewline
        Move-Item -Force "$cmd.tmp" $cmd
    }
    function WaitAll($pattern, $who, $seconds) {
        $deadline = (Get-Date).AddSeconds($seconds)
        while ((Get-Date) -lt $deadline) {
            $ok = @($who | Where-Object { (Status $_) -match $pattern })
            if ($ok.Count -eq @($who).Count) { return $true }
            Start-Sleep -Milliseconds 250
        }
        "timed out waiting for '$pattern': " + (($who | ForEach-Object { "$($_.dir)=$(Status $_)" }) -join ", ")
        return $false
    }
    if (-not (WaitAll "^ready$" $inst $TimeoutS)) { return }
    "all ready"
    $match = "link-" + (Get-Date -Format "HHmmss")
    $players = $inst | Where-Object { $_.slot -ge 0 }
    foreach ($i in $inst) {
        $p = if ($Picks -and $i.slot -ge 0) { "p1=$Picks p2=$Picks" } else { "" }
        if ($i.slot -ge 0) {
            $f = if ($Spectator) { "feed=$($i.feed)" } else { "" }
            $line = "start mode=$Mode slot=$($i.slot) players=2 local_port=$($i.port) peers=$($i.peer) delay=$Delay netcode=$Netcode seed=12345 match=$match $p $f"
        } else {
            $line = "start mode=$Mode watch=1 feed=$($i.feed) players=2 seed=12345 match=$match"
        }
        Send $i $line
    }
    "sent start ($match)"
    if (-not $Attack) { return }
    if (-not (WaitAll "^result" $players 600)) { return }
    foreach ($i in $inst) { "$($i.dir): $(Status $i)" }
    if ($Rematch) {
        $match2 = "$match-2"
        foreach ($i in $players) { Send $i "rematch match=$match2" }
        "sent rematch ($match2)"
        Start-Sleep -Seconds 5
        if (-not (WaitAll "^result.*$match2" $players 600)) { return }
        foreach ($i in $inst) { "$($i.dir): $(Status $i)" }
    }
    if ($Park) {
        foreach ($i in $inst) { Send $i "park" }
        "sent park"
        if (-not (WaitAll "^ready$" $inst 120)) { return }
        "all back at the hold point"
    }
}
