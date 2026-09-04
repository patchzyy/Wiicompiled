{
  description = "Wiicompiled: native Mario Kart Wii recompilation";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = {
    self,
    nixpkgs,
  }: let
    systems = [
      "x86_64-linux"
      "aarch64-linux"
    ];
    forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

    mkOutputs = pkgs: let
      lib = pkgs.lib;
      # The build consumes projects/, runtime/ and aurora-main/ only; keeping
      # the flake's own nix/ files out means flake-only edits do not
      # re-trigger the translation and native build derivations.
      repoSrc = lib.cleanSourceWith {
        src = lib.cleanSource self;
        filter = path: type:
          !(type == "directory" && baseNameOf path == "nix")
          && baseNameOf path != "flake.nix"
          && baseNameOf path != "flake.lock";
      };

      # The disc image and the extracted tree derive from the user's own
      # game dump, which the project may not redistribute, so they eval in
      # an allow-unfree scope. Everything else in the flake stays free.
      unfreePkgs = import pkgs.path {
        system = pkgs.stdenv.hostPlatform.system;
        config.allowUnfree = true;
      };
      discImage = unfreePkgs.requireFile {
        name = "RMCP01.rvz";
        hashMode = "recursive";
        hash = "sha256-x2xbamRFEyVsJH08SeW1mpiLzVPmJOhBX2xSVbpt6LA=";
        message = ''
          WiiCompiled needs your legally dumped clean PAL RMCP01 RVZ named
          RMCP01.rvz. Convert a clean PAL RMCP01 ISO with Dolphin's CLI:

            dolphin-tool convert --input /path/to/MarioKart.iso --output /path/to/RMCP01.rvz --format rvz --block_size 131072 --compression zstd --compression_level 5

          Then add that RVZ to the Nix store with:

            nix-store --add-fixed sha256 --recursive /path/to/RMCP01.rvz
        '';
      };

      # The datatree carries the extracted game data (unfree), so the whole
      # pipeline is built in an allow-unfree scope; the tools it consumes are
      # the same nixpkgs ones either way.
      build = unfreePkgs.callPackage ./nix/build.nix {
        inherit repoSrc;
        translator = pkgs.callPackage ./nix/translator {};
        deps = pkgs.callPackage ./nix/deps.nix {};
        llvmPackages = pkgs.llvmPackages;
      };

      dataTree = build.extractDisc {} {inherit discImage;};

      # The Retro-WFC shared payload the workspace Retro Rewind build bakes
      # in; fetched from the same endpoint the Windows build uses. Wrapped in
      # the binary/ layout local-build.sh's --retro-wfc-offline-dir expects.
      retroWfcPayload = pkgs.fetchurl {
        url = "http://nas.play.rwfc.net/payload?g=RMCPD00";
        hash = "sha256-CZCXoPhal8NIEj2RQ4Q4sksRI2Lx+yPJTfajQ8U2ZHE=";
      };
      retroWfcOffline = pkgs.runCommand "retro-wfc-payload-offline" {} ''
        mkdir -p $out/binary
        cp ${retroWfcPayload} $out/binary/payload.RMCPD00.bin
      '';

      translation = build.translate {inherit dataTree;};

      game = build.buildNative {inherit translation;};

      retroRewindApp = pkgs.callPackage ./nix/retro-rewind-app.nix {
        inherit repoSrc;
        datatree = dataTree;
        translator = pkgs.callPackage ./nix/translator {};
        wfcOffline = retroWfcOffline;
        launcher = build.launcher {inherit game dataTree;};
      };
    in rec {
      nodtool = pkgs.nodtool;
      datatree = dataTree;
      inherit translation game;

      wiicompiled = build.launcher {
        inherit game dataTree;
      };

      retro-rewind = retroRewindApp;

      default = wiicompiled;
    };
  in {
    devShells = forAllSystems (pkgs: {
      default = pkgs.mkShell {
        packages = with pkgs; [
          dotnet-sdk_8
          llvmPackages.clang
          cmake
          ninja
          pkg-config
          sdl3
          vulkan-headers
          vulkan-loader
          libx11
          libxcursor
          libxi
          libxrandr
          wayland
          wayland-protocols
          libxkbcommon
        ];
      };

      # Environment for the impure workspace Retro Rewind build the
      # retro-rewind launcher drives via `nix develop .#retro-rewind-build`.
      # The cc wrapper puts every package's lib dir on the binary's rpath, so
      # the resulting executable runs on NixOS without further fixups; the
      # dlopen-via-rpath details match nix/build.nix's findings.
      retro-rewind-build = let
        buildDeps = with pkgs; [
          vulkan-headers
          vulkan-loader
          wayland
          wayland-protocols
          libxkbcommon
          libffi
          libGL
          mesa
          alsa-lib
          libpulseaudio
          pipewire
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
      in
        pkgs.mkShell {
          packages =
            [
              pkgs.llvmPackages.clang
              pkgs.cmake
              pkgs.ninja
              pkgs.pkg-config
              # SDL's CheckWayland drops the Wayland video driver when the
              # scanner is missing (see nix/build.nix).
              pkgs."wayland-scanner"
            ]
            ++ buildDeps;
          # The cmake setup hook only populates CMAKE_PREFIX_PATH during
          # real builds, not in interactive shells; feed the dependencies to
          # find_package manually (dev output for headers, lib for libs).
          # NIX_LDFLAGS rpath entries likewise: the workspace build has no
          # fixupPhase to repair rpaths, and SDL dlopens X11/Wayland/audio
          # by default, so the linked binary must carry the store lib dirs
          # itself.
          shellHook = ''
            export CMAKE_PREFIX_PATH="${pkgs.lib.concatMapStringsSep ":" (p: "${pkgs.lib.getDev p}:${pkgs.lib.getLib p}") buildDeps}"
            export NIX_LDFLAGS="$NIX_LDFLAGS ${pkgs.lib.concatMapStringsSep " " (p: "-rpath ${pkgs.lib.getLib p}/lib") buildDeps}"
          '';
        };
    });

    packages = forAllSystems mkOutputs;

    # The pinned FetchContent sources are a nested set, so they live outside
    # the packages output (whose values must be derivations). Handy for
    # inspecting a single dep: nix build .#legacyPackages.x86_64-linux.deps.fmt
    legacyPackages = forAllSystems (pkgs: {
      deps = pkgs.callPackage ./nix/deps.nix {};
    });

    apps = forAllSystems (
      pkgs: let
        outputs = mkOutputs pkgs;
      in {
        wiicompiled = {
          type = "app";
          program = "${outputs.wiicompiled}/bin/wiicompiled";
          meta.description = "Mario Kart Wii (WiiCompiled base product)";
        };
        retro-rewind = {
          type = "app";
          program = "${outputs.retro-rewind}/bin/retro-rewind";
          meta.description = "Mario Kart Wii Retro Rewind (WiiCompiled), impure install/update/launch";
        };
      }
    );
  };
}
