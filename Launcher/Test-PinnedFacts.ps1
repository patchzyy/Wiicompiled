# Fails the release build when a fact duplicated across the repo stops agreeing with the copy
# that owns it (recomp.yml). Scripts read pinned facts through Get-MkwProjectPins, but three
# consumers can't read YAML (the C++ runtime header, the C# constants, shell scripts, and hand-written lists on
# both sides of the C#/PowerShell boundary), so those are checked here instead.
[CmdletBinding()]
param([string]$RepositoryRoot)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

. (Join-Path $PSScriptRoot 'NativeBuildFlags.ps1')

if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = Join-Path $PSScriptRoot '..'
}
$repoRoot = [IO.Path]::GetFullPath($RepositoryRoot)
$launcher = Join-Path $repoRoot 'Launcher'
$setup = Join-Path $launcher 'WiiCompiled.Setup.Windows'
$common = Join-Path $launcher 'WiiCompiled.Setup.Common'

$failures = [Collections.Generic.List[string]]::new()
function Add-Failure([string]$Message) { $failures.Add($Message) }

function Read-SourceFile([string]$Path, [string]$Description) {
    Assert-File $Path $Description
    return [IO.File]::ReadAllText($Path)
}

function Get-CapturedValue([string]$Text, [string]$Pattern, [string]$Description) {
    $match = [regex]::Match($Text, $Pattern)
    if (-not $match.Success) { throw "$Description could not be located; update Test-PinnedFacts.ps1." }
    return $match.Groups[1].Value
}

function Get-QuotedList([string]$Text, [string]$BlockPattern, [string]$ItemPattern, [string]$Description) {
    $block = Get-CapturedValue $Text $BlockPattern $Description
    $items = [regex]::Matches($block, $ItemPattern) | ForEach-Object { $_.Groups[1].Value }
    if (@($items).Count -eq 0) { throw "$Description is empty; update Test-PinnedFacts.ps1." }
    return [string[]]@($items)
}

function Compare-Set([string[]]$Expected, [string[]]$Actual, [string]$ExpectedName, [string]$ActualName) {
    $missing = @($Expected | Where-Object { $Actual -notcontains $_ })
    $extra = @($Actual | Where-Object { $Expected -notcontains $_ })
    if ($missing.Count -gt 0) { Add-Failure "$ActualName is missing: $($missing -join ', ') (present in $ExpectedName)." }
    if ($extra.Count -gt 0) { Add-Failure "$ActualName has entries $ExpectedName does not: $($extra -join ', ')." }
}

$pins = Get-MkwProjectPins (Join-Path $repoRoot 'projects\mkwii\recomp.yml')

# --- The Retro-WFC endpoint: recomp.yml owns it; the installer host pins the same string so a
# --- redirected or rewritten endpoint cannot be fetched from. The literal lives in
# --- WiiCompiled.Setup.Common (shared with WiiCompiled.Setup.Linux) - InputValidation.cs only
# --- re-exports it as `= RetroWfcPayload.CurrentRetroWfcPayloadUri;`, no literal to capture there.
$retroWfcPayload = Read-SourceFile (Join-Path $common 'RetroWfcPayload.cs') 'RetroWfcPayload.cs'
$hostUri = Get-CapturedValue $retroWfcPayload 'CurrentRetroWfcPayloadUri\s*=\s*"([^"]+)"' `
    'The host Retro-WFC endpoint constant'
if ($hostUri -cne $pins.RetroWfcPayloadUri) {
    Add-Failure "InputValidation.CurrentRetroWfcPayloadUri is '$hostUri' but recomp.yml pins '$($pins.RetroWfcPayloadUri)'."
}
$macosSetup = Read-SourceFile (Join-Path $launcher 'macos\setup.command') 'macOS setup.command'
$macosUri = Get-CapturedValue $macosSetup "'([^']*/api/wfc/payload\?g=RMCPD00)'" `
    'The macOS Retro-WFC endpoint'
if ($macosUri -cne $pins.RetroWfcPayloadUri) {
    Add-Failure "macOS setup.command downloads '$macosUri' but recomp.yml pins '$($pins.RetroWfcPayloadUri)'."
}

# --- The game identity: the manifest carries it, but the host also compiles a fallback for a
# --- manifest that predates the field, and that fallback decides which disc is accepted.
$models = Read-SourceFile (Join-Path $setup 'Models.cs') 'Models.cs'
$hostGameId = Get-CapturedValue $models 'ExpectedGameId\s*\{\s*get;\s*set;\s*\}\s*=\s*"([^"]+)"' `
    'The host expected game id'
if ($hostGameId -cne $pins.GameId) {
    Add-Failure "PayloadManifest.ExpectedGameId defaults to '$hostGameId' but recomp.yml pins '$($pins.GameId)'."
}

# --- The translation entry point: the translator is told it by LocalBuild.ps1 (which reads
# --- recomp.yml), while the runtime enters the translated image at its own constant.
$systemBridge = Read-SourceFile (Join-Path $repoRoot 'runtime\include\system_bridge.h') 'system_bridge.h'
$runtimeEntry = (Get-CapturedValue $systemBridge 'kDefaultEntryAddress\s*=\s*(0[xX][0-9a-fA-F]+)' `
    'The runtime default entry address').ToLowerInvariant()
