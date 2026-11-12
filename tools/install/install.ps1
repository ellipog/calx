# Installs Calx on Windows:
#
#     irm https://calxlang.com/install.ps1 | iex
#
# It downloads the newest release from github.com/ellipog/calx, checks its SHA-256, unpacks it
# into %USERPROFILE%\.calx, and puts .calx\bin on your PATH; then calx-update fetches the
# toolchain it builds programs with, clang, lld and mingw-w64, unless that's installed already.
# No administrator rights are needed. Run it again to reinstall; `calx update` keeps an
# installation up to date.
#
# CALX_INSTALL installs somewhere else, CALX_CHANNEL=nightly picks the nightly releases,
# CALX_NO_MODIFY_PATH=1 leaves PATH alone, and CALX_RELEASES points at a mirror.

$ErrorActionPreference = 'Stop'
# Invoke-WebRequest is many times slower while it draws its progress bar.
$ProgressPreference = 'SilentlyContinue'
# Windows PowerShell 5.1 may not offer TLS 1.2 by itself, which GitHub requires.
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

# How it looks (CLX-250), as calx does: in a terminal someone's watching, colour, a check mark
# before each step that's done, and a progress bar for each download. In a pipe, a file, CI or
# with NO_COLOR set, the plain lines it always printed. CLICOLOR_FORCE=1 or FORCE_COLOR turns the
# terminal's look on anyway, for a recording. The marks are written as code points, since
# `irm | iex` in Windows PowerShell can read this file as Latin-1.
$forced = ($env:CLICOLOR_FORCE -eq '1') -or ($env:FORCE_COLOR -and $env:FORCE_COLOR -ne '0')
$styled = $forced -or (-not [Console]::IsOutputRedirected -and $Host.UI.SupportsVirtualTerminal -and -not $env:NO_COLOR -and
    $env:TERM -ne 'dumb' -and -not $env:CI)
# A recording reads this through a pipe, which Windows PowerShell writes in the console's code
# page unless told otherwise; a console takes the marks as they are.
if ($styled -and [Console]::IsOutputRedirected) {
    try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false } catch { }
}
$esc = [char]27
$tick = [string][char]0x2713
$cross = [string][char]0x2717
$invariant = [Globalization.CultureInfo]::InvariantCulture

function Paint($sgr, $text) {
    if ($styled) { return "$esc[${sgr}m$text$esc[0m" }
    return $text
}
# Calx's amber, the brand's (`--calx` on calxlang.com), as calx's.
$amber = '38;2;188;133;54'
function Accent($text) { return Paint $amber $text }
function Dim($text) { return Paint '90' $text }
function Write-Done($text) { Write-Host "$(Paint '32' $tick) $text" }

# The dither: a quarter, a half, three quarters and all of a cell in ink.
$shade = @(' ', [string][char]0x2591, [string][char]0x2592, [string][char]0x2593, [string][char]0x2588)

# A result: `text` on a chip of amber that dithers in and out, as calx's.
function Format-Band($text) {
    $fade = $shade[1] + $shade[2] + $shade[3]
    $back = $shade[3] + $shade[2] + $shade[1]
    return "$esc[${amber}m$fade$esc[0m$esc[48;2;188;133;54;38;2;24;24;27;1m $text $esc[0m$esc[${amber}m$back$esc[0m"
}

# The Calx mark, as calx draws it: how much of each cell the C and its x cover, in quarters.
$mark = @('0002344443210000013442', '0244443334443101344431', '2444200001343234443100', '3443000000013444320000',
    '2444200001344432243100', '0244443334443101344431', '0002344443210000013442')
# Ordered dithering's thresholds, in 16ths: a 4 by 4 Bayer matrix, row by row.
$bayer = @(0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5)

