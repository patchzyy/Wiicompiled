[CmdletBinding()]
param([Parameter(Mandatory)] [string]$PayloadRoot)

$ErrorActionPreference = 'Stop'
$PayloadRoot = [IO.Path]::GetFullPath($PayloadRoot)
if (-not (Test-Path -LiteralPath $PayloadRoot -PathType Container)) { throw "Payload does not exist: $PayloadRoot" }

$forbiddenNames = @(
    'WiiCompiled.exe', 'RetroRewind.exe', 'mkw_recompiled.exe',
    'main.dol', 'StaticR.rel', 'Code.pul', 'base_translation_sources.bin',
    'data_sections_init.cpp', 'data_sections_init_blobs.S', 'mkwii_base_manifest.json',
    'generated_func_aliases.cpp', 'translated_function_decls.h'
)
$forbiddenExtensions = @('.iso', '.gcm', '.gcz', '.ciso', '.wbfs', '.wia', '.rvz', '.dol', '.rel', '.pul')
$forbiddenSegments = @(
    '\BuildWorkspace\generated\', '\BuildWorkspace\runtime\build\', '\GameAssets\', '\BuildWorkspace\Assets\',
    '\BuildWorkspace\PulsarPacks\', '\BuildWorkspace\build\base\', '\BuildWorkspace\build\mods\'
)
$violations = [Collections.Generic.List[string]]::new()
foreach ($file in Get-ChildItem -LiteralPath $PayloadRoot -Recurse -File) {
    $rootPrefix = $PayloadRoot.TrimEnd('\') + '\'
    if (-not $file.FullName.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Payload enumeration escaped its root: $($file.FullName)"
    }
    $relative = '\' + $file.FullName.Substring($rootPrefix.Length).Replace('/', '\')
    $underCompiler = $relative.StartsWith('\Toolkit\llvm-mingw\', [StringComparison]::OrdinalIgnoreCase)
    $underPinnedDependency = $relative.StartsWith('\BuildWorkspace\Dependencies\', [StringComparison]::OrdinalIgnoreCase)
    if ($forbiddenNames -contains $file.Name -or $forbiddenExtensions -contains $file.Extension.ToLowerInvariant()) {
        $violations.Add($relative)
        continue
    }
    if ($forbiddenSegments | Where-Object { $relative.IndexOf($_, [StringComparison]::OrdinalIgnoreCase) -ge 0 }) {
        $violations.Add($relative)
        continue
    }
    if (-not $underCompiler -and -not $underPinnedDependency -and $file.Extension -in @('.obj','.o','.lib','.a')) {
        $violations.Add("$relative (precompiled object/library outside the licensed compiler payload)")
    }
}
if ($violations.Count -gt 0) {
    throw "Release payload contains forbidden game-derived or precompiled artifacts:`n$($violations -join "`n")"
}

# Build-Installer.ps1 prunes unused tool payload; the license texts and the x86_64 target runtime
# files must survive it.
$toolkitRoot = Join-Path $PayloadRoot 'Toolkit'
$missingRequired = [Collections.Generic.List[string]]::new()
function Test-RequiredPattern([string]$Directory, [string]$Filter, [string]$Description, [switch]$Directories) {
    $found = @()
    if (Test-Path -LiteralPath $Directory -PathType Container) {
        $found = @(Get-ChildItem -LiteralPath $Directory -Filter $Filter -Force |
            Where-Object { $_.PSIsContainer -eq [bool]$Directories })
        if ($Directories) {
            $found = @($found | Where-Object { @(Get-ChildItem -LiteralPath $_.FullName -Recurse -File -Force).Count -gt 0 })
        } else {
            $found = @($found | Where-Object { $_.Length -gt 0 })
        }
    }
    if ($found.Count -eq 0) { $missingRequired.Add("$Description ($Directory\$Filter)") }
}
$cmakeDocs = @(Get-ChildItem -LiteralPath (Join-Path $toolkitRoot 'CMake\doc') -Directory -Filter 'cmake*' -ErrorAction SilentlyContinue)
if ($cmakeDocs.Count -eq 0) { $missingRequired.Add('CMake license documentation (Toolkit\CMake\doc\cmake*)') }
foreach ($docs in $cmakeDocs) {
    Test-RequiredPattern $docs.FullName 'LICENSE.rst' 'CMake license'
    Test-RequiredPattern $docs.FullName 'cm*' 'CMake bundled-library licenses' -Directories
}
$cmakeShares = @(Get-ChildItem -LiteralPath (Join-Path $toolkitRoot 'CMake\share') -Directory -Filter 'cmake-*' -ErrorAction SilentlyContinue)
if ($cmakeShares.Count -eq 0) { $missingRequired.Add('CMake modules (Toolkit\CMake\share\cmake-*)') }
foreach ($share in $cmakeShares) {
    Test-RequiredPattern $share.FullName 'Licenses' 'CMake module licenses' -Directories
    Test-RequiredPattern $share.FullName 'Modules' 'CMake modules' -Directories
}
$sysroot = Join-Path $toolkitRoot 'llvm-mingw\x86_64-w64-mingw32'
Test-RequiredPattern (Join-Path $toolkitRoot 'llvm-mingw') 'LICENSE.TXT' 'LLVM-MinGW license'
Test-RequiredPattern (Join-Path $sysroot 'share\mingw32') 'COPYING*' 'mingw-w64 licenses'
Test-RequiredPattern (Join-Path $sysroot 'bin') 'libc++.dll' 'x86_64 libc++ runtime'
Test-RequiredPattern (Join-Path $sysroot 'bin') 'libunwind.dll' 'x86_64 libunwind runtime'
if ($missingRequired.Count -gt 0) {
    throw "Release payload is missing required license or runtime files:`n$($missingRequired -join "`n")"
}
Write-Host "Payload boundary audit passed: no translated game executable, game input, generated shard, or generated data was packaged; required licenses and target runtimes are present."
