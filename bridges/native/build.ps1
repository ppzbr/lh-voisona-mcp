<#
.SYNOPSIS
    Build lhai_bridge.dll (and lhai_bridge.exe) with zig.

.DESCRIPTION
    Two artefacts from one source:

      lhai_bridge.dll   injected into VoiSona / VOCALOID 6, hosts MCP in-process
      lhai_bridge.exe   the same server standalone, so the transport can be
                        tested without injecting into anyone's editor

    zig is used because it needs no Visual Studio install: it ships its own
    clang, linker and Windows headers, and the output depends only on system
    DLLs (kernel32/user32/ws2_32/psapi), so there is no runtime to install.

    zig is resolved from $env:ZIG, then PATH, then the pip wheel (ziglang), then
    a download into %TEMP%\zig.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File build.ps1
#>
[CmdletBinding()]
param(
    [string]$Zig,
    [string]$OutDir,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $here 'lhai_bridge.cpp'
if (-not $OutDir) { $OutDir = $here }

function Resolve-Zig {
    param([string]$Explicit)
    if ($Explicit) {
        if (Test-Path $Explicit) { return (Resolve-Path $Explicit).Path }
        throw "zig not found at $Explicit"
    }
    if ($env:ZIG -and (Test-Path $env:ZIG)) { return (Resolve-Path $env:ZIG).Path }
    $onPath = Get-Command zig -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    $python = Get-Command python -ErrorAction SilentlyContinue
    if ($python) {
        $candidate = & $python.Source -c "import os,ziglang;print(os.path.join(os.path.dirname(ziglang.__file__),'zig.exe'))" 2>$null
        if ($candidate -and (Test-Path $candidate)) { return $candidate.Trim() }
    }
    $cache = Join-Path $env:TEMP 'zig'
    $cached = Join-Path $cache 'zig.exe'
    if (Test-Path $cached) { return $cached }
    $version = '0.17.1'
    $zip = Join-Path $env:TEMP "zig-$version.zip"
    $url = "https://ziglang.org/download/$version/zig-x86_64-windows-$version.zip"
    Write-Host "downloading zig $version -> $zip"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
    $extract = Join-Path $env:TEMP "zig-extract-$version"
    if (Test-Path $extract) { Remove-Item $extract -Recurse -Force }
    Expand-Archive -Path $zip -DestinationPath $extract -Force
    $inner = Get-ChildItem $extract -Directory | Select-Object -First 1
    if (Test-Path $cache) { Remove-Item $cache -Recurse -Force }
    Move-Item $inner.FullName $cache
    return (Join-Path $cache 'zig.exe')
}

$zigExe = Resolve-Zig -Explicit $Zig
Write-Host "zig: $zigExe"
& $zigExe version

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$common = @(
    'c++',
    '-target', 'x86_64-windows-gnu',
    '-std=c++17',
    '-O2',
    '-DUNICODE', '-D_UNICODE',
    '-D_WIN32_WINNT=0x0601'
)
$libs = @('-lws2_32', '-lpsapi', '-luser32', '-ladvapi32', '-lversion', '-lshell32')

function Build-One {
    param([string]$Out, [string[]]$Extra)
    if ((Test-Path $Out) -and -not $Force) {
        try {
            $stream = [IO.File]::Open($Out, 'Open', 'ReadWrite', 'None')
            $stream.Close()
        } catch {
            throw "$Out is locked - eject the bridge from every editor, then retry"
        }
    }
    $arguments = $common + $Extra + @('-o', $Out, $source) + $libs
    Write-Host "compiling $Out"
    & $zigExe @arguments
    if ($LASTEXITCODE -ne 0) { throw "zig c++ failed with exit code $LASTEXITCODE for $Out" }
    $item = Get-Item $Out
    Write-Host ("built {0} ({1:N1} KB)" -f $item.FullName, ($item.Length / 1KB))
    return $item
}

$dll = Join-Path $OutDir 'lhai_bridge.dll'
$exe = Join-Path $OutDir 'lhai_bridge.exe'
$dllItem = Build-One -Out $dll -Extra @('-shared')
$exeItem = Build-One -Out $exe -Extra @('-DLHAI_BRIDGE_EXE')

$dllHash = (Get-FileHash $dll -Algorithm SHA256).Hash
$exeHash = (Get-FileHash $exe -Algorithm SHA256).Hash
Write-Host ""
Write-Host "dll sha256 : $dllHash"
Write-Host "exe sha256 : $exeHash"

$buildInfo = @(
    "Bridge=LhBridge"
    "Version=0.1.0"
    "Protocol=1"
    "TargetFramework=native-x64"
    "BuiltAgainst=VoiSona / VOCALOID6 (native x64)"
    "Built=$([DateTime]::UtcNow.ToString('o'))"
    "DllSha256=$dllHash"
    "ExeSha256=$exeHash"
    "Loader=inject (lhai/live/inject.py)"
)
$buildInfo -join "`n" | Set-Content -Path (Join-Path $OutDir 'lhai_bridge.build-info.txt') -Encoding UTF8
Write-Host "wrote lhai_bridge.build-info.txt"

# zig emits a .lib, a .exp and a .pdb beside a -shared build. Nothing here links
# against them - the bridge is loaded by path - so they are deleted rather than
# left to pile up in the tree next to the real artefacts.
foreach ($extension in @('.lib', '.exp', '.pdb')) {
    Remove-Item (Join-Path $OutDir ('lhai_bridge' + $extension)) -Force -ErrorAction SilentlyContinue
}
