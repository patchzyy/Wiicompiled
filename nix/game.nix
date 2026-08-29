# Native build derivation: configures runtime/ CMake against the translated
# shard graph and builds both products (WiiCompiled, RetroRewind) in one build
# so every profile-neutral shard object is shared between them (the same
# decision LocalBuild.ps1 makes with -Profile both).
#
# shards.cmake embeds absolute paths pointing at the translation workspace, so
# this derivation recreates that exact layout at /build/workspace: the repo
# sources plus the translation outputs in their original positions. Configure
# flags mirror Launcher/NativeBuildFlags.ps1 (Get-MkwNativeFixedConfigureFlags)
# with the FetchContent trees supplied from nix/deps.nix.
{
  lib,
  stdenv,
  cmake,
  ninja,
  pkg-config,
  patchelf,
  wayland-scanner,
  llvmPackages,
  vulkan-loader,
  wayland,
  wayland-protocols,
  libxkbcommon,
  alsa-lib,
  libpulseaudio,
  pipewire,
  libffi,
  libGL,
  mesa,
  libx11,
  libxcb,
  libxext,
  libxrender,
  libxcursor,
  libxi,
  libxrandr,
  libxscrnsaver,
  libxfixes,
  libxtst,
  libxv,
  deps,
  repoSrc,
}: {translation}:
# Clang is the tested toolchain for this project (GCC untested upstream);
# the clang stdenv is what makes CMake pick clang over the default gcc.
llvmPackages.stdenv.mkDerivation {
  name = "wiicompiled-native";
  pname = "wiicompiled";
  version = "0-unstable";

  src = repoSrc;

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
    patchelf
    # SDL's CheckWayland runs find_program(WAYLAND_SCANNER) and silently
    # drops the Wayland video driver when it is missing; nixpkgs ships the
    # scanner as a standalone package, not in wayland's outputs.
    wayland-scanner
  ];
  buildInputs = [
    vulkan-loader
    wayland
    wayland-protocols
    libxkbcommon
    libGL
    mesa
    alsa-lib
    libpulseaudio
    pipewire
    # wayland-client.pc Requires: libffi; without it SDL's pkg-config check
    # fails and the Wayland video driver silently drops out of the build.
    libffi
    libx11
    libxcb
    libxext
    libxrender
    libxcursor
    libxi
    libxrandr
    libxscrnsaver
    libxfixes
    libxtst
    libxv
  ];

  # The clang wrapper's default hardening (pie, fortify, relro) is harmless
  # here, but the fixed guest address space needs no extra hints from us.
  hardeningDisable = ["format"];
  dontFixCmake = true;

  unpackPhase = ''
    runHook preUnpack
    cp -r $src workspace
    chmod -R u+w workspace
    mkdir -p workspace/generated workspace/build
    cp -r ${translation}/generated/. workspace/generated/
    cp -r ${translation}/build/. workspace/build/
    chmod -R u+w workspace/generated workspace/build
    cd workspace
    runHook postUnpack
  '';

  configurePhase = ''
    runHook preConfigure
    # SDL builds backends in dlopen mode by default (SDL_*_SHARED=ON), and
    # this stdenv's fixupPhase shrinks every RPATH entry not needed for
    # DT_NEEDED resolution - which deletes exactly the dlopen'd libraries.
    # Force direct linking so the normal buildInputs rpath survives.
    cmake -S runtime -B native-build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
      -DAURORA_DAWN_PROVIDER=package \
      -DAURORA_SDL3_PROVIDER=vendor \
      -DSDL_X11_SHARED=OFF \
      -DSDL_WAYLAND_SHARED=OFF \
      -DSDL_WAYLAND_LIBDECOR_SHARED=OFF \
      -DSDL_ALSA_SHARED=OFF \
      -DSDL_PIPEWIRE_SHARED=OFF \
      -DSDL_PULSEAUDIO_SHARED=OFF \
      -DCMAKE_POLICY_DEFAULT_CMP0168=NEW \
      -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
      -DAWK:FILEPATH= \
      -DFETCHCONTENT_SOURCE_DIR_ABSEIL-CPP=${deps.abseil-cpp} \
      -DFETCHCONTENT_SOURCE_DIR_XXHASH=${deps.xxhash} \
      -DFETCHCONTENT_SOURCE_DIR_FMT=${deps.fmt} \
      -DFETCHCONTENT_SOURCE_DIR_ZLIB=${deps.zlib} \
      -DFETCHCONTENT_SOURCE_DIR_PNG=${deps.png} \
      -DFETCHCONTENT_SOURCE_DIR_FREETYPE=${deps.freetype} \
      -DFETCHCONTENT_SOURCE_DIR_IMGUI=${deps.imgui} \
      -DFETCHCONTENT_SOURCE_DIR_SQLITE3=${deps.sqlite3} \
      -DFETCHCONTENT_SOURCE_DIR_ZSTD=${deps.zstd} \
      -DFETCHCONTENT_SOURCE_DIR_TRACY=${deps.tracy} \
      -DFETCHCONTENT_SOURCE_DIR_SDL=${deps.sdl} \
      -DFETCHCONTENT_SOURCE_DIR_DAWN_PREBUILT=${deps.dawn-prebuilt} \
      -DMKW_TRANSLATED_COMPILE_JOBS=$((NIX_BUILD_CORES / 2))
    runHook postConfigure
  '';

  buildPhase = ''
    runHook preBuild
    # mkw_release covers both products when the shard graph includes Retro
    # Rewind, and just WiiCompiled for a base-only translation.
    cmake --build native-build --target mkw_release
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p $out/bin $out/lib
    install -m755 native-build/WiiCompiled $out/bin/
    if [ -f native-build/RetroRewind ]; then
      install -m755 native-build/RetroRewind $out/bin/
    fi
    # The vendored libpng/zlib are built shared and the CMake build-tree
    # RPATH points back into /build, which Nix refuses. Install those two
    # libraries beside the product and rewrite the RPATH to the remaining
    # (store-path) entries plus $out/lib. Everything else — SDL3, Dawn,
    # Crypto++ — is statically linked, and the backends SDL dlopens upstream
    # (X11, Wayland, audio) are DT_NEEDED here, so the normal buildInputs
    # rpath covers them; Vulkan is supplied by the launcher wrapper.
    foundAny=0
    for lib in native-build/_deps/*/libpng16.so* native-build/_deps/*/libz.so*; do
      if [ -e "$lib" ]; then
        cp -L "$lib" $out/lib/
        foundAny=1
      fi
    done
    # The vendored libs' own RPATHs also point into /build (their _deps
    # build directory); everything they need lives in $out/lib now.
    for so in $out/lib/*.so*; do
      patchelf --set-rpath "$out/lib" "$so"
    done
    for exe in $out/bin/*; do
      [ -f "$exe" ] || continue
      oldRpath="$(patchelf --print-rpath "$exe")"
      kept="$(echo "$oldRpath" | tr ':' '\n' | grep -v '^/build/' | paste -sd:)"
      if [ -n "$kept" ]; then
        patchelf --set-rpath "$out/lib:$kept" "$exe"
      else
        patchelf --set-rpath "$out/lib" "$exe"
      fi
    done
    # First-run assets the CMake build stages beside each product
    # (runtime/cmake/PublicProducts.cmake); the DSP ROM is a hard startup
    # requirement (runtime/src/hle/audio/ax_mix.cpp). Installed after the
    # RPATH pass above, which must only touch ELF executables.
    for asset in dsp_coef.bin initial_pipeline_cache.db; do
      if [ -f "native-build/$asset" ]; then
        install -m644 "native-build/$asset" "$out/bin/"
      fi
    done
    for d in wii_bootstrap config; do
      if [ -d native-build/$d ]; then
        cp -r native-build/$d $out/bin/
      fi
    done
    # Static Dawn and SDL3 link directly into the products; nothing else to
    # carry beside the per-product assets staged above.
    runHook postInstall
  '';

  meta = {
    description = "WiiCompiled native runtime (Mario Kart Wii static recompilation), base and Retro Rewind products";
    license = lib.licenses.gpl3Only;
    platforms = ["x86_64-linux"];
    mainProgram = "WiiCompiled";
  };
}
