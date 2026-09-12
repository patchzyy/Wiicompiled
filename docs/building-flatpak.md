# Building the WiiCompiled Flatpak

This guide covers building the **Linux distribution asset**: the self-contained setup tool
packaged as a Flatpak bundle (`WiiCompiled-Setup-<arch>.flatpak`, ~210 MiB compressed, ~925 MiB
installed) that users download and run with no `dotnet`, toolchain, or build prerequisites of
their own. The bundle carries the installer and translator binaries, bundled nodtool, a pruned
clang/lld/cmake/ninja toolchain, the precompiled aurora package, a workspace snapshot, and the
SDK compile dev files the toolchain resolves against, laid out under a Flatpak `/app` running on
`org.freedesktop.Platform`. The same script CI uses produces the release asset, so a local build
and the release bundle are one and the same.

> [!NOTE]
> Everything is `Launcher/build-flatpak.sh`: it stages a slim copy of the source tree, renders
> `Launcher/flatpak/io.github.TeamWheelWizard.Wiicompiled.yml.in`, runs `flatpak-builder`,
> exports the repo, and `build-bundle`s the single-file asset. The script's header explains the
> Platform-as-runtime + bundled-devroot design.

---

## 1. How the bundle works

The app installs on `org.freedesktop.Platform` — **not** the SDK. The bundled toolchain compiles
the user's game at *install time* inside the running sandbox, and that link step needs glibc/CRT
headers, the libstdc++ header tree, and crt object files / linker scripts that only the SDK
ships. So the `dev-files` manifest module copies exactly those SDK subtrees into `/app/usr`, and
the toolchain is pointed at them with `--sysroot=/app` (see the sandbox entrypoint in
`Launcher/flatpak/files/wiicompiled-setup`). The full module pipeline is:

1. **toolchain** — prunes official pinned clang/lld/llvm-ar, CMake and Ninja releases into
   `/app/usr/toolchain`, the same way `Launcher/prepare-portable-tools.sh` does on a build host.
2. **lld-host-libs** — bundles the `libxml2` / ICU 70 host libs the official lld release resolves
   on Ubuntu 22.04, which the Platform runtime does not provide.
3. **native-prebuilt** — copies the verified precompiled aurora + third-party package harvested
   up front by `Launcher/Prepare-NativePrebuilt.sh` (an immutable, fingerprint-pinned artifact)
   into `/app/native-prebuilt`.
