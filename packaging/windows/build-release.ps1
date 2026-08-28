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

    The staged payload is deliberately one executable plus documents. No sidecar, no models.json,
    no Python, no CRT redistributable — the build is /MT (CMakeLists.txt), so there is nothing to
    redistribute. models.json is NOT staged: release-plan.md §2.2 makes it a bench file, and
    shipping it would put a "mock" model in front of a new user, which types invented sentences.

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
$buildDir = Join-Path $repo 'build-windows-msvc-release'
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
& $cmake --preset windows-msvc-release
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
$strings = [System.IO.File]::ReadAllBytes((Join-Path $stageDir 'kvit-dictate.exe'))
$ascii = [System.Text.Encoding]::ASCII.GetString($strings)
foreach ($dll in @('VCRUNTIME140', 'MSVCP140', 'api-ms-win-crt-runtime')) {
    if ($ascii.Contains($dll)) {
        throw "the executable imports $dll — the build is not /MT and the installer would need a redistributable"
    }
}
Write-Host "[release] static CRT confirmed (no VCRUNTIME/MSVCP/UCRT imports)"

$staged = Get-ChildItem -Recurse -File $stageDir
Write-Host "[release] staged $($staged.Count) files, $([math]::Round(($staged | Measure-Object Length -Sum).Sum / 1MB, 2)) MB"

# ─── Package ─────────────────────────────────────────────────────────────────────────────────

New-Item -ItemType Directory -Force -Path $distDir | Out-Null

$zip = Join-Path $distDir "kvit-dictate-$version-win-x64.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path (Join-Path $stageDir '*') -DestinationPath $zip
Write-Host "[release] wrote $zip"

$iscc = Find-Tool 'iscc.exe' @(
    'C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
    'C:\Program Files\Inno Setup 6\ISCC.exe'
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