# The mark with `lines` beside it. In a live terminal it dissolves in: each cell fades up to its
# shade in its turn, in the order the Bayer matrix gives, over four tenths of a second.
function Show-Mark($lines) {
    $frames = if ($forced -or -not [Console]::IsOutputRedirected) { 10 } else { 1 }
    for ($f = 1; $f -le $frames; $f++) {
        $t = $f / $frames
        $rows = for ($r = 0; $r -lt 7; $r++) {
            $cells = ''
            for ($c = 0; $c -lt 22; $c++) {
                $level = [int]::Parse($mark[$r][$c])
                $start = 0.6 * $bayer[($r % 4) * 4 + ($c % 4)] / 16
                $shown = [math]::Round($level * [math]::Min(1.0, [math]::Max(0.0, ($t - $start) / 0.4)))
                $cells += $shade[[int]$shown]
            }
            $text = if ($r -lt $lines.Count -and $lines[$r]) { "   $($lines[$r])" } else { '' }
            "$esc[${amber}m$cells$esc[0m$text"
        }
        if ($f -gt 1) { Write-Host -NoNewline "$esc[7A" }
        Write-Host -NoNewline (($rows | ForEach-Object { "`r$esc[2K$_" }) -join "`n")
        Write-Host ''
        if ($f -lt $frames) { Start-Sleep -Milliseconds 40 }
    }
}

function Stop-Install($message) {
    if ($styled) {
        Write-Host "$(Paint '31' $cross) calx install: $message"
    } else {
        Write-Host "calx install: $message" -ForegroundColor Red
    }
    # `irm | iex` runs this in the caller's session, where `exit` would close their window.
    throw "calx install: $message"
}

# Megabytes with one decimal, written the same in every culture.
function Format-MB($bytes) { return [string]::Format($invariant, '{0:0.0}', $bytes / 1MB) }

# `label`, a bar of how much of `total` has come, and the speed, over the line before. The bar is
# calx's: full cells, then an edge that fades through the shades over four cells, breathing from
# frame to frame, on a grey track.
function Show-Progress($label, $got, $total, $seconds, $frame) {
    $rate = if ($seconds -gt 0) { Dim "$(Format-MB ($got / $seconds)) MB/s" } else { '' }
    $width = 24
    $fraction = [math]::Min(1.0, $got / $total)
    $head = -1.5 + $fraction * ($width + 3)
    if ($fraction -gt 0 -and $fraction -lt 1) { $head += 0.6 * [math]::Sin($frame * 0.7) }
    $cells = ''
    $lit = $null
    for ($i = 0; $i -lt $width; $i++) {
        $v = [math]::Min(1.0, [math]::Max(0.0, ($head - ($i + 0.5)) / 4.0 + 0.5))
        $q = [int][math]::Floor($v * 4 + 0.5)
        $want = $q -gt 0
        if ($want -ne $lit) {
            if ($null -ne $lit) { $cells += "$esc[0m" }
            $cells += if ($want) { "$esc[${amber}m" } else { "$esc[90m" }
            $lit = $want
        }
        $cells += if ($want) { $shade[$q] } else { $shade[1] }
    }
    Write-Host -NoNewline "`r$esc[2K  $label  $cells$esc[0m  $(Format-MB $got) of $(Format-MB $total) MB  $rate"
}

# Downloads `url` to the file `path`, drawing its progress in a terminal once a quarter of a second
# has passed, so a quick download draws nothing.
function Save-Download($url, $path, $label) {
    Add-Type -AssemblyName System.Net.Http
    $client = New-Object System.Net.Http.HttpClient
    $client.Timeout = [TimeSpan]::FromMinutes(15)
    [void]$client.DefaultRequestHeaders.UserAgent.TryParseAdd('calx-install')
    try {
        try {
            $resp = $client.GetAsync($url, [Net.Http.HttpCompletionOption]::ResponseHeadersRead).GetAwaiter().GetResult()
        } catch {
            $why = $_.Exception
            while ($why.InnerException) { $why = $why.InnerException }
            throw "can't reach ${url}: $($why.Message)"
        }
        if (-not $resp.IsSuccessStatusCode) { throw "HTTP $([int]$resp.StatusCode) from $url" }
        $total = $resp.Content.Headers.ContentLength
        $in = $resp.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
        $out = [IO.File]::Create($path)
        $drawn = $false
        try {
            $buf = New-Object byte[] 262144
            $got = 0L
            $clock = [Diagnostics.Stopwatch]::StartNew()
            $next = 250
            $frame = 0
            while (($n = $in.Read($buf, 0, $buf.Length)) -gt 0) {
                $out.Write($buf, 0, $n)
                $got += $n
                if ($styled -and $total -and $clock.ElapsedMilliseconds -ge $next) {
                    Show-Progress $label $got $total $clock.Elapsed.TotalSeconds $frame
                    $drawn = $true
                    $frame++
                    $next = $clock.ElapsedMilliseconds + 80
                }
            }
        } finally {
            $out.Close()
            $in.Close()
            if ($drawn) { Write-Host -NoNewline "`r$esc[2K" }
        }
    } finally {
        $client.Dispose()
    }
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
    Stop-Install 'Calx releases are for 64-bit Windows, on x64 or Arm64.'
}

