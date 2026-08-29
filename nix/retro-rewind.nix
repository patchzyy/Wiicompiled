# Retro Rewind is opt-in. Add the distribution zip to the Nix store with:
#
#   nix-store --add-fixed sha256 --recursive /path/to/RetroRewind6.zip
#
# then paste the printed hash below. An empty string keeps the flake
# building the base product only.
{
  hash = ""; # <- paste your hash here ("": base product only)

  # Normalizes the Retro Rewind distribution into the layout the translator
  # and the runtime expect:
  #
  #   RetroRewind6/
  #     Binaries/Code.pul          (translator: recomp.yml profile code_pul)
  #     xml/RetroRewind6.xml       (translator: profile riivolution pin)
  #     riivolution/RetroRewind6.xml (runtime: overlay discovery)
  #     files/...                  (runtime: overlay game files, when shipped)
  #
  # The upstream zip spells these differently (RetroRewind6/ +
  # Riivolution/*.xml), so this derivation locates the pieces and re-lays
  # them out. If a future pack moves things around, this is the one place to
  # adjust.
  normalize = {
    lib,
    stdenvNoCC,
    unzip,
  }: {retroRewindPack}:
    stdenvNoCC.mkDerivation {
      name = "retro-rewind-pack";
      nativeBuildInputs = [unzip];

      buildCommand = ''
        mkdir src
        cd src
        case "${retroRewindPack}" in
          *.zip|*.7z)
            unzip -q ${retroRewindPack} -d .
            ;;
          *)
            # A store path already laid out by a custom override.
            cp -r ${retroRewindPack}/. .
            ;;
        esac

        findDir() {
          find . -type d -name "$1" | head -n 1
        }

        rrDir=$(findDir 'RetroRewind6' || true)
        if [ -z "$rrDir" ]; then
          # Some distributions nest one level; take the only top-level directory.
          rrDir=$(find . -mindepth 2 -maxdepth 2 -type d -name 'RetroRewind6' | head -n 1)
        fi
        pul=$(find . -iname 'Code.pul' -type f | head -n 1)
        xml=$(find . -iname 'RetroRewind6.xml' -type f | head -n 1)

        test -n "$pul" || { echo "Retro Rewind pack contains no Code.pul" >&2; exit 1; }
        test -n "$xml" || { echo "Retro Rewind pack contains no Riivolution XML" >&2; exit 1; }

        mkdir -p $out/RetroRewind6/Binaries $out/RetroRewind6/xml $out/RetroRewind6/riivolution
        cp "$pul" $out/RetroRewind6/Binaries/Code.pul
        cp "$xml" $out/RetroRewind6/xml/RetroRewind6.xml
        cp "$xml" $out/RetroRewind6/riivolution/RetroRewind6.xml

        # Carry the mod's game-file overlay (whatever the pack shipped under a
        # files/ directory) so the runtime can serve it as the Riivolution root.
        if [ -d "$rrDir/files" ]; then
          cp -r "$rrDir/files" $out/RetroRewind6/files
        elif [ -d ./files ]; then
          cp -r ./files $out/RetroRewind6/files
        fi
      '';

      meta = {
        description = "Normalized Retro Rewind pack for WiiCompiled";
        license = lib.licenses.unfree;
      };
    };
}
