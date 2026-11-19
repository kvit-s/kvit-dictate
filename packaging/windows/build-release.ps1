# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
<#
.SYNOPSIS
    Build kvit-dictate for release: configure, build, test, stage, and package.

.DESCRIPTION
    release-plan.md §3. Produces two artefacts in dist\:

        kvit-dictate-<version>-setup.exe    the Inno Setup per-user installer
        kvit-dictate-<version>-win-x64.zip  the portable ZIP

    The ZIP costs nothing and is what a suspicious user tries first — which, for a program that
    hooks the keyboard, is a reasonable instinct rather than an inconvenience.

    **Nothing here signs anything.** release-plan.md §4 calls signing the largest risk in this
    release and no certificate exists yet; a script that quietly produced unsigned output while
    looking like it signed would be worse than one that says so. It says so, at the end.

    The staged payload is **two** executables, one helper folder and documents. No models.json,
    no Python, no CRT redistributable — the build is /MT (CMakeLists.txt), so there is nothing
    to redistribute. models.json is NOT staged: release-plan.md §2.2 makes it a bench file, and
    shipping it would put a "mock" model in front of a new user, which types invented sentences.

        kvit-dictate.exe   the client: hotkey, capture, session, injection, overlay
        kvit-asrd.exe      the translator: docs/protocol.md on one side, and on the other an
                           OpenAI-compatible /v1/audio/transcriptions service, or the model on
                           this PC through the helper below
        kvit-speech\       the on-device Windows AI speech helper (tools/kvit-speech), published
                           self-contained with its .NET and Windows App SDK runtimes

    ── The spawn check changed, and it is worth reading before "fixing" it ────────────────────

    This script used to assert that the shipped `kvit-dictate.exe` contained **no**
    `CreateProcessW` and no `AssignProcessToJobObject`, on release-plan.md §1.2's argument that
    attack surface a binary does not contain cannot be reached by a bug elsewhere in it.

    openai-translator.md §4.6 reverses that, deliberately and with a reason: the client now
    launches and supervises `kvit-asrd.exe`, because the alternative is the user keeping a
    Python window open. So those two imports are now **expected**, and asserting their absence
    would only stop the release.

    What replaced it is narrower and still has teeth:

      1. `KD_ENABLE_SIDECAR_SPAWN=OFF` is still passed to CMake. The named-pipe / models.json /
         arbitrary-interpreter path — a configuration file that can execute anything — stays
         compiled out. That is the surface §1.2 was actually about.
      2. `kvit-asrd.exe` is present in the stage directory, because a client that cannot find
         its translator is a broken install.
      3. The staged client does **not** contain the string `spawned sidecar pid`, which is the
         unique log line of the old path. Its presence would mean §1.2's spawn came back.

    The security property that remains is in `src/translator.h`: the image path is
    `GetModuleFileName(nullptr)`'s directory plus `kvit-asrd.exe`, and nothing else.

.PARAMETER SkipTests
    Package without running ctest. For iterating on the packaging itself only; the release build
    is never made this way.
#>
[CmdletBinding()]
param(
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
# Its own directory, not the developer preset's: the two differ in KD_ENABLE_SIDECAR_SPAWN and
# sharing a CMake cache between them is how a release ends up built with development options.
$buildDir = Join-Path $repo 'build-release'
$stageDir = Join-Path $repo 'build-stage'
$distDir = Join-Path $repo 'dist'

function Find-Tool([string]$name, [string[]]$candidates) {
    foreach ($c in $candidates) { if (Test-Path $c) { return $c } }
    $found = Get-Command $name -ErrorAction SilentlyContinue
    if ($found) { return $found.Source }
    return $null
}

$cmake = Find-Tool 'cmake.exe' @('C:\Program Files\CMake\bin\cmake.exe')
if (-not $cmake) { throw 'cmake.exe not found on PATH or in C:\Program Files\CMake\bin' }
$ctest = $cmake -replace 'cmake\.exe$', 'ctest.exe'

# ─── Version, from the one place that owns it ────────────────────────────────────────────────
#
# CMakeLists.txt's project() line. Read rather than passed, so a release cannot be built with a
# version the binary's own resource block disagrees with.
$cmakeLists = Get-Content (Join-Path $repo 'CMakeLists.txt') -Raw
if ($cmakeLists -notmatch 'project\(kvit-dictate\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
    throw 'could not read the version from CMakeLists.txt project()'
}
$version = $Matches[1]
Write-Host "[release] version   : $version"
Write-Host "[release] repo      : $repo"

# ─── Build ───────────────────────────────────────────────────────────────────────────────────

Write-Host "`n[release] === configure ==="
& $cmake -S $repo -B $buildDir -G 'Visual Studio 17 2022' -A x64 `
    -DKD_ENABLE_SIDECAR_SPAWN=OFF -DKD_BUILD_TOOLS=OFF -DKD_BUILD_TESTS=ON
if ($LASTEXITCODE -ne 0) { throw 'configure failed' }

Write-Host "`n[release] === build ==="
& $cmake --build $buildDir --config Release -- -m
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

if (-not $SkipTests) {
    Write-Host "`n[release] === ctest ==="
    & $ctest --test-dir $buildDir -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'tests failed; nothing is packaged from a red build' }
}

