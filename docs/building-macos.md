# Building WiiCompiled and Retro Rewind on macOS

This guide covers building **WiiCompiled** (base game) and **Retro Rewind** from source on macOS for Apple Silicon (`arm64`). Follow these instructions to compile the native executables directly.

> [!NOTE]
> If you only want to build the base game (**WiiCompiled**), look for sections marked **`(Skip if only building WiiCompiled)`** to bypass Retro Rewind and online payload steps.

---

## 1. Prerequisites

### System Requirements
- **Hardware**: Apple Silicon Mac (M1/M2/M3/M4)
- **Operating System**: macOS 14 (Sonoma) or later
- **Xcode Command Line Tools**:
  ```bash
  xcode-select --install
  ```

### Toolchain Dependencies
Install the required tools using [Homebrew](https://brew.sh):
```bash
brew install cmake ninja
brew install --cask dotnet-sdk@8
```

Verify that Clang, CMake, Ninja, and the .NET 8 runtime are available:
```bash
clang --version
cmake --version
ninja --version
dotnet --list-runtimes   # Must list Microsoft.NETCore.App 8.x
```

---

## 2. Required Game and Mod Assets

Due to legal requirements, no proprietary Nintendo assets or code are included in this repository. You must provide your own legally dumped game files.

1. **Mario Kart Wii PAL (`RMCP01`) Disc Image** *(Required)*:
   - Supported formats: `.iso`, `.wbfs`, `.ciso`, `.rvz`, `.gcm`, `.gcz`.
2. **nodtool** *(Required for disc extraction)*:
   - Download the macOS Apple Silicon binary of [nodtool](https://github.com/encounter/nod/releases):
     ```bash
     curl -fsSL "https://github.com/encounter/nod/releases/download/v2.0.0-alpha.10/nodtool-macos-arm64" -o nodtool
     chmod +x nodtool
     ```
3. **Retro Rewind Distribution** *(Skip if only building WiiCompiled)*:
   - Download the [Retro Rewind](https://wiki.tockdom.com/wiki/Retro_Rewind) release package. You will need the `RetroRewind6` folder (which contains `Binaries/Code.pul`).
4. **Retro-WFC Payload** *(Skip if only building WiiCompiled or building offline)*:
   - Required for online multiplayer on Retro Rewind. Downloaded during setup from `http://nas.play.rwfc.net/payload?g=RMCPD00`.

---

## 3. Step 1: Extract Disc Assets

Extract your clean PAL `RMCP01` disc into the `Assets/` directory of the repository:

```bash
# Using nodtool directly into a temporary scratch directory
mkdir -p /tmp/mkw-extract
./nodtool extract /path/to/RMCP01.iso /tmp/mkw-extract

# Copy extracted assets into the repository Assets directory
rm -rf Assets/DATA/files Assets/DATA/sys
mkdir -p Assets/DATA
cp /tmp/mkw-extract/*/sys/main.dol Assets/main.dol
cp /tmp/mkw-extract/*/files/rel/StaticR.rel Assets/StaticR.rel
cp -R /tmp/mkw-extract/*/files Assets/DATA/files
cp -R /tmp/mkw-extract/*/sys Assets/DATA/sys

# Clean up temporary files
rm -rf /tmp/mkw-extract
```

> [!TIP]
> Alternatively, you can use the repository's helper script:
> ```bash
> Launcher/macos/extract-disc.command --game /path/to/RMCP01.iso --assets-dir Assets --nodtool ./nodtool
> ```

### Verify Extracted Asset Hashes
Confirm that the extracted files match the expected clean PAL revision:
```bash
shasum -a 256 Assets/main.dol Assets/StaticR.rel
```
- `Assets/main.dol`: `80d18895b39c63bd80f457398bfcbb91b7d16ac116a41a88967e954080155b05`
- `Assets/StaticR.rel`: `16d9d146112541fefea701ecb5bc1a496f9d50e4a752fbb5b6778e7c6399f67d`

---

## 4. Step 2: Build the Translator CLI

Compile the static recompiler CLI:

```bash
dotnet build translator/src/Translator.Cli/Translator.Cli.csproj -c Release
```

Define a shell function to invoke the translator (ensuring paths with spaces are handled safely):
```bash
translator() {
  dotnet "$(pwd)/translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll" "$@"
}
```

---

## 5. Step 3: Translation

### A. Translate Base Game Functions
```bash
mkdir -p generated/functions build/base

translator translate-recursive 0x8000629c \
  --project projects/mkwii/recomp.yml \
  --outdir generated/functions \
  --output-metadata generated/base_translation_output.json \
  --production-source-bundle generated/base_translation_sources.bin \
  --no-function-files \
  --prune-stale \
  --threads $(sysctl -n hw.ncpu)
```

### B. Emit Base Manifest
```bash
translator emit-base-manifest \
  --project projects/mkwii/recomp.yml \
  --out build/base \
  --functions-dir generated/functions \
  --translation-output-metadata generated/base_translation_output.json \
  --region P
```

---

### C. Stage and Translate Retro Rewind *(Skip this step if you only want to build WiiCompiled)*

1. Stage `Code.pul`:
   ```bash
   RETRO_DIR="/path/to/RetroRewind6"
   mkdir -p PulsarPacks/completed/RetroRewind/RetroRewind6/Binaries
   cp "$RETRO_DIR/Binaries/Code.pul" PulsarPacks/completed/RetroRewind/RetroRewind6/Binaries/Code.pul
   ```

2. **Retro-WFC Payload Setup (for Online Multiplayer)**:
   Online play in Retro Rewind requires the shared Retro-WFC payload. Download and validate it:
   ```bash
   mkdir -p build/retro-wfc/binary
   curl -fsSL --retry 3 "https://nas.play.rwfc.net/payload?g=RMCPD00" \
     -o build/retro-wfc/binary/payload.RMCPD00.bin

   # Validate payload signature and integrity
   translator validate-retro-wfc-payload --directory build/retro-wfc
   ```

3. Run Retro Rewind translation:
   ```bash
   mkdir -p build/mods/retro_rewind_full_cpp

   translator translate-mod \
     --project projects/mkwii/recomp.yml \
     --profile retro-rewind \
     --base-manifest build/base/mkwii_base_manifest.json \
     --base-translation-output-metadata generated/base_translation_output.json \
     --code-pul "$RETRO_DIR/Binaries/Code.pul" \
     --mod-root "$RETRO_DIR" \
     --mod-name "Retro Rewind" \
     --region P \
     --out build/mods/retro_rewind_full_cpp \
     --prefer-cached-inputs \
     --emit-cpp \
     --threads $(sysctl -n hw.ncpu) \
     --retro-wfc-payload build/retro-wfc/binary/payload.RMCPD00.bin
   ```
   > [!TIP]
   > If you do not want online play or do not have an internet connection, replace `--retro-wfc-payload ...` with `--skip-retro-wfc`.

---

### D. Generate Data Initialization and Build Shards

First, generate the embedded game data initializer:
```bash
translator generate-data-init --project projects/mkwii/recomp.yml
```

Next, generate the CMake build shards using **one** of the following options:

#### Option 1: Base Game Only (WiiCompiled)
```bash
mkdir -p generated/build_shards
translator emit-build-shards \
  --project projects/mkwii/recomp.yml \
  --base-metadata generated/base_translation_output.json \
  --base-functions-dir generated/functions \
  --native-source-dir runtime/src \
  --out generated/build_shards
```

#### Option 2: Base Game + Retro Rewind
```bash
mkdir -p generated/build_shards
translator emit-build-shards \
  --project projects/mkwii/recomp.yml \
  --base-metadata generated/base_translation_output.json \
  --base-functions-dir generated/functions \
  --native-source-dir runtime/src \
  --out generated/build_shards \
  --resolved-profile build/mods/retro_rewind_full_cpp/resolved_dispatch_profile.json \
  --retro-cpp-dir build/mods/retro_rewind_full_cpp/cpp
```

---

## 6. Step 4: Configure and Compile with CMake & Ninja

Configure the native C++ build targeting Apple Silicon:

```bash
cmake -S runtime -B build-macos -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DAURORA_SDL3_PROVIDER=vendor
```

Compile the desired target:

```bash
# To build WiiCompiled only:
cmake --build build-macos --target WiiCompiled --parallel $(sysctl -n hw.ncpu)

# OR to build both WiiCompiled and Retro Rewind:
cmake --build build-macos --target WiiCompiled RetroRewind --parallel $(sysctl -n hw.ncpu)
```

Once compilation completes, the executables are ready in your build directory:
- `build-macos/WiiCompiled`
- `build-macos/RetroRewind` (if built)

During the build, CMake automatically copies the required runtime assets into `build-macos/`:
- `build-macos/dsp_coef.bin`
- `build-macos/initial_pipeline_cache.db`
- `build-macos/wii_bootstrap/`

---

## 7. Step 5: Running Executables from the Build Folder

### Configure `Config.toml`
The runtime reads configuration from `~/Library/Application Support/WiiCompiled/Config.toml`.

Create the directory and configuration file:

```bash
mkdir -p "$HOME/Library/Application Support/WiiCompiled"
```

#### For Base Game Only (WiiCompiled):
```toml
# ~/Library/Application Support/WiiCompiled/Config.toml
[video]
widescreen = true
resolution_multiplier = 1.0
graphics_api = "metal"

[paths]
dvd_root = "/absolute/path/to/Wiicompiled/Assets/DATA"
```

#### For Base Game and Retro Rewind:
```toml
# ~/Library/Application Support/WiiCompiled/Config.toml
[video]
widescreen = true
resolution_multiplier = 1.0
graphics_api = "metal"

[paths]
dvd_root = "/absolute/path/to/Wiicompiled/Assets/DATA"
retro_rewind_root = "/path/to/RetroRewind6"
```

> [!NOTE]
> Ensure `dvd_root` points to the directory containing `files` and `sys/fst.bin`.

### Launching the Game
Run the compiled binaries directly from your terminal or by double clicking:

```bash
# Run base WiiCompiled
./build-macos/WiiCompiled

# Run Retro Rewind
./build-macos/RetroRewind
```



Press **F10** in-game at any time to open the configuration bar (controls, resolution, display settings, audio).

---

## Quick Reference: Automated Helper Script

The repository provides a script (`Launcher/local-build-macos.command`) that handles extraction, translation, and compilation in a single command.

### Building Base Game Only:
```bash
Launcher/local-build-macos.command \
  --profile base \
  --output-dir build-macos/Products \
  --game /path/to/RMCP01.iso \
  --nodtool ./nodtool
```

### Building Both (with Online Retro-WFC Payload):
```bash
# 1. Download Retro-WFC payload into a staging directory:
mkdir -p build/retro-wfc/binary
curl -fsSL --retry 3 "http://nas.play.rwfc.net/payload?g=RMCPD00" \
  -o build/retro-wfc/binary/payload.RMCPD00.bin

# 2. Run the automated build with the payload directory:
Launcher/local-build-macos.command \
  --profile both \
  --output-dir build-macos/Products \
  --base-output-dir build-macos/Products \
  --game /path/to/RMCP01.iso \
  --nodtool ./nodtool \
  --retro-rewind-package-dir /path/to/RetroRewind6 \
  --retro-wfc-offline-dir build/retro-wfc
```

### Building Both (Offline, Skipping Payload):
```bash
Launcher/local-build-macos.command \
  --profile both \
  --output-dir build-macos/Products \
  --base-output-dir build-macos/Products \
  --game /path/to/RMCP01.iso \
  --nodtool ./nodtool \
  --retro-rewind-package-dir /path/to/RetroRewind6 \
  --skip-retro-wfc-payload
```

When finished, the compiled executables reside in `native-build-macos/` and the bundled `.app` packages are placed in `build-macos/Products/`.
