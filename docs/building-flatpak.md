# Building the WiiCompiled Flatpak

This guide covers building the **Linux distribution asset**: the self-contained setup tool
packaged as a Flatpak bundle (`WiiCompiled-Setup-<arch>.flatpak`, ~185 MiB) that users download
and run with no `dotnet`, toolchain, or build prerequisites of their own. The bundle carries the
installer and translator binaries, bundled nodtool, a pruned clang/lld/cmake/ninja toolchain, the
precompiled aurora package, and a workspace snapshot, laid out under a Flatpak `/app` running on
the `org.freedesktop.Sdk` runtime. The same scripts CI uses produce the release asset, so a local
build and the release bundle are one and the same.

> [!NOTE]
> Local-build script: `Launcher/build-flatpak.sh`. It stages the `/app` tree, renders
> `Launcher/flatpak/io.github.TeamWheelWizard.Wiicompiled.yml.in`, runs
> `flatpak-builder`, exports the repo, and `build-bundle`s the single-file asset. The
> script's header explains the SDK-as-runtime design and every step.

---

## Two-phase builds: why staging runs in a container

`flatpak-builder` needs to actually mount the sandbox when it builds, and that means bwrap —
bubblewrap — which has to mount a new `proc` namespace. Inside a container (Docker/podman) that
mount cannot happen, so `build-flatpak.sh` splits the work:

1. **`--staged-only`** — publish the binaries, prepare the toolchain / native prebuilt, snapshot
   the workspace, render the manifest. Requires a toolchain but no sandbox, so it runs fine
   inside a container.
2. **`--skip-stage`** — run `flatpak-builder` + export + bundle from an existing staging tree.
   Requires bwrap, so it must run on the host (or any machine where bubblewrap works).

A host with working bwrap just runs the script in **full** mode (default) and no split is needed —
this is exactly what CI does on its Linux VM runners.

---

## 1. Prerequisites

### Host
- A Linux machine (NixOS, Ubuntu, …) with `git` and, for the finish step, `flatpak` +
  `flatpak-builder` working with bwrap:
  - **Ubuntu/Debian**: `sudo apt-get install flatpak flatpak-builder dbus`
  - **NixOS**: `nix shell nixpkgs#flatpak-builder nixpkgs#flatpak`
- The `org.freedesktop.Sdk//25.08` runtime installed. If it isn't, `build-flatpak.sh` prints the
  exact `flatpak remote-add` / `flatpak install` commands to run.

### Container
- `podman` (recommended, rootless) or `docker`, to run the staging phase.

---

## 2. Build the staging image

```bash
container/build-flatpak.sh --build
```

This builds `wiicompiled/flatpak-env:22.04` from `container/Dockerfile`: an `ubuntu:22.04` image
with the same dependency set CI installs, the .NET 8 SDK, and the `org.freedesktop.Sdk//25.08`
runtime baked in. The image is large (~2 GiB) and the first build downloads it all — allow a few
minutes.

## 3. Stage the app tree in the container

```bash
container/build-flatpak.sh
```

The wrapper bind-mounts the repository into the container, runs
`bash Launcher/build-flatpak.sh --staged-only`, and — when a `~/.nuget/packages` cache exists —
mounts it in so repeated `dotnet restore`s are fast. Staging writes only under
`Launcher/artifacts/` (git-ignored); nothing in the source tree is touched.

> [!NOTE]
> With rootless podman the container's root user is mapped to your host user, so the staged
> files are owned by you. With rootful `docker` they end up root-owned; run
> `sudo chown -R "$(id -u):$(id -g)" Launcher/artifacts` afterwards if you need to finish on the
> host later.

Common flags: `--podman` / `--docker` to force a runtime, `--image` to override the tag, and
anything after `--` is passed through to `build-flatpak.sh` (e.g. `-- --runtime-branch 25.08`).

## 4. Finish the bundle on the host

Run the command the wrapper prints when staging succeeds:

```bash
# NixOS (flatpak-builder is not on the host PATH):
nix shell nixpkgs#flatpak-builder nixpkgs#flatpak -c \
    bash Launcher/build-flatpak.sh --skip-stage

# Ubuntu/Debian host (flatpak + flatpak-builder installed):
bash Launcher/build-flatpak.sh --skip-stage
```

Output: `Launcher/dist/WiiCompiled-Setup-<arch>.flatpak`.

---

## 5. The single-host route (Ubuntu/Debian, or CI)

On any machine where bwrap works — a standard Linux VM like a GitHub Actions runner — there's no
container step at all:

```bash
sudo apt-get install -y flatpak flatpak-builder dbus build-essential git make pkg-config \
  cmake ninja-build libxml2 libicu70 # + the SDL3 dev deps listed in .github/workflows/package.yml
flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak install --user -y flathub org.freedesktop.Sdk//25.08
# install the .NET 8 SDK, then:
bash Launcher/build-flatpak.sh
```

The CI `linux-flatpak` job in `.github/workflows/package.yml` automates exactly this, except for
ARM64: `aarch64` bundles require an ARM64 host (CI uses `ubuntu-22.04-arm`) — an x86 container or
runner cannot produce one.

---

## 6. Smoke-test the bundle

The bundle is the release contract, so install it and verify inside the sandbox:

```bash
flatpak install --user --noninteractive -y \
  ./Launcher/dist/WiiCompiled-Setup-x86_64.flatpak
flatpak run io.github.TeamWheelWizard.Wiicompiled --version
flatpak info --show-permissions io.github.TeamWheelWizard.Wiicompiled \
  | grep -E '^(devices=.*input|sockets=.*session-bus)'
flatpak run --command=sh io.github.TeamWheelWizard.Wiicompiled -c 'test -d /dev/input && echo gamepad-devices-present'
```

---

## Troubleshooting

- **`org.freedesktop.Sdk//25.08 is not installed`** — `build-flatpak.sh` prints the exact
  `remote-add` / `install` commands; run them. The image already has this baked in, so this only
  hits on the host finish step.
- **`bwrap: ... Operation not permitted` / proc mount failures** — flatpak-builder cannot run
  inside a container. Run the `--skip-stage` step on the host with working bwrap.
- **Staged files are root-owned** — rootful `docker` wrote them; `chown` the
  `Launcher/artifacts` directory back to your user (see above).
- **`Native prebuilt package is missing or stale; harvesting a fresh one`** — every staging run
  goes through `Prepare-NativePrebuilt.sh`, which compiles aurora once when its fingerprint
  changes. Slow the first time, reused afterwards.
- **`lld has dependencies the build host cannot resolve`** — the toolchain's `lld` needs
  `libxml2.so.2` / `libicuuc.so.70` / `libicudata.so.70` present on the host *at build time*,
  because they do not exist in the sandbox runtime. This is why the manifest copies them into the
  bundle and the `--staged-only` host (or container) must have `libxml2 libicu70` installed.