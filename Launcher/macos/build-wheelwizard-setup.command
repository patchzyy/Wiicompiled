#!/usr/bin/env bash
# Build a redistributable, self-contained WheelWizard setup asset containing source and tools only.
set -euo pipefail
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
workspace=$(cd "$script_dir/../.." && pwd)
tools_root=""; output=""; version=""
while (($#)); do
    case "$1" in
        --tools-root) tools_root=$2; shift 2 ;;
        --output) output=$2; shift 2 ;;
        --version) version=${2#v}; shift 2 ;;
        *) echo "Usage: $0 --tools-root DIR --output FILE [--version VERSION]" >&2; exit 1 ;;
    esac
done
[[ -n "$output" && -d "$tools_root" ]] || { echo 'Supply --tools-root and --output' >&2; exit 1; }
for tool in nodtool Translator.Cli ninja cmake/bin/cmake; do
    [[ -x "$tools_root/$tool" ]] || { echo "Missing tool: $tools_root/$tool" >&2; exit 1; }
done
stage=$(mktemp -d "${TMPDIR:-/tmp}/wiicompiled-wheelwizard.XXXXXX")
trap 'rm -rf "$stage"' EXIT
version_arg=(); [[ -n "$version" ]] && version_arg=("-p:Version=$version")
dotnet publish "$workspace/Launcher/WiiCompiled.Setup.MacOS" -c Release -r osx-arm64 --self-contained true \
    -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true "${version_arg[@]}" -o "$stage/payload"
version=$("$stage/payload/WiiCompiled.Setup.MacOS" --version)
mkdir -p "$stage/payload/workspace"
# Tracked source only: never copy ignored game extractions, generated C++, build output or local caches.
git -C "$workspace" ls-files -z aurora-main projects runtime translator Launcher/local-build-macos.command Launcher/macos/extract-disc.command Launcher/macos/publish-app.command LICENSE THIRD-PARTY-NOTICES.md > "$stage/source-files"
(cd "$workspace" && /usr/bin/tar --null -T "$stage/source-files" -cf -) | /usr/bin/tar -xf - -C "$stage/payload/workspace"
DITTONORSRC=1 /usr/bin/ditto --norsrc --noqtn "$tools_root" "$stage/payload/tools"
rm -f "$stage/payload/"*.pdb
mkdir -p "$(dirname "$output")"
# --version must be fast; all other operations unpack to private scratch and leave cleanup to the shell.
cat > "$output" <<EOF_HEADER
#!/bin/bash
set -euo pipefail
if [[ \$# == 1 && \$1 == --version ]]; then printf '%s\\n' '$version'; exit 0; fi
scratch=\$(mktemp -d "\${TMPDIR:-/tmp}/wiicompiled-setup.XXXXXX")
trap 'rm -rf "\$scratch"' EXIT
line=\$(awk '/^__WIICOMPILED_PAYLOAD__\$/{print NR+1; exit}' "\$0")
tail -n +"\$line" "\$0" | /usr/bin/tar -xz -C "\$scratch"
"\$scratch/WiiCompiled.Setup.MacOS" "\$@"
exit \$?
__WIICOMPILED_PAYLOAD__
EOF_HEADER
COPYFILE_DISABLE=1 /usr/bin/tar -czf "$stage/payload.tar.gz" -C "$stage/payload" .
cat "$stage/payload.tar.gz" >> "$output"
chmod +x "$output"
printf 'Created WheelWizard native setup: %s\n' "$output"
