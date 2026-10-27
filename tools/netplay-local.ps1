# Two (or more) game instances on this PC playing a scripted versus match over loopback netplay.
# Each instance boots with a fresh save in <Out>\<slot letter>, presses START at the title, meets
# the others at frame -MeetFrame on the main menu, then everything runs in sync: slot 0's script
# steers the menus and player 1, slot 1's picks for player 2. Logs and snapshots per instance.
#   . tools\netplay-local.ps1; Start-NetplayLocal -Out <folder> [-Mode lockstep|rollback] [-Delay 2]
#     [-Lag 0] [-Jitter 0] [-Snaps "2200,3400"] [-ContinueOnDesync]
function Start-NetplayLocal {
    param([Parameter(Mandatory = $true)][string]$Out, [string]$Mode = "lockstep", [int]$Delay = 2,
          [int]$Lag = 0, [int]$Jitter = 0, [string]$Snaps = "2200,3400,4600,5400,5900,6400",
          [int]$MeetFrame = 2100, [switch]$ContinueOnDesync, [switch]$TraceRand, [string]$Extra = "")
    $root = Split-Path $PSScriptRoot -Parent
    if (-not $root) { $root = "C:\Users\Owner\source\repos\mk9-rollback" }
    $bin = Join-Path $root "mk9\out\build\win-amd64-release"
    $disc = Join-Path $root "game\disc"
    New-Item -ItemType Directory -Force (Join-Path $Out "a"), (Join-Path $Out "b") | Out-Null
    # main menu: 2 PLAYERS, VERSUS MODE, ONE VS ONE; select screen: Y (arena select), A (its arena), picks
    $nav = "1900:START,+10:-,2150:UP,+8:-,2250:RIGHT,+8:-,2550:DOWN,+8:-,2650:A,+8:-,2850:A,+8:-,3300:Y,+8:-,3500:A,+8:-,3700:A,+8:-,4100:A,+8:-,4500:A,+8:-"
    $p = @(); for ($i = 0; $i -lt 6; $i++) { $p += "+12:X"; $p += "+6:-" }; $burst = ($p -join ",")
    $atk = "5900:RIGHT,+170:-,$burst,+60:RIGHT,+30:-,$burst,+60:RIGHT,+30:-,$burst"
    $common = @{ MK9_NO_PHYSICAL = "1"; MK9_PADS = "2"; MK9_NET_PLAYERS = "2"; MK9_NET_MODE = $Mode;
                 MK9_NET_DELAY = "$Delay"; MK9_NET_MEET_FRAME = "$MeetFrame"; MK9_NET_LOOPBACK = "1";
                 MK9_NET_LAG = "$Lag"; MK9_NET_JITTER = "$Jitter"; MK9_SNAP_FRAMES = $Snaps; MK9_SNAP_WIDTH = "480" }
    if ($ContinueOnDesync) { $common["MK9_NET_DESYNC_CONTINUE"] = "1" }
    foreach ($e in ($Extra -split ";")) { if ($e -match "^(\w+)=(.*)$") { $common[$Matches[1]] = $Matches[2] } }
    $inst = @(
        @{ dir = "a"; slot = 0; bind = 7101; peers = "1@127.0.0.1:7102"; p1 = "$nav,$atk"; p2 = "" },
        @{ dir = "b"; slot = 1; bind = 7102; peers = "0@127.0.0.1:7101"; p1 = "1900:START,+10:-"; p2 = "3750:A,+8:-,4150:A,+8:-,4550:A,+8:-" }
    )
    $procs = @()
    foreach ($i in $inst) {
        $d = Join-Path $Out $i.dir
        $envs = $common.Clone()
        $envs["MK9_NET_SLOT"] = "$($i.slot)"; $envs["MK9_NET_BIND"] = "$($i.bind)"; $envs["MK9_NET_PEERS"] = $i.peers
        $envs["MK9_NET_LOG"] = (Join-Path $d "net.log"); $envs["MK9_SNAP_DIR"] = $d
        $envs["MK9_INPUT_P1"] = $i.p1; $envs["MK9_INPUT_P2"] = $i.p2
        if ($TraceRand) { $envs["MK9_TRACE_RAND"] = (Join-Path $d "rand.txt") }
        foreach ($k in $envs.Keys) { Set-Item "Env:$k" $envs[$k] }
        $procs += Start-Process -FilePath (Join-Path $bin "mk9.exe") -WorkingDirectory $bin -PassThru -ArgumentList `
            "--game_data_root=$disc", "--user_data_root=$(Join-Path $d 'user')", "--log_file=$(Join-Path $d 'game.log')"
        foreach ($k in $envs.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
    }
    "started pids $($procs.Id -join ', ') into $Out"
}
