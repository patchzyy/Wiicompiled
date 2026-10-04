# The impure Retro Rewind launcher: install/update/build/launch orchestration
# running in the user's workspace (see nix/retro-rewind.sh). Everything the
# script needs at runtime is either on its wrapped PATH or baked in as a store
# path; the workspace build itself happens inside the retro-rewind-build
# devShell (flake.nix) so the compiled binary carries store rpaths and runs
# on NixOS unmodified.
{
  lib,
  runCommand,
  makeWrapper,
  replaceVars,
  bash,
  coreutils,
  curl,
  unzip,
  findutils,
  gnugrep,
  gnused,
  gawk,
  nix,
  vulkan-loader,
  repoSrc,
  datatree,
  translator,
  drirc,
}: let
  script = replaceVars ./retro-rewind.sh {
    inherit
      bash
      datatree
      translator
      drirc
      repoSrc
      vulkan-loader
      ;
  };
in
  runCommand "wiicompiled-retro-rewind" {
    nativeBuildInputs = [makeWrapper];
    meta = {
      description = "Retro Rewind install/update/build/launch orchestration for WiiCompiled";
      mainProgram = "retro-rewind";
      platforms = ["x86_64-linux" "aarch64-linux"];
    };
  } ''
    install -Dm755 ${script} $out/bin/retro-rewind
    wrapProgram $out/bin/retro-rewind \
      --prefix PATH : ${lib.makeBinPath [
      bash
      coreutils
      curl
      unzip
      findutils
      gnugrep
      gnused
      gawk
      nix
    ]}
  ''
