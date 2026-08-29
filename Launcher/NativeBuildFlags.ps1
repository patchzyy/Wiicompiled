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

function Get-MkwShellSafeToolchainRoot([string]$ToolchainRoot) {
    if ([string]::IsNullOrWhiteSpace($ToolchainRoot)) { throw 'A toolchain root is required.' }
    $full = [IO.Path]::GetFullPath($ToolchainRoot)
    # A drive root keeps its separator: "C:" is relative to the current directory on that drive.
    if ($full -ne [IO.Path]::GetPathRoot($full)) { $full = $full.TrimEnd('\') }
    if ($full -notmatch '[()&^%!]') { return $full }

    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = $sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($full.ToLowerInvariant()))
    } finally { $sha.Dispose() }
    $linkName = 'toolchain-' + ((($bytes[0..7]) | ForEach-Object { $_.ToString('x2') }) -join '')

    $failures = @()
    foreach ($base in @($env:ProgramData, $env:PUBLIC)) {
        if ([string]::IsNullOrWhiteSpace($base) -or $base -match '[()&^%! ]') { continue }
        $link = Join-Path (Join-Path $base 'WiiCompiled') $linkName
        try {
            [IO.Directory]::CreateDirectory((Split-Path -Parent $link)) | Out-Null
            # The name already identifies the target, so an existing junction that still resolves is
            # this one; only a broken leftover is replaced. Directory.Delete removes the reparse
            # point itself, where Remove-Item -Recurse would delete the toolchain it points at.
            if (-not (Test-Path -LiteralPath (Join-Path $link 'CMake\bin\cmake.exe') -PathType Leaf)) {
                if (Test-Path -LiteralPath $link) { [IO.Directory]::Delete($link) }
                New-Item -ItemType Junction -Path $link -Target $full -ErrorAction Stop | Out-Null
            }
            Write-Host "MKWCBUILD: Building through $link, because $full contains characters cmd.exe cannot parse"
            return $link
        } catch {
            $failures += "$link ($($_.Exception.Message))"
        }
    }
    throw ("The toolchain path $full contains a character (one of ( ) & ^ % !) that the compiler " +
        'cannot be invoked through, and no junction to it could be created: ' + ($failures -join '; ') +
        '. Install to a path without those characters.')
}

function Get-MkwProjectPins([string]$ProjectFile) {
    <#
    The Mario Kart Wii facts pinned by projects/mkwii/recomp.yml (game identity, clean input
    digests, translation entry point, Retro-WFC endpoint), read from one place instead of being
    restated; Test-PinnedFacts.ps1 checks the copies that can't read YAML (C++ header, C#
    constants) against these values. Parsing is literal, not a YAML dependency, since the file is
    machine-written with a fixed shape.
    #>
    Assert-File $ProjectFile 'Translation project file'
    $pins = [ordered]@{
        GameId = ''; DolSha256 = ''; RelSha256 = ''; EntryPoint = ''; RetroWfcPayloadUri = ''
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

function Invoke-Checked([string]$FilePath, [string[]]$Arguments, [string]$Description,
    [string]$LogPrefix = 'MKWCBUILD', [string]$StepId = '') {
    <#
    Runs a build tool and turns a non-zero exit code into a described failure. Start-Process -Wait
    is deliberate: it waits for the whole process tree, since a .NET single-file bundle host may
    hand off to an extracted child that PowerShell's call operator would not wait for. Start-Process
    doesn't publish $LASTEXITCODE, so this sets it manually for callers that check it.
    -StepId emits the machine-readable form the installer's progress bar consumes (BuildStepIds in
    WiiCompiled.Setup/InstallProgress.cs); the human sentence stays on the same log line.
    #>
    if ($StepId) { Write-Host "${LogPrefix}:STEP:${StepId} $Description" }
    else { Write-Host "${LogPrefix}: $Description" }
    $quotedArguments = @($Arguments | ForEach-Object {
        if ($_.Contains('"')) { throw "A native build argument contains an unsupported quote: $_" }
        '"' + $_ + '"'
    })
    $process = Start-Process -FilePath $FilePath -ArgumentList $quotedArguments `
        -NoNewWindow -Wait -PassThru
    $exitCode = $process.ExitCode
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

function Get-MkwThirdPartySourceFingerprint([string]$ThirdPartyDirectory) {
    <#
    SHA-256 over runtime/third_party, the vendored sources behind the prebuilt package. The
    precompiled Crypto++ archive is linked while first-party TUs compile against the same headers,
    so a workspace whose vendored tree moved on must not consume archives harvested from an older
    one (same reasoning as the aurora fingerprint below). Unlike that scan this hashes hidden files
    too, since Build-Installer stages this tree with Copy-Item -Force. Returns '' when absent.
    #>
    if ([string]::IsNullOrWhiteSpace($ThirdPartyDirectory)) { return '' }
    if (-not (Test-Path -LiteralPath $ThirdPartyDirectory -PathType Container)) { return '' }
    $root = [IO.Path]::GetFullPath($ThirdPartyDirectory).TrimEnd('\')
    $files = Get-ChildItem -LiteralPath $root -Recurse -File |
        Sort-Object { $_.FullName.Substring($root.Length + 1).Replace('\', '/') }
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $builder = [Text.StringBuilder]::new()
        foreach ($file in $files) {
            $relative = $file.FullName.Substring($root.Length + 1).Replace('\', '/')
            $content = Get-MkwFileSha256 $file.FullName
            [void]$builder.Append($relative).Append(' ').Append($content).Append("`n")
        }
        $bytes = $sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($builder.ToString()))
    } finally { $sha.Dispose() }
    return (($bytes | ForEach-Object { $_.ToString('x2') }) -join '')
}

