# Wiicompiled Static Recompiler

The translator parses GameCube/Wii DOL files, decodes PowerPC instructions, lifts them through
IR/SSA and type inference, and emits C++ that is compiled into a native executable together with
a runtime in `runtime/`. Project-specific paths and addresses are supplied through a versioned
YAML manifest, so the translator itself contains no game-specific data.

> Translating a DOL does not exempt you from owning the game it came from.

## How translating a DOL works

A project manifest (YAML) names the input DOL and pins its layout; everything else is derived.

Translation is four commands:

1.  **`translate-recursive <entry-point> --project <manifest>`** - walks the call graph from the
entry point, decodes every reachable function, and emits C++ (plus JSON metadata describing
what was emitted).

2.  **`generate-data-init --project <manifest>`** - writes the embedded `.data`/`.rodata`/`.sdata`
section initializer and `RuntimeConfig.h`. 

3.  **`emit-build-shards --project <manifest>`** - emits the CMake build graph (`shards.cmake`)
covering both generated sources and `runtime/src`.

4.  **CMake + Ninja with Clang** compiles `runtime/` plus the generated output into one executable.

Discovery is purely recursive from the entry point unless the manifest provides an optional
`function_map` (one `hexaddr name` per line) that seeds additional function boundaries. Unsupported
instructions fail translation by default.

See `projects/examples/generic-dol.yml` for a minimal manifest driven by `RECOMP_GENERIC_DOL`.

## Prerequisites

| Tool | Notes |
| --- | --- |
| .NET 8 SDK | Builds and runs the translator. |
| CMake ≥ 3.16 and Ninja | Configures and drives the native build. |
| Clang / LLVM | The shipped build uses LLVM-MinGW targeting `x86-64-v3`. MSVC is not the tested path. |

Build the CLI once and invoke the assembly directly:

```powershell
dotnet build translator/src/Translator.Cli/Translator.Cli.csproj -c Release
$translator = 'translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll'
```

## Manifest essentials

-  `inputs.dol.path` - the DOL to translate; optional SHA-256 pinning rejects wrong revisions.
-  `memory.base` / `size` - guest address space.
-  `memory.sda_base` / `sda2_base` - the r13/r2 Small Data Area bases your DOL's boot code installs
(`lis`/`ori` pairs in `__init_registers`). Required by any command that writes `RuntimeConfig.h`;
the translator does not guess them.
-  `translation.function_map.path` - optional symbol map used as the discovery oracle.
-  `translation.allow_unsupported_instructions` - off by default; enabling it emits runtime traps
instead of failing, and such a build can never ship.

Relative paths resolve from `workspace_root`, which itself resolves from the manifest directory.

## Commands

-  `info [--project path]`
-  `translate-recursive <address> --project path`
-  `generate-data-init --project path`
-  `emit-base-manifest --project path`
-  `emit-build-shards --project path`
-  `translate-mod --project path [--profile name] ...` - static Kamek/Pulsar module translation

Any command prints its own option list with `--help`.

## Test

```powershell
dotnet test translator/Translator.sln -c Release
```
