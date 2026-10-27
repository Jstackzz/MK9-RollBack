# One game instance playing the scripted versus match of netplay-local.ps1 on its own, with the
# rollback sync test on from frame -From: every -Every frames the game goes back -Depth frames and
# runs them again; each frame run again must write what it wrote the first time (mk9/net/synctest.cpp).
#   . tools\synctest.ps1; Start-Synctest -Out <folder> [-Depth 4] [-From 6200] [-Every 10] [-Snaps "6300"]
# -Depth 0 runs the same scripted match without the sync test (dumps, snapshots: -Extra "K=V;...").
function Start-Synctest {
    param([Parameter(Mandatory = $true)][string]$Out, [int]$Depth = 4, [int]$From = 6200, [int]$Every = 10,
          [string]$Snaps = "", [string]$Extra = "")
    $root = Split-Path $PSScriptRoot -Parent
    if (-not $root) { $root = "C:\Users\Owner\source\repos\mk9-rollback" }
    $bin = Join-Path $root "mk9\out\build\win-amd64-release"
    $disc = Join-Path $root "game\disc"
    New-Item -ItemType Directory -Force $Out | Out-Null
    # the same menus and fight as netplay-local.ps1 (player 2's picks on the second pad)
    $nav = "1900:START,+10:-,2150:UP,+8:-,2250:RIGHT,+8:-,2550:DOWN,+8:-,2650:A,+8:-,2850:A,+8:-,3300:Y,+8:-,3500:A,+8:-,3700:A,+8:-,4100:A,+8:-,4500:A,+8:-"
    $p = @(); for ($i = 0; $i -lt 6; $i++) { $p += "+12:X"; $p += "+6:-" }; $burst = ($p -join ",")
    $atk = "5900:RIGHT,+170:-,$burst,+60:RIGHT,+30:-,$burst,+60:RIGHT,+30:-,$burst"
    $envs = @{ MK9_NO_PHYSICAL = "1"; MK9_PADS = "2"; MK9_INPUT_P1 = "$nav,$atk"; MK9_INPUT_P2 = "3750:A,+8:-,4150:A,+8:-,4550:A,+8:-";
               MK9_SYNCTEST = "$Depth"; MK9_SYNCTEST_FROM = "$From"; MK9_SYNCTEST_EVERY = "$Every";
               MK9_SNAP_FRAMES = $Snaps; MK9_SNAP_DIR = $Out; MK9_SNAP_WIDTH = "480" }
    if ($Depth -le 0) { $envs.Remove("MK9_SYNCTEST"); $envs.Remove("MK9_SYNCTEST_FROM"); $envs.Remove("MK9_SYNCTEST_EVERY") }
    foreach ($e in ($Extra -split ";")) { if ($e -match "^(\w+)=(.*)$") { $envs[$Matches[1]] = $Matches[2] } }
    foreach ($k in $envs.Keys) { Set-Item "Env:$k" $envs[$k] }
    $proc = Start-Process -FilePath (Join-Path $bin "mk9.exe") -WorkingDirectory $bin -PassThru -ArgumentList `
        "--game_data_root=$disc", "--user_data_root=$(Join-Path $Out 'user')", "--log_file=$(Join-Path $Out 'game.log')"
    foreach ($k in $envs.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
    "started pid $($proc.Id) into $Out"
}
