# Build, verify and package a StockTool release.
#
#   .\release.ps1                     verify the current version, build, test, package
#   .\release.ps1 -Version 0.8.0      bump the three version-bearing files first
#   .\release.ps1 -Publish -NotesFile notes.md
#                                     ... then commit, tag, push and create the GitHub release
#   .\release.ps1 -AllowRunning       carry on even if a StockTool instance holds the exe
#   .\release.ps1 -NoInstaller        skip Inno Setup (zip only)
#
# Every gate here exists because something once went out wrong: a README whose
# em dashes had been double-encoded by a PowerShell round-trip, a version left
# behind in one of the three files, an installer linked against the VC runtime,
# and a build that silently reused a stale exe because a running instance held
# the linker out.
#
# Windows PowerShell 5.1 compatible: no &&, no ternary, no null-coalescing.

[CmdletBinding()]
param(
    [string]$Version,
    [switch]$Publish,
    [string]$NotesFile,
    [switch]$AllowRunning,
    [switch]$NoInstaller
)

$ErrorActionPreference = 'Stop'
Set-Location -Path $PSScriptRoot

# Native tools log to stderr even on success, which would otherwise decide this
# script's exit code. Every step is judged on its exit code instead: `exit 0` at
# the bottom, and this trap for anything that throws.
trap {
    Write-Host ''
    Write-Host "RELEASE FAILED: $_" -ForegroundColor Red
    exit 1
}

$repo        = 'Flinterpop/StockTool'
$versionFiles = @('CMakeLists.txt', 'README.md', 'installer\StockTool.iss')
# Exactly what may be shipped. Anything else in an artifact is a bug.
$zipContents = @('README.md', 'StockTool.exe', 'stocktool.cfg')
# Config sections that must ship empty: they hold personal financial data.
$privateSections = @('holdings', 'transactions', 'alerts', 'cash')

function Write-Step($text) {
    Write-Host ''
    Write-Host "==> $text" -ForegroundColor Cyan
}

function Invoke-Native {
    <# Run a native executable and return its exit code, letting its stderr through. #>
    param(
        [Parameter(Mandatory)][string]$Exe,
        [string[]]$Arguments = @(),
        [switch]$Quiet
    )
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        if ($Quiet) { & $Exe @Arguments | Out-Null } else { & $Exe @Arguments | Out-Host }
        return $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }
}

function Assert-Native {
    <# Run a native command and throw unless it exits 0. #>
    param(
        [Parameter(Mandatory)][string]$What,
        [Parameter(Mandatory)][string]$Exe,
        [string[]]$Arguments = @(),
        [switch]$Quiet
    )
    $code = Invoke-Native -Exe $Exe -Arguments $Arguments -Quiet:$Quiet
    if ($code -ne 0) { throw "$What failed with exit code $code" }
}

function Read-Utf8($path) {
    <#
    Read a text file as UTF-8 without letting PowerShell guess the encoding.
    Get-Content decodes as the ANSI code page on 5.1, and writing that back is
    exactly how a README's em dashes turn into mojibake.
    #>
    return [IO.File]::ReadAllText((Resolve-Path $path), [Text.UTF8Encoding]::new($false))
}

function Write-Utf8($path, $text) {
    [IO.File]::WriteAllText((Resolve-Path $path), $text, [Text.UTF8Encoding]::new($false))
}

function Get-ProjectVersion {
    $text = Read-Utf8 'CMakeLists.txt'
    if ($text -notmatch 'project\(StockTool VERSION (\d+\.\d+\.\d+)') {
        throw 'CMakeLists.txt has no project(StockTool VERSION x.y.z) line'
    }
    return $Matches[1]
}

function Find-Tool($name, $candidates) {
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path $candidate)) { return (Resolve-Path $candidate).Path }
    }
    $onPath = Get-Command $name -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    return $null
}

# --- Preflight --------------------------------------------------------------
Write-Step 'Preflight'
Assert-Native -What 'git rev-parse' -Exe 'git' -Arguments @('rev-parse', '--is-inside-work-tree') -Quiet
$branch = (& git rev-parse --abbrev-ref HEAD).Trim()
Write-Host "  branch: $branch"
if ($branch -ne 'main') { Write-Host '  WARNING: not on main' -ForegroundColor Yellow }

if (-not $AllowRunning) {
    $running = Get-Process -Name 'StockTool' -ErrorAction SilentlyContinue
    if ($running) {
        throw ("StockTool is running (PID $($running.Id -join ', ')) and holds build\Release\StockTool.exe, " +
               'so the linker would fail. Close it, or pass -AllowRunning to reuse the existing exe.')
    }
}

if ($Publish) {
    if (-not $NotesFile) { throw '-Publish needs -NotesFile pointing at the release notes' }
    if (-not (Test-Path $NotesFile)) { throw "notes file not found: $NotesFile" }
    if (-not (Find-Tool 'gh' @())) { throw '-Publish needs the GitHub CLI (gh) on PATH' }
}

