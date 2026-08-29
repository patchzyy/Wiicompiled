# Disc extraction derivation: consumes the user-supplied RMCP01 image (via
# pkgs.requireFile or an override) and produces the extracted filesystem tree
# the runtime serves as dvd_root and the translator feeds on. Validation mirrors
# the Windows installer (Launcher/WiiCompiled.Setup/InstallerEngine.cs): the
# required files must exist and main.dol / StaticR.rel must hash to the pins
# recorded in projects/mkwii/recomp.yml.
{
  lib,
  stdenvNoCC,
  nodtool,
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
}