function Get-MkwAuroraSourceFingerprint([string]$AuroraDirectory) {
    <#
    SHA-256 over the aurora sources the precompiled archives were harvested from. The compiler,
    flag, and Dawn digests only prove the package agrees with its target environment; none notice
    aurora itself moving on, and since the runtime calls aurora headers directly, a package
    harvested before an aurora change links old code against new call sites (undefined symbol at
    best, silently stale renderer at worst). extern/ is excluded because the payload ships those
    trees separately. Returns '' when the tree is absent.
    #>
    if ([string]::IsNullOrWhiteSpace($AuroraDirectory)) { return '' }
    if (-not (Test-Path -LiteralPath $AuroraDirectory -PathType Container)) { return '' }
    $root = [IO.Path]::GetFullPath($AuroraDirectory).TrimEnd('\')
    $excludedPrefixes = @(
        (Join-Path $root 'extern') + '\',
        (Join-Path $root 'build') + '\'
    )
    # Hidden files are skipped deliberately: Build-Installer.ps1 stages this tree
    # with Copy-Item -Recurse (no -Force), so they never reach the payload and
    # hashing them would make the two sides disagree by construction.
    $files = Get-ChildItem -LiteralPath $root -Recurse -File |
        Where-Object {
            $full = $_.FullName
            -not ($excludedPrefixes | Where-Object { $full.StartsWith($_, [StringComparison]::OrdinalIgnoreCase) })
        } |
        Sort-Object { $_.FullName.Substring($root.Length + 1).Replace('\', '/') }
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $builder = [Text.StringBuilder]::new()
        foreach ($file in $files) {
            $relative = $file.FullName.Substring($root.Length + 1).Replace('\', '/')
            $content = Get-MkwFileSha256 $file.FullName
            [void]$builder.Append($relative).Append(' ').Append($content).Append("`n")
        }
        $bytes = $sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($builder.ToString()))
    } finally { $sha.Dispose() }
    return (($bytes | ForEach-Object { $_.ToString('x2') }) -join '')
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

    if (-not [string]::IsNullOrWhiteSpace($NativePrebuiltDirectory)) {
        $arguments += "-DMKW_NATIVE_PREBUILT_DIR=$(ConvertTo-MkwCMakePath $NativePrebuiltDirectory)"
    }

    $arguments += $AdditionalArguments
    return $arguments
}
