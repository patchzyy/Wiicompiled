# WiiCompiled base-game pipeline, top to bottom. flake.nix supplies the
# user-owned disc dump and wires the stages together:
#
#   extractDisc   user-supplied RMCP01 image -> validated nodtool extraction
#                 (the extracted tree is both the translator's input and the
#                 runtime's dvd_root)
#   translate     Translator.Cli over main.dol/StaticR.rel -> emitted C++
#                 shard build graph
#   buildNative   cmake/ninja/clang over runtime/ + the shard graph -> the
#                 WiiCompiled executable
#   launcher      wrapper binaries: user state seeding, RADV workaround
#
# Retro Rewind is deliberately not a store product: the pack updates too
# often to pin, so nix/retro-rewind-app.nix builds it in the user's
# workspace (upstream parity) with the tools and sources from this flake.
#
# Supporting files: nix/deps.nix (pinned FetchContent tarballs feeding
# buildNative) and nix/translator (dotnet build of Translator.Cli feeding
# translate).
{
  lib,
  stdenvNoCC,
  nodtool,
  translator,
  repoSrc,
  deps,
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
  makeWrapper,
  runCommand,
}: {
  # Disc extraction: consumes the user-supplied RMCP01 image (via
  # pkgs.requireFile or an override) and produces the extracted filesystem
  # tree. Validation mirrors the Windows installer: the required files must
  # exist and main.dol / StaticR.rel must hash to the pins recorded in
  # projects/mkwii/recomp.yml.
  extractDisc = {
    dolSha256 ? "80d18895b39c63bd80f457398bfcbb91b7d16ac116a41a88967e954080155b05",
    relSha256 ? "16d9d146112541fefea701ecb5bc1a496f9d50e4a752fbb5b6778e7c6399f67d",
  }: {discImage}:
    stdenvNoCC.mkDerivation {
      name = "wiicompiled-datatree";
      nativeBuildInputs = [nodtool];

      buildCommand = ''
        mkdir -p $out
        nodtool extract ${discImage} $out
        test -d $out/files
        test -f $out/sys/fst.bin
        test -f $out/sys/main.dol
        test -f $out/files/rel/StaticR.rel
        test "$(sha256sum $out/sys/main.dol | cut -d' ' -f1)" = "${dolSha256}"
        test "$(sha256sum $out/files/rel/StaticR.rel | cut -d' ' -f1)" = "${relSha256}"
      '';

      meta = {
        description = "Extracted RMCP01 disc filesystem for WiiCompiled";
        license = lib.licenses.unfree;
      };
    };

  # Translation: runs the Translator.Cli over the extracted main.dol /
  # StaticR.rel, then emits the data initializer and the shard manifest the
  # native build consumes.
  #
  # The command sequence mirrors Launcher/local-build.sh's base profile:
  #   1. translate-recursive      -> generated/functions + base metadata
  #   2. emit-base-manifest       -> build/base/mkwii_base_manifest.json
  #   3. generate-data-init       -> generated/{RuntimeConfig.h,data_sections_init*}
  #   4. emit-build-shards        -> generated/build_shards/shards.cmake
  #
  # Retro Rewind is not built here: it is translated and compiled in the
  # user's workspace by the impure retro-rewind launcher (upstream parity),
  # because the pack updates far too often for store pinning.
  #
  # shards.cmake embeds absolute paths (Translator.Cli GetFullPath's its
  # inputs), so the native build (buildNative) must place the repo and these
  # outputs at the same /build/workspace paths this derivation used.
  translate = {dataTree}:
    stdenvNoCC.mkDerivation {
      name = "wiicompiled-translation";

      nativeBuildInputs = [translator];

      unpackPhase = ''
        runHook preUnpack
        cp -r ${repoSrc} workspace
        chmod -R u+w workspace
        cd workspace
        runHook postUnpack
      '';

      buildPhase = ''
        runHook preBuild
        mkdir -p Assets generated build/base
        cp ${dataTree}/sys/main.dol Assets/main.dol
        cp ${dataTree}/files/rel/StaticR.rel Assets/StaticR.rel

        THREADS=$([ "$NIX_BUILD_CORES" -gt 16 ] && echo 16 || echo "$NIX_BUILD_CORES")

        Translator.Cli translate-recursive 0x800060A4 --project projects/mkwii/recomp.yml \
          --outdir generated/functions --output-metadata generated/base_translation_output.json \
          --production-source-bundle generated/base_translation_sources.bin \
          --no-function-files --prune-stale --threads "$THREADS"

        Translator.Cli emit-base-manifest --project projects/mkwii/recomp.yml \
          --out build/base --functions-dir generated/functions \
          --translation-output-metadata generated/base_translation_output.json --region P

        Translator.Cli generate-data-init --project projects/mkwii/recomp.yml

        Translator.Cli emit-build-shards --project projects/mkwii/recomp.yml \
          --base-metadata generated/base_translation_output.json \
          --base-functions-dir generated/functions --native-source-dir runtime/src \
          --out generated/build_shards
        runHook postBuild
      '';

      installPhase = ''
        runHook preInstall
        mkdir $out
        cp -r generated build $out/
        # shards.cmake embeds absolute paths pointing at /build/workspace; the
        # native build (buildNative) recreates that exact layout from repoSrc
        # plus these outputs, so no workspace copy is shipped here.
        runHook postInstall
      '';

      meta = {
        description = "Translated WiiCompiled build graph (base + Retro Rewind)";
        license = lib.licenses.gpl3Only;
      };
    };

  # Native build: configures runtime/ CMake against the translated shard
  # graph and builds the base product.
  #
  # shards.cmake embeds absolute paths pointing at the translation workspace,
  # so this derivation recreates that exact layout at /build/workspace: the
  # repo sources plus the translation outputs in their original positions.
  # Configure flags mirror Launcher/NativeBuildFlags.ps1 with the FetchContent
  # trees supplied from nix/deps.nix.
  buildNative = {translation}:
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
        # wayland-client.pc Requires: libffi; without it SDL's pkg-config
        # check fails and the Wayland video driver silently drops out.
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

      # The clang wrapper's default hardening (pie, fortify, relro) is
      # harmless here, but the fixed guest address space needs no extra
      # hints from us.
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
        # SDL builds backends in dlopen mode by default (SDL_*_SHARED=ON),
        # and this stdenv's fixupPhase shrinks every RPATH entry not needed
        # for DT_NEEDED resolution - which deletes exactly the dlopen'd
        # libraries. Force direct linking so the normal buildInputs rpath
        # survives.
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
        # Base-only shard graph: mkw_release builds just WiiCompiled.
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
        # Crypto++ — is statically linked, and the backends SDL dlopens
        # upstream (X11, Wayland, audio) are DT_NEEDED here, so the normal
        # buildInputs rpath covers them; Vulkan is supplied by the launcher.
        for lib in native-build/_deps/*/libpng16.so* native-build/_deps/*/libz.so*; do
          if [ -e "$lib" ]; then
            cp -L "$lib" $out/lib/
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
        # Static Dawn and SDL3 link directly into the products; nothing else
        # to carry beside the per-product assets staged above.
        runHook postInstall
      '';

      meta = {
        description = "WiiCompiled native runtime (Mario Kart Wii static recompilation), base product";
        license = lib.licenses.gpl3Only;
        platforms = ["x86_64-linux"];
        mainProgram = "WiiCompiled";
      };
    };

  # Launcher wrapper: seed the user's Config.toml on first run and keep
  # [paths] dvd_root pointing at this build's extracted disc tree (the game
  # owns every other setting afterwards). dvd_root is deterministic per
  # flake, but the runtime's own default Config.toml template ships the line
  # commented out, so an existing file is healed in place rather than
  # clobbered. User state (Config.toml, NAND, Cache, Logs) lives in
  # $XDG_DATA_HOME/WiiCompiled, mirroring runtime_config.h's
  # ApplicationDataDirectory().
  #
  # The wrapper also scopes a Mesa drirc file to this game capping RADV at
  # Vulkan 1.3: Dawn emits SPIR-V 1.4 (with the Vulkan memory model) when the
  # driver reports API 1.4 and then validates it under 1.0 semantics, so
  # pipeline creation dies with "Produced invalid SPIRV" on RDNA4-era RADV.
  # Other drivers ignore the radv device match. Remove once upstream Dawn
  # validates against a matching environment. The impure retro-rewind
  # launcher reuses the same drirc file for the workspace-built binary.
  launcher = {
    game,
    dataTree,
  }:
    runCommand "wiicompiled-launcher-wiicompiled" {
      nativeBuildInputs = [makeWrapper];
      passthru = {inherit game dataTree;};
    } ''
      mkdir -p $out/bin $out/etc
      cat > $out/etc/drirc <<'DRIRC'
      <driconf>
        <device driver="radv">
          <application name="WiiCompiled" executable="WiiCompiled">
            <option name="radv_override_api_version" value="1.3"/>
          </application>
          <application name="RetroRewind" executable="RetroRewind">
            <option name="radv_override_api_version" value="1.3"/>
          </application>
        </device>
      </driconf>
      DRIRC
      makeWrapper ${game}/bin/WiiCompiled $out/bin/wiicompiled \
        --prefix LD_LIBRARY_PATH : ${vulkan-loader}/lib \
        --run '
          data_dir="''${XDG_DATA_HOME:-$HOME/.local/share}/WiiCompiled"
          mkdir -p "$data_dir"

          # Scope the RADV Vulkan-1.3 cap to this game via Mesa drirc; the
          # XDG_CONFIG_HOME redirection applies to the game process only.
          export XDG_CONFIG_HOME="$data_dir/xdg-config"
          mkdir -p "$XDG_CONFIG_HOME"
          if [ ! -f "$XDG_CONFIG_HOME/drirc" ]; then
            cp ${placeholder "out"}/etc/drirc "$XDG_CONFIG_HOME/drirc"
          fi

          if [ -f "$data_dir/Config.toml" ]; then
            if grep -q "^[[:space:]]*dvd_root" "$data_dir/Config.toml"; then
              sed -i "s|^[[:space:]]*dvd_root = .*|dvd_root = \"${dataTree}\"|" "$data_dir/Config.toml"
            elif grep -q "^\[paths\]" "$data_dir/Config.toml"; then
              sed -i "/^\[paths\]/a dvd_root = \"${dataTree}\"" "$data_dir/Config.toml"
            else
              printf "\n[paths]\ndvd_root = \"%s\"\n" "${dataTree}" >> "$data_dir/Config.toml"
            fi
          else
            printf "[paths]\ndvd_root = \"%s\"\n" "${dataTree}" > "$data_dir/Config.toml"
          fi
        '
    '';
}
