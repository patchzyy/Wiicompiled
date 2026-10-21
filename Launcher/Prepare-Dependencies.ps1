# Populates Launcher/artifacts/dependencies: the pinned, unpatched upstream source trees (plus the
# prebuilt Dawn package and generated C++/WinRT headers) that Build-Installer.ps1 ships inside the
# installer so the user's build runs with FETCHCONTENT_FULLY_DISCONNECTED=ON. Every pin is the one
# aurora-main's own CMake declares; the Pin fields below are asserted against those files so the
# two cannot drift apart silently. native_prebuilt is not fetched here - Prepare-NativePrebuilt.ps1
# compiles it from these trees.
[CmdletBinding()]
param(
    [string]$Destination,
    # Windows metadata source for cppwinrt.exe: 'local' (this machine's WinMetadata), 'sdk', or an
    # installed SDK version such as 10.0.26100.0.
    [string]$CppWinRtInput = 'local'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$auroraCMake = Join-Path $repoRoot 'aurora-main\CMakeLists.txt'
$auroraExtern = Join-Path $repoRoot 'aurora-main\extern\CMakeLists.txt'
$auroraDawn = Join-Path $repoRoot 'aurora-main\cmake\AuroraDawnProvider.cmake'
$auroraLibUsb = Join-Path $repoRoot 'aurora-main\cmake\AuroraLibUSB.cmake'
$auroraSdl = Join-Path $repoRoot 'aurora-main\cmake\AuroraSDL3Provider.cmake'
$auroraVersions = Join-Path $repoRoot 'aurora-main\cmake\AuroraDependencyVersions.cmake'
$auroraImgui = Join-Path $repoRoot 'aurora-main\cmake\AuroraDearImguiProvider.cmake'
if (-not (Test-Path -LiteralPath $auroraCMake -PathType Leaf)) {
    throw 'Aurora sources are missing. Run git submodule update --init --recursive from the repository root first.'
}

# Name = directory under the destination; FetchContent maps it back through
# FETCHCONTENT_SOURCE_DIR_<UPPERCASE NAME> (NativeBuildFlags.ps1), so the names are the declared
# FetchContent names, not the upstream project names.
$packages = @(
    [pscustomobject]@{
        Name = 'SDL'; File = 'SDL3-3.4.10.tar.gz'
        Sha256 = '12b34280415ec8418c864408b93d008a20a6530687ee613d60bfbd20411f2785'
        Uris = @('https://github.com/libsdl-org/SDL/releases/download/release-3.4.10/SDL3-3.4.10.tar.gz')
        Pins = @(@{ File = $auroraVersions; Text = '_aurora_dependency_version(AURORA_SDL3_REF "refs/tags/release-3.4.10"' },
                 @{ File = $auroraSdl; Text = 'URL "https://github.com/libsdl-org/SDL/archive/${AURORA_SDL3_REF}.tar.gz"' })
    },
    [pscustomobject]@{
        Name = 'abseil-cpp'; File = 'abseil-cpp-20240722.0.tar.gz'
        Sha256 = 'f50e5ac311a81382da7fa75b97310e4b9006474f9560ac46f54a9967f07d4ae3'
        Uris = @('https://github.com/abseil/abseil-cpp/archive/refs/tags/20240722.0.tar.gz')
        Pins = @(@{ File = $auroraExtern; Text = 'https://github.com/abseil/abseil-cpp/archive/refs/tags/20240722.0.tar.gz' })
    },
    [pscustomobject]@{
        Name = 'dawn_prebuilt'; File = 'dawn-v20260807.225922-windows-amd64.tar.gz'
        Sha256 = '42a7a295c87b2e8a36312e2f84a5823aac63325e8e1d271bc7ced6f83bfb075f'
        Uris = @('https://github.com/encounter/dawn/releases/download/v20260807.225922/dawn-windows-amd64.tar.gz')
        Pins = @(@{ File = $auroraVersions; Text = '_aurora_dependency_version(AURORA_DAWN_VERSION "v20260807.225922"' },
                 @{ File = $auroraDawn; Text = 'SHA256=42a7a295c87b2e8a36312e2f84a5823aac63325e8e1d271bc7ced6f83bfb075f' })
    },
    [pscustomobject]@{
        Name = 'fmt'; File = 'fmt-12.1.0.tar.gz'
        Sha256 = 'ea7de4299689e12b6dddd392f9896f08fb0777ac7168897a244a6d6085043fea'
        Uris = @('https://github.com/fmtlib/fmt/archive/refs/tags/12.1.0.tar.gz')
        Pins = @(@{ File = $auroraExtern; Text = 'https://github.com/fmtlib/fmt/archive/refs/tags/12.1.0.tar.gz' })
    },
    [pscustomobject]@{
        Name = 'freetype'; File = 'freetype-2.14.3.tar.gz'
        Sha256 = 'e61b31ab26358b946e767ed7eb7f4bb2e507da1cfefeb7a8861ace7fd5c899a1'
        Uris = @('https://files.twilitrealm.dev/freetype-2.14.3.tar.gz',
                 'https://download.savannah.gnu.org/releases/freetype/freetype-2.14.3.tar.gz',
                 'https://downloads.sourceforge.net/project/freetype/freetype2/2.14.3/freetype-2.14.3.tar.gz')
        Pins = @(@{ File = $auroraExtern; Text = 'https://files.twilitrealm.dev/freetype-2.14.3.tar.gz' })
    },
    [pscustomobject]@{
        Name = 'imgui'; File = 'imgui-1.91.9b-docking.tar.gz'
        Sha256 = '466fdef9b18de15f0bb6e288e3d00ffa3d82200ec458ce5e4f724a161d9528a5'
        Uris = @('https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b-docking.tar.gz')
        Pins = @(@{ File = $auroraImgui; Text = 'https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b-docking.tar.gz' })
    },
    [pscustomobject]@{
        Name = 'libusb'; File = 'libusb-1.0.30.tar.bz2'
        Sha256 = 'fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf'
        Uris = @('https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.tar.bz2')
        Pins = @(@{ File = $auroraVersions; Text = '_aurora_dependency_version(AURORA_LIBUSB_VERSION "1.0.30"' },
                 @{ File = $auroraLibUsb; Text = 'releases/download/v${AURORA_LIBUSB_VERSION}/libusb-${AURORA_LIBUSB_VERSION}.tar.bz2' })
    },
    [pscustomobject]@{
        Name = 'png'; File = 'libpng-1.6.58.tar.gz'
        Sha256 = 'a9d4df463d36a6e5f9c29bd6f4967312d17e996c1854f3511f833924eb1993cf'
        Uris = @('https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.58.tar.gz')
        Pins = @(@{ File = $auroraExtern; Text = 'https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.58.tar.gz' })
    },
    [pscustomobject]@{
        Name = 'sqlite3'; File = 'sqlite-amalgamation-3510300.zip'
        Sha256 = 'acb1e6f5d832484bf6d32b681e858c38add8b2acdfd42ac5df24b8afb46552b4'
        Uris = @('https://sqlite.org/2026/sqlite-amalgamation-3510300.zip')
        Pins = @(@{ File = $auroraExtern; Text = 'https://sqlite.org/2026/sqlite-amalgamation-3510300.zip' })
    },
    [pscustomobject]@{
        Name = 'tracy'; File = 'tracy-0.14.1.zip'
        Sha256 = '908f3a2917fa86a247abfcf85dcf04bad1db6986a4d40f94b70512f3e9e98d5b'
        Uris = @('https://github.com/wolfpld/tracy/archive/refs/tags/v0.14.1.zip')
        Pins = @(@{ File = $auroraExtern; Text = 'https://github.com/wolfpld/tracy/archive/refs/tags/v0.14.1.zip' })
    },
    [pscustomobject]@{
        Name = 'xxhash'; File = 'xxHash-0.8.4.tar.gz'
        Sha256 = '5738270935e7c3d38a79b3adf7c9692566ce7895a25f67de43ad52ab504acd32'
        Uris = @('https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.4.tar.gz')
        Pins = @(@{ File = $auroraExtern; Text = 'https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.4.tar.gz' })
    },
    [pscustomobject]@{
        Name = 'zlib'; File = 'zlib-ng-2.3.3.tar.gz'
        Sha256 = 'f9c65aa9c852eb8255b636fd9f07ce1c406f061ec19a2e7d508b318ca0c907d1'
        Uris = @('https://github.com/zlib-ng/zlib-ng/archive/refs/tags/2.3.3.tar.gz')
        Pins = @(@{ File = $auroraExtern; Text = 'https://github.com/zlib-ng/zlib-ng/archive/refs/tags/2.3.3.tar.gz' })
    },
    [pscustomobject]@{
        Name = 'zstd'; File = 'zstd-1.5.7.tar.gz'
        Sha256 = 'eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3'
        Uris = @('https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz')
        Pins = @(@{ File = $auroraExtern; Text = 'https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz' })
    }
)

# The C++/WinRT compiler (NuGet package = zip) that generates the projection headers.
$cppWinRtTool = [pscustomobject]@{
    Name = 'cppwinrt-tool'; File = 'Microsoft.Windows.CppWinRT.3.0.260818.1.nupkg'
    Sha256 = 'a8993608ae9263a8288e1a1d9ee2684a5632f4acc59e1e89ddd86b3b889a7367'
    Uris = @('https://www.nuget.org/api/v2/package/Microsoft.Windows.CppWinRT/3.0.260818.1')
    Pins = @()
}

if (-not $Destination) { $Destination = Join-Path $PSScriptRoot 'artifacts\dependencies' }
$Destination = [IO.Path]::GetFullPath($Destination)
$downloads = Join-Path $PSScriptRoot 'artifacts\downloads'
$tar = Join-Path $env:SystemRoot 'System32\tar.exe'
if (-not (Test-Path -LiteralPath $tar -PathType Leaf)) { throw "Windows archive tool is missing: $tar" }
[IO.Directory]::CreateDirectory($Destination) | Out-Null
[IO.Directory]::CreateDirectory($downloads) | Out-Null

function Assert-Pinned($Package) {
    foreach ($pin in $Package.Pins) {
        if (-not (Test-Path -LiteralPath $pin.File -PathType Leaf)) { throw "Pin source is missing: $($pin.File)" }
        $content = [IO.File]::ReadAllText($pin.File)
        if (-not $content.Contains($pin.Text)) {
            throw "$($Package.Name) is pinned to '$($pin.Text)' here but $($pin.File) no longer declares it; update both."
        }
    }
}

function Get-Sha256([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }

# Every archive this returns, cached or downloaded, matches its pinned Sha256.
function Get-Archive($Package) {
    $archive = Join-Path $downloads $Package.File
    if (Test-Path -LiteralPath $archive -PathType Leaf) {
        $actual = Get-Sha256 $archive
        if ($actual -eq $Package.Sha256) { return $archive }
        Write-Warning "$($Package.Name): cached $archive has SHA-256 $actual, expected $($Package.Sha256); discarding it and downloading again."
        Remove-Item -LiteralPath $archive -Force
    }
    $temporary = $archive + '.partial'
    $failures = @()
    $mismatch = $false
    foreach ($uri in $Package.Uris) {
        Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
        Write-Host "Downloading $($Package.Name) from $uri..."
        try {
            Invoke-WebRequest -UseBasicParsing -Uri $uri -OutFile $temporary
        } catch {
            $failures += "$uri ($($_.Exception.Message))"
            continue
        }
        $actual = Get-Sha256 $temporary
        if ($actual -ne $Package.Sha256) {
            $failures += "$uri (SHA-256 $actual, expected $($Package.Sha256))"
            $mismatch = $true
            continue
        }
        Move-Item -LiteralPath $temporary -Destination $archive -Force
        return $archive
    }
    Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
    $hint = ''
    if ($mismatch) {
        $hint = ' If upstream republished this archive, check the new file against an upstream-published checksum before updating its Sha256 in Prepare-Dependencies.ps1.'
    }
    throw "$($Package.Name) could not be fetched: $($failures -join '; ').$hint"
}

function Expand-Package([string]$Archive, [string]$Target) {
    # Release archives wrap everything in one versioned directory (SDL3-3.4.4/, tracy-<sha>/) while
    # dawn-build's package is flat; either way the tree lands directly under the target.
    $extract = Join-Path (Split-Path -Parent $Target) ('.extract-' + (Split-Path -Leaf $Target))
    if (Test-Path -LiteralPath $extract) { Remove-Item -LiteralPath $extract -Recurse -Force }
    [IO.Directory]::CreateDirectory($extract) | Out-Null
    # Symbolic links need a privilege Windows tar usually lacks; the only ones in these archives
    # are zstd test aliases, and none are build inputs.
    $listing = & $tar -tvf $Archive
    if ($LASTEXITCODE -ne 0) { throw "Listing $Archive failed with exit code $LASTEXITCODE." }
    $excludes = @($listing | ForEach-Object { if ($_ -match '^l.*\s(\S+)\s->\s') { '--exclude=' + $Matches[1] } })
    & $tar -xf $Archive -C $extract @excludes
    if ($LASTEXITCODE -ne 0) { throw "Extracting $Archive failed with exit code $LASTEXITCODE." }
    $entries = @(Get-ChildItem -LiteralPath $extract -Force)
    $source = $extract
    if ($entries.Count -eq 1 -and $entries[0].PSIsContainer) { $source = $entries[0].FullName }
    if (Test-Path -LiteralPath $Target) { Remove-Item -LiteralPath $Target -Recurse -Force }
    Move-Item -LiteralPath $source -Destination $Target
    if (Test-Path -LiteralPath $extract) { Remove-Item -LiteralPath $extract -Recurse -Force }
}

# Records which archive a tree came from, so a pin change replaces a stale tree.
function Get-Stamp($Package) { "$($Package.File) $($Package.Sha256)" }

function Test-Stamp([string]$Target, [string]$Text) {
    $stamp = $Target + '.source'
    (Test-Path -LiteralPath $stamp -PathType Leaf) -and ([IO.File]::ReadAllText($stamp) -eq $Text)
}

function Install-Package($Package, [string]$Target, [string]$RequiredFile) {
    $ready = if ($RequiredFile) { Test-Path -LiteralPath $RequiredFile -PathType Leaf } else { Test-Path -LiteralPath $Target -PathType Container }
    if ($ready -and (Test-Stamp $Target (Get-Stamp $Package))) { return }
    Expand-Package (Get-Archive $Package) $Target
    [IO.File]::WriteAllText($Target + '.source', (Get-Stamp $Package))
    Write-Host "Prepared $($Package.Name)"
}

foreach ($package in $packages) {
    Assert-Pinned $package
    Install-Package $package (Join-Path $Destination $package.Name)
}

$cppWinRt = Join-Path $Destination 'cppwinrt'
$cppWinRtCurrent = (Test-Path -LiteralPath (Join-Path $cppWinRt 'winrt\base.h') -PathType Leaf) -and
    (Test-Stamp $cppWinRt (Get-Stamp $cppWinRtTool))
if (-not $cppWinRtCurrent) {
    $toolRoot = Join-Path $PSScriptRoot 'artifacts\cppwinrt-tool'
    $compiler = Join-Path $toolRoot 'bin\cppwinrt.exe'
    Install-Package $cppWinRtTool $toolRoot $compiler
    if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) { throw "cppwinrt.exe is missing: $compiler" }
    if (Test-Path -LiteralPath $cppWinRt) { Remove-Item -LiteralPath $cppWinRt -Recurse -Force }
    [IO.Directory]::CreateDirectory($cppWinRt) | Out-Null
    Write-Host "Generating C++/WinRT headers (-input $CppWinRtInput)..."
    & $compiler -input $CppWinRtInput -output $cppWinRt
    if ($LASTEXITCODE -ne 0) { throw "cppwinrt.exe failed with exit code $LASTEXITCODE." }
    if (-not (Test-Path -LiteralPath (Join-Path $cppWinRt 'winrt\base.h') -PathType Leaf)) {
        throw "cppwinrt.exe produced no winrt\base.h under $cppWinRt"
    }
    Copy-Item -LiteralPath (Join-Path $toolRoot 'LICENSE') -Destination (Join-Path $cppWinRt 'LICENSE.txt')
    [IO.File]::WriteAllText($cppWinRt + '.source', (Get-Stamp $cppWinRtTool))
    Write-Host 'Prepared cppwinrt'
}

Write-Host "Pinned dependency sources ready: $Destination"
