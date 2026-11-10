# Runs a package's build-game.cmd as on a PC with nothing installed: no compiler environment, a PATH of Windows only.
# Usage: tools\player\test-clean-build.ps1 -Game <package>\games\mk9 [-March x86-64-v2] [-Log file]
# -March x86-64-v2 is what a CPU without AVX2 builds. The package must be in a folder with a short path (under the
# repository's dist\) and hold the disc in games\mk9\game (a junction to game\disc will do: remove it on its own before
# deleting the folder). tools\player\check_package.py checks what the result needs.
param([Parameter(Mandatory)][string]$Game, [string]$March = "", [string]$Log = "")
$script = Join-Path $Game "build-game.cmd"
if (-not (Test-Path -LiteralPath $script)) { throw "no $script" }
if (-not $Log) { $Log = Join-Path (Split-Path (Split-Path (Split-Path $Game))) "build-out.txt" }
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = "$env:SystemRoot\System32\cmd.exe"
$psi.Arguments = "/d /s /c `"`"$script`"`""  # how the launcher runs it
$psi.WorkingDirectory = $Game
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.CreateNoWindow = $true
$keep = @{}
foreach ($n in "SystemRoot", "windir", "SystemDrive", "ComSpec", "PATHEXT", "NUMBER_OF_PROCESSORS", "PROCESSOR_ARCHITECTURE", "PROCESSOR_IDENTIFIER", "OS", "TEMP", "TMP", "USERPROFILE", "LOCALAPPDATA", "APPDATA", "ProgramData", "ProgramFiles", "ProgramW6432", "CommonProgramFiles", "HOMEDRIVE", "HOMEPATH", "USERNAME", "COMPUTERNAME") {
    $v = [Environment]::GetEnvironmentVariable($n)
    if ($v) { $keep[$n] = $v }
}
$psi.EnvironmentVariables.Clear()
foreach ($k in $keep.Keys) { $psi.EnvironmentVariables[$k] = $keep[$k] }
$psi.EnvironmentVariables["PATH"] = "$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\Wbem"
if ($March) { $psi.EnvironmentVariables["MK9_BUILD_MARCH"] = $March }
$sw = [Diagnostics.Stopwatch]::StartNew()
$p = [Diagnostics.Process]::Start($psi)
$errTask = $p.StandardError.ReadToEndAsync()
$out = $p.StandardOutput.ReadToEnd()
$p.WaitForExit()
$sw.Stop()
[IO.File]::WriteAllText($Log, $out + "`n--- stderr ---`n" + $errTask.Result)
"exit code $($p.ExitCode), $([int]$sw.Elapsed.TotalSeconds) s, log $Log"
($out -split "`n" | Select-Object -Last 6) -join "`n"
