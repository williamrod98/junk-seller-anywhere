[CmdletBinding()]
param(
    # vcpkg checkout (contains scripts\buildsystems\vcpkg.cmake).
    [Parameter(Mandatory = $true)][string]$VcpkgRoot,
    # Folder the vcpkg ports were installed into (vcpkg install --x-install-root=<this>);
    # it must contain x64-windows-static-md\share\fmt\fmt-config.cmake. The build never
    # downloads or installs dependencies (VCPKG_MANIFEST_INSTALL=OFF).
    [Parameter(Mandatory = $true)][string]$VcpkgInstalledDir,
    # Scratch root: every run copies the sources into a new <WorkRoot>\<runId> folder.
    [Parameter(Mandatory = $true)][string]$WorkRoot,
    # Receives the DLL, PDB, the plugin file and its SEQ file, SHA256SUMS.txt,
    # provenance.json and exports.txt.
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [ValidateSet('Release')][string]$Configuration = 'Release',
    [string]$RepositoryRoot = (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent),
    # CommonLibSSE-NG 9.1.0 source tree (README.md, "Getting CommonLibSSE-NG 9.1.0").
    [string]$CommonLibSource = '',
    # Python 3 interpreter that runs generate_js.py.
    [string]$Python = 'python.exe',
    # Build identity strings compiled into the DLL (v0.9.0 shipped these defaults).
    [string]$ReleaseId = 'w-probe-r71',
    [string]$AcceptanceRun = 'none'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
# Process-only settings: never edit global Git configuration.
$env:GIT_CONFIG_COUNT = '1'
$env:GIT_CONFIG_KEY_0 = 'safe.directory'
$env:GIT_CONFIG_VALUE_0 = '*'
$env:VCPKG_ROOT = $VcpkgRoot
$env:VCPKG_DISABLE_METRICS = '1'
$ExpectedCommonLibCommit = 'a898f469851c464d05137bb74b069dd234897643'
$NativeSource = Join-Path $PSScriptRoot 'native'
if ([string]::IsNullOrWhiteSpace($CommonLibSource)) {
    $CommonLibSource = Join-Path $RepositoryRoot 'vendor\commonlibsse-ng-v9.1.0'
}
$HeaderPinPath = Join-Path $PSScriptRoot 'native\commonlib-pin-v9.json'
# The shared runtime gate headers; copied and digested like the other trees and
# passed to CMake as JS_AGH_RUNTIME_DIR.
$SharedRuntimeSource = Join-Path $RepositoryRoot 'pipeline\shared\runtime'
$BaseIni = Join-Path $PSScriptRoot 'AGH_JunkSeller.ini'
$EspGenerator = Join-Path $PSScriptRoot 'generate_js.py'

function Invoke-Native {
    param([string]$FilePath, [Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments)
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Command failed ($LASTEXITCODE): $FilePath $($Arguments -join ' ')" }
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

# Digest of a tree: sorted "relative-path<TAB>sha256<LF>" records, skipping any
# .git or build folder; SHA-256 of the UTF-8 bytes.
function Get-TreeDigest([string]$Root) {
    $prefix = [IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    $records = [Text.StringBuilder]::new()
    [string[]]$files = @(Get-ChildItem -LiteralPath $Root -File -Recurse | ForEach-Object {
        $_.FullName.Substring($prefix.Length).Replace('\', '/')
    })
    [Array]::Sort($files, [StringComparer]::Ordinal)
    foreach ($relative in $files) {
        if ($relative -match '(^|/)(\.git|build)(/|$)') { continue }
        [void]$records.Append($relative).Append("`t").Append((Get-Sha256 (Join-Path $Root $relative))).Append("`n")
    }
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($records.ToString()))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Copy-SourceTree([string]$Source, [string]$Destination) {
    New-Item -ItemType Directory -Path $Destination | Out-Null
    # Each run has a new scratch tree. No /MIR deletion and no source mutation.
    # Function-local 'Continue': robocopy's stderr must not throw; its exit code is checked.
    $ErrorActionPreference = 'Continue'
    & robocopy.exe $Source $Destination /E /XD .git build /XF .git /NFL /NDL /NJH /NJS /NP /R:2 /W:5
    if ($LASTEXITCODE -gt 7) { throw "robocopy failed ($LASTEXITCODE): $Source" }
}

# 1. Prerequisites, before creating scratch/output files. Run from an x64
# "Developer PowerShell for VS 2022".
if ($env:OS -ne 'Windows_NT') { throw 'Windows/MSVC only' }
foreach ($path in @($WorkRoot, $OutputDirectory)) {
    if (-not [IO.Path]::IsPathRooted($path)) { throw "Use an absolute path: $path" }
}
$WorkRoot = [IO.Path]::GetFullPath($WorkRoot)
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
foreach ($tool in @('cmake.exe', 'cl.exe', 'dumpbin.exe', 'robocopy.exe')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "Missing $tool; use an x64 Developer PowerShell for VS 2022" }
}
if (-not (Get-Command $Python -ErrorAction SilentlyContinue)) { throw "Missing Python 3: $Python" }
if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64' -or $env:VCToolsVersion -notmatch '^14\.44\.') {
    throw 'Expected VS 2022 Build Tools x64 / MSVC 19.44 (VCToolsVersion 14.44.x)'
}
# Native steps run under 'Continue' so a stderr line (CMake warnings) cannot throw
# under 'Stop'; every exit code is checked (Invoke-Native, LASTEXITCODE).
$ErrorActionPreference = 'Continue'
$cmakeVersion = (& cmake.exe --version | Out-String).Trim()
$ErrorActionPreference = 'Stop'
if ($LASTEXITCODE -ne 0 -or $cmakeVersion -notmatch '^cmake version 4\.4\.3(?:\s|$)') { throw "Expected CMake 4.4.3 first on PATH: $cmakeVersion" }
foreach ($path in @($NativeSource, $CommonLibSource, $SharedRuntimeSource,
    (Join-Path $SharedRuntimeSource 'include\agh\runtime\RuntimeGate.h'),
    (Join-Path $VcpkgRoot 'scripts\buildsystems\vcpkg.cmake'),
    (Join-Path $VcpkgInstalledDir 'x64-windows-static-md\share\fmt\fmt-config.cmake'),
    $HeaderPinPath, $BaseIni, $EspGenerator)) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing prerequisite: $path" }
}
if ($ReleaseId -notmatch '^[A-Za-z0-9._-]+$' -or $AcceptanceRun -notmatch '^[A-Za-z0-9._-]+$') {
    throw 'ReleaseId and AcceptanceRun must contain only ASCII letters, digits, dot, underscore, or dash'
}
$headerPin = Get-Content -LiteralPath $HeaderPinPath -Raw | ConvertFrom-Json
if ($headerPin.expectedCommit -ne $ExpectedCommonLibCommit) { throw 'Unexpected header pin metadata' }
foreach ($header in $headerPin.files) {
    if ((Get-Sha256 (Join-Path $CommonLibSource $header.path)) -ne $header.sha256) { throw "CommonLib header pin mismatch: $($header.path)" }
}
if ((Get-TreeDigest $CommonLibSource) -ne $headerPin.sourceTreeSha256) { throw 'Complete CommonLib source pin mismatch (see README.md, "Getting CommonLibSSE-NG 9.1.0")' }

$runId = [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$runRoot = Join-Path $WorkRoot $runId
if (Test-Path -LiteralPath $runRoot) { throw 'Scratch run already exists' }
New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
# The folder names below are part of the build: CMakeLists.txt maps these roots
# with /pathmap so no absolute path is embedded.
$workNative = Join-Path $runRoot 'native'
$workCommonLib = Join-Path $runRoot 'commonlibsse-ng'
$workRuntime = Join-Path $runRoot 'agh-runtime'
$buildDirectory = Join-Path $runRoot 'build'
Copy-SourceTree $NativeSource $workNative
Copy-SourceTree $CommonLibSource $workCommonLib
Copy-SourceTree $SharedRuntimeSource $workRuntime
$depsDigest = Get-TreeDigest $VcpkgInstalledDir
$nativeDigest = Get-TreeDigest $NativeSource
$commonLibDigest = Get-TreeDigest $CommonLibSource
$runtimeDigest = Get-TreeDigest $SharedRuntimeSource
if ((Get-TreeDigest $workNative) -ne $nativeDigest -or (Get-TreeDigest $workCommonLib) -ne $commonLibDigest -or
    (Get-TreeDigest $workRuntime) -ne $runtimeDigest) { throw 'Source copy digest mismatch' }

# The DLL embeds the SHA-256 of the base INI and of the generated plugin file,
# so both are computed before the compile. The plugin file and its SEQ file
# come from the deterministic generator.
$baseIniHash = Get-Sha256 $BaseIni
$generatedEsp = Join-Path $runRoot 'generated-esp'
Invoke-Native $Python $EspGenerator '--out' $generatedEsp
$espPath = Join-Path $generatedEsp 'AGH_JunkSeller.esp'
$seqPath = Join-Path $generatedEsp 'Seq\AGH_JunkSeller.seq'
foreach ($path in @($espPath, $seqPath)) { if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing generated file: $path" } }
$espHash = Get-Sha256 $espPath
# src\BuildInfo.h of the scratch copy is replaced with the build identity; the
# file in the repository keeps host-test defaults.
$buildInfo = @(
    '#pragma once',
    'namespace js::BuildInfo {',
    'inline constexpr const char* Version = "0.9.0";',
    "inline constexpr const char* ReleaseId = `"$ReleaseId`";",
    'inline constexpr const char* AddressLibrary = "versionlib runtime check";',
    "inline constexpr const char* BaseIniSha256 = `"$baseIniHash`";",
    "inline constexpr const char* EspSha256 = `"$espHash`";",
    "inline constexpr const char* AcceptanceRun = `"$AcceptanceRun`";",
    '}'
)
[IO.File]::WriteAllLines((Join-Path $workNative 'src\BuildInfo.h'), $buildInfo, [Text.Encoding]::ASCII)
$workNativeDigest = Get-TreeDigest $workNative
$inputProvenance = [ordered]@{
    expectedCommonLibCommit = $ExpectedCommonLibCommit
    completeSourcePinVerified = $true
    headerPinSha256 = Get-Sha256 $HeaderPinPath
    dependenciesTreeSha256 = $depsDigest
    nativeTreeSha256 = $nativeDigest
    commonLibTreeSha256 = $commonLibDigest
    aghRuntimeTreeSha256 = $runtimeDigest
    buildScriptSha256 = Get-Sha256 $PSCommandPath
    iniSha256 = $baseIniHash
    espGeneratorSha256 = Get-Sha256 $EspGenerator
}

# 2. Configure: add_subdirectory uses the local CommonLib tree. vcpkg manifest
# mode with the already installed x64-windows-static-md tree; dependency
# installation/download is disabled (VCPKG_MANIFEST_INSTALL=OFF), and so is
# CommonLibSSE-NG's configure-time download of MinHook's hde64
# (SKSE_SUPPORT_PATCH_SAFETY=OFF).
$ErrorActionPreference = 'Continue'
Invoke-Native cmake.exe '-S' $workNative '-B' $buildDirectory '-G' 'Visual Studio 17 2022' '-A' 'x64' '-T' 'v143,version=14.44' `
    "-DJS_COMMONLIB_DIR=$workCommonLib" "-DJS_AGH_RUNTIME_DIR=$workRuntime" '-DJS_BUILD_PLUGIN=ON' `
    "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot\scripts\buildsystems\vcpkg.cmake" `
    '-DVCPKG_TARGET_TRIPLET=x64-windows-static-md' '-DVCPKG_HOST_TRIPLET=x64-windows-static-md' `
    "-DVCPKG_INSTALLED_DIR=$VcpkgInstalledDir" '-DVCPKG_MANIFEST_MODE=ON' '-DVCPKG_MANIFEST_INSTALL=OFF' '-DFETCHCONTENT_FULLY_DISCONNECTED=ON' `
    '-DSKSE_SUPPORT_PATCH_SAFETY=OFF' '-DENABLE_SKYRIM_SE=ON' '-DENABLE_SKYRIM_AE=ON' '-DENABLE_SKYRIM_VR=OFF'

# 3. Build. Quote '--' so PowerShell passes it to CMake/MSBuild.
$ErrorActionPreference = 'Continue'
Invoke-Native cmake.exe --build $buildDirectory --config $Configuration --target AGH_JunkSeller '--' /m
$ErrorActionPreference = 'Stop'
$dll = Join-Path $buildDirectory "bin\$Configuration\AGH_JunkSeller.dll"
$pdb = Join-Path $buildDirectory "bin\$Configuration\AGH_JunkSeller.pdb"
foreach ($path in @($dll, $pdb)) { if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing artifact: $path" } }
$exports = (& dumpbin.exe /nologo /exports $dll 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0 -or $exports -notmatch '\bSKSEPlugin_Load\b' -or $exports -notmatch '\bSKSEPlugin_Version\b') { throw 'Required SKSE exports missing' }
$headers = (& dumpbin.exe /nologo /headers $dll 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0 -or $headers -notmatch '8664 machine') { throw 'Expected x64 PE image' }
$dependents = (& dumpbin.exe /nologo /dependents $dll 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) { throw 'dumpbin /dependents failed' }
# Refuse sources changed during compilation.
if ((Get-TreeDigest $NativeSource) -ne $nativeDigest -or (Get-TreeDigest $CommonLibSource) -ne $commonLibDigest -or
    (Get-TreeDigest $workNative) -ne $workNativeDigest -or (Get-TreeDigest $workCommonLib) -ne $commonLibDigest -or
    (Get-TreeDigest $SharedRuntimeSource) -ne $runtimeDigest -or (Get-TreeDigest $workRuntime) -ne $runtimeDigest) { throw 'Source changed during build' }
if ((Get-Sha256 $BaseIni) -ne $inputProvenance.iniSha256 -or
    (Get-Sha256 $HeaderPinPath) -ne $inputProvenance.headerPinSha256 -or
    (Get-Sha256 $PSCommandPath) -ne $inputProvenance.buildScriptSha256) { throw 'Build inputs changed during compilation' }
# VCPKG_MANIFEST_INSTALL=OFF must leave the installed tree untouched.
if ((Get-TreeDigest $VcpkgInstalledDir) -ne $depsDigest) { throw 'Dependency tree changed during build' }

# 4. Copy the verified DLL/PDB and the plugin and SEQ files; record SHA-256 and provenance.
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $OutputDirectory 'Seq') -Force | Out-Null
$hashLines = @()
foreach ($entry in @(
        @{Path = $dll; Label = 'AGH_JunkSeller.dll'},
        @{Path = $pdb; Label = 'AGH_JunkSeller.pdb'},
        @{Path = $espPath; Label = 'AGH_JunkSeller.esp'},
        @{Path = $seqPath; Label = 'Seq\AGH_JunkSeller.seq'})) {
    $hash = Get-Sha256 $entry.Path
    $destination = Join-Path $OutputDirectory $entry.Label
    Copy-Item -LiteralPath $entry.Path -Destination $destination -Force
    if ((Get-Sha256 $destination) -ne $hash) { throw "Copy hash mismatch: $($entry.Label)" }
    $hashLines += "$hash  $($entry.Label)"
}
$hashLines | Set-Content -LiteralPath (Join-Path $OutputDirectory 'SHA256SUMS.txt') -Encoding ASCII
$manifest = [ordered]@{
    schemaVersion = 1
    project = 'AGH_JunkSeller'
    version = '0.9.0'
    releaseId = $ReleaseId
    acceptanceRun = $AcceptanceRun
    assetHashes = [ordered]@{ baseIni = $baseIniHash; esp = $espHash }
    inputs = $inputProvenance
    configuration = $Configuration
    generator = 'Visual Studio 17 2022'
    toolset = $env:VCToolsVersion
    cmakeVersion = $cmakeVersion
    triplet = 'x64-windows-static-md'
    targetRuntimes = @('1.6.1170.0', '1.7.104.0')
    minimumSKSE = '2.2.6'
    cmakeCacheSha256 = Get-Sha256 (Join-Path $buildDirectory 'CMakeCache.txt')
    hashes = $hashLines
    dependents = $dependents
    generatedUtc = [DateTimeOffset]::UtcNow.ToString('o')
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'provenance.json') -Encoding UTF8
$exports | Set-Content -LiteralPath (Join-Path $OutputDirectory 'exports.txt') -Encoding UTF8
Write-Host "Verified DLL, PDB, plugin and SEQ files: $OutputDirectory"
