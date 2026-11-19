# Project structure

A map of the repository: what lives where, how the pieces connect, and where the
knobs behind common "I want to hack on this" questions actually sit. It complements
[`translator/README.md`](../translator/README.md), which documents the recompiler itself,
and [`docs/building-macos.md`](building-macos.md), which walks through a full manual
build on macOS.

## The short version

| Path | What it is |
| --- | --- |
| `translator/` | The C# (.NET 8) static recompiler: `Translator.Cli` (pipeline driver) and `Translator.Core` (shared helpers), plus a test suite that needs no game data |
| `projects/` | Per-region translation profiles: `mkwii` (PAL `RMCP01`), `mkwii-ntsc-u` (`RMCE01`), `mkwii-ntsc-j` (`RMCJ01`), and `examples/generic-dol.yml` for translating arbitrary DOLs |
| `runtime/` | The C++ runtime the translated code links against: the guest OS surfaces in `src/hle/`, input and settings UI, and a host-only test suite in `tests/` |
| `aurora-main/` | Submodule: the aurora fork providing the GX rendering/windowing backend |
| `Launcher/` | The Setup applications (Windows/Linux/macOS), the orchestration and packaging scripts, and the `Test-*.ps1` gate checks |
| `docs/` | The documents you are reading right now |
| `generated/` | Translator output (created during a build; not committed): emitted C++ and the `build_shards/shards.cmake` manifest CMake consumes |

## From disc image to native app

1. **Extraction and verification.** Setup resolves and runs `nodtool`
   ([encounter/nod](https://github.com/encounter/nod)) for Wii disc validation and
   extraction; see `Launcher/WiiCompiled.Setup.Common/NodToolProvider.cs`. ISO, GCM,
   GCZ, CISO, WBFS, WIA and RVZ images are accepted, so you never need to convert a
   WBFS yourself; the toolchain handles the format and verifies the disc is a clean
   supported revision before anything else runs.
2. **Translation.** The translator compiles to `dotnet build translator/Translator.sln`
   and is driven by a `projects/<region>/recomp.yml` profile. The bulk of the pipeline
   lives in `translator/src/Translator.Cli/Program.cs`; `Translator.Core` holds the
   shared index/parsing helpers. See [`translator/README.md`](../translator/README.md)
   for how a DOL is translated and what goes into a manifest.
3. **Native build.** The translator emits the generated C++ and a
   `generated/build_shards/shards.cmake` manifest. Configuring `runtime/` with CMake
   picks that manifest up through `MKW_TRANSLATED_SHARD_MANIFEST`
   (`runtime/CMakeLists.txt`), and `runtime/cmake/PublicProducts.cmake` links the
   base-game and Retro Rewind executables. `Launcher/LocalBuild.ps1`,
   `Launcher/local-build.sh` and `Launcher/local-build-macos.command` orchestrate the
   whole pipeline end to end; `docs/building-macos.md` shows the individual commands.

## The runtime

`runtime/src` is game-independent; per-game glue lives in `runtime/game` and
`runtime/src/product` (one product for the base game, one for Retro Rewind). The
largest piece is `runtime/src/hle/`, the guest OS surfaces the translated code calls
into: `input/` (PAD), `storage/` (DVD, NAND, Riivolution), `bluetooth/` (Wii Remotes,
SYSCONF), `net/` and `os/`. Top-level files implement host-side features: the F10
settings bar (`settings_overlay.cpp`), input expressions and bindings
(`input_expr.cpp`, `input_bindings.cpp`), audio, memory, and the fiber manager.

`runtime/tests/` holds the host platform tests. They build and run without graphics
dependencies or game assets:

```bash
cmake -S runtime -B build-platform -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DMKW_BUILD_PRODUCTS=OFF -DMKW_PLATFORM_TESTS_ONLY=ON
cmake --build build-platform && ctest --test-dir build-platform
```

This is the same configuration CI runs on every pull request, and the cheapest way to
check a runtime change compiles and behaves before you ever involve a game dump.

## Floating-point and ISA policy

Two questions that cost time to answer from source alone:

**Where is `-ffast-math`, and why?** It is not a project-wide setting; there are three
tiers. Translated PowerPC code is compiled with `-fno-fast-math -ffp-contract=off`
(`MKW_TRANSLATED_PPC_FP_OPTIONS` in `runtime/CMakeLists.txt`, applied in
`runtime/cmake/PublicProducts.cmake`) because guest floating point must match the
original hardware (the README FAQ covers how ghosts prove the physics match).
Host-side runtime code is
compiled with `-O3 -ffast-math` (`mkw_apply_common_compile_options` in
`runtime/cmake/RuntimeLibrary.cmake`), where fast math is safe. Where exactness is
load-bearing even on the host side, individual files opt back out; see the
`frame_interpolation.cpp` `-fno-finite-math-only` override in `runtime/CMakeLists.txt`
and the `-fno-fast-math` flags on the host tests in
`runtime/cmake/PlatformTests.cmake`.

**Where is the SSE4.2 requirement?** There is no intrinsic-by-intrinsic dependency to
find. x86-64 builds compile with `-march=x86-64-v3` by default (`MKW_X86_CPU_PROFILE`,
selectable `v2`/`v3`, applied as `MKW_BASELINE_ARCH_FLAG` in
`runtime/cmake/RuntimeLibrary.cmake`); v3 is what pulls in SSE4.2, AVX2 and friends.
Two deliberate exceptions: the startup CPU check in `runtime/src/host_cpu_baseline.cpp`
builds at the plain baseline ISA so the check itself runs on anything (see the
`mkw_cpu_baseline` object library in `runtime/cmake/PublicProducts.cmake`), and the
vendored Crypto++ disables its algorithm-specific SIMD so the static linker only
retains what the runtime's own policy decides. AArch64 builds, including Apple
Silicon, compile with `-mcpu=native` instead. Experiments like SIMDe or
`std::simd` would start from these same knobs.

## Build and release scripts

Everything that turns the above into shippable artifacts lives in `Launcher/`:
dependency preparation (`Prepare-Dependencies.ps1`, `prepare-portable-tools.sh`), the
native prebuilt harvest that lets end-user installs skip recompiling aurora and the
vendored libraries (`Prepare-NativePrebuilt.ps1` / `.sh`, consumed through
`runtime/cmake/NativePrebuilt.cmake`), packaging (`Build-Installer.ps1`,
`build-appimage.sh`, `macos/`), and the release gates (`Test-Recompilation.ps1`,
`Test-PinnedFacts.ps1`, `Test-PayloadBoundary.ps1`, `Test-NativeDependencies.ps1`).
Wheel Wizard, the recommended installer frontend, drives these same pieces.
