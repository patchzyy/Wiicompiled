# Pinned sources for every FetchContent dependency aurora-main declares. These
# are injected into the configure line as -DFETCHCONTENT_SOURCE_DIR_<NAME> under
# FETCHCONTENT_FULLY_DISCONNECTED=ON, mirroring how the Windows toolkit feeds
# the same trees from Dependencies/ (see Launcher/NativeBuildFlags.ps1). The
# URLs, versions and source subdirectories match
# aurora-main/extern/CMakeLists.txt, cmake/AuroraSDL3Provider.cmake and
# cmake/AuroraDawnProvider.cmake exactly.
{
  lib,
  stdenv,
  fetchurl,
  runCommand,
  unzip,
}: let
  # FetchContent finds the CMakeLists below the top directory of every
  # tarball here; --strip-components=1 reproduces that layout.
  unpackTar = name: src:
    runCommand "aurora-fc-${name}" {} ''
      mkdir -p $out
      tar -xf ${src} -C $out --strip-components=1
    '';
  unpackZip = name: src:
    runCommand "aurora-fc-${name}"
    {
      nativeBuildInputs = [unzip];
    }
    ''
      mkdir -p $out
      unzip -q ${src} -d $out
      # Some amalgamation zips nest everything one level down; FetchContent
      # consumers expect sqlite3.c at the source root either way.
      first=$(find $out -mindepth 1 -maxdepth 1 -type d | head -n 1)
      if [ -n "$first" ] && [ ! -f $out/sqlite3.c ]; then
        find "$first" -mindepth 1 -maxdepth 1 -exec mv -t $out {} +
        rmdir "$first" 2>/dev/null || true
      fi
    '';
in {
  abseil-cpp = unpackTar "abseil-cpp" (fetchurl {
    url = "https://github.com/abseil/abseil-cpp/archive/refs/tags/20240722.0.tar.gz";
    hash = "sha256-9Q5awxGoE4Laf6dblzEOS5AGR0+VYKxG9UqZZ/B9SuM=";
  });

  xxhash = unpackTar "xxhash" (fetchurl {
    url = "https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.3.tar.gz";
    hash = "sha256-quYI3+ghPf0F2QmldxjvgvMHIsOSNEWD0/OQUMfymoA=";
  });

  fmt = unpackTar "fmt" (fetchurl {
    url = "https://github.com/fmtlib/fmt/archive/refs/tags/11.1.4.tar.gz";
    hash = "sha256-rDZre0wunw3eY6WbP+te5Ztnl0sU7l3J6orXiqLB7h4=";
  });

  zlib = unpackTar "zlib" (fetchurl {
    url = "https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz";
    hash = "sha256-uzKaCizQJ00FUZ1hxmfAYuBpkNcuEl7i36jeZPARnRY=";
  });

  png = unpackTar "png" (fetchurl {
    url = "https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.58.tar.gz";
    hash = "sha256-qdTfRj02puX5wpvW9JZzEtF+mWwYVPNRH4M5JOsZk88=";
  });

  freetype = unpackTar "freetype" (fetchurl {
    url = "https://files.twilitrealm.dev/freetype-2.14.3.tar.gz";
    hash = "sha256-5hsxqyY1i5Rudn7X639LsuUH2hz+/reohhrOf9XImaE=";
  });

  imgui = unpackTar "imgui" (fetchurl {
    url = "https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b-docking.tar.gz";
    hash = "sha256-Rm/e+bGN4V8LtuKI49AP+j2CIA7EWM5eT3JKFh2VKKU=";
  });

  sqlite3 = unpackZip "sqlite3" (fetchurl {
    url = "https://sqlite.org/2026/sqlite-amalgamation-3510300.zip";
    hash = "sha256-rLHm9dgySEv20ytoHoWMOK3Ysqzf1CrF3yS4r7RlUrQ=";
  });

  zstd = unpackTar "zstd" (fetchurl {
    url = "https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz";
    hash = "sha256-6zPlH0mhXgI5UM14Jcp0pKK0Pbg1SCWsJPwbfuCeb6M=";
  });

  tracy = unpackTar "tracy" (fetchurl {
    url = "https://github.com/wolfpld/tracy/archive/a64b9a20294d59421a2f57aeca3c6383d8c48169.tar.gz";
    hash = "sha256-JNNCtRJ9f2Wdw8+U8kNHs0jMc28lsXppH9f2lUGTdlg=";
  });

  sdl = unpackTar "sdl" (fetchurl {
    url = "https://github.com/libsdl-org/SDL/releases/download/release-3.4.4/SDL3-3.4.4.tar.gz";
    hash = "sha256-7nEtvmqJuxQLv8LOcjWPte5cwiQKvqvVSFUBLbMLOGQ=";
  });

  # Prebuilt Dawn install tree (DawnConfig.cmake, DAWN_ENABLE_INSTALL=ON),
  # consumed via aurora's AURORA_DAWN_PROVIDER=package.
  dawn-prebuilt = unpackTar "dawn-prebuilt" (fetchurl (
    if stdenv.hostPlatform.system == "x86_64-linux"
    then {
      url = "https://github.com/encounter/dawn-build/releases/download/v20260603.191052/dawn-linux-x86_64.tar.gz";
      hash = "sha256-+eEdQYVI4pFP1bL280VipZIKa52dVS6/K/zHR6XfmuE=";
    }
    else if stdenv.hostPlatform.system == "aarch64-linux"
    then {
      url = "https://github.com/encounter/dawn-build/releases/download/v20260603.191052/dawn-linux-aarch64.tar.gz";
      hash = "sha256-C9pMaUOkSknwQfIw45+WIFve+noTiqud+RiQ0/kV4fA=";
    }
    else
      throw "Unsupported Dawn prebuilt system: ${stdenv.hostPlatform.system}"
  ));
}
