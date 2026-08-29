# Launcher wrappers: seed the user's Config.toml on first run (the game owns it
# afterwards, exactly like the Windows installer's install-time write) and exec
# the product binary. dvd_root points at the store's extracted disc tree; the
# Retro Rewind wrapper additionally pins retro_rewind_root at the normalized
# pack. User state (Config.toml, NAND, Cache, Logs) lives in
# $XDG_DATA_HOME/WiiCompiled, mirroring runtime_config.h's
# ApplicationDataDirectory().
#
# LD_LIBRARY_PATH carries vulkan-loader because Dawn loads libvulkan.so.1 via
# dlopen, and RPATH entries for dlopen'd libraries do not survive this
# stdenv's fixupPhase RPATH shrink.
{
  lib,
  makeWrapper,
  runCommand,
  vulkan-loader,
}: {
  game,
  dataTree,
  retroRewindRoot ? null,
}: let
  product =
    if retroRewindRoot == null
    then {
      binary = "WiiCompiled";
      command = "wiicompiled";
      extraPaths = "";
    }
    else {
      binary = "RetroRewind";
      command = "retro-rewind";
      extraPaths = "retro_rewind_root = \"${retroRewindRoot}\"";
    };
in
  runCommand "wiicompiled-launcher-${product.command}"
  {
    nativeBuildInputs = [makeWrapper];
    passthru = {inherit game dataTree;};
  }
  ''
    mkdir -p $out/bin
    makeWrapper ${game}/bin/${product.binary} $out/bin/${product.command} \
      --prefix LD_LIBRARY_PATH : ${vulkan-loader}/lib \
      --run '
        data_dir="''${XDG_DATA_HOME:-$HOME/.local/share}/WiiCompiled"
        mkdir -p "$data_dir"
        if [ ! -f "$data_dir/Config.toml" ]; then
          {
            echo "[paths]"
            echo "dvd_root = \"${dataTree}\""
            echo "${product.extraPaths}"
          } > "$data_dir/Config.toml"
        fi
      '
  ''
