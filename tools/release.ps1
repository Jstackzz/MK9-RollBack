# Makes a release for the launcher's updater: the player package (tools\package.ps1) zipped as
# dist\MK9Online-<version>.zip with its SHA-256 beside it (MK9Online-<version>.zip.sha256). Every
# installed launcher finds the newest GitHub release of the repository (Settings: update/github_repo,
# default Jstackzz/MK9-RollBack), downloads the zip, checks it and swaps the files in; when the
# release changed the game it builds the game again on the player's PC.
#
# Steps for a new release:
#   1. raise the number in VERSION (the launcher and the game both take it from there)
#   2. build: mk9\build.cmd release (to test), launcher\build.cmd, and the runtime for players:
#        tools\devenv.cmd ; cmake --build external\rexglue-sdk\out\build\win-amd64 --config Release
#        cmake --install external\rexglue-sdk\out\build\win-amd64 --config Release --prefix dist\sdk
#   3. . tools\release.ps1; New-Release -Notes "what changed"
#      With the GitHub CLI (gh) signed in, the release is created and both files uploaded; without it,
#      upload them on https://github.com/<repo>/releases/new under the tag v<version>.
function New-Release {
    param([string]$Notes = "", [string]$Repo = "Jstackzz/MK9-RollBack")
    $root = Split-Path $PSScriptRoot -Parent
    if (-not $root) { $root = "C:\Users\Owner\source\repos\mk9-rollback" }
    $version = (Get-Content (Join-Path $root "VERSION") -TotalCount 1).Trim()
    if (-not (Test-Path (Join-Path $root "build\launcher\MK9Launcher.exe"))) { throw "build the launcher first (launcher\build.cmd)" }
    # a fresh folder each time (never one holding a test's disc link)
    $stage = Join-Path $root "dist\release-$version"
    if (Test-Path $stage) { throw "$stage exists: delete it first" }
    . (Join-Path $PSScriptRoot "package.ps1")
    New-PlayerPackage -Out (Join-Path $stage "MK9-Online") | Write-Host
    $zip = Join-Path $root "dist\MK9Online-$version.zip"
    if (Test-Path $zip) { Remove-Item -Force $zip }
    Push-Location $stage
    try { tar.exe -a -c -f $zip "MK9-Online" } finally { Pop-Location }
    if (-not (Test-Path $zip)) { throw "zipping failed" }
    $hash = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
    [System.IO.File]::WriteAllText("$zip.sha256", "$hash  MK9Online-$version.zip`n")
    $mb = [math]::Round((Get-Item $zip).Length / 1MB)
    Write-Host "release $version : $zip ($mb MB), sha256 $hash"
    $gh = Get-Command gh -ErrorAction SilentlyContinue
    if ($gh) {
        $notesFile = Join-Path $stage "notes.md"
        [System.IO.File]::WriteAllText($notesFile, $(if ($Notes) { $Notes } else { "MK9 Online $version" }))
        & gh release create "v$version" $zip "$zip.sha256" --repo $Repo --title "MK9 Online $version" --notes-file $notesFile
    } else {
        Write-Host "GitHub CLI not found: create the release v$version on https://github.com/$Repo/releases/new and attach"
        Write-Host "  $zip"
        Write-Host "  $zip.sha256"
    }
}
