# Build the real host runtime with translated, entirely synthetic PowerPC code.
# No game dump, game symbol map, REL, mod download, or existing generated/ output is used.
[CmdletBinding()]
param(
    [string]$PortableToolsDirectory = 'Launcher/artifacts/portable-tools',
    [string]$DependencySourceDirectory = 'Launcher/artifacts/dependencies',
    [string]$StageDirectory = 'build/recomp-test',
    [ValidateSet('auto', 'windows', 'linux', 'macos')] [string]$Platform = 'auto',
    [switch]$RunAuroraTests,
    [ValidateRange(1, 64)] [int]$Parallel = 3
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
. (Join-Path $PSScriptRoot 'NativeBuildFlags.ps1')

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
function Full([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}
$portableTools = Full $PortableToolsDirectory
$dependencies = Full $DependencySourceDirectory
$stage = Full $StageDirectory
$dotnet = (Get-Command dotnet -CommandType Application | Select-Object -First 1).Source
if ($Platform -eq 'auto') {
    if ($IsWindows) { $Platform = 'windows' }
    elseif ($IsLinux) { $Platform = 'linux' }
    elseif ($IsMacOS) { $Platform = 'macos' }
    else { throw 'Could not identify a supported host platform.' }
}
if (($Platform -eq 'windows') -ne $IsWindows -or
    ($Platform -eq 'linux') -ne $IsLinux -or
    ($Platform -eq 'macos') -ne $IsMacOS) {
    throw "Requested platform '$Platform' does not match this host."
}

if ($Platform -eq 'windows') {
    $cmake = Join-Path $portableTools 'CMake/bin/cmake.exe'
    $ctest = Join-Path $portableTools 'CMake/bin/ctest.exe'
    $ninja = Join-Path $portableTools 'Ninja/ninja.exe'
    $compilerBin = Join-Path $portableTools 'llvm-mingw/bin'
    Assert-File $cmake 'Pinned CMake (run Prepare-PortableTools.ps1 first)'
    Assert-File $ctest 'Pinned CTest'
    Assert-File $ninja 'Pinned Ninja'
    Assert-File (Join-Path $dependencies 'cppwinrt/winrt/base.h') 'Pinned dependencies (run Prepare-Dependencies.ps1 first)'
} else {
    $cmake = (Get-Command cmake -CommandType Application | Select-Object -First 1).Source
    $ctest = (Get-Command ctest -CommandType Application | Select-Object -First 1).Source
    $ninja = (Get-Command ninja -CommandType Application | Select-Object -First 1).Source
    if ($Platform -eq 'macos') {
        $cCompiler = '/usr/bin/clang'
        $cxxCompiler = '/usr/bin/clang++'
        Assert-File $cCompiler 'AppleClang C compiler'
        Assert-File $cxxCompiler 'AppleClang C++ compiler'
    } else {
        $cCompiler = (Get-Command clang -CommandType Application | Select-Object -First 1).Source
        $cxxCompiler = (Get-Command clang++ -CommandType Application | Select-Object -First 1).Source
    }
}

# Refuse reuse so a developer's game translation or an earlier build cannot make
# the test pass. Keep the staging tree after the run for diagnostics.
if (Test-Path -LiteralPath $stage) { throw "Test stage already exists; choose a fresh -StageDirectory: $stage" }
[IO.Directory]::CreateDirectory($stage) | Out-Null
Write-Host "Synthetic recompilation workspace: $stage"

# Copy current sources, including uncommitted edits, but no ignored build output.
# An isolated workspace preserves the developer's real generated/ directory.
$sourceFiles = & git -C $repoRoot -c core.quotepath=false ls-files --cached --others --exclude-standard -- runtime aurora-main
if ($LASTEXITCODE -ne 0) { throw 'Could not enumerate runtime and Aurora sources.' }
foreach ($relative in $sourceFiles | Sort-Object -Unique) {
    $destination = Join-Path $stage $relative
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) | Out-Null
    Copy-Item -LiteralPath (Join-Path $repoRoot $relative) -Destination $destination
}

# One synthetic text section: li r3,40; addi r3,r3,2; nop; blr.
# Native HLE wrappers also call these eight guest symbols directly. Give each
# its own generated blr function so the real product can link without game code.
# Keep this list explicit: a new unresolved guest dependency must fail the test.
[uint32]$entry = 0x80001000L
[uint32[]]$guestCallbacks = @(
    0x8012B830L, 0x801A0620L, 0x801A1ED8L, 0x801A961CL,
    0x801AADE0L, 0x801D8D30L, 0x801D9E94L, 0x8055531CL
)
$textSize = [int]($guestCallbacks[-1] - $entry + 4)
$dataOffset = 0x100 + $textSize
$dol = [byte[]]::new($dataOffset + 4)
function Write-BigEndian32([int]$Offset, [uint32]$Value) {
    $dol[$Offset] = [byte](($Value -shr 24) -band 255)
    $dol[$Offset + 1] = [byte](($Value -shr 16) -band 255)
    $dol[$Offset + 2] = [byte](($Value -shr 8) -band 255)
    $dol[$Offset + 3] = [byte]($Value -band 255)
}
Write-BigEndian32 0x00 0x100       # text[0] file offset
Write-BigEndian32 0x48 $entry      # text[0] guest address
Write-BigEndian32 0x90 $textSize   # text[0] length (unreachable gaps are zero)
Write-BigEndian32 0x1C $dataOffset # data[0] file offset
Write-BigEndian32 0x64 0x80600000L # data[0] guest address
Write-BigEndian32 0xAC 4           # data[0] length
Write-BigEndian32 0xD8 0x80601000L # BSS address
Write-BigEndian32 0xDC 32          # BSS length
Write-BigEndian32 0xE0 $entry      # entry point
Write-BigEndian32 0x100 0x38600028 # li r3,40
Write-BigEndian32 0x104 0x38630002 # addi r3,r3,2
Write-BigEndian32 0x108 0x60000000 # nop
Write-BigEndian32 0x10C 0x4E800020 # blr
foreach ($address in $guestCallbacks) {
    Write-BigEndian32 ([int](0x100 + $address - $entry)) 0x4E800020
}
Write-BigEndian32 $dataOffset 0x12345678
[IO.File]::WriteAllBytes((Join-Path $stage 'synthetic.dol'), $dol)
$entryPoints = (@($entry) + $guestCallbacks | ForEach-Object { '0x{0:X8}' -f $_ }) -join ', '
$functionMap = (@($entry) + $guestCallbacks | ForEach-Object { '{0:X8} func_{0:X8}' -f $_ }) -join "`n"
[IO.File]::WriteAllText((Join-Path $stage 'synthetic-functions.txt'), $functionMap)
$manifest = Join-Path $stage 'recomp.yml'
[IO.File]::WriteAllText($manifest, @"
schema_version: 1
workspace_root: .
project:
  id: ci-synthetic-dol
  display_name: CI Synthetic DOL
memory:
  base: 0x80000000
  size: 0x01800000
  sda_base: 0x80600000
  sda2_base: 0x80600000
inputs:
  dol:
    path: synthetic.dol
translation:
  entry_points: [$entryPoints]
  function_map:
    path: synthetic-functions.txt
  allow_unsupported_instructions: false
runtime:
  native_abi_directories: []
  native_registration_root: runtime/src
output:
  root: generated
"@)

$translatorProject = Join-Path $repoRoot 'translator/src/Translator.Cli/Translator.Cli.csproj'
Invoke-Checked $dotnet @('restore', $translatorProject, '--locked-mode', '--disable-build-servers', '-m:1') `
    'Restoring locked translator dependencies'
Invoke-Checked $dotnet @('build', $translatorProject, '-c', 'Release', '--no-restore', '--disable-build-servers', '-m:1') `
    'Building the translator'
$translator = Join-Path $repoRoot 'translator/src/Translator.Cli/bin/Release/net10.0/Translator.Cli.dll'
$metadata = Join-Path $stage 'generated/base_translation_output.json'
Invoke-Checked $dotnet @($translator, 'translate-recursive', '0x80001000', '--project', $manifest,
    '--output-metadata', $metadata, '--threads', "$Parallel") `
    'Translating the synthetic DOL'
# Function-map seeds can be skipped by discovery; do not accept a partial fixture.
$translated = Get-Content -LiteralPath $metadata -Raw | ConvertFrom-Json
foreach ($address in @($entry) + $guestCallbacks) {
    if ($address -notin $translated.functions.entryPoint) {
        throw ('Synthetic function 0x{0:X8} was not translated.' -f $address)
    }
}
Invoke-Checked $dotnet @($translator, 'generate-data-init', '--project', $manifest) 'Generating synthetic data and runtime configuration'
Invoke-Checked $dotnet @($translator, 'emit-build-shards', '--project', $manifest) 'Emitting the production build graph'

$nativeBuild = Join-Path $stage 'native-build'
$testResults = Join-Path $stage 'test-results'
[IO.Directory]::CreateDirectory($testResults) | Out-Null
$oldPath = $env:PATH
try {
    if ($Platform -eq 'windows') {
        $env:PATH = Get-MkwToolchainPath $portableTools
        $additionalArguments = @(
            '-DMKW_BUILD_PRODUCTS=ON', "-DMKW_TRANSLATED_COMPILE_JOBS=$Parallel")
        if (-not [string]::IsNullOrWhiteSpace($env:SCCACHE_PATH)) {
            $additionalArguments += "-DCMAKE_C_COMPILER_LAUNCHER=$($env:SCCACHE_PATH)"
            $additionalArguments += "-DCMAKE_CXX_COMPILER_LAUNCHER=$($env:SCCACHE_PATH)"
        }
        $configure = Get-MkwNativeConfigureArguments -SourceDirectory (Join-Path $stage 'runtime') -BuildDirectory $nativeBuild `
            -Ninja $ninja -CCompiler (Join-Path $compilerBin 'x86_64-w64-mingw32-clang.exe') `
            -CxxCompiler (Join-Path $compilerBin 'x86_64-w64-mingw32-clang++.exe') `
            -ResourceCompiler (Join-Path $compilerBin 'x86_64-w64-mingw32-windres.exe') `
            -DependenciesDirectory $dependencies -AdditionalArguments $additionalArguments
    } else {
        $configure = @(
            '-S', (Join-Path $stage 'runtime'), '-B', $nativeBuild, '-G', 'Ninja',
            "-DCMAKE_MAKE_PROGRAM=$ninja", "-DCMAKE_C_COMPILER=$cCompiler",
            "-DCMAKE_CXX_COMPILER=$cxxCompiler", '-DCMAKE_BUILD_TYPE=Release',
            '-DMKW_BUILD_PRODUCTS=ON', "-DMKW_TRANSLATED_COMPILE_JOBS=$Parallel")
        if (-not [string]::IsNullOrWhiteSpace($env:SCCACHE_PATH)) {
            $configure += "-DCMAKE_C_COMPILER_LAUNCHER=$($env:SCCACHE_PATH)"
            $configure += "-DCMAKE_CXX_COMPILER_LAUNCHER=$($env:SCCACHE_PATH)"
        }
        if (-not [string]::IsNullOrWhiteSpace($env:FETCHCONTENT_BASE_DIR)) {
            $configure += "-DFETCHCONTENT_BASE_DIR=$(Join-Path $env:FETCHCONTENT_BASE_DIR 'runtime')"
        }
    }
    Invoke-Checked $cmake $configure "Configuring the production $Platform runtime" `
        -WaitForProcessTree $false
    Invoke-Checked $cmake @('--build', $nativeBuild, '--parallel', "$Parallel") `
        'Compiling the runtime tests and linking the synthetic product' `
        -WaitForProcessTree $false
    $productName = if ($Platform -eq 'windows') { 'WiiCompiled.exe' } else { 'WiiCompiled' }
    Assert-File (Join-Path $nativeBuild $productName) 'Linked synthetic product'
    Invoke-Checked $ctest @('--test-dir', $nativeBuild, '--output-on-failure', '--output-junit',
        (Join-Path $testResults 'runtime.xml')) 'Running runtime CTests' -WaitForProcessTree $false

    if ($RunAuroraTests) {
        $auroraBuild = Join-Path $stage 'aurora-tests-build'
        $auroraConfigure = @(
            '-S', (Join-Path $stage 'aurora-main'), '-B', $auroraBuild, '-G', 'Ninja',
            "-DCMAKE_MAKE_PROGRAM=$ninja", '-DCMAKE_BUILD_TYPE=Release',
            '-DAURORA_ENABLE_DVD=OFF', '-DFETCHCONTENT_FULLY_DISCONNECTED=OFF')
        if ($Platform -eq 'windows') {
            $auroraConfigure += "-DCMAKE_C_COMPILER=$(Join-Path $compilerBin 'x86_64-w64-mingw32-clang.exe')"
            $auroraConfigure += "-DCMAKE_CXX_COMPILER=$(Join-Path $compilerBin 'x86_64-w64-mingw32-clang++.exe')"
            $auroraConfigure += '-DAURORA_DAWN_PROVIDER=package'
            $auroraConfigure += '-DAURORA_SDL3_PROVIDER=vendor'
            foreach ($directory in Get-ChildItem -LiteralPath $dependencies -Directory) {
                if ($directory.Name -eq 'native_prebuilt') { continue }
                $name = $directory.Name.ToUpperInvariant()
                $auroraConfigure += "-DFETCHCONTENT_SOURCE_DIR_$name=$($directory.FullName.Replace('\', '/'))"
            }
        } else {
            $auroraConfigure += "-DCMAKE_C_COMPILER=$cCompiler"
            $auroraConfigure += "-DCMAKE_CXX_COMPILER=$cxxCompiler"
        }
        if (-not [string]::IsNullOrWhiteSpace($env:SCCACHE_PATH)) {
            $auroraConfigure += "-DCMAKE_C_COMPILER_LAUNCHER=$($env:SCCACHE_PATH)"
            $auroraConfigure += "-DCMAKE_CXX_COMPILER_LAUNCHER=$($env:SCCACHE_PATH)"
        }
        if (-not [string]::IsNullOrWhiteSpace($env:FETCHCONTENT_BASE_DIR)) {
            $auroraConfigure += "-DFETCHCONTENT_BASE_DIR=$(Join-Path $env:FETCHCONTENT_BASE_DIR 'aurora')"
            $runtimeFetchContent = Join-Path $env:FETCHCONTENT_BASE_DIR 'runtime'
            if (Test-Path -LiteralPath $runtimeFetchContent -PathType Container) {
                foreach ($source in Get-ChildItem -LiteralPath $runtimeFetchContent -Directory -Filter '*-src') {
                    $name = $source.Name.Substring(0, $source.Name.Length - 4).ToUpperInvariant()
                    $auroraConfigure += "-DFETCHCONTENT_SOURCE_DIR_$name=$($source.FullName)"
                }
            }
        }
        Invoke-Checked $cmake $auroraConfigure 'Configuring standalone Aurora tests' -WaitForProcessTree $false
        Invoke-Checked $cmake @('--build', $auroraBuild, '--target', 'gx_fifo_tests', 'os_alloc_tests',
            '--parallel', "$Parallel") 'Building standalone Aurora tests' -WaitForProcessTree $false
        Invoke-Checked $ctest @('--test-dir', $auroraBuild, '--output-on-failure', '--output-junit',
            (Join-Path $testResults 'aurora.xml')) 'Running standalone Aurora CTests' -WaitForProcessTree $false
    }
} finally {
    $env:PATH = $oldPath
}
Write-Host 'Synthetic recompilation passed (translation, data generation, runtime tests, and product link).'