# --- Version ----------------------------------------------------------------
$current = Get-ProjectVersion
if ($Version) {
    if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "version must look like x.y.z, got '$Version'" }
    if ($Version -eq $current) {
        Write-Step "Version already $Version"
    } else {
        Write-Step "Bumping $current -> $Version"
        foreach ($file in $versionFiles) {
            $text = Read-Utf8 $file
            $hits = ([regex]::Matches($text, [regex]::Escape($current))).Count
            if ($hits -eq 0) { throw "$file does not mention $current" }
            Write-Utf8 $file $text.Replace($current, $Version)
            Write-Host "  $file : $hits occurrence(s)"
        }
        $current = Get-ProjectVersion
        if ($current -ne $Version) { throw 'version bump did not take' }
    }
}
Write-Step "Releasing $current"

# Version lockstep. Every x.y.z in a version-bearing file must be this release
# (bar the installer's own fallback default), and no tracked file anywhere may
# name a StockTool build of another version. The first rule is what catches a
# forgotten README badge; the second catches stray references elsewhere.
$tracked = & git ls-files
$ignoredVersions = @('0.0.0')       # installer fallback: #define AppVersion "0.0.0"
$stale = @()
foreach ($file in $versionFiles) {
    $text = Read-Utf8 $file
    foreach ($match in [regex]::Matches($text, '\d+\.\d+\.\d+')) {
        $found = $match.Value
        if ($found -ne $current -and $ignoredVersions -notcontains $found) { $stale += "$file names $found" }
    }
}
foreach ($file in $tracked) {
    $bytes = [IO.File]::ReadAllBytes((Join-Path $PSScriptRoot $file))
    $text  = [Text.Encoding]::UTF8.GetString($bytes)
    foreach ($match in [regex]::Matches($text, 'StockTool[- ](\d+\.\d+\.\d+)')) {
        $found = $match.Groups[1].Value
        if ($found -ne $current) { $stale += "$file names StockTool $found" }
    }
}
$stale = $stale | Sort-Object -Unique
if ($stale.Count -gt 0) { throw "version lockstep broken: $($stale -join '; ')" }
Write-Host "  version lockstep ok across $($versionFiles.Count) files, and no stray references in $($tracked.Count) tracked files"

# --- Encoding gate ----------------------------------------------------------
Write-Step 'Checking markdown encoding'
$mojibake = [byte[]](0xC3, 0xA2, 0xE2, 0x82, 0xAC)   # UTF-8 bytes re-encoded as UTF-8
foreach ($file in ($tracked | Where-Object { $_ -like '*.md' })) {
    $bytes = [IO.File]::ReadAllBytes((Join-Path $PSScriptRoot $file))
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        throw "$file starts with a UTF-8 BOM"
    }
    try {
        $strict = [Text.UTF8Encoding]::new($false, $true)
        [void]$strict.GetString($bytes)
    } catch {
        throw "$file is not valid UTF-8"
    }
    $text = [Text.Encoding]::UTF8.GetString($bytes)
    if ($text.Contains([char]0x00E2 + [string][char]0x20AC)) { throw "$file contains double-encoded text (mojibake)" }
    Write-Host "  ok: $file"
}

# --- Build ------------------------------------------------------------------
Write-Step 'Configuring'
Assert-Native -What 'cmake --preset default' -Exe 'cmake' -Arguments @('--preset', 'default') -Quiet

Write-Step 'Building Release'
Assert-Native -What 'cmake --build' -Exe 'cmake' -Arguments @('--build', '--preset', 'release') -Quiet

Write-Step 'Running tests'
Assert-Native -What 'stocktool_tests' -Exe '.\build\Release\stocktool_tests.exe'

# --- Binary checks ----------------------------------------------------------
Write-Step 'Checking the executable'
$exe = '.\build\Release\StockTool.exe'
if (-not (Test-Path $exe)) { throw "not built: $exe" }
$fileVersion = (Get-Item $exe).VersionInfo.FileVersion
Write-Host "  VERSIONINFO: $fileVersion"
if ($fileVersion -ne "$current.0") { throw "exe reports $fileVersion, expected $current.0" }

$msvcRoot = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC'
$dumpbin = $null
if (Test-Path $msvcRoot) {
    $newest = Get-ChildItem $msvcRoot | Sort-Object Name -Descending | Select-Object -First 1
    $dumpbin = Find-Tool 'dumpbin' @((Join-Path $newest.FullName 'bin\Hostx64\x64\dumpbin.exe'))
}
if ($dumpbin) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $dependents = & $dumpbin /dependents $exe
    $ErrorActionPreference = $previous
    $crt = $dependents | Select-String -Pattern 'VCRUNTIME|MSVCP|api-ms-win-crt'
    if ($crt) { throw "exe depends on the VC runtime: $($crt -join ', ')" }
    Write-Host '  no VC runtime dependency (static CRT)'
} else {
    Write-Host '  WARNING: dumpbin not found, skipped the CRT check' -ForegroundColor Yellow
}

# --- Package ----------------------------------------------------------------
Write-Step 'Packaging'
$dist = Join-Path $PSScriptRoot 'dist'
$stage = Join-Path $dist "StockTool-v$current-win64"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item $exe, 'stocktool.cfg', 'README.md' -Destination $stage
$zip = Join-Path $dist "StockTool-v$current-win64.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Write-Host "  wrote $zip"

