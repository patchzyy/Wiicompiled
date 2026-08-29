{
  description = "WiiCompiled: native Mario Kart Wii port (Linux, Nix-first)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = {
    self,
    nixpkgs,
  }: let
    systems = [
      "x86_64-linux"
    ];
    forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    lib' = nixpkgs.lib;

    # Retro Rewind is opt-in: pin the pack's recursive sha256 in nix/rr-hash.nix
    # (see the instructions in that file) to enable the Retro Rewind product.
    rrHash = import ./nix/rr-hash.nix;

    mkOutputs = pkgs: let
      lib = pkgs.lib;
      repoSrc = lib.cleanSource self;

      # The disc image and the extracted tree derive from the user's own
      # game dump, which the project may not redistribute, so they eval in
      # an allow-unfree scope. Everything else in the flake stays free.
      unfreePkgs = import pkgs.path {
        inherit (pkgs) system;
        config.allowUnfree = true;
      };
      discImage = unfreePkgs.requireFile {
        name = "RMCP01.rvz";
        hashMode = "recursive";
        hash = "sha256-cP/ZT1wpezNbHYZFgtvCAFnWQwKkupe7/46uFPlFbJU=";
        message = ''
          WiiCompiled needs your legally dumped clean PAL RMCP01 image
          (RVZ/ISO/GCM/GCZ/CISO/WBFS/WIA). Nothing is downloaded; add your
          own dump to the Nix store with:

            nix-store --add-fixed sha256 --recursive /path/to/RMCP01.rvz
        '';
      };
      dataTree = (unfreePkgs.callPackage ./nix/extract.nix {
        nodtool = pkgs.nodtool;
      }) {inherit discImage;};

      retroRewindPack =
        if rrHash.hash == ""
        then null
        else
          (pkgs.callPackage ./nix/rr-pack.nix {}) {
            retroRewindPack = pkgs.requireFile {
              name = "RetroRewind6.zip";
              hashMode = "recursive";
              inherit (rrHash) hash;
              message = ''
                The Retro Rewind product needs the Retro Rewind distribution
                zip. Add it to the Nix store with:

                  nix-store --add-fixed sha256 --recursive /path/to/RetroRewind6.zip

                then pin the printed hash in nix/rr-hash.nix.
              '';
            };
          };

      # The Retro-WFC shared payload the mod translation bakes in; fetched
      # from the same endpoint the Windows build uses.
      retroWfcPayload = pkgs.fetchurl {
        url = "http://nas.play.rwfc.net/payload?g=RMCPD00";
        hash = "sha256-/Y8m1q8m8aDPrs0eRy/nRKJddfUTaRBTO3x14+yi8dI=";
      };

      translator = pkgs.callPackage ./nix/translator.nix {};

      translation =
        (pkgs.callPackage ./nix/translate.nix {
          inherit repoSrc translator;
        }) {
          inherit dataTree retroRewindPack retroWfcPayload;
        };

      deps = pkgs.callPackage ./nix/deps.nix {};
      game = (pkgs.callPackage ./nix/game.nix {
        inherit repoSrc deps;
        llvmPackages = pkgs.llvmPackages;
      }) {inherit translation;};

      wrapper = pkgs.callPackage ./nix/wrapper.nix {};
    in rec {
      nodtool = pkgs.nodtool;
      datatree = dataTree;
      inherit translation game;

      wiicompiled = wrapper {
        inherit game dataTree;
      };

      retro-rewind = lib.mapNullable (pack:
        wrapper {
          inherit game dataTree;
          retroRewindRoot = "${pack}/RetroRewind6";
        })
      retroRewindPack;

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
    });

    packages = forAllSystems (
      pkgs:
        lib'.filterAttrs (_: value: value != null) (mkOutputs pkgs)
    );

    # The pinned FetchContent sources are a nested set, so they live outside
    # the packages output (whose values must be derivations). Handy for
    # inspecting a single dep: nix build .#legacyPackages.x86_64-linux.deps.fmt
    legacyPackages = forAllSystems (pkgs: {
      inherit (mkOutputs pkgs) deps;
    });

    apps = forAllSystems (
      pkgs: let
        outputs = mkOutputs pkgs;
      in
        {
          wiicompiled = {
            type = "app";
            program = "${outputs.wiicompiled}/bin/wiicompiled";
            meta.description = "Mario Kart Wii (WiiCompiled base product)";
          };
        }
        // (pkgs.lib.optionalAttrs (outputs.retro-rewind != null) {
          retro-rewind = {
            type = "app";
            program = "${outputs.retro-rewind}/bin/retro-rewind";
            meta.description = "Mario Kart Wii Retro Rewind (WiiCompiled)";
          };
        })
    );
  };
}
