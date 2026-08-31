# Builds the redistributable precompiled aurora + third-party package: aurora (~43% of local build
# CPU time) and vendored Crypto++ are identical for every user, so this configures runtime/ like
# LocalBuild.ps1 (both dot-source NativeBuildFlags.ps1), builds just that closure, and harvests the
# archives plus a generated CMake description into launcher/artifacts/dependencies/native_prebuilt.
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$PortableToolsDirectory = 'launcher/artifacts/portable-tools',
    [string]$DependencySourceDirectory = 'launcher/artifacts/dependencies',
    [string]$OutputDirectory,
    [string]$StageDirectory = 'build/native-prebuilt-stage',
    [switch]$KeepStage,
    # Maintainer iteration aid: keep an already configured staging build so a
    # re-harvest does not recompile aurora from scratch.
    [switch]$ReuseStage,
    [int]$Parallel = 0
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

. (Join-Path $PSScriptRoot 'NativeBuildFlags.ps1')

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
function Full([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}
function Normalize([string]$Path) { return ([IO.Path]::GetFullPath($Path)).Replace('\', '/') }
function StartsWithPath([string]$Candidate, [string]$Root) {
    $rootSlash = $Root.TrimEnd('/') + '/'
    return $Candidate.StartsWith($rootSlash, [StringComparison]::OrdinalIgnoreCase)
}
function RelativeTo([string]$Candidate, [string]$Root) {
    $rootSlash = $Root.TrimEnd('/') + '/'
    return $Candidate.Substring($rootSlash.Length)
}

$portableTools = Full $PortableToolsDirectory
$dependencies = Full $DependencySourceDirectory
$stage = Full $StageDirectory
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $dependencies 'native_prebuilt'
}
$package = Full $OutputDirectory

$toolchainBin = Join-Path $portableTools 'llvm-mingw\bin'
$cmake = Join-Path $portableTools 'CMake\bin\cmake.exe'
$ninja = Join-Path $portableTools 'Ninja\ninja.exe'
$cc = Join-Path $toolchainBin 'x86_64-w64-mingw32-clang.exe'
$cxx = Join-Path $toolchainBin 'x86_64-w64-mingw32-clang++.exe'
$windres = Join-Path $toolchainBin 'x86_64-w64-mingw32-windres.exe'
$clangBinary = Join-Path $toolchainBin 'clang-22.exe'
$runtimeSource = Join-Path $repoRoot 'runtime'
$auroraSource = Join-Path $repoRoot 'aurora-main'

Assert-File $cmake 'Portable CMake'
Assert-File $ninja 'Portable Ninja'
Assert-File $cc 'Portable C compiler'
Assert-File $cxx 'Portable C++ compiler'
Assert-File $windres 'Portable resource compiler'
Assert-File $clangBinary 'Portable clang driver binary'
Assert-Directory $dependencies 'Pinned offline dependency sources'
Assert-Directory $auroraSource 'aurora-main source tree'

if ($Parallel -le 0) { $Parallel = [Environment]::ProcessorCount }

# The package must never contain a stale mixture of two builds.
if (Test-Path -LiteralPath $package) { Remove-Item -LiteralPath $package -Recurse -Force }
[IO.Directory]::CreateDirectory($package) | Out-Null
[IO.Directory]::CreateDirectory((Join-Path $package 'lib')) | Out-Null
[IO.Directory]::CreateDirectory((Join-Path $package 'bin')) | Out-Null
[IO.Directory]::CreateDirectory((Join-Path $package 'include')) | Out-Null

# The staging build has to be configured without the package present, otherwise
# the runtime would consume the very package this script is producing.
if (-not $ReuseStage -and (Test-Path -LiteralPath $stage)) { Remove-Item -LiteralPath $stage -Recurse -Force }
$exportDirectory = Join-Path $stage 'export'
[IO.Directory]::CreateDirectory($exportDirectory) | Out-Null

$oldPath = $env:PATH
try {
    $env:PATH = Get-MkwToolchainPath $portableTools

    $configure = Get-MkwNativeConfigureArguments -SourceDirectory $runtimeSource -BuildDirectory $stage `
        -Ninja $ninja -CCompiler $cc -CxxCompiler $cxx -ResourceCompiler $windres `
        -DependenciesDirectory $dependencies `
        -AdditionalArguments @("-DMKW_NATIVE_PREBUILT_EXPORT_DIR=$(ConvertTo-MkwCMakePath $exportDirectory)")
    Invoke-Checked $cmake $configure 'Configuring the aurora/third-party staging build' -LogPrefix 'MKWNP'

    # mkw_np_probe links exactly what mkw_runtime_common and the public products
    # link, so building it compiles the whole redistributable closure and nothing
    # else - and a successful link proves the harvested archives are complete.
    Invoke-Checked $cmake @('--build', $stage, '--target', 'mkw_np_probe', '--parallel', "$Parallel") `
        'Building the aurora/third-party closure' -LogPrefix 'MKWNP'
} finally {
    $env:PATH = $oldPath
}

# ---------------------------------------------------------------------------
# Read the description CMake wrote, plus the resolved command lines Ninja holds.
# ---------------------------------------------------------------------------
$metaPath = Join-Path $exportDirectory 'meta.txt'
Assert-File $metaPath 'Native prebuilt export metadata'
$meta = @{}
foreach ($line in Get-Content -LiteralPath $metaPath) {
    if ($line -match '^([a-z0-9_]+)=(.*)$') { $meta[$Matches[1]] = $Matches[2] }
}

$buildNinja = Join-Path $stage 'build.ninja'
Assert-File $buildNinja 'Generated build.ninja'
$ninjaLines = [IO.File]::ReadAllLines($buildNinja)

function Get-NinjaStatementVariables([string[]]$Lines, [string]$RulePrefix) {
    $variables = $null
    for ($i = 0; $i -lt $Lines.Length; $i++) {
        $line = $Lines[$i]
        if (-not $line.StartsWith('build ')) { continue }
        $colon = $line.IndexOf(': ')
        if ($colon -lt 0) { continue }
        $rule = $line.Substring($colon + 2)
        $space = $rule.IndexOf(' ')
        if ($space -ge 0) { $rule = $rule.Substring(0, $space) }
        if ($rule -ne $RulePrefix) { continue }
        $variables = [ordered]@{}
        for ($j = $i + 1; $j -lt $Lines.Length; $j++) {
            $entry = $Lines[$j]
            if ($entry -notmatch '^  ([A-Z_]+) = ?(.*)$') { break }
            $variables[$Matches[1]] = $Matches[2]
        }
        break
    }
    if ($null -eq $variables) { throw "No Ninja statement used rule $RulePrefix." }
    return $variables
}
function Get-NinjaValue($Variables, [string]$Name) {
    if ($Variables.Contains($Name)) { return [string]$Variables[$Name] }
    return ''
}
function Split-NinjaList([string]$Value) {
    if ([string]::IsNullOrWhiteSpace($Value)) { return , [string[]]@() }
    return , [string[]]@($Value -split '\s+' | Where-Object { $_ })
}
function Subtract-Ordered([string[]]$All, [string[]]$Remove) {
    if ($null -eq $All) { return [string[]]@() }
    $result = [Collections.Generic.List[string]]::new()
    $pending = [Collections.Generic.List[string]]::new()
    if ($null -ne $Remove) { foreach ($item in $Remove) { $pending.Add($item) } }
    foreach ($item in $All) {
        $index = $pending.IndexOf($item)
        if ($index -ge 0) { $pending.RemoveAt($index); continue }
        $result.Add($item)
    }
    return $result.ToArray()
}

$probeCompile = Get-NinjaStatementVariables $ninjaLines 'CXX_COMPILER__mkw_np_probe_unscanned_Release'
$controlCompile = Get-NinjaStatementVariables $ninjaLines 'CXX_COMPILER__mkw_np_probe_control_unscanned_Release'
$probeLink = Get-NinjaStatementVariables $ninjaLines 'CXX_EXECUTABLE_LINKER__mkw_np_probe_Release'
$controlLink = Get-NinjaStatementVariables $ninjaLines 'CXX_EXECUTABLE_LINKER__mkw_np_probe_control_Release'

$includeArguments = Subtract-Ordered (Split-NinjaList (Get-NinjaValue $probeCompile 'INCLUDES')) `
    (Split-NinjaList (Get-NinjaValue $controlCompile 'INCLUDES'))
$defineArguments = Subtract-Ordered (Split-NinjaList (Get-NinjaValue $probeCompile 'DEFINES')) `
    (Split-NinjaList (Get-NinjaValue $controlCompile 'DEFINES'))
$compileOptions = Subtract-Ordered (Split-NinjaList (Get-NinjaValue $probeCompile 'FLAGS')) `
    (Split-NinjaList (Get-NinjaValue $controlCompile 'FLAGS'))
$linkItems = Subtract-Ordered (Split-NinjaList (Get-NinjaValue $probeLink 'LINK_LIBRARIES')) `
    (Split-NinjaList (Get-NinjaValue $controlLink 'LINK_LIBRARIES'))
if ($includeArguments.Count -eq 0) { throw 'The aurora compile interface is empty; the probe did not link aurora.' }
if ($linkItems.Count -eq 0) { throw 'The aurora link interface is empty; the probe did not link aurora.' }

# ---------------------------------------------------------------------------
# Harvest the built libraries.
# ---------------------------------------------------------------------------
$binaryRoot = Normalize $meta['binary_dir']
$auroraRoot = Normalize $meta['aurora_dir']
$runtimeRoot = Normalize $meta['runtime_dir']
$dependencyRoot = Normalize $dependencies

$targets = @{}
foreach ($line in Get-Content -LiteralPath (Join-Path $exportDirectory 'targets.txt')) {
    if ([string]::IsNullOrWhiteSpace($line)) { continue }
    $parts = $line -split '\|'
    if ($parts.Count -ne 4) { throw "Malformed exported target line: $line" }
    $targets[$parts[0]] = [pscustomobject]@{
        Name = $parts[0]; Type = $parts[1]
        File = Normalize $parts[2]; LinkerFile = Normalize $parts[3]
    }
}

$dawnLinkerFile = ''
$dawnRuntimeFile = ''
foreach ($line in Get-Content -LiteralPath (Join-Path $exportDirectory 'dawn.txt')) {
    if ([string]::IsNullOrWhiteSpace($line)) { continue }
    $parts = $line -split '\|'
    $dawnRuntimeFile = Normalize $parts[1]
    $dawnLinkerFile = Normalize $parts[2]
}

# linker-file path -> package reference, built only from libraries that were
# actually produced by this build.
$linkerFileToReference = @{}
$sharedImports = [Collections.Generic.List[object]]::new()
$copiedFiles = [Collections.Generic.List[string]]::new()
function Copy-Harvested([string]$SourcePath, [string]$SubDirectory) {
    $name = [IO.Path]::GetFileName($SourcePath)
    $destination = Join-Path (Join-Path $package $SubDirectory) $name
    if (Test-Path -LiteralPath $destination) {
        throw "Two harvested files collide on the name ${name}: $SourcePath"
    }
    Copy-Item -LiteralPath $SourcePath -Destination $destination
    $copiedFiles.Add("$SubDirectory/$name")
    return "$SubDirectory/$name"
}

foreach ($name in ($targets.Keys | Sort-Object)) {
    $target = $targets[$name]
    if (-not (Test-Path -LiteralPath $target.LinkerFile -PathType Leaf)) {
        # Not part of the closure mkw_np_probe pulled in; deliberately not shipped.
        continue
    }
    $relativeLinker = Copy-Harvested $target.LinkerFile 'lib'
    if ($target.Type -eq 'SHARED_LIBRARY') {
        if (-not (Test-Path -LiteralPath $target.File -PathType Leaf)) {
            throw "Shared library $name has no runtime file: $($target.File)"
        }
        $relativeRuntime = Copy-Harvested $target.File 'bin'
        $sharedImports.Add([pscustomobject]@{
            Target = "mkw_np::$name"; Runtime = $relativeRuntime; Implib = $relativeLinker })
        $linkerFileToReference[$target.LinkerFile] = "mkw_np::$name"
    } else {
        $linkerFileToReference[$target.LinkerFile] = "`@PKG`@/$relativeLinker"
    }
}
if ($sharedImports.Count -eq 0) { throw 'No shared libraries were harvested; the SDL/zlib/libpng DLLs would be missing.' }
if ($dawnLinkerFile) { $linkerFileToReference[$dawnLinkerFile] = 'dawn::webgpu_dawn' }

# ---------------------------------------------------------------------------
# Rewrite the compile/link interface into workspace-relative tokens.
# ---------------------------------------------------------------------------
$generatedIncludeIndex = 0
function ConvertTo-Token([string]$AbsolutePath, [string]$Kind) {
    $path = Normalize $AbsolutePath
    if (StartsWithPath $path $auroraRoot) { return "`@AURORA`@/$(RelativeTo $path $auroraRoot)" }
    if (StartsWithPath $path $runtimeRoot) { return "`@RUNTIME`@/$(RelativeTo $path $runtimeRoot)" }
    if (StartsWithPath $path $dependencyRoot) { return "`@DEPS`@/$(RelativeTo $path $dependencyRoot)" }
    if (StartsWithPath $path $binaryRoot) {
        if ($Kind -ne 'include') { throw "Unhandled build-tree artifact: $path" }
        # A generated header directory (SDL_revision.h and friends). The source
        # trees ship, but these do not exist until something configures them, so
        # they travel inside the package.
        $script:generatedIncludeIndex++
        $name = 'generated{0:d2}' -f $script:generatedIncludeIndex
        $destination = Join-Path (Join-Path $package 'include') $name
        [IO.Directory]::CreateDirectory($destination) | Out-Null
        Copy-Item -Path (Join-Path $path '*') -Destination $destination -Recurse -Force
        foreach ($file in Get-ChildItem -LiteralPath $destination -Recurse -File) {
            $copiedFiles.Add(('include/{0}/{1}' -f $name,
                $file.FullName.Substring($destination.Length + 1).Replace('\', '/')))
        }
        return "`@PKG`@/include/$name"
    }
    throw "Path escapes every shippable root ($Kind): $path"
}

# Ninja splits "-isystem <path>" into two list entries (the vendored Crypto++
# headers are declared SYSTEM, so they arrive that way), while plain aurora
# includes stay glued to -I. Only the directory itself travels in the package;
# the consumer re-emits it through INTERFACE_INCLUDE_DIRECTORIES and CMake
# chooses the flag form per usage context.
$packageIncludeDirectories = @()
for ($i = 0; $i -lt $includeArguments.Count; $i++) {
    $entry = $includeArguments[$i]
    if ($entry.StartsWith('-I')) {
        $packageIncludeDirectories += (ConvertTo-Token $entry.Substring(2) 'include')
    } elseif ($entry -eq '-isystem' -and ($i + 1) -lt $includeArguments.Count) {
        $i++
        $packageIncludeDirectories += (ConvertTo-Token $includeArguments[$i] 'include')
    } elseif ($entry.StartsWith('-isystem')) {
        $packageIncludeDirectories += (ConvertTo-Token $entry.Substring('-isystem'.Length) 'include')
    } else {
        throw "Unexpected include argument: $entry"
    }
}

$packageDefinitions = @($defineArguments | ForEach-Object {
    if (-not $_.StartsWith('-D')) { throw "Unexpected define argument: $_" }
    # Ninja carries the Windows command-line escaping; CMake re-applies it.
    $_.Substring(2).Replace('\"', '"')
})

$packageLinkItems = @($linkItems | ForEach-Object {
    $item = $_
    if ($item.StartsWith('-')) { return $item }
    $absolute = if ([IO.Path]::IsPathRooted($item)) { Normalize $item } else { Normalize (Join-Path $stage $item) }
    if ($linkerFileToReference.ContainsKey($absolute)) { return $linkerFileToReference[$absolute] }
    if (StartsWithPath $absolute $dependencyRoot) { return "`@DEPS`@/$(RelativeTo $absolute $dependencyRoot)" }
    throw "Link item is neither a system library nor a harvested archive: $item"
})

# fmt's `-include cstdlib` (and anything like it) is recorded from a C++ compile
# line, so re-scope it to C++. The runtime also assembles generated .S blobs
# through the same targets, and a forced C++ header include would break them.
$packageCompileOptions = @($compileOptions | ForEach-Object { "`$<`$<COMPILE_LANGUAGE:CXX>:$_>" })

# ---------------------------------------------------------------------------
# Emit the consumer-facing CMake description.
# ---------------------------------------------------------------------------
function Format-CMakeBlock([string]$Name, [string[]]$Items) {
    if ($null -eq $Items -or $Items.Count -eq 0) { return "set($Name `"`")`n" }
    $body = ($Items | ForEach-Object { '    "' + $_.Replace('\', '/').Replace('"', '\"') + '"' }) -join "`n"
    return "set($Name`n$body)`n"
}

$generated = [Collections.Generic.List[string]]::new()
$generated.Add('# Generated by launcher/Prepare-NativePrebuilt.ps1 - do not edit.')
$generated.Add('#')
$generated.Add('# Describes the precompiled aurora + third-party archives that replace a')
$generated.Add('# from-source aurora-main build. Every path is a token resolved against the')
$generated.Add('# consuming workspace, because the source trees still ship next to this')
$generated.Add('# package and the runtime compiles against their headers.')
$generated.Add('')
$generated.Add('if(NOT MKW_NP_PACKAGE_DIR OR NOT MKW_NP_AURORA_DIR OR NOT MKW_NP_RUNTIME_DIR OR NOT MKW_NP_DEPS_DIR)')
$generated.Add('    message(FATAL_ERROR "native_prebuilt.cmake must be included by runtime/cmake/NativePrebuilt.cmake")')
$generated.Add('endif()')
$generated.Add('')
$generated.Add((Format-CMakeBlock 'MKW_NP_INCLUDE_DIRECTORIES' $packageIncludeDirectories).TrimEnd())
$generated.Add((Format-CMakeBlock 'MKW_NP_COMPILE_DEFINITIONS' $packageDefinitions).TrimEnd())
$generated.Add((Format-CMakeBlock 'MKW_NP_COMPILE_OPTIONS' $packageCompileOptions).TrimEnd())
$generated.Add((Format-CMakeBlock 'MKW_NP_LINK_LIBRARIES' $packageLinkItems).TrimEnd())
$generated.Add((Format-CMakeBlock 'MKW_NP_AURORA_TARGETS' @('aurora::gx','aurora::pad','aurora::si','aurora::vi','aurora::mtx')).TrimEnd())
$generated.Add('')
$generated.Add("set(MKW_NP_DAWN_CONFIG_DIR `"$(ConvertTo-Token $meta['dawn_config_dir'] 'dawn')`")")
$generated.Add("set(MKW_NP_DAWN_VERSION `"$($meta['aurora_dawn_version'])`")")
$generated.Add('')
$generated.Add('# Shared third-party libraries keep imported SHARED targets so that')
$generated.Add('# $<TARGET_RUNTIME_DLLS> still copies them next to the game executable.')
foreach ($import in $sharedImports) {
    $generated.Add("add_library($($import.Target) SHARED IMPORTED GLOBAL)")
    $generated.Add("set_target_properties($($import.Target) PROPERTIES")
    $generated.Add("    IMPORTED_LOCATION `"`${MKW_NP_PACKAGE_DIR}/$($import.Runtime)`"")
    $generated.Add("    IMPORTED_IMPLIB `"`${MKW_NP_PACKAGE_DIR}/$($import.Implib)`")")
}
$generatedText = ($generated -join "`r`n") + "`r`n"
$generatedPath = Join-Path $package 'native_prebuilt.cmake'
[IO.File]::WriteAllText($generatedPath, $generatedText, (New-Object Text.UTF8Encoding($false)))

# --- Provenance ---
# The pinned Dawn tree is what this harvest compiled against; without its digest the
# consumer-side ABI check would silently pass on a mismatched Dawn.
$dawnRuntimeHash = Get-MkwDawnRuntimeSha256 $dependencies
if (-not $dawnRuntimeHash) {
    throw "The pinned Dawn runtime is missing: $(Join-Path $dependencies 'dawn_prebuilt\bin\webgpu_dawn.dll')"
}

$auroraSourceFingerprint = Get-MkwAuroraSourceFingerprint $auroraSource
if (-not $auroraSourceFingerprint) {
    throw "The aurora source tree could not be fingerprinted: $auroraSource"
}

# The harvested Crypto++ archive is consumed against this tree's headers, so it
# is recorded for the same reason as the aurora fingerprint above.
$thirdPartyDirectory = Join-Path $runtimeSource 'third_party'
$thirdPartySourceFingerprint = Get-MkwThirdPartySourceFingerprint $thirdPartyDirectory
if (-not $thirdPartySourceFingerprint) {
    throw "The vendored third-party tree could not be fingerprinted: $thirdPartyDirectory"
}

$contents = @(Get-ChildItem -LiteralPath $package -Recurse -File | ForEach-Object {
    [pscustomobject][ordered]@{
        Path = $_.FullName.Substring($package.Length + 1).Replace('\', '/')
        Bytes = $_.Length
        Sha256 = Get-MkwFileSha256 $_.FullName
    }
} | Sort-Object Path)

$provenance = [ordered]@{
    SchemaVersion = 1
    BuiltUtc = [DateTime]::UtcNow.ToString('O')
    # A precompiled archive is only interchangeable with locally compiled objects
    # when the compiler and the flag set are bit-for-bit the ones the user's
    # machine will use. LocalBuild.ps1 re-checks both before consuming this.
    CompilerSha256 = Get-MkwFileSha256 $clangBinary
    CompilerVersion = $meta['cxx_compiler_version']
    FlagFingerprint = Get-MkwNativeFlagFingerprint
    DawnVersion = $meta['aurora_dawn_version']
    # The version tag is not a content identity: the upstream prebuilt asset is
    # mutable, so record the digest of the Dawn runtime these archives were
    # actually compiled and linked against. Consumers re-check it.
    DawnRuntimeSha256 = $dawnRuntimeHash
    # libaurora_*.a is compiled from this tree, while the runtime that calls into
    # it is compiled on the user's machine. Record what was harvested so both the
    # release build and the local build can refuse a package aurora has outgrown.
    AuroraSourceFingerprint = $auroraSourceFingerprint
    ThirdPartySourceFingerprint = $thirdPartySourceFingerprint
    Sdl3Target = $meta['aurora_sdl3_target']
    HarvestedLibraryCount = @($linkerFileToReference.Keys).Count
    Contents = $contents
}
$provenance | ConvertTo-Json -Depth 6 |
    Set-Content -LiteralPath (Join-Path $package 'provenance.json') -Encoding UTF8

if (-not $KeepStage) { Remove-Item -LiteralPath $stage -Recurse -Force }

$totalBytes = ($contents | Measure-Object -Property Bytes -Sum).Sum
Write-Host ''
Write-Host "Native prebuilt package: $package"
Write-Host ("  files: {0}; size: {1:n1} MiB" -f $contents.Count, ($totalBytes / 1MB))
Write-Host ("  archives: {0}; shared imports: {1}" -f (@(Get-ChildItem (Join-Path $package 'lib') -File).Count), $sharedImports.Count)
Write-Host ("  flag fingerprint: {0}" -f $provenance.FlagFingerprint)
