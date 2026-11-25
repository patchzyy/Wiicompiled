# Canonical native (CMake) configure flags, plus the shared helpers (file assertions, checked
# process spawn, file hashing) both LocalBuild.ps1 and Prepare-NativePrebuilt.ps1 dot-source.
# They MUST stay byte-identical: the prebuilt package ships static archives linked into objects
# the user's machine compiles, so any flag divergence is an ABI/ODR hazard, not just a rebuild.
# The flag fingerprint below is recorded in the package provenance and re-checked before use.

Set-StrictMode -Version 3.0

function Assert-File([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "$Description is missing: $Path" }
}

function Assert-Directory([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) { throw "$Description is missing: $Path" }
}

function Get-MkwFileSha256([string]$Path) {
    <#
    Lower-case SHA-256 of one file. -LiteralPath is required: Get-FileHash treats a positional
    path as a wildcard, so an install directory containing [, ] or * would hash the wrong file.
    #>
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-MkwToolchainPath([string]$ToolchainRoot) {
    <#
    The scrubbed PATH a bundled-toolchain build runs with. The bundled toolchain must be the only
    one visible, or an ambient Visual Studio/MSYS install on PATH could pull in its own headers,
    libraries, or linker. Shared by LocalBuild.ps1 and Prepare-NativePrebuilt.ps1 since the
    installed Toolkit and the maintainer portable-tools tree share the same subdirectory layout.
    #>
    if ([string]::IsNullOrWhiteSpace($ToolchainRoot)) { throw 'A toolchain root is required.' }
    $root = [IO.Path]::GetFullPath($ToolchainRoot)
    return (@(
        (Join-Path $root 'llvm-mingw\bin'),
        (Join-Path $root 'CMake\bin'),
        (Join-Path $root 'Ninja'),
        (Join-Path $env:SystemRoot 'System32'),
        $env:SystemRoot
    ) -join ';')
}

function Get-MkwBuildSafePath([string]$Path, [string]$Kind, [string]$MarkerFile) {
    <#
    The Windows native build passes workspace paths through CMake, Ninja response files and
    clang, which do not all interpret quotes the same way. Keep those paths plain even when the
    user's install directory contains an apostrophe, ampersand or other punctuation.
    #>
    if ([string]::IsNullOrWhiteSpace($Path)) { throw "A $Kind path is required." }
    $full = [IO.Path]::GetFullPath($Path)
    # A drive root keeps its separator: "C:" is relative to the current directory on that drive.
    if ($full -ne [IO.Path]::GetPathRoot($full)) { $full = $full.TrimEnd('\') }
    if ($full -cmatch '^[A-Za-z0-9 ._\\:-]+$') { return $full }

    Assert-File (Join-Path $full $MarkerFile) "$Kind marker"

    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = $sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($full.ToLowerInvariant()))
    } finally { $sha.Dispose() }
    $linkName = "$Kind-" + ((($bytes[0..7]) | ForEach-Object { $_.ToString('x2') }) -join '')

    $failures = @()
    foreach ($base in @($env:ProgramData, $env:PUBLIC)) {
        if ([string]::IsNullOrWhiteSpace($base) -or $base -cnotmatch '^[A-Za-z0-9._\\:-]+$') { continue }
        $link = Join-Path (Join-Path $base 'WiiCompiled') $linkName
        try {
            [IO.Directory]::CreateDirectory((Split-Path -Parent $link)) | Out-Null
            $existing = Get-Item -LiteralPath $link -Force -ErrorAction SilentlyContinue
            if ($null -ne $existing) {
                # Never trust a directory just because it has the marker: it could point at a
                # different installation. Nor may we remove a directory we did not create.
                if ($existing.LinkType -ne 'Junction' -or
                    @($existing.Target).Count -ne 1 -or
                    -not [string]::Equals([IO.Path]::GetFullPath(@($existing.Target)[0]),
                        $full, [StringComparison]::OrdinalIgnoreCase)) {
                    throw "An existing path is not the expected junction: $link"
                }
            } else {
                New-Item -ItemType Junction -Path $link -Target $full -ErrorAction Stop | Out-Null
            }
            Assert-File (Join-Path $link $MarkerFile) "$Kind junction marker"
            Write-Host "MKWCBUILD: Building the $Kind through $link, because $full contains characters the native build cannot quote reliably"
            return $link
        } catch {
            $failures += "$link ($($_.Exception.Message))"
        }
    }
    throw ("The $Kind path $full cannot be passed safely to the native build, and no junction " +
        'to it could be created: ' + ($failures -join '; ') + '. Install to a path of plain ' +
        'letters, digits and spaces, or make a safe junction location available.')
}

function Get-MkwProjectPins([string]$ProjectFile) {
    <#
    The Mario Kart Wii facts pinned by a projects/*/recomp.yml (game identity, clean input
    digests, translation entry point, Retro-WFC endpoint), read from one place instead of being
    restated; Test-PinnedFacts.ps1 checks the copies that can't read YAML (C++ header, C#
    constants) against these values. Parsing is literal, not a YAML dependency, since the file is
    machine-written with a fixed shape.
    #>
    Assert-File $ProjectFile 'Translation project file'
    $pins = [ordered]@{
        GameId = ''; Region = ''; DolSha256 = ''; RelSha256 = ''; EntryPoint = ''; RetroWfcPayloadUri = ''
    }
    $section = ''
    $inputKey = ''
    foreach ($raw in [IO.File]::ReadAllLines($ProjectFile)) {
        $line = ($raw -replace '#.*$', '')
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        if ($line -match '^([A-Za-z0-9_]+):') { $section = $Matches[1]; $inputKey = ''; continue }
        if ($section -eq 'inputs' -and $line -match '^\s{2}([A-Za-z0-9_]+):\s*$') { $inputKey = $Matches[1]; continue }
        if ($section -eq 'project' -and $line -match '^\s*game_id:\s*(\S+)\s*$') {
            $pins.GameId = $Matches[1]
        } elseif ($section -eq 'project' -and $line -match '^\s*region:\s*(\S+)\s*$') {
            $pins.Region = $Matches[1]
        } elseif ($section -eq 'inputs' -and $line -match '^\s*sha256:\s*([0-9a-fA-F]{64})\s*$') {
            if ($inputKey -eq 'dol') { $pins.DolSha256 = $Matches[1].ToLowerInvariant() }
            elseif ($inputKey -eq 'rel') { $pins.RelSha256 = $Matches[1].ToLowerInvariant() }
        } elseif ($section -eq 'translation' -and $pins.EntryPoint -eq '' -and
                  $line -match '^\s*-\s*(0[xX][0-9a-fA-F]+)\s*$') {
            $pins.EntryPoint = $Matches[1].ToLowerInvariant()
        } elseif ($line -match '^\s*retro_wfc_payload:\s*(\S+)\s*$') {
            $pins.RetroWfcPayloadUri = $Matches[1]
        }
    }
    foreach ($name in @($pins.Keys)) {
        if ([string]::IsNullOrWhiteSpace($pins[$name])) {
            throw "$ProjectFile does not pin $name; the project file is not the shape this build expects."
        }
    }
    return $pins
}

function Resolve-MkwProjectForAssets([string]$Workspace, [string]$AssetsDirectory) {
    <#
    The translation project pinned to the main.dol in AssetsDirectory, after checking that
    StaticR.rel and any extracted disc data belong to the same release.
    #>
    $dolSha = Get-MkwFileSha256 (Join-Path $AssetsDirectory 'main.dol')
    foreach ($file in Get-ChildItem -LiteralPath (Join-Path $Workspace 'projects') -Filter 'recomp.yml' -Recurse -File) {
        $pins = Get-MkwProjectPins $file.FullName
        if ($pins.DolSha256 -ne $dolSha) { continue }
        if ((Get-MkwFileSha256 (Join-Path $AssetsDirectory 'StaticR.rel')) -ne $pins.RelSha256) {
            throw "StaticR.rel does not match the clean $($pins.GameId) release of main.dol."
        }
        $bootBin = Join-Path $AssetsDirectory 'DATA\sys\boot.bin'
        if (Test-Path -LiteralPath $bootBin -PathType Leaf) {
            $discId = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($bootBin), 0, 6)
            if ($discId -ne $pins.GameId) {
                throw "The extracted game data is $discId but main.dol is $($pins.GameId); extract the game again."
            }
        }
        return [pscustomobject]@{ Path = $file.FullName; Pins = $pins }
    }
    throw "main.dol (sha256 $dolSha) is not a supported clean Mario Kart Wii release."
}

function Invoke-Checked([string]$FilePath, [string[]]$Arguments, [string]$Description,
    [string]$LogPrefix = 'MKWCBUILD', [string]$StepId = '', [bool]$WaitForProcessTree = $true,
    [int]$TimeoutSeconds = 0) {
    <#
    Runs a build tool and turns a non-zero exit code into a described failure. By default,
    Start-Process -Wait waits for the whole process tree, since a .NET single-file bundle host may
    hand off to an extracted child that PowerShell's call operator would not wait for. Callers that
    need to avoid waiting on unrelated descendants can opt into the call-operator path.
    Start-Process doesn't publish $LASTEXITCODE, so this sets it manually for callers that check it.
    -StepId emits the machine-readable form the installer's progress bar consumes (BuildStepIds in
    WiiCompiled.Setup/InstallProgress.cs); the human sentence stays on the same log line.
    -TimeoutSeconds waits on the process itself and kills its tree when time runs out.
    #>
    if ($StepId) { Write-Host "${LogPrefix}:STEP:${StepId} $Description" }
    else { Write-Host "${LogPrefix}: $Description" }
    $quotedArguments = @($Arguments | ForEach-Object {
        if ($_.Contains('"')) { throw "A native build argument contains an unsupported quote: $_" }
        # Backslashes directly before the closing quote would otherwise escape it.
        '"' + ($_ -replace '(\\+)$', '$1$1') + '"'
    })
    if ($TimeoutSeconds -gt 0) {
        $process = Start-Process -FilePath $FilePath -ArgumentList $quotedArguments -NoNewWindow -PassThru
        # Holding the handle keeps ExitCode readable once the process has exited.
        $null = $process.Handle
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            # taskkill's stderr would end the script under 'Stop' before the message below.
            try { & taskkill.exe /T /F /PID $process.Id 2>&1 | Out-Null } catch {}
            throw "$Description did not finish within $TimeoutSeconds seconds and was stopped."
        }
        $exitCode = $process.ExitCode
    } elseif ($WaitForProcessTree) {
        $process = Start-Process -FilePath $FilePath -ArgumentList $quotedArguments `
            -NoNewWindow -Wait -PassThru
        $exitCode = $process.ExitCode
    } else {
        # Tools report progress on stderr (CMake message()); only the exit code decides failure.
        $previousPreference = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            & $FilePath @Arguments
            $exitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previousPreference
        }
    }
    $global:LASTEXITCODE = $exitCode
    if ($exitCode -ne 0) { throw "$Description failed with exit code $exitCode." }
}

function Get-MkwNativeFixedConfigureFlags {
    <#
    The path-independent half of the configure command line, identical on every machine, which is
    what makes one precompiled aurora/third-party package valid for all users. Path-bearing
    arguments are added separately by Get-MkwNativeConfigureArguments and excluded from the fingerprint.
    #>
    return @(
        '-DCMAKE_BUILD_TYPE=Release',
        '-DCMAKE_SYSTEM_PROCESSOR=x86_64',
        '-DAURORA_DAWN_PROVIDER=package', '-DAURORA_SDL3_PROVIDER=vendor',
        '-DCMAKE_POLICY_DEFAULT_CMP0168=NEW', '-DFETCHCONTENT_FULLY_DISCONNECTED=ON',
        '-DAWK:FILEPATH='
    )
}

function Get-MkwNativeFlagFingerprint {
    <#
    .SYNOPSIS
    SHA-256 over the canonical flag list, used to reject a stale package.
    #>
    $canonical = (Get-MkwNativeFixedConfigureFlags) -join "`n"
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = $sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($canonical))
    } finally { $sha.Dispose() }
    return (($bytes | ForEach-Object { $_.ToString('x2') }) -join '')
}

function Get-MkwDawnRuntimeSha256([string]$DependenciesDirectory) {
    <#
    SHA-256 of the pinned Dawn runtime the prebuilt aurora must match. The precompiled aurora
    archives are compiled and linked against the pinned dawn_prebuilt tree's headers/import
    library, but the upstream release asset is mutable (the same version tag has already served
    two different windows-amd64 archives), so the version string alone can't prove agreement.
    A mismatch surfaces as an access violation at surface creation, not a link error, so the
    digest is recorded at harvest and re-checked before use. Returns '' when the tree is absent.
    #>
    if ([string]::IsNullOrWhiteSpace($DependenciesDirectory)) { return '' }
    $runtime = Join-Path $DependenciesDirectory 'dawn_prebuilt\bin\webgpu_dawn.dll'
    if (-not (Test-Path -LiteralPath $runtime -PathType Leaf)) { return '' }
    return Get-MkwFileSha256 $runtime
}

function ConvertTo-MkwHex([byte[]]$Bytes) {
    return [BitConverter]::ToString($Bytes).Replace('-', '').ToLowerInvariant()
}

function Get-MkwSourceTreeFingerprint([string]$Directory, [string[]]$ExcludedTopDirectories = @()) {
    <#
    SHA-256 over "relative-path content-sha256" lines of a source tree. It must come out the same
    under powershell.exe 5.1 and pwsh 7 and for any checkout's line endings, because the package is
    harvested on a release machine and re-checked on every user's: paths are sorted ordinally (not
    with Sort-Object, whose culture collation differs between NLS and ICU) and text content is hashed
    with CRLF folded to LF. Dot-paths are skipped by name, not by the hidden attribute, which the
    payload zip drops. Returns '' when the tree is absent.
    #>
    if ([string]::IsNullOrWhiteSpace($Directory)) { return '' }
    if (-not (Test-Path -LiteralPath $Directory -PathType Container)) { return '' }
    $root = [IO.Path]::GetFullPath($Directory).TrimEnd('\')
    $relatives = [Collections.Generic.List[string]]::new()
    foreach ($file in [IO.Directory]::EnumerateFiles($root, '*', [IO.SearchOption]::AllDirectories)) {
        $relative = $file.Substring($root.Length + 1).Replace('\', '/')
        $parts = $relative.Split('/')
        if ($parts.Length -gt 1 -and $ExcludedTopDirectories -contains $parts[0]) { continue }
        $hidden = $false
        foreach ($part in $parts) { if ($part.StartsWith('.', [StringComparison]::Ordinal)) { $hidden = $true; break } }
        if (-not $hidden) { $relatives.Add($relative) }
    }
    $sorted = $relatives.ToArray()
    [Array]::Sort($sorted, [StringComparer]::Ordinal)

    $latin1 = [Text.Encoding]::GetEncoding(28591)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $builder = [Text.StringBuilder]::new()
        foreach ($relative in $sorted) {
            $bytes = [IO.File]::ReadAllBytes($root + '\' + $relative.Replace('/', '\'))
            # Latin-1 maps bytes 1:1, so the round trip only changes the line endings.
            $text = $latin1.GetString($bytes)
            if ($text.IndexOf([char]0) -lt 0 -and $text.Contains("`r`n")) {
                $bytes = $latin1.GetBytes($text.Replace("`r`n", "`n"))
            }
            [void]$builder.Append($relative).Append(' ').Append((ConvertTo-MkwHex $sha.ComputeHash($bytes))).Append("`n")
        }
        return ConvertTo-MkwHex $sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($builder.ToString()))
    } finally { $sha.Dispose() }
}

function Get-MkwThirdPartySourceFingerprint([string]$ThirdPartyDirectory) {
    <#
    runtime/third_party, the vendored sources behind the prebuilt package. The precompiled Crypto++
    archive is linked while first-party TUs compile against the same headers, so a workspace whose
    vendored tree moved on must not consume archives harvested from an older one.
    #>
    return Get-MkwSourceTreeFingerprint $ThirdPartyDirectory
}

function Get-MkwAuroraSourceFingerprint([string]$AuroraDirectory) {
    <#
    The aurora sources the precompiled archives were harvested from. The compiler, flag, and Dawn
    digests only prove the package agrees with its target environment; none notice aurora itself
    moving on, and since the runtime calls aurora headers directly, a package harvested before an
    aurora change links old code against new call sites. extern/ is excluded because the payload
    ships those trees separately.
    #>
    return Get-MkwSourceTreeFingerprint $AuroraDirectory @('extern', 'build')
}

function Resolve-MkwNativePrebuiltDirectory([string]$DependenciesDirectory, [string]$ToolchainBin,
    [string]$AuroraDirectory, [string]$ThirdPartyDirectory) {
    <#
    The shipped precompiled aurora/third-party package, verified against its recorded provenance
    (exact compiler, exact flags, sources) before use. Anything unexpected falls back to building
    from source. Shared with Build-Installer.ps1, which runs it against the staged payload under
    powershell.exe so a package users would reject never ships.
    #>
    $package = Join-Path $DependenciesDirectory 'native_prebuilt'
    $provenancePath = Join-Path $package 'provenance.json'
    if (-not (Test-Path -LiteralPath $provenancePath -PathType Leaf)) {
        Write-Host 'MKWCBUILD: No precompiled aurora package was shipped; building it from source'
        return ''
    }
    try {
        $provenance = Get-Content -LiteralPath $provenancePath -Raw | ConvertFrom-Json
        if ($provenance.CompilerSha256 -ne (Get-MkwFileSha256 (Join-Path $ToolchainBin 'clang-22.exe'))) {
            Write-Host 'MKWCBUILD: The precompiled aurora package was built by a different compiler; building it from source'
            return ''
        }
        if ($provenance.FlagFingerprint -ne (Get-MkwNativeFlagFingerprint)) {
            Write-Host 'MKWCBUILD: The precompiled aurora package was built with different flags; building it from source'
            return ''
        }
        # The archives embed Dawn's headers and link its import library; a different Dawn is an
        # ABI mismatch that only shows up as a crash inside the GPU backend.
        $recordedDawn = $provenance.PSObject.Properties['DawnRuntimeSha256']
        if ($null -eq $recordedDawn -or
            $recordedDawn.Value -ne (Get-MkwDawnRuntimeSha256 $DependenciesDirectory)) {
            Write-Host 'MKWCBUILD: The precompiled aurora package was built against a different Dawn; building it from source'
            return ''
        }
        $recordedAurora = $provenance.PSObject.Properties['AuroraSourceFingerprint']
        if ($null -eq $recordedAurora -or
            $recordedAurora.Value -ne (Get-MkwAuroraSourceFingerprint $AuroraDirectory)) {
            Write-Host 'MKWCBUILD: The precompiled aurora package was built from different aurora sources; building it from source'
            return ''
        }
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

function ConvertTo-MkwCMakePath([string]$Path) {
    return [IO.Path]::GetFullPath($Path).Replace('\', '/')
}

function Get-MkwNativeConfigureArguments {
    <# The complete `cmake` configure argument list for a local runtime build. #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string]$SourceDirectory,
        [Parameter(Mandatory)] [string]$BuildDirectory,
        [Parameter(Mandatory)] [string]$Ninja,
        [Parameter(Mandatory)] [string]$CCompiler,
        [Parameter(Mandatory)] [string]$CxxCompiler,
        [Parameter(Mandatory)] [string]$ResourceCompiler,
        [string]$DependenciesDirectory,
        [string]$NativePrebuiltDirectory,
        [string[]]$AdditionalArguments = @()
    )

    $arguments = @(
        '-S', (ConvertTo-MkwCMakePath $SourceDirectory),
        '-B', (ConvertTo-MkwCMakePath $BuildDirectory),
        '-G', 'Ninja',
        "-DCMAKE_MAKE_PROGRAM=$(ConvertTo-MkwCMakePath $Ninja)",
        "-DCMAKE_C_COMPILER=$(ConvertTo-MkwCMakePath $CCompiler)",
        "-DCMAKE_CXX_COMPILER=$(ConvertTo-MkwCMakePath $CxxCompiler)",
        "-DCMAKE_RC_COMPILER=$(ConvertTo-MkwCMakePath $ResourceCompiler)"
    )
    $arguments += Get-MkwNativeFixedConfigureFlags

    if (-not [string]::IsNullOrWhiteSpace($DependenciesDirectory) -and
        (Test-Path -LiteralPath $DependenciesDirectory -PathType Container)) {
        $cppWinRt = Join-Path $DependenciesDirectory 'cppwinrt'
        if (Test-Path -LiteralPath (Join-Path $cppWinRt 'winrt\base.h') -PathType Leaf) {
            $arguments += "-DMKW_CPPWINRT_INCLUDE_DIR=$(ConvertTo-MkwCMakePath $cppWinRt)"
        }
        foreach ($directory in Get-ChildItem -LiteralPath $DependenciesDirectory -Directory) {
            # native_prebuilt holds compiled output, not a FetchContent source tree.
            if ($directory.Name -eq 'native_prebuilt') { continue }
            # FetchContent uppercases the declared name but preserves punctuation
            # (for example abseil-cpp -> FETCHCONTENT_SOURCE_DIR_ABSEIL-CPP).
            $name = $directory.Name.ToUpperInvariant()
            $arguments += "-DFETCHCONTENT_SOURCE_DIR_$name=$(ConvertTo-MkwCMakePath $directory.FullName)"
        }
    }

    # Always set, so a reused CMake cache can never keep a package this run rejected.
    $arguments += if ([string]::IsNullOrWhiteSpace($NativePrebuiltDirectory)) { '-DMKW_NATIVE_PREBUILT_DIR=' }
        else { "-DMKW_NATIVE_PREBUILT_DIR=$(ConvertTo-MkwCMakePath $NativePrebuiltDirectory)" }

    $arguments += $AdditionalArguments
    return $arguments
}