# ─── Stage ───────────────────────────────────────────────────────────────────────────────────

Write-Host "`n[release] === stage ==="
if (Test-Path $stageDir) { Remove-Item -Recurse -Force $stageDir }
New-Item -ItemType Directory -Force -Path $stageDir | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stageDir 'docs') | Out-Null

$exe = Join-Path $buildDir 'bin\Release\kvit-dictate.exe'
if (-not (Test-Path $exe)) { throw "built executable not found at $exe" }
Copy-Item $exe $stageDir

# Both halves, same build, same version. openai-translator.md §4.8: the Inno script already
# copies {#StageDir}\* with recursesubdirs, so staging is the whole change on the packaging side.
$asrd = Join-Path $buildDir 'bin\Release\kvit-asrd.exe'
if (-not (Test-Path $asrd)) {
    throw "kvit-asrd.exe not found at $asrd — the translator is half the application and an installer without it produces a client that cannot dictate"
}
Copy-Item $asrd $stageDir

# ─── The on-device speech helper (src/asrd/backend_windows.h) ───────────────────────────────
#
# What lets "This PC" in Settings > Server dictate with no server at all: kvit-asrd launches
# kvit-speech\kvit-speech.exe, which reaches the Windows AI speech model through the Windows App
# SDK. It is C#, so it is built here with the .NET SDK rather than by CMake.
#
# **Published self-contained, .NET runtime included.** The csproj leaves the .NET runtime to the
# machine, which is right for a developer and wrong for an installer: a clean Windows has no .NET
# 9, and a helper that fails to start on one is exactly the failure a clean-VM test exists to
# find. The Windows App SDK runtime is carried beside it either way (WindowsAppSDKSelfContained,
# because the API is only in the experimental channel; tools/kvit-speech/README.md). That makes
# this folder most of the installer's size, and it is the price of needing no server.
$dotnet = Find-Tool 'dotnet.exe' @((Join-Path $env:ProgramFiles 'dotnet\dotnet.exe'))
if (-not $dotnet) {
    throw 'dotnet.exe (the .NET 9 SDK) not found; it builds tools\kvit-speech, without which "This PC" in Settings cannot dictate'
}
$speechOut = Join-Path $buildDir 'kvit-speech'
if (Test-Path $speechOut) { Remove-Item -Recurse -Force $speechOut }
Write-Host "`n[release] === kvit-speech (dotnet publish, self-contained) ==="
& $dotnet publish (Join-Path $repo 'tools\kvit-speech\KvitSpeech.csproj') -c Release -r win-x64 `
    --self-contained true -p:SelfContained=true -o $speechOut --nologo
if ($LASTEXITCODE -ne 0) { throw 'dotnet publish of tools\kvit-speech failed' }
if (-not (Test-Path (Join-Path $speechOut 'kvit-speech.exe'))) {
    throw "kvit-speech.exe not found in $speechOut after publish"
}
# The .NET runtime really is inside: without it the helper would ask the machine for one.
if (-not (Test-Path (Join-Path $speechOut 'coreclr.dll'))) {
    throw 'the published kvit-speech has no coreclr.dll, so it is not self-contained and would need .NET 9 installed'
}
# Debug symbols are not shipped; they are a third of the folder and help nobody who installs it.
Copy-Item -Recurse $speechOut (Join-Path $stageDir 'kvit-speech')
Get-ChildItem -Recurse -File -Filter '*.pdb' (Join-Path $stageDir 'kvit-speech') | Remove-Item -Force

# The whole payload, and the list is short on purpose (release-plan.md §3.2).
foreach ($f in @('LICENSE', 'LICENSE-APACHE', 'README.md')) {
    Copy-Item (Join-Path $repo $f) $stageDir
}
foreach ($f in @('setup.md', 'protocol.md', 'supported-targets.md')) {
    $src = Join-Path $repo "docs\$f"
    if (Test-Path $src) { Copy-Item $src (Join-Path $stageDir 'docs') }
}
Copy-Item (Join-Path $PSScriptRoot 'disclosure.txt') (Join-Path $stageDir 'docs\what-this-does.txt')

# A sanity check with teeth: the static CRT is what removes the redistributable step from the
# installer, and a build that silently reverted to /MD would produce an installer that works on
# this machine and fails on a clean one — the exact failure a clean-VM test is meant to catch and
# the exact one that is easiest to skip.
# Both binaries: a /MD kvit-asrd.exe would fail on a clean machine exactly as a /MD client would,
# and it is the one nobody would think to test.
foreach ($name in @('kvit-dictate.exe', 'kvit-asrd.exe')) {
    $bytes = [System.IO.File]::ReadAllBytes((Join-Path $stageDir $name))
    $text = [System.Text.Encoding]::ASCII.GetString($bytes)
    foreach ($dll in @('VCRUNTIME140', 'MSVCP140', 'api-ms-win-crt-runtime')) {
        if ($text.Contains($dll)) {
            throw "$name imports $dll — the build is not /MT and the installer would need a redistributable"
        }
    }
}
Write-Host "[release] static CRT confirmed in both binaries (no VCRUNTIME/MSVCP/UCRT imports)"

# The replacement for the old CreateProcessW grep. See the .DESCRIPTION block above for why the
# import-table check had to go and what this checks instead.
#
# `spawned sidecar pid` is `ipc_client.cpp`'s log line for the named-pipe interpreter spawn, and
# it exists in the binary only when KD_ENABLE_SIDECAR_SPAWN was on. Unlike an import, a string
# literal is compiled out with the code that uses it, so its absence is a real statement about
# what this build can do — and it is checkable without a PE parser.
$clientBytes = [System.IO.File]::ReadAllBytes((Join-Path $stageDir 'kvit-dictate.exe'))
$clientText = [System.Text.Encoding]::ASCII.GetString($clientBytes)
if ($clientText.Contains('spawned sidecar pid')) {
    throw "kvit-dictate.exe contains the old sidecar spawn path — KD_ENABLE_SIDECAR_SPAWN was not off, and this build can launch an interpreter named in models.json"
}
Write-Host "[release] the models.json interpreter spawn is compiled out (no 'spawned sidecar pid')"

# And the translator is actually there. A client that cannot find kvit-asrd.exe reports a broken
# installation rather than dictating, which is correct behaviour and a terrible release.
if (-not (Test-Path (Join-Path $stageDir 'kvit-asrd.exe'))) {
    throw 'kvit-asrd.exe is missing from the stage directory'
}
# Where the translator looks for it, and nowhere else (default_speech_helper_path).
if (-not (Test-Path (Join-Path $stageDir 'kvit-speech\kvit-speech.exe'))) {
    throw 'kvit-speech\kvit-speech.exe is missing from the stage directory; "This PC" could not dictate'
}
Write-Host "[release] both halves staged (kvit-dictate.exe + kvit-asrd.exe), and the on-device helper (kvit-speech\)"

$staged = Get-ChildItem -Recurse -File $stageDir
Write-Host "[release] staged $($staged.Count) files, $([math]::Round(($staged | Measure-Object Length -Sum).Sum / 1MB, 2)) MB"

# ─── Package ─────────────────────────────────────────────────────────────────────────────────

New-Item -ItemType Directory -Force -Path $distDir | Out-Null

$zip = Join-Path $distDir "kvit-dictate-$version-win-x64.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path (Join-Path $stageDir '*') -DestinationPath $zip
Write-Host "[release] wrote $zip"

# All three locations Inno's own installer offers, in the order it offers them. The third is the
# per-user one — the same %LOCALAPPDATA%\Programs this project's own installer uses — and leaving
# it out is how a machine that has Inno Setup gets told it does not.
$iscc = Find-Tool 'iscc.exe' @(
    (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
    (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe'),
    (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
)
if ($iscc) {
    Write-Host "`n[release] === installer ==="
    & $iscc "/DKvitVersion=$version" "/DStageDir=$stageDir" "/DOutputDir=$distDir" `
        (Join-Path $PSScriptRoot 'kvit-dictate.iss')
    if ($LASTEXITCODE -ne 0) { throw 'iscc failed' }
    Write-Host "[release] wrote $distDir\kvit-dictate-$version-setup.exe"
} else {
    Write-Warning @'
Inno Setup 6 was not found, so only the portable ZIP was produced.
Install it from https://jrsoftware.org/isdl.php and run this again for the installer.
'@
}

Write-Host @"

[release] Done. Two things this script did NOT do, both of them deliberately:

  * Nothing is signed. release-plan.md §4: an unsigned binary that hooks the keyboard and
    synthesises input earns a SmartScreen warning at best and a silent quarantine at worst.
    That risk is carried, not solved, and no certificate exists yet.

  * Nothing was tested on a clean machine. release-plan.md §3's gate is a clean-VM install,
    with Defender and SmartScreen at their defaults, downloading through a browser so that the
    Mark of the Web is intact. The development machine has exclusions that hide exactly the
    failure that test exists to find.
"@
