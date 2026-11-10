# Installs Calx on Windows:
#
#     irm https://calxlang.com/install.ps1 | iex
#
# It downloads the newest release from github.com/ellipog/calx, checks its SHA-256, unpacks it
# into %USERPROFILE%\.calx, and puts .calx\bin on your PATH. No administrator rights are needed.
# Run it again to reinstall; `calx update` keeps an installation up to date.
#
# CALX_INSTALL installs somewhere else, CALX_CHANNEL=nightly picks the nightly releases,
# CALX_NO_MODIFY_PATH=1 leaves PATH alone, and CALX_RELEASES points at a mirror.

$ErrorActionPreference = 'Stop'
# Invoke-WebRequest is many times slower while it draws its progress bar.
$ProgressPreference = 'SilentlyContinue'
# Windows PowerShell 5.1 may not offer TLS 1.2 by itself, which GitHub requires.
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

function Stop-Install($message) {
    Write-Host "calx install: $message" -ForegroundColor Red
    # `irm | iex` runs this in the caller's session, where `exit` would close their window.
    throw "calx install: $message"
}

function Get-Bytes($url) {
    try {
        $r = Invoke-WebRequest -UseBasicParsing -Uri $url -Headers @{ 'User-Agent' = 'calx-install' }
    } catch {
        $status = $null
        if ($_.Exception.Response) { $status = [int]$_.Exception.Response.StatusCode }
        if ($status) { throw "HTTP $status from $url" }
        throw "can't reach ${url}: $($_.Exception.Message)"
    }
    # GitHub serves release files as octet-stream, which gives bytes; text types give a string.
    if ($r.Content -is [string]) { return [Text.Encoding]::UTF8.GetBytes($r.Content) }
    return [byte[]]$r.Content
}

function Get-Text($url) {
    return [Text.Encoding]::UTF8.GetString((Get-Bytes $url)).Trim()
}

if (-not [Environment]::Is64BitOperatingSystem) {
    Stop-Install 'Calx releases are for 64-bit Windows and Linux.'
}

$releases = if ($env:CALX_RELEASES) { $env:CALX_RELEASES.TrimEnd('/') } else { 'https://github.com/ellipog/calx/releases' }
$root = if ($env:CALX_INSTALL) { $env:CALX_INSTALL } else { Join-Path $env:USERPROFILE '.calx' }

# The newest version: the latest stable release, or the nightly when asked for, or while there's
# no stable release yet.
$version = $null
if ($env:CALX_CHANNEL -ne 'nightly') {
    try {
        $version = Get-Text "$releases/latest/download/version.txt"
    } catch {
        if ("$_" -notmatch 'HTTP 404') { Stop-Install "$_" }
        Write-Host "There's no stable release yet, so this installs the newest nightly."
    }
}
if (-not $version) {
    try { $version = Get-Text "$releases/download/nightly/version.txt" } catch { Stop-Install "$_" }
}
if ($version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+') { Stop-Install "the release's version.txt doesn't hold a version" }
$tag = if ($version -match '-nightly\.') { 'nightly' } else { "v$version" }
$file = "calx-$version-windows-x64.zip"
$url = "$releases/download/$tag/$file"

$temp = Join-Path ([IO.Path]::GetTempPath()) ("calx-install-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
try {
    Write-Host "Downloading Calx $version..."
    $zip = Join-Path $temp $file
    try {
        [IO.File]::WriteAllBytes($zip, (Get-Bytes $url))
        $want = ((Get-Text "$url.sha256") -split '\s+')[0].ToLowerInvariant()
    } catch { Stop-Install "$_" }
    $got = (Get-FileHash -Algorithm SHA256 -Path $zip).Hash.ToLowerInvariant()
    if ($got -ne $want) {
        Stop-Install "the download's SHA-256 is $got, but the release says $want. Nothing was installed; try again in a minute."
    }
    $mb = [math]::Round((Get-Item $zip).Length / 1MB, 1)
    Write-Host "Checked its SHA-256 ($mb MB)."

    $unpacked = Join-Path $temp 'unpacked'
    # .NET's own unzip: Expand-Archive takes minutes over the toolchain's thousands of files.
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($zip, $unpacked)
    $new = Join-Path $unpacked 'calx'
    if (-not (Test-Path (Join-Path $new 'bin\calx.exe'))) { Stop-Install 'the download has no bin\calx.exe' }
    Get-ChildItem -Recurse -File $new | Unblock-File

    # What's there from an earlier install moves aside first, and the release moves in.
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    $aside = Join-Path $root (".update\replaced-" + (Get-Date -Format 'yyyyMMddHHmmss'))
    foreach ($part in Get-ChildItem $new) {
        $target = Join-Path $root $part.Name
        if (Test-Path $target) {
            New-Item -ItemType Directory -Force -Path $aside | Out-Null
            try { Move-Item -Path $target -Destination (Join-Path $aside $part.Name) }
            catch { Stop-Install "can't move the old $target aside; is calx running? $($_.Exception.Message)" }
        }
        Move-Item -Path $part.FullName -Destination $target
    }
    if (Test-Path $aside) { Remove-Item -Recurse -Force $aside -ErrorAction SilentlyContinue }
} finally {
    Remove-Item -Recurse -Force $temp -ErrorAction SilentlyContinue
}

$bin = Join-Path $root 'bin'
if ($env:CALX_NO_MODIFY_PATH -ne '1') {
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $parts = @(); if ($userPath) { $parts = $userPath -split ';' | Where-Object { $_ } }
    if ($parts -notcontains $bin) {
        [Environment]::SetEnvironmentVariable('Path', (($parts + $bin) -join ';'), 'User')
        Write-Host "Added $bin to your PATH."
    }
    if (($env:Path -split ';') -notcontains $bin) { $env:Path = "$env:Path;$bin" }
}

& (Join-Path $bin 'calx.exe') version
Write-Host ""
Write-Host "Calx is installed in $root." -ForegroundColor Green

# Calx builds programs with clang. A release carries its own, with the Windows libraries it links
# with, in toolchain\ (CLX-110), so nothing else needs installing; one from before that needs one.
$clang = if ($env:CALX_CC) { $env:CALX_CC } else { (Get-Command clang -ErrorAction SilentlyContinue).Source }
if (-not $clang -and -not (Test-Path (Join-Path $root 'toolchain\bin\clang.exe'))) {
    Write-Host ""
    Write-Host "Calx builds programs with clang, which isn't on your PATH yet. Install LLVM:"
    Write-Host "    winget install LLVM.LLVM"
    Write-Host "and Visual Studio's Build Tools, with the 'Desktop development with C++' workload, for the"
    Write-Host "Windows libraries clang links with: https://visualstudio.microsoft.com/downloads/"
}
Write-Host ""
Write-Host "Try it in a new terminal:  calx run hello.calx"
Write-Host "Keep it up to date with:   calx update"