$releases = if ($env:CALX_RELEASES) { $env:CALX_RELEASES.TrimEnd('/') } else { 'https://github.com/ellipog/calx/releases' }
$root = if ($env:CALX_INSTALL) { $env:CALX_INSTALL } else { Join-Path $env:USERPROFILE '.calx' }
if ($styled) {
    Write-Host ''
    Show-Mark @('', '', (Paint '1' 'Installing Calx'), (Dim 'the compiler, its standard library,'), (Dim 'and the toolchain it builds with'), '', '')
    Write-Host ''
}

# The newest version: the latest stable release, or the nightly when asked for, or while there's
# no stable release yet.
$version = $null
if ($env:CALX_CHANNEL -ne 'nightly') {
    try {
        $version = Get-Text "$releases/latest/download/version.txt"
    } catch {
        if ("$_" -notmatch 'HTTP 404') { Stop-Install "$_" }
        if ($styled) {
            Write-Host "$(Dim ([string][char]0xB7)) $(Dim "There's no stable release yet, so this installs the newest nightly.")"
        } else {
            Write-Host "There's no stable release yet, so this installs the newest nightly."
        }
    }
}
if (-not $version) {
    try { $version = Get-Text "$releases/download/nightly/version.txt" } catch { Stop-Install "$_" }
}
if ($version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+') { Stop-Install "the release's version.txt doesn't hold a version" }
$tag = if ($version -match '-nightly\.') { 'nightly' } else { "v$version" }
# The native build on an Arm processor (CLX-276), which the registry names even to a PowerShell
# that Windows runs by translating it, or else the x64 one, which Windows on Arm runs that way:
# where the release has no Arm64 build, as releases before it don't.
$cpu = 'x64'
$native = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment' -Name PROCESSOR_ARCHITECTURE -ErrorAction SilentlyContinue).PROCESSOR_ARCHITECTURE
if ($native -eq 'ARM64') {
    try {
        Get-Bytes "$releases/download/$tag/calx-$version-windows-arm64.zip.sha256" | Out-Null
        $cpu = 'arm64'
    } catch {
        if ("$_" -notmatch 'HTTP 404') { Stop-Install "$_" }
        Write-Host "This release has no build for Arm64 Windows, so this installs the x64 one, which Windows runs by translating it."
    }
}
$file = "calx-$version-windows-$cpu.zip"
$url = "$releases/download/$tag/$file"

$temp = Join-Path ([IO.Path]::GetTempPath()) ("calx-install-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
try {
    if (-not $styled) { Write-Host "Downloading Calx $version..." }
    $zip = Join-Path $temp $file
    try {
        Save-Download $url $zip $file
        $want = ((Get-Text "$url.sha256") -split '\s+')[0].ToLowerInvariant()
    } catch { Stop-Install "$_" }
    $got = (Get-FileHash -Algorithm SHA256 -Path $zip).Hash.ToLowerInvariant()
    if ($got -ne $want) {
        Stop-Install "the download's SHA-256 is $got, but the release says $want. Nothing was installed; try again in a minute."
    }
    $mb = [math]::Round((Get-Item $zip).Length / 1MB, 1)
    if ($styled) {
        Write-Done "Downloaded Calx $(Accent $version) $(Dim "$(Format-MB (Get-Item $zip).Length) MB, SHA-256 checked")"
    } else {
        Write-Host "Checked its SHA-256 ($mb MB)."
    }

    $unpacked = Join-Path $temp 'unpacked'
    # .NET's own unzip, which is many times faster than Expand-Archive.
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
    if ($styled) { Write-Done "Unpacked it into $(Accent $root)" }
} finally {
    Remove-Item -Recurse -Force $temp -ErrorAction SilentlyContinue
}

$bin = Join-Path $root 'bin'
if ($env:CALX_NO_MODIFY_PATH -ne '1') {
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $parts = @(); if ($userPath) { $parts = $userPath -split ';' | Where-Object { $_ } }
    if ($parts -notcontains $bin) {
        [Environment]::SetEnvironmentVariable('Path', (($parts + $bin) -join ';'), 'User')
        if ($styled) { Write-Done "Added $(Accent $bin) to your PATH" } else { Write-Host "Added $bin to your PATH." }
    }
    if (($env:Path -split ';') -notcontains $bin) { $env:Path = "$env:Path;$bin" }
}

# The toolchain programs are built with is a download of its own, since it changes only with LLVM
# (CLX-110, CLX-251). calx-update fetches it when the one here isn't the one the release names in
# toolchain.txt, and unpacks the .tar.xz, which Windows' own tools may not read.
# CALX_INSTALLER=1 has it leave out its closing line, as install.sh says the same steps (CLX-265); it's
# taken away after, since `irm | iex` runs this in the caller's session.
if (Test-Path (Join-Path $root 'toolchain.txt')) {
    $env:CALX_INSTALLER = '1'
    try {
        & (Join-Path $bin 'calx-update.exe') --home $root --current $version --toolchain
    } finally {
        Remove-Item Env:CALX_INSTALLER -ErrorAction SilentlyContinue
    }
    if ($LASTEXITCODE -ne 0) { Stop-Install 'the toolchain didn''t install; `calx update --toolchain` tries again' }
}

$calx = Join-Path $bin 'calx.exe'
$clang = if ($env:CALX_CC) { $env:CALX_CC } else { (Get-Command clang -ErrorAction SilentlyContinue).Source }
$carried = Test-Path (Join-Path $root 'toolchain\bin\clang.exe')
if ($styled) {
    # A first program, built and run, which checks it all works and compiles the runtime, so the
    # first program someone writes builds as quickly as the rest. calx draws what it's doing on
    # standard error meanwhile; the program's output isn't wanted.
    if ($carried -or $clang) {
        $first = Join-Path $root '.update\first'
        New-Item -ItemType Directory -Force -Path $first | Out-Null
        $hello = Join-Path $first 'hello.calx'
        [IO.File]::WriteAllText($hello, "fn main() {`n    println(`"hello`")`n}`n")
        $clock = [Diagnostics.Stopwatch]::StartNew()
        & $calx run $hello | Out-Null
        $ok = $LASTEXITCODE -eq 0
        $took = [string]::Format($invariant, '{0:0.0} s', $clock.Elapsed.TotalSeconds)
        Remove-Item -Recurse -Force $first -ErrorAction SilentlyContinue
        if ($ok) {
            Write-Done "Built and ran a first program $(Dim $took)"
        } else {
            Write-Host "$(Paint '33' '!') A first program didn't build; $(Accent 'calx doctor') says why."
        }
    }
    Write-Host ""
    Write-Host (Format-Band "$tick Calx $version is installed")
    Write-Host (Dim "in $root")
} else {
    & $calx version
    Write-Host ""
    Write-Host "Calx is installed in $root." -ForegroundColor Green
}

# Calx builds programs with clang. A release carries its own, with the Windows libraries it links
# with, in toolchain\ (CLX-110), so nothing else needs installing; one from before that needs one.
if (-not $clang -and -not $carried) {
    Write-Host ""
    Write-Host "Calx builds programs with clang, which isn't on your PATH yet. Install LLVM:"
    Write-Host "    winget install LLVM.LLVM"
    Write-Host "and Visual Studio's Build Tools, with the 'Desktop development with C++' workload, for the"
    Write-Host "Windows libraries clang links with: https://visualstudio.microsoft.com/downloads/"
}
Write-Host ""
if ($styled) {
    Write-Host "Try it in a new terminal   $(Accent 'calx run hello.calx')"
    Write-Host "Keep it up to date         $(Accent 'calx update')"
    Write-Host "See what it can do         $(Accent 'calx help')"
} else {
    Write-Host "Try it in a new terminal:  $(Accent 'calx run hello.calx')"
    Write-Host "Keep it up to date with:   $(Accent 'calx update')"
}
