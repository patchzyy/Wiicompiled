[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$Workspace,
    [Parameter(Mandatory)] [string]$Toolkit,
    # 'both' translates once (retro-aware) and compiles the two products from one build
    # graph, which shares every profile-neutral shard object between them. Building the
    # legs separately recompiles all of mkw_base_shared in the second leg, because the
    # retro-aware shard emission changes the content identity of every base_common shard.
    [Parameter(Mandatory)] [ValidateSet('base', 'retro-rewind', 'both')] [string]$Profile,
    [Parameter(Mandatory)] [string]$OutputDirectory,
    [string]$BaseOutputDirectory,
    [string]$RetroRewindPackageDirectory,
    [string]$RetroWfcOfflineDirectory,
    [ValidateSet('offline', 'downloaded')] [string]$RetroWfcPayloadOrigin = 'offline',
    [switch]$SkipRetroWfcPayload,
    # The caller's recomputed cache-reuse identities (see ToolkitFingerprint.ComputeComponents).
    # They are compared against the provenance recorded beside the caches themselves; a missing or
    # mismatched identity degrades that cache to a clean rebuild, never the other way around.
    [string]$TranslationFingerprint = '',
    [string]$NativeToolchainFingerprint = '',
    # Discards every cache first. Used when a product was reported broken/blocked, so a possibly
    # corrupted translation or build directory can never contribute to the repaired product.
    [switch]$ForceCleanBuild,
    [int]$Parallel = 0
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

# Canonical configure flags (plus Assert-File / Invoke-Checked / Get-MkwFileSha256),
# shared with launcher/Prepare-NativePrebuilt.ps1 so the precompiled aurora/third-party
# archives and the objects compiled here can never be produced with different settings.
# See NativeBuildFlags.ps1; Build-Installer.ps1 stages it beside this script.
. (Join-Path $PSScriptRoot 'NativeBuildFlags.ps1')

function Resolve-NativePrebuiltDirectory([string]$DependenciesDirectory, [string]$ToolchainBin, [string]$AuroraDirectory, [string]$ThirdPartyDirectory) {
    <#
    The shipped precompiled aurora/third-party package, verified against its recorded provenance
    (exact compiler, exact flags) before use. Anything unexpected falls back to building from source.
    #>
    $package = Join-Path $DependenciesDirectory 'native_prebuilt'
    $provenancePath = Join-Path $package 'provenance.json'
    if (-not (Test-Path -LiteralPath $provenancePath -PathType Leaf)) {
        Write-Host 'MKWCBUILD: No precompiled aurora package was shipped; building it from source'
        return ''
    }
    try {
        $provenance = Get-Content -LiteralPath $provenancePath -Raw | ConvertFrom-Json
        $compiler = Join-Path $ToolchainBin 'clang-22.exe'
        $compilerHash = Get-MkwFileSha256 $compiler
        if ($provenance.CompilerSha256 -ne $compilerHash) {
            Write-Host 'MKWCBUILD: The precompiled aurora package was built by a different compiler; building it from source'
            return ''
        }
        if ($provenance.FlagFingerprint -ne (Get-MkwNativeFlagFingerprint)) {
            Write-Host 'MKWCBUILD: The precompiled aurora package was built with different flags; building it from source'
            return ''
        }
        # The archives embed Dawn's headers and link its import library, so a
        # dawn_prebuilt tree that is not the one they were harvested against is
        # an ABI mismatch that only shows up as a crash inside the GPU backend.
        $recordedDawn = $provenance.PSObject.Properties['DawnRuntimeSha256']
        if ($null -eq $recordedDawn -or
            $recordedDawn.Value -ne (Get-MkwDawnRuntimeSha256 $DependenciesDirectory)) {
            Write-Host 'MKWCBUILD: The precompiled aurora package was built against a different Dawn; building it from source'
            return ''
        }
        # The archives are aurora; the runtime compiled here calls its headers.
        # If this workspace's aurora sources are not the ones that were harvested
        # (a newer toolkit, or a locally modified tree), linking the package would
        # mix two versions of aurora, so build it from source instead.
        $recordedAurora = $provenance.PSObject.Properties['AuroraSourceFingerprint']
        if ($null -eq $recordedAurora -or
            $recordedAurora.Value -ne (Get-MkwAuroraSourceFingerprint $AuroraDirectory)) {
            Write-Host 'MKWCBUILD: The precompiled aurora package was built from different aurora sources; building it from source'
            return ''
        }
        # Same reasoning for the vendored Crypto++ the package also carries:
        # first-party TUs compile against this workspace's third_party headers,
        # so a package harvested from an older tree must not be linked here.
        $recordedThirdParty = $provenance.PSObject.Properties['ThirdPartySourceFingerprint']
        if ($null -eq $recordedThirdParty -or
            $recordedThirdParty.Value -ne (Get-MkwThirdPartySourceFingerprint $ThirdPartyDirectory)) {
            Write-Host 'MKWCBUILD: The precompiled aurora package was built from different vendored third-party sources; building it from source'
            return ''
        }
    } catch {
        Write-Host "MKWCBUILD: The precompiled aurora package could not be validated ($($_.Exception.Message)); building it from source"
        return ''
    }
    Write-Host 'MKWCBUILD: Using the precompiled aurora and third-party libraries'
    return $package
}

function Write-MkwBuildStep([string]$StepId, [string]$Message) {
    <#
    Announces a build step. The identifier is the contract (BuildStepIds in
    WiiCompiled.Setup/InstallProgress.cs); the trailing sentence is only for the log.
    #>
    Write-Host "MKWCBUILD:STEP:${StepId} $Message"
}

function Reset-LocalDirectory([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    $root = [IO.Path]::GetFullPath($Workspace).TrimEnd('\') + '\'
    $installRoot = [IO.Path]::GetFullPath((Split-Path -Parent $Workspace)).TrimEnd('\') + '\'
    # The caller-supplied output destinations are legitimate reset targets by
    # definition, wherever the caller placed them: a fresh install's operation
    # scratch lives beside the installation directory rather than inside it.
    $allowedDestinations = @($OutputDirectory, $BaseOutputDirectory) |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        ForEach-Object { [IO.Path]::GetFullPath($_).TrimEnd('\') }
    $isAllowedDestination = $allowedDestinations |
        Where-Object { $full.TrimEnd('\').Equals($_, [StringComparison]::OrdinalIgnoreCase) }
    if (-not $isAllowedDestination -and
        -not $full.StartsWith($root, [StringComparison]::OrdinalIgnoreCase) -and
        -not $full.StartsWith($installRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to reset a directory outside the local build workspace: $full"
    }
    if (Test-Path -LiteralPath $full) { Remove-Item -LiteralPath $full -Recurse -Force }
    [IO.Directory]::CreateDirectory($full) | Out-Null
}

$Workspace = [IO.Path]::GetFullPath($Workspace)
$Toolkit = [IO.Path]::GetFullPath($Toolkit)
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (-not [string]::IsNullOrWhiteSpace($BaseOutputDirectory)) {
    $BaseOutputDirectory = [IO.Path]::GetFullPath($BaseOutputDirectory)
}
if (-not [string]::IsNullOrWhiteSpace($RetroRewindPackageDirectory)) {
    $RetroRewindPackageDirectory = [IO.Path]::GetFullPath($RetroRewindPackageDirectory)
}
if (-not [string]::IsNullOrWhiteSpace($RetroWfcOfflineDirectory)) {
    $RetroWfcOfflineDirectory = [IO.Path]::GetFullPath($RetroWfcOfflineDirectory)
}
$hasOfflineRetroWfc = -not [string]::IsNullOrWhiteSpace($RetroWfcOfflineDirectory)
$buildsRetro = $Profile -in @('retro-rewind', 'both')
if (-not $buildsRetro -and ($hasOfflineRetroWfc -or $SkipRetroWfcPayload)) {
    throw 'Retro-WFC payload options are valid only for a Retro Rewind build.'
}
if ($buildsRetro -and ($hasOfflineRetroWfc -eq [bool]$SkipRetroWfcPayload)) {
    throw 'Choose exactly one Retro-WFC mode: -RetroWfcOfflineDirectory or -SkipRetroWfcPayload.'
}
if (-not $buildsRetro -and -not [string]::IsNullOrWhiteSpace($RetroRewindPackageDirectory)) {
    throw '-RetroRewindPackageDirectory is valid only for a Retro Rewind build.'
}
if ($Profile -eq 'both' -and [string]::IsNullOrWhiteSpace($BaseOutputDirectory)) {
    throw "-BaseOutputDirectory is required with -Profile both; -OutputDirectory receives the Retro Rewind product."
}
if ($Profile -ne 'both' -and -not [string]::IsNullOrWhiteSpace($BaseOutputDirectory)) {
    throw '-BaseOutputDirectory is valid only with -Profile both.'
}
$translator = Join-Path $Toolkit 'Translator\Translator.Cli.exe'
$toolchain = Get-MkwShellSafeToolchainRoot $Toolkit
$cmake = Join-Path $toolchain 'CMake\bin\cmake.exe'
$ninja = Join-Path $toolchain 'Ninja\ninja.exe'
$toolchainBin = Join-Path $toolchain 'llvm-mingw\bin'
$cc = Join-Path $toolchainBin 'x86_64-w64-mingw32-clang.exe'
$cxx = Join-Path $toolchainBin 'x86_64-w64-mingw32-clang++.exe'
$windres = Join-Path $toolchainBin 'x86_64-w64-mingw32-windres.exe'
$project = Join-Path $Workspace 'projects\mkwii\recomp.yml'
$assets = Join-Path $Workspace 'Assets'
$generated = Join-Path $Workspace 'generated'
$functions = Join-Path $generated 'functions'
$baseMetadata = Join-Path $generated 'base_translation_output.json'
$baseManifestDir = Join-Path $Workspace 'build\base'
$baseManifest = Join-Path $baseManifestDir 'mkwii_base_manifest.json'
$shards = Join-Path $generated 'build_shards'
$build = Join-Path $Workspace 'native-build'
$retroRoot = if ([string]::IsNullOrWhiteSpace($RetroRewindPackageDirectory)) {
    Join-Path $Workspace 'PulsarPacks\completed\RetroRewind\RetroRewind6'
} else {
    $RetroRewindPackageDirectory
}

foreach ($required in @(
    @($translator, 'Bundled translator'), @($cmake, 'Bundled CMake'), @($ninja, 'Bundled Ninja'),
    @($cc, 'Bundled C compiler'), @($cxx, 'Bundled C++ compiler'), @($windres, 'Bundled resource compiler'), @($project, 'Translation project'),
    @((Join-Path $assets 'main.dol'), 'Extracted main.dol'), @((Join-Path $assets 'StaticR.rel'), 'Extracted StaticR.rel')
)) { Assert-File $required[0] $required[1] }

# Everything Mario-Kart-specific this script needs is read from the project file rather than
# restated here: the translation entry point below, and (through the retro-rewind profile the
# translator loads itself) the module guest/link bases the mod translation uses.
$pins = Get-MkwProjectPins $project

# Three independent knobs: $translatorThreads (the translator's own worker threads; peak memory is
# one heap that barely moves with thread count, so clamping it only slows translation), $translatedJobs
# (the real RAM guard, capping concurrent clang compiles of memory-hungry translated TUs via the Ninja
# MKW_TRANSLATED_COMPILE_JOBS pool), and $globalJobs (Ninja's overall parallelism for everything else).
# An explicit -Parallel pins all three.
$memoryGiB = [math]::Max(1, [math]::Floor((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB))
if ($Parallel -gt 0) {
    $translatorThreads = $Parallel
    $translatedJobs = $Parallel
    $globalJobs = $Parallel
} else {
    $translatorThreads = [math]::Max(1, [math]::Min([Environment]::ProcessorCount, 16))
    $translatedJobs = [math]::Max(1, [math]::Min([Environment]::ProcessorCount,
        [math]::Floor($memoryGiB / 2)))
    $globalJobs = [math]::Max($translatedJobs, [Environment]::ProcessorCount)
}

$oldPath = $env:PATH
$oldDotnet = $env:DOTNET_ROOT
try {
    $env:PATH = Get-MkwToolchainPath $toolchain
    Remove-Item Env:DOTNET_ROOT -ErrorAction SilentlyContinue
    Push-Location $Workspace
    try {
        # This script is the single owner of every cache-reuse decision: it is what would misbuild.
        # The caller only supplies the identities to compare recorded provenance against.
        $translationProvenance = Join-Path $generated 'translation-provenance.json'
        $toolchainProvenance = Join-Path $build 'toolchain-provenance.json'

        function Get-RecordedFingerprint([string]$Path, [string]$Property) {
            if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return '' }
            try {
                $record = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
                $value = $record.PSObject.Properties[$Property]
                if ($null -eq $value -or [string]::IsNullOrWhiteSpace($value.Value)) { return '' }
                return [string]$value.Value
            } catch { return '' }
        }

        if ($ForceCleanBuild) {
            Write-Host 'MKWCBUILD: A clean build was requested; discarding every translation and build cache'
            Reset-LocalDirectory $generated
            if (Test-Path -LiteralPath $baseManifestDir) { Remove-Item -LiteralPath $baseManifestDir -Recurse -Force }
            if (Test-Path -LiteralPath $build) { Remove-Item -LiteralPath $build -Recurse -Force }
        }

        # Reused only when its exact inputs match, its artifacts survived, and (for a modded build) it
        # already knows this Code.pul. Otherwise it retranslates incrementally: content-addressed
        # outputs plus --prune-stale mean Ninja only recompiles shards whose bytes actually moved.
        $reuseBase = $false
        if (-not $ForceCleanBuild -and -not [string]::IsNullOrWhiteSpace($TranslationFingerprint) -and
            (Get-RecordedFingerprint $translationProvenance 'TranslationFingerprint') -eq $TranslationFingerprint) {
            $artifacts = @($baseMetadata, $baseManifest, (Join-Path $generated 'base_translation_sources.bin'),
                (Join-Path $generated 'base_translation_mod_awareness.json'))
            $reuseBase = -not ($artifacts | Where-Object { -not (Test-Path -LiteralPath $_ -PathType Leaf) })
        }
        if ($buildsRetro) {
            # The translator discovers mods through the project file's workspace-relative profile
            # paths, and both the base and the mod leg block leaf inlining at every address those
            # profiles patch. A staged install workspace ships without any mod payload, so the
            # selected Retro Rewind Code.pul must sit at the profile's mod_root before either leg
            # runs - and it must be this build's pul, not the one a previous build left behind.
            $awarenessBinaries = Join-Path $Workspace 'PulsarPacks\completed\RetroRewind\RetroRewind6\Binaries'
            $sourcePul = Join-Path $retroRoot 'Binaries\Code.pul'
            Assert-File $sourcePul 'Retro Rewind Code.pul'
            [IO.Directory]::CreateDirectory($awarenessBinaries) | Out-Null
            $stagedPul = Join-Path $awarenessBinaries 'Code.pul'
            if (-not $stagedPul.Equals($sourcePul, [StringComparison]::OrdinalIgnoreCase)) {
                Copy-Item -LiteralPath $sourcePul -Destination $stagedPul -Force
            }
        }

        if ($reuseBase -and $buildsRetro) {
            # The base translation bakes leaf-inlining and residency decisions around the mod patch
            # sets it knew about when it ran, and it records that knowledge as a modPatchAwareness
            # stamp. A base tree that never saw this Code.pul would silently bake vanilla code into
            # the modded product, so it may only be reused once that has been ruled out.
            $retroCodePul = Join-Path $retroRoot 'Binaries\Code.pul'
            Assert-File $retroCodePul 'Retro Rewind Code.pul'
            $pulSha = Get-MkwFileSha256 $retroCodePul
            # The metadata file is tens of megabytes of machine-written JSON; a raw substring probe
            # for the uniquely named stamp field avoids a multi-minute ConvertFrom-Json parse.
            $rawMetadata = [IO.File]::ReadAllText($baseMetadata)
            if (-not $rawMetadata.Contains('"codePulSha256":"' + $pulSha + '"')) {
                # A different Code.pul is not automatically a different base translation: the base
                # reads a pul for one thing, the addresses it patches, so a pul that patches the
                # same translated functions produces the same base tree. Only the translator can
                # decide that - it takes parsing the Kamek patch set against the recorded translated
                # function ranges - and it fails closed, so anything but exit 0 retranslates.
                & $translator @(
                    'check-base-mod-awareness', '--project', $project, '--profile', 'retro-rewind',
                    '--translation-output-metadata', $baseMetadata, '--code-pul', $retroCodePul
                ) | Write-Host
                if ($LASTEXITCODE -ne 0) {
                    Write-MkwBuildStep 'retranslate-base' 'The base translation is stale; retranslating the base game for the new Code.pul'
                    $reuseBase = $false
                }
            }
        }

        if ($reuseBase) {
            Write-MkwBuildStep 'reuse-base-translation' 'Reusing the completed base translation'
        } else {
            # A failed translation must never leave provenance claiming the old outputs are current.
            if (Test-Path -LiteralPath $translationProvenance) { Remove-Item -LiteralPath $translationProvenance -Force }
            [IO.Directory]::CreateDirectory($generated) | Out-Null
            [IO.Directory]::CreateDirectory($baseManifestDir) | Out-Null

            # No --clean-outdir: shard names are content-addressed and unchanged files keep their
            # bytes and mtimes, which is exactly what lets Ninja skip them. --prune-stale (active
            # only without --clean-outdir) removes everything the new translation no longer emits.
            Invoke-Checked $translator @(
                'translate-recursive', $pins.EntryPoint, '--project', $project,
                '--outdir', $functions, '--output-metadata', $baseMetadata,
                '--production-source-bundle', (Join-Path $generated 'base_translation_sources.bin'),
                '--no-function-files', '--prune-stale', '--threads', $translatorThreads
            ) 'Translating the user-owned base game' -StepId 'translate-base'

            Invoke-Checked $translator @(
                'emit-base-manifest', '--project', $project, '--out', $baseManifestDir,
                '--functions-dir', $functions, '--translation-output-metadata', $baseMetadata, '--region', 'P'
            ) 'Creating the local base translation manifest' -StepId 'emit-base-manifest'

            if (-not [string]::IsNullOrWhiteSpace($TranslationFingerprint)) {
                [ordered]@{ SchemaVersion = 1; TranslationFingerprint = $TranslationFingerprint } |
                    ConvertTo-Json | Set-Content -LiteralPath $translationProvenance -Encoding UTF8
            }
        }

        if ($buildsRetro) {
            $codePul = Join-Path $retroRoot 'Binaries\Code.pul'
            Assert-File $codePul 'Retro Rewind Code.pul'
            $retroOut = Join-Path $Workspace 'build\mods\retro_rewind_full_cpp'
            $translateModArguments = @(
                'translate-mod', '--project', $project, '--profile', 'retro-rewind',
                '--base-manifest', $baseManifest, '--base-translation-output-metadata', $baseMetadata,
                '--code-pul', $codePul, '--mod-root', $retroRoot, '--mod-name', 'Retro Rewind',
                '--region', 'P', '--out', $retroOut, '--prefer-cached-inputs', '--emit-cpp',
                '--threads', $translatorThreads
            )
            if ($SkipRetroWfcPayload) {
                $translateModArguments += '--skip-retro-wfc'
            } elseif (-not [string]::IsNullOrWhiteSpace($RetroWfcOfflineDirectory)) {
                $offlineRoot = [IO.Path]::GetFullPath($RetroWfcOfflineDirectory)
                $offlinePayload = Join-Path $offlineRoot 'binary\payload.RMCPD00.bin'
                Assert-File $offlinePayload 'Offline Retro-WFC shared payload'
                $translateModArguments += @('--retro-wfc-payload', $offlinePayload)
            } else {
                throw 'Retro Rewind requires either -RetroWfcOfflineDirectory or -SkipRetroWfcPayload.'
            }
            Invoke-Checked $translator $translateModArguments 'Translating the selected Retro Rewind Code.pul' `
                -StepId 'translate-mod'
        }

        Invoke-Checked $translator @('generate-data-init', '--project', $project) `
            'Generating local game data initialization' -StepId 'generate-data-init'

        $shardArgs = @(
            'emit-build-shards', '--project', $project, '--base-metadata', $baseMetadata,
            '--base-functions-dir', $functions,
            '--native-source-dir', (Join-Path $Workspace 'runtime\src'), '--out', $shards
        )
        if ($buildsRetro) {
            $retroOut = Join-Path $Workspace 'build\mods\retro_rewind_full_cpp'
            $shardArgs += @('--resolved-profile', (Join-Path $retroOut 'resolved_dispatch_profile.json'),
                '--retro-cpp-dir', (Join-Path $retroOut 'cpp'))
        }
        Invoke-Checked $translator $shardArgs 'Preparing local native build shards' -StepId 'emit-build-shards'

        # The build directory is kept whenever the exact toolchain that filled it is the one
        # installed now and its CMake cache still belongs to this workspace path. Object files are
        # only ever unsafe to reuse when the compiler/flags changed - source changes are what
        # CMake/Ninja exist to track, so a kept directory turns a small runtime or dependency
        # change into a recompile of just the affected objects instead of every shard.
        if (Test-Path -LiteralPath $build) {
            $keepNativeBuild = -not [string]::IsNullOrWhiteSpace($NativeToolchainFingerprint) -and
                (Get-RecordedFingerprint $toolchainProvenance 'NativeToolchainFingerprint') -eq $NativeToolchainFingerprint -and
                (Test-Path -LiteralPath (Join-Path $build 'CMakeCache.txt') -PathType Leaf)
            if ($keepNativeBuild) {
                # A moved workspace invalidates every absolute path in the cache; CMake would hard
                # error rather than reconfigure. Move healing already deletes the directory for
                # portable installs, so this only fires for hand-moved trees.
                $expectedHome = (Join-Path $Workspace 'runtime')
                $cacheHome = Select-String -LiteralPath (Join-Path $build 'CMakeCache.txt') `
                    -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$' | Select-Object -First 1
                if ($null -eq $cacheHome -or
                    -not [string]::Equals([IO.Path]::GetFullPath($cacheHome.Matches[0].Groups[1].Value),
                        [IO.Path]::GetFullPath($expectedHome), [StringComparison]::OrdinalIgnoreCase)) {
                    $keepNativeBuild = $false
                }
            }
            if ($keepNativeBuild) {
                Write-Host 'MKWCBUILD: Reusing the incremental native build directory'
            } else {
                Write-Host 'MKWCBUILD: The native build cache does not belong to this toolchain; rebuilding from scratch'
                Remove-Item -LiteralPath $build -Recurse -Force
            }
        }
        $dependencies = Join-Path $Workspace 'Dependencies'
        $nativePrebuilt = Resolve-NativePrebuiltDirectory $dependencies $toolchainBin `
            (Join-Path $Workspace 'aurora-main') (Join-Path $Workspace 'runtime\third_party')
        $configure = Get-MkwNativeConfigureArguments `
            -SourceDirectory (Join-Path $Workspace 'runtime') -BuildDirectory $build `
            -Ninja $ninja -CCompiler $cc -CxxCompiler $cxx -ResourceCompiler $windres `
            -DependenciesDirectory $dependencies -NativePrebuiltDirectory $nativePrebuilt `
            -AdditionalArguments @("-DMKW_TRANSLATED_COMPILE_JOBS=$translatedJobs")
        Invoke-Checked $cmake $configure 'Configuring the bundled native toolchain' -StepId 'configure-native'
        if (-not [string]::IsNullOrWhiteSpace($NativeToolchainFingerprint)) {
            [ordered]@{ SchemaVersion = 1; NativeToolchainFingerprint = $NativeToolchainFingerprint } |
                ConvertTo-Json | Set-Content -LiteralPath $toolchainProvenance -Encoding UTF8
        }
        $targets = switch ($Profile) {
            'base' { @('WiiCompiled') }
            'retro-rewind' { @('RetroRewind') }
            'both' { @('WiiCompiled', 'RetroRewind') }
        }
        $buildArguments = @('--build', $build)
        foreach ($target in $targets) { $buildArguments += @('--target', $target) }
        $buildArguments += @('--parallel', $globalJobs)
        Invoke-Checked $cmake $buildArguments "Compiling $($targets -join ' and ') locally" -StepId 'compile'

        # Hashed once here; every published product records the same digests.
        $dolSha = Get-MkwFileSha256 (Join-Path $assets 'main.dol')
        $relSha = Get-MkwFileSha256 (Join-Path $assets 'StaticR.rel')
        $compilerSha = Get-MkwFileSha256 (Join-Path $toolchainBin 'clang-22.exe')

        function Publish-BuiltProduct([string]$Target, [string]$Destination, [string]$ProvenanceProfile) {
            Reset-LocalDirectory $Destination
            $exe = Join-Path $build "$Target.exe"
            Assert-File $exe 'Locally compiled game executable'
            Copy-Item -LiteralPath $exe -Destination (Join-Path $Destination "$Target.exe")
            foreach ($name in @('dxcompiler.dll','dxil.dll','libc++.dll','libpng16.dll','libunwind.dll','libz.dll','SDL3.dll','sqlite3.dll','webgpu_dawn.dll','z.dll','noshaders.zip','dsp_coef.bin','initial_pipeline_cache.db')) {
                $file = Join-Path $build $name
                if (Test-Path -LiteralPath $file -PathType Leaf) { Copy-Item -LiteralPath $file -Destination $Destination }
            }
            $redist = Join-Path $Toolkit 'Redist'
            if (Test-Path -LiteralPath $redist -PathType Container) {
                Copy-Item -Path (Join-Path $redist '*.dll') -Destination $Destination -Force
            }
            foreach ($name in @('config','wii_bootstrap')) {
                $directory = Join-Path $build $name
                if (Test-Path -LiteralPath $directory -PathType Container) {
                    Copy-Item -LiteralPath $directory -Destination (Join-Path $Destination $name) -Recurse
                }
            }
            $isRetro = $ProvenanceProfile -eq 'retro-rewind'
            $provenance = [ordered]@{
                SchemaVersion = 1
                Profile = $ProvenanceProfile
                BuiltUtc = [DateTime]::UtcNow.ToString('O')
                DolSha256 = $dolSha
                RelSha256 = $relSha
                CodePulSha256 = if ($isRetro) { Get-MkwFileSha256 (Join-Path $retroRoot 'Binaries\Code.pul') } else { $null }
                RetroWfcPayloadMode = if (-not $isRetro) { $null } elseif ($SkipRetroWfcPayload) { 'skipped' } else { $RetroWfcPayloadOrigin }
                RetroWfcPayloadSha256 = if ($isRetro -and -not $SkipRetroWfcPayload) { Get-MkwFileSha256 (Join-Path $RetroWfcOfflineDirectory 'binary\payload.RMCPD00.bin') } else { $null }
                RetroWfcPayloadLength = if ($isRetro -and -not $SkipRetroWfcPayload) { (Get-Item -LiteralPath (Join-Path $RetroWfcOfflineDirectory 'binary\payload.RMCPD00.bin')).Length } else { $null }
                Compiler = "llvm-mingw clang-22 sha256:$compilerSha"
            }
            $provenance | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Destination 'local-build.json') -Encoding UTF8
        }

        if ($Profile -eq 'both') {
            Publish-BuiltProduct 'WiiCompiled' $BaseOutputDirectory 'base'
            Publish-BuiltProduct 'RetroRewind' $OutputDirectory 'retro-rewind'
        } elseif ($Profile -eq 'retro-rewind') {
            Publish-BuiltProduct 'RetroRewind' $OutputDirectory 'retro-rewind'
        } else {
            Publish-BuiltProduct 'WiiCompiled' $OutputDirectory 'base'
        }
        Write-Host "MKWCBUILD:OUTPUT=$OutputDirectory"
    } finally { Pop-Location }
} finally {
    $env:PATH = $oldPath
    if ($null -ne $oldDotnet) { $env:DOTNET_ROOT = $oldDotnet }
}
