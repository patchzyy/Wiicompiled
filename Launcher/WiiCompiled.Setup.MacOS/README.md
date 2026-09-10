# WheelWizard Mac setup backend

A self-contained .NET CLI for macOS 14+ on Apple Silicon. It implements the WheelWizard v1 NDJSON
contract used by the Windows installer: `--version`, `--silent`, `--check-products`,
`--repair-products`, `--launch-base`, and `--launch-retro`. Install takes `--game`, `--install-dir`,
`--portable`, and optionally `--retro-dir` with exactly one of `--download-retro-wfc-payload` or
`--skip-retro-wfc-payload`. Repair and check receive `--install-dir` and `--retro-dir`.

The install location must be `<portable-root>/Install`. Its neighboring `Cache/BuildWorkspace`
contains only locally extracted and translated game data. `UserData/Config.toml` is shared with
WheelWizard and the native runtime. Native bundles sit directly in `Install` so their executable
folders are within the runtime's four-parent portable-marker search bound.

`--check-products` returns 0 for current products, 2 for required repairs, or 1 when the check fails.
It checks installation identity, hashes the installed setup/source/tools and product bundles, checks
runtime data paths, and fingerprints the same Retro Rewind compile inputs as the Windows installer.
Asset-only changes do not rebuild. A repair currently runs the Mac build script for both products;
Ninja can reuse unchanged native compilation output. This does not implement Windows' individual
product build scheduling.

Publication stages apps and the repair host beside the live installation. A journal preserves the
previous configuration and installation until commit. The next operation recovers interrupted
publication before checking or launching. A sibling `.mkwc-operation-macos.lock` is held for every
operation and the full lifetime of games launched by this helper. Directly launched apps do not hold
that lock. WheelWizard requests cooperative cancellation with `MKWCOMPILED_CANCEL_FILE`; the helper
stops build children and returns a terminal failure before publication, while a committed operation
reports success. WheelWizard kills the process tree if the helper does not respond within ten seconds.

Build the game-code-free release asset using verified native tools:

```sh
Launcher/macos/build-wheelwizard-setup.command \
  --tools-root '/Applications/WiiCompiled Setup.app/Contents/Resources/tools' \
  --output Launcher/dist/WiiCompiled-Setup-macos-arm64.run
```

The builder packages only tracked source files and supplied tools. The self-extracting `.run` archive
uses a private temporary directory, supports a fast `--version` query, and leaves a self-contained
repair host inside the installation. It needs neither a system .NET runtime nor administrator access.
Xcode Command Line Tools remain required to compile the user's disc locally.

Validation:

```sh
dotnet test Launcher/WiiCompiled.Setup.MacOS.Tests
```

For an end-to-end test, pass a user-owned clean PAL RMCP01 disc and an installed RetroRewind6 folder
to the setup, check products, launch through WheelWizard, and confirm the runtime uses Metal and the
portable configuration. Online matchmaking requires a separate live test; embedding a verified
Retro-WFC payload alone does not establish online compatibility.