$setup = $null
if (-not $NoInstaller) {
    # Inno Setup moves around on this machine, so try the known homes in turn.
    $isccCandidates = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
                        'C:\bin\InnoSetup6\ISCC.exe',
                        'C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
                        'C:\Program Files\Inno Setup 6\ISCC.exe')
    $iscc = Find-Tool 'ISCC' $isccCandidates
    if (-not $iscc) { throw "ISCC.exe not found. Tried: $($isccCandidates -join '; ')" }
    Write-Host "  using $iscc"
    Assert-Native -What 'ISCC' -Exe $iscc -Arguments @("/DAppVersion=$current", 'installer\StockTool.iss') -Quiet
    $setup = ".\installer\Output\StockTool-$current-setup.exe"
    if (-not (Test-Path $setup)) { throw "installer not produced: $setup" }
    Write-Host "  wrote $setup"
}

# --- What is actually inside the artifacts ----------------------------------
# Everything under C:\source_games is export-controlled, and this repo is
# public: enumerate, never assume.
Write-Step 'Artifact contents'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($zip)
try {
    $entries = $archive.Entries | ForEach-Object { $_.FullName }
    foreach ($entry in ($entries | Sort-Object)) { Write-Host "  zip: $entry" }
    $unexpected = $entries | Where-Object { $zipContents -notcontains $_ }
    if ($unexpected) { throw "unexpected file(s) in the zip: $($unexpected -join ', ')" }
    $missing = $zipContents | Where-Object { $entries -notcontains $_ }
    if ($missing) { throw "missing from the zip: $($missing -join ', ')" }

    $readme = $archive.GetEntry('README.md')
    $stream = $readme.Open()
    $buffer = New-Object byte[] $readme.Length
    [void]$stream.Read($buffer, 0, $readme.Length)
    $stream.Close()
    if ([Text.Encoding]::UTF8.GetString($buffer).Contains([char]0x00E2 + [string][char]0x20AC)) {
        throw 'the packaged README is double-encoded'
    }
    Write-Host '  packaged README is clean UTF-8'
} finally {
    $archive.Dispose()
}

# The shipped config must be the template, not anyone's portfolio.
$shipped = Read-Utf8 (Join-Path $stage 'stocktool.cfg')
$section = ''
foreach ($line in ($shipped -split "`r?`n")) {
    $trimmed = $line.Trim()
    if ($trimmed -match '^\[(.+)\]$') { $section = $Matches[1].ToLower(); continue }
    if ($trimmed -eq '' -or $trimmed.StartsWith(';')) { continue }
    if ($privateSections -contains $section) {
        throw "the shipped stocktool.cfg carries personal data under [$section]: $trimmed"
    }
}
Write-Host "  shipped stocktool.cfg has no data under [$($privateSections -join '], [')]"

foreach ($artifact in @($zip, $setup)) {
    if ($artifact) {
        $hash = (Get-FileHash $artifact -Algorithm SHA256).Hash.Substring(0, 16)
        $mb = [math]::Round((Get-Item $artifact).Length / 1MB, 2)
        Write-Host "  $([IO.Path]::GetFileName($artifact))  $mb MB  sha256:$hash..."
    }
}

Foreach ($file in $versionFiles) {
    $changed = & git status --porcelain -- $file
    if ($changed) { Write-Host "  note: $file is modified (the version bump is not committed yet)" -ForegroundColor Yellow }
}

# --- Publish ----------------------------------------------------------------
if ($Publish) {
    Write-Step "Publishing v$current"
    $existing = & git tag --list "v$current"
    if ($existing) { throw "tag v$current already exists" }

    $dirty = & git status --porcelain
    if ($dirty) {
        & git add -A
        & git commit -m "v$current"
        if ($LASTEXITCODE -ne 0) { throw 'commit failed' }
        Write-Host '  committed the pending changes'
    }
    Assert-Native -What 'git push' -Exe 'git' -Arguments @('push', 'origin', $branch)
    Assert-Native -What 'git tag' -Exe 'git' -Arguments @('tag', '-a', "v$current", '-m', "v$current")
    Assert-Native -What 'git push tag' -Exe 'git' -Arguments @('push', 'origin', "v$current")
    $ghArgs = @('release', 'create', "v$current", $zip)
    if ($setup) { $ghArgs += $setup }
    $ghArgs += @('--repo', $repo, '--title', "v$current", '--notes-file', $NotesFile)
    Assert-Native -What 'gh release create' -Exe 'gh' -Arguments $ghArgs
} else {
    Write-Step 'Not published'
    Write-Host '  re-run with -Publish -NotesFile <file> to commit, tag, push and create the release'
}

Write-Step 'Done'
Write-Host "  version: $current"
Write-Host "  zip:     $zip"
if ($setup) { Write-Host "  setup:   $setup" }

# Explicit: leftover stderr ErrorRecords from the native tools must not decide
# the exit code. Reaching here means every checked step returned success.
exit 0