4. **dotnet-apps** — publishes the installer/translator against offline NuGet feeds
   (a pinned YamlDotNet package fetched up front from NuGet.org plus the dotnet8 SDK
   extension's own offline feed).
5. **dev-files** — copies the SDK's `/usr/include`, multiarch headers, gcc crt objects and the
   libc/libm linker scripts into `/app/usr`.
6. **workspace-snapshot** — copies the `runtime`/`aurora-main`/`projects` sources plus
   `local-build.sh` under `/app/workspace` (a `.bundle-version` stamp gates the entrypoint's
   re-sync to the writable data dir).
7. **runtime-files** — installs the sandbox entrypoint, desktop entry and generated icon.

Only archives with pinned sha256 are fetched at build time. The build sandbox has **no network**,
so every module is self-contained: pinned toolchain archives, pinned deb packages (launchpad,
for the lld host libs), pinned nodtool releases, and offline NuGet feeds.

---

## 2. Prerequisites

- A Linux machine (NixOS, Ubuntu, …) with `git` and `flatpak` + `flatpak-builder` working with
  bwrap. bwrap mounts a new proc namespace, so the build **cannot run inside a container** —
  run it on a host/VM (that is exactly what CI does on its Linux VM runners).
  - **Ubuntu/Debian**: `sudo apt-get install flatpak flatpak-builder dbus`
  - **NixOS**: `nix shell nixpkgs#flatpak-builder nixpkgs#flatpak nixpkgs#python3`
- The runtimes the build needs, installed up front (`build-flatpak.sh` also prints the exact
  commands if any are missing):
  ```bash
  flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
  flatpak install --user -y flathub \
    org.freedesktop.Platform//25.08 org.freedesktop.Sdk//25.08 \
    org.freedesktop.Sdk.Extension.dotnet8//25.08
  ```
- `python3` for the native-prebuilt provenance check (the script falls back to
  `nix shell nixpkgs#python3` when python3 is not on PATH).
- A .NET 8 SDK only if `Prepare-NativePrebuilt.sh` needs to harvest a fresh native-prebuilt
  package (it skips straight to the build when the stored fingerprint is current).

---

## 3. Build

```bash
bash Launcher/build-flatpak.sh
```

Output: `Launcher/dist/WiiCompiled-Setup-<arch>.flatpak`, built from
`Launcher/artifacts/flatpak-build/` (git-ignored staging tree, rendered manifest, build dir and
local repo).

Useful flags (see the script's `--help`):
- `--arch aarch64` — build for ARM64 (requires an ARM64 host; an x86 runner cannot produce one).
- `--runtime-branch 24.08` — build against a different freedesktop branch.
- `--flatpak PATH` / `--flatpak-builder PATH` — point at flatpak/flatpak-builder binaries that
  are not on the default PATH (NixOS: `nix shell nixpkgs#flatpak-builder nixpkgs#flatpak -c
  bash Launcher/build-flatpak.sh --flatpak-builder $(command -v flatpak-builder)`).

The `linux-flatpak` job in `.github/workflows/package.yml` automates the same steps on
`ubuntu-22.04` / `ubuntu-22.04-arm`, including a per-bundle compile smoke test inside the
sandbox.

---

## 4. Install and smoke-test the bundle

The bundle is the release contract, so install it and verify inside the sandbox:

```bash
flatpak install --user --noninteractive -y \
  ./Launcher/dist/WiiCompiled-Setup-x86_64.flatpak
flatpak run io.github.TeamWheelWizard.Wiicompiled --version
flatpak info --show-permissions io.github.TeamWheelWizard.Wiicompiled \
  | grep -E '^(devices=.*input|sockets=.*session-bus)'
flatpak run --command=sh io.github.TeamWheelWizard.Wiicompiled -c 'test -d /dev/input && echo gamepad-devices-present'
```

And the compile path that actually links the user's game (the devroot + `--sysroot=/app`):

```bash
flatpak run --command=bash io.github.TeamWheelWizard.Wiicompiled -c '
  export APP=/app
  export LIBRARY_PATH="$APP/usr/lib/x86_64-linux-gnu"
  export LD_LIBRARY_PATH="$APP/usr/toolchain/lib"
  printf "#include <vector>\nint main(){}\n" > /tmp/probe.cpp
  "$APP/usr/toolchain/bin/clang++" --sysroot="$APP" -fuse-ld=lld -std=c++20 /tmp/probe.cpp -o /tmp/probe
  /tmp/probe
'
```

---

## Troubleshooting

- **`org.freedesktop.Sdk.Extension.dotnet8//25.08 is not installed`** — run the
  `remote-add`/`install` commands `build-flatpak.sh` prints.
- **`bwrap: ... Operation not permitted` / proc mount failures** — flatpak-builder cannot run
  inside a container; run the build on a host/VM with working bwrap.
- **`ld.lld: ... cannot find /usr/lib/.../libc_nonshared.a inside /app`** — an lld error with a
  double sysroot prefix means the libc.so linker script was rewritten by hand. It must keep its
  original absolute `/usr/...` members: lld prepends `--sysroot` itself. The only member outside
  the mirrored libdir (`/lib64/ld-linux-x86-64.so.2`) is mirrored by `dev-files` into `/app/lib64`.
- **`NU1301`/`NU1101` restore errors** — the build sandbox has no network; the dotnet-apps.module
  resolves everything from the pinned YamlDotNet feed (fetched by flatpak-builder before the
  offline build starts) + the dotnet8 extension's offline feed.
- **Disk space** — each failed run can leave a multi-GB `.flatpak-builder/build/` tree; clean the
  build dir before a rerun (`rm -rf Launcher/artifacts/flatpak-build/.build-*`). Downloaded
  archives stay cached under `.flatpak-builder/downloads`.