# Translation derivation: runs the Translator.Cli over the extracted main.dol /
# StaticR.rel (base) and the Retro Rewind Code.pul (retro-rewind profile), then
# emits the data initializer and the shard manifest the native build consumes.
#
# The command sequence mirrors Launcher/LocalBuild.ps1 exactly:
#   1. stage the RR Code.pul at the profile's mod_root (workspace-relative)
#   2. translate-recursive      -> generated/functions + base metadata
#   3. emit-base-manifest       -> build/base/mkwii_base_manifest.json
#   4. translate-mod            -> build/mods/retro_rewind_full_cpp
#   5. generate-data-init       -> generated/{RuntimeConfig.h,data_sections_init*}
#   6. emit-build-shards        -> generated/build_shards/shards.cmake
#
# shards.cmake embeds absolute paths (Translator.Cli GetFullPath's its inputs),
# so the native build (nix/game.nix) must place the repo and these outputs at
# the same /build/workspace paths this derivation used.
{
  lib,
  stdenvNoCC,
  translator,
  repoSrc,
}: {
  dataTree,
  retroRewindPack,
  retroWfcPayload ? null,
}:
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

    ${lib.optionalString (retroRewindPack != null) ''
      # Stage the profile's Code.pul at the mod_root recomp.yml pins so the
      # base translation becomes retro-aware (leaf inlining blocks at patch
      # addresses) before the mod leg runs.
      mkdir -p PulsarPacks/completed/RetroRewind
      cp -r ${retroRewindPack}/RetroRewind6 PulsarPacks/completed/RetroRewind/RetroRewind6
      chmod -R u+w PulsarPacks

      ${lib.optionalString (retroWfcPayload != null) ''
        WFC_ARGS="--retro-wfc-payload ${retroWfcPayload}"
      ''}
      ${lib.optionalString (retroWfcPayload == null) ''
        WFC_ARGS="--skip-retro-wfc"
      ''}

      Translator.Cli translate-mod --project projects/mkwii/recomp.yml --profile retro-rewind \
        --base-manifest build/base/mkwii_base_manifest.json \
        --base-translation-output-metadata generated/base_translation_output.json \
        --code-pul PulsarPacks/completed/RetroRewind/RetroRewind6/Binaries/Code.pul \
        --mod-root PulsarPacks/completed/RetroRewind/RetroRewind6 \
        --mod-name "Retro Rewind" --region P --out build/mods/retro_rewind_full_cpp \
        --prefer-cached-inputs --emit-cpp --threads "$THREADS" $WFC_ARGS
    ''}

    Translator.Cli generate-data-init --project projects/mkwii/recomp.yml

    SHARD_ARGS=""
    ${lib.optionalString (retroRewindPack != null) ''
      SHARD_ARGS="--resolved-profile build/mods/retro_rewind_full_cpp/resolved_dispatch_profile.json --retro-cpp-dir build/mods/retro_rewind_full_cpp/cpp"
    ''}

    Translator.Cli emit-build-shards --project projects/mkwii/recomp.yml \
      --base-metadata generated/base_translation_output.json \
      --base-functions-dir generated/functions --native-source-dir runtime/src \
      --out generated/build_shards $SHARD_ARGS
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    mkdir $out
    cp -r generated build $out/
    # shards.cmake embeds absolute paths pointing at /build/workspace; the
    # native build (nix/game.nix) recreates that exact layout from repoSrc
    # plus these outputs, so no workspace copy is shipped here.
    runHook postInstall
  '';

  meta = {
    description = "Translated WiiCompiled build graph (base + Retro Rewind)";
    license = lib.licenses.gpl3Only;
  };
}
