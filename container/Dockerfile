# Staging image for the WiiCompiled Flatpak build (container/build-flatpak.sh).
#
# A container cannot run the flatpak-builder phase (bwrap cannot mount a new proc
# namespace inside a container), so this image only covers the --staged-only phase
# of build-flatpak.sh: publish the .NET binaries, prepare the toolchain/native
# prebuilt, snapshot the workspace, and render the manifest. The host then finishes
# with --skip-stage. Ubuntu 22.04 matches the GitHub Actions runners byte for byte.
#
# Build:  container/build-flatpak.sh --build
# Run:    container/build-flatpak.sh
# Docs:   docs/building-flatpak.md
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV DOTNET_CLI_TELEMETRY_OPTOUT=1
ENV DOTNET_NOLOGO=1

# The same dependency set the CI linux-flatpak job installs: flatpak (for the runtime
# installation below and the script's runtime check), dbus (session bus for flatpak),
# libxml2/libicu70 (host libs bundled into the app so the sandbox's lld resolves
# them), and build-essential/cmake/ninja + the SDL3 dev headers, which
# Prepare-NativePrebuilt.sh needs when it has to harvest a fresh precompiled package.
RUN apt-get update && apt-get install -y --no-install-recommends \
        flatpak \
        flatpak-builder \
        dbus \
        build-essential \
        libxml2 \
        libicu70 \
        git \
        make \
        pkg-config \
        cmake \
        ninja-build \
        gnome-desktop-testing \
        libasound2-dev \
        libpulse-dev \
        libaudio-dev \
        libfribidi-dev \
        libjack-dev \
        libsndio-dev \
        libx11-dev \
        libxext-dev \
        libxrandr-dev \
        libxcursor-dev \
        libxfixes-dev \
        libxi-dev \
        libxss-dev \
        libxtst-dev \
        libxkbcommon-dev \
        libdrm-dev \
        libgbm-dev \
        libgl1-mesa-dev \
        libgles2-mesa-dev \
        libegl1-mesa-dev \
        libdbus-1-dev \
        libibus-1.0-dev \
        libudev-dev \
        libthai-dev \
        libusb-1.0-0-dev \
        libpipewire-0.3-dev \
        libwayland-dev \
        libdecor-0-dev \
        liburing-dev \
        curl \
        unzip \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# The .NET 8 SDK, pinned to the 8.0 channel the way actions/setup-dotnet pins it in CI.
RUN curl -fsSL https://dot.net/v1/dotnet-install.sh -o /tmp/dotnet-install.sh \
    && chmod +x /tmp/dotnet-install.sh \
    && /tmp/dotnet-install.sh --channel 8.0 --install-dir /usr/local/dotnet \
    && rm /tmp/dotnet-install.sh \
    && ln -s /usr/local/dotnet/dotnet /usr/local/bin/dotnet

# The org.freedesktop.Sdk runtime that build-flatpak.sh renders into the manifest and
# checks with `flatpak info` before staging. Installed as the root user (the image's
# default account) using a session bus the way CI does; rootless podman maps the
# resulting files to the invoking host user automatically.
RUN flatpak remote-add --if-not-exists --user flathub https://flathub.org/repo/flathub.flatpakrepo \
    && dbus-run-session -- flatpak install --user --noninteractive -y flathub org.freedesktop.Sdk//25.08

WORKDIR /wiicompiled