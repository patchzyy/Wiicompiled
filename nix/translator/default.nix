{
  lib,
  buildDotnetModule,
  dotnet-sdk_8,
}:
buildDotnetModule rec {
  pname = "wiicompiled-translator";
  version = "0-unstable";
  src = ../..;
  projectFile = "translator/src/Translator.Cli/Translator.Cli.csproj";
  nugetDeps = ./nuget-deps.nix;
  dotnet-sdk = dotnet-sdk_8;
  executables = ["Translator.Cli"];

  passthru.fetch-deps = buildDotnetModule.fetchDeps {
    inherit pname version src projectFile;
  };

  meta = {
    description = "WiiCompiled static recompiler: DOL/REL to C++ translator";
    license = lib.licenses.gpl3Only;
    mainProgram = "Translator.Cli";
  };
}
