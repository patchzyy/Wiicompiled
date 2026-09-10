# Build the real Windows runtime with translated, entirely synthetic PowerPC code.
# No game dump, game symbol map, REL, mod download, or existing generated/ output is used.
[CmdletBinding()]
param(
    [string]$PortableToolsDirectory = 'Launcher/artifacts/portable-tools',
    [string]$DependencySourceDirectory = 'Launcher/artifacts/dependencies',
    [string]$StageDirectory = 'build/recomp-test',
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
$dotnet = (Get-Command dotnet -CommandType Application).Source
$cmake = Join-Path $portableTools 'CMake/bin/cmake.exe'
$ninja = Join-Path $portableTools 'Ninja/ninja.exe'
$compilerBin = Join-Path $portableTools 'llvm-mingw/bin'
Assert-File $cmake 'Pinned CMake (run Prepare-PortableTools.ps1 first)'
Assert-File $ninja 'Pinned Ninja'
Assert-File (Join-Path $dependencies 'cppwinrt/winrt/base.h') 'Pinned dependencies (run Prepare-Dependencies.ps1 first)'

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
Invoke-Checked $dotnet @('build', $translatorProject, '-c', 'Release', '--disable-build-servers') 'Building the translator'
$translator = Join-Path $repoRoot 'translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll'
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
$oldPath = $env:PATH
try {
    $env:PATH = Get-MkwToolchainPath $portableTools
    $configure = Get-MkwNativeConfigureArguments -SourceDirectory (Join-Path $stage 'runtime') -BuildDirectory $nativeBuild `
        -Ninja $ninja -CCompiler (Join-Path $compilerBin 'x86_64-w64-mingw32-clang.exe') `
        -CxxCompiler (Join-Path $compilerBin 'x86_64-w64-mingw32-clang++.exe') `
        -ResourceCompiler (Join-Path $compilerBin 'x86_64-w64-mingw32-windres.exe') `
        -DependenciesDirectory $dependencies -AdditionalArguments @('-DMKW_BUILD_PRODUCTS=ON')
    Invoke-Checked $cmake $configure 'Configuring the production Windows runtime'
    Invoke-Checked $cmake @('--build', $nativeBuild, '--target', 'WiiCompiled', '--parallel', "$Parallel") `
        'Compiling and linking the synthetic product with the full runtime'
    Assert-File (Join-Path $nativeBuild 'WiiCompiled.exe') 'Linked synthetic product'
} finally {
    $env:PATH = $oldPath
}
Write-Host 'Synthetic recompilation passed (translation, data generation, runtime compilation, and product link).'
