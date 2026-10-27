# Two game instances driven the way the launcher drives them (cmd.txt / status.txt, docs/PROTOCOL.md
# section 3): both boot to the hold point, then each gets its start line and they play an online
# match over loopback. For testing the game's side of the launcher link without the launcher.
#   . tools\link-local.ps1; Start-LinkLocal -Out <folder> [-Mode versus|tag|tyl] [-Netcode lockstep]
#     [-Picks "cursor"] [-Snaps "match:frame,..."]
function Start-LinkLocal {
    param([Parameter(Mandatory = $true)][string]$Out, [string]$Mode = "versus", [string]$Netcode = "lockstep",
          [int]$Delay = 2, [string]$Picks = "cursor", [string]$Snaps = "2:300", [int]$TimeoutS = 240,
          [string]$Extra = "")
    $root = Split-Path $PSScriptRoot -Parent
    if (-not $root) { $root = "C:\Users\Owner\source\repos\mk9-rollback" }
    $bin = Join-Path $root "mk9\out\build\win-amd64-release"
    $disc = Join-Path $root "game\disc"
    $inst = @(@{ dir = "a"; slot = 0; port = 7101; peer = "1@127.0.0.1:7102" },
              @{ dir = "b"; slot = 1; port = 7102; peer = "0@127.0.0.1:7101" })
    $procs = @()
    foreach ($i in $inst) {
        $d = Join-Path $Out $i.dir
        New-Item -ItemType Directory -Force $d | Out-Null
        $envs = @{ MK9_LINK = "1"; MK9_CMD = (Join-Path $d "cmd.txt"); MK9_STATUS = (Join-Path $d "status.txt");
                   MK9_NO_PHYSICAL = "1"; MK9_NET_LOG = (Join-Path $d "net-{match}.log"); MK9_NET_SNAPS = $Snaps;
                   MK9_SNAP_DIR = $d; MK9_SNAP_WIDTH = "480" }
        foreach ($e in ($Extra -split ";")) { if ($e -match "^(\w+)=(.*)$") { $envs[$Matches[1]] = $Matches[2] } }
        foreach ($k in $envs.Keys) { Set-Item "Env:$k" $envs[$k] }
        $procs += Start-Process -FilePath (Join-Path $bin "mk9.exe") -WorkingDirectory $bin -PassThru -ArgumentList `
            "--game_data_root=$disc", "--user_data_root=$(Join-Path $d 'user')", "--log_file=$(Join-Path $d 'game.log')"
        foreach ($k in $envs.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
    }
    "started pids $($procs.Id -join ', ')"
    # both at the hold point
    $deadline = (Get-Date).AddSeconds($TimeoutS)
    while ((Get-Date) -lt $deadline) {
        $ready = $inst | Where-Object { (Get-Content (Join-Path (Join-Path $Out $_.dir) "status.txt") -ErrorAction SilentlyContinue | Select-Object -First 1) -eq "ready" }
        if (@($ready).Count -eq 2) { break }
        Start-Sleep -Milliseconds 250
    }
    if (@($ready).Count -ne 2) { "not both ready"; return }
    "both ready"
    $match = "link-" + (Get-Date -Format "HHmmss")
    foreach ($i in $inst) {
        $p = if ($Picks) { "p1=$Picks p2=$Picks" } else { "" }
        $line = "start mode=$Mode slot=$($i.slot) players=2 local_port=$($i.port) peers=$($i.peer) delay=$Delay netcode=$Netcode seed=12345 match=$match $p"
        $cmd = Join-Path (Join-Path $Out $i.dir) "cmd.txt"
        Set-Content -Path "$cmd.tmp" -Value $line -Encoding ascii -NoNewline
        Move-Item -Force "$cmd.tmp" $cmd
    }
    "sent start ($match)"
}