if ($runtimeEntry -ne $pins.EntryPoint) {
    Add-Failure "system_bridge.h enters at $runtimeEntry but recomp.yml translates from $($pins.EntryPoint)."
}

# --- The pinned dependency set: Build-Installer.ps1 stages it, the installed host requires it.
$installedLayout = Read-SourceFile (Join-Path $setup 'InstalledLayout.cs') 'InstalledLayout.cs'
$buildInstaller = Read-SourceFile (Join-Path $launcher 'Build-Installer.ps1') 'Build-Installer.ps1'
Compare-Set (Get-QuotedList $installedLayout '(?s)DependencyNames\s*=\s*\[(.*?)\]' '"([^"]+)"' `
        'InstalledLayout.DependencyNames') `
    (Get-QuotedList $buildInstaller '(?s)\$requiredDependencies\s*=\s*@\((.*?)\)' "'([^']+)'" `
        'Build-Installer.ps1 $requiredDependencies') `
    'InstalledLayout.DependencyNames' 'Build-Installer.ps1 $requiredDependencies'

# --- The runtime assets copied beside a product: the host hashes exactly these names, so the build
# --- script must publish every one of them or every product reports its support files as stale.
$localBuild = Read-SourceFile (Join-Path $launcher 'LocalBuild.ps1') 'LocalBuild.ps1'
$productAssets = @(Get-CapturedValue $installedLayout 'ProductBootstrapDirectoryName\s*=\s*"([^"]+)"' `
        'The product bootstrap directory name') +
    @(Get-QuotedList $installedLayout '(?s)ProductFileName\)\[\]\s*Files\s*=\s*\[(.*?)\n\s*\];' `
        '\]\s*,\s*"([^"]+)"\s*\)' 'ProductRuntimeAssets.Files')
foreach ($asset in $productAssets) {
    if ($localBuild.IndexOf("'$asset'", [StringComparison]::Ordinal) -lt 0) {
        Add-Failure "LocalBuild.ps1 never publishes the product runtime asset '$asset'."
    }
}

# --- The command-line contract between the host and the build script.
$localBuildService = Read-SourceFile (Join-Path $setup 'LocalBuildService.cs') 'LocalBuildService.cs'
$parameterBlock = Get-CapturedValue $localBuild '(?sm)^param\((.*?)\r?\n\)' 'The LocalBuild.ps1 param block'
$scriptParameters = [regex]::Matches($parameterBlock, '\$(\w+)') | ForEach-Object { $_.Groups[1].Value }
# powershell.exe's own switches travel in the same argument list and are not script parameters.
$hostSwitches = @('NoLogo', 'NoProfile', 'NonInteractive', 'ExecutionPolicy', 'File')
$passedSwitches = [regex]::Matches($localBuildService, '"-([A-Za-z]\w*)"') |
    ForEach-Object { $_.Groups[1].Value } | Where-Object { $hostSwitches -notcontains $_ } |
    Sort-Object -Unique
foreach ($switch in $passedSwitches) {
    if ($scriptParameters -notcontains $switch) {
        Add-Failure "LocalBuildService.cs passes -$switch, which LocalBuild.ps1 does not declare."
    }
}
foreach ($mandatory in ([regex]::Matches($parameterBlock,
        '\[Parameter\(Mandatory\)\][^$]*\$(\w+)') | ForEach-Object { $_.Groups[1].Value })) {
    if ($passedSwitches -notcontains $mandatory) {
        Add-Failure "LocalBuild.ps1 requires -$mandatory, which LocalBuildService.cs never passes."
    }
}

# --- The build steps the installer's progress bar acts on.
$installProgress = Read-SourceFile (Join-Path $setup 'InstallProgress.cs') 'InstallProgress.cs'
$knownSteps = Get-QuotedList $installProgress '(?s)class BuildStepIds\s*\{(.*?)\n\}' `
    'const string \w+ = "([^"]+)";' 'BuildStepIds'
$emittedSteps = @([regex]::Matches($localBuild, "(?:-StepId|Write-MkwBuildStep)\s+'([^']+)'") |
    ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
Compare-Set $knownSteps $emittedSteps 'BuildStepIds' 'the steps LocalBuild.ps1 emits'

if ($failures.Count -gt 0) {
    throw "Pinned facts disagree between their copies:`n  $($failures -join "`n  ")"
}
Write-Host ("Pinned-fact audit passed: game {0}, entry {1}, endpoint {2}; dependency, runtime-asset, " -f
    $pins.GameId, $pins.EntryPoint, $pins.RetroWfcPayloadUri) -NoNewline
Write-Host 'build-step, and build-script parameter contracts all agree.'
