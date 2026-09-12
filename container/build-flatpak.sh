#!/usr/bin/env bash
# Runs the --staged-only phase of build-flatpak.sh inside the container image defined
# in this directory, for hosts where nested bwrap cannot run (a container cannot mount
# a new proc namespace, so the flatpak-builder/export step fails there). The staging
# tree this writes under Launcher/artifacts/flatpak-build/ lands in the repo bind mount,
# so the host can finish the bundle with --skip-stage. See docs/building-flatpak.md.
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
workspace=$(cd "$script_dir/.." && pwd)

image=${WIICOMPILED_FLATPAK_IMAGE:-wiicompiled/flatpak-env:22.04}
container_cmd=${CONTAINER_CMD:-}
do_build=0

usage() {
    cat >&2 <<EOF
Usage: container/build-flatpak.sh [--build] [--podman|--docker] [--image TAG] [-- ARGS...]

  --build       Build the staging image first (container/Dockerfile)
  --podman      Use podman (default when available)
  --docker      Use docker
  --image TAG   Override the image tag ($image)
  -- ARGS...    Passed through to build-flatpak.sh --staged-only

Prints the host --skip-stage command to finish the bundle once staging succeeds.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build) do_build=1; shift ;;
        --podman) container_cmd=podman; shift ;;
        --docker) container_cmd=docker; shift ;;
        --image) image=$2; shift 2 ;;
        --) shift; break ;;
        -h|--help) usage; exit 0 ;;
        *) echo "container/build-flatpak.sh: unknown argument: $1" >&2; usage; exit 1 ;;
    esac
done

if [[ -z "$container_cmd" ]]; then
    if command -v podman >/dev/null 2>&1; then
        container_cmd=podman
    elif command -v docker >/dev/null 2>&1; then
        container_cmd=docker
    else
        echo "container/build-flatpak.sh: neither podman nor docker found" >&2
        exit 1
    fi
fi

if [[ "$do_build" == "1" ]]; then
    echo "Building the staging image $image..."
    "$container_cmd" build -t "$image" "$script_dir"
fi

if ! "$container_cmd" image inspect "$image" >/dev/null 2>&1; then
    echo "container/build-flatpak.sh: image $image not found (build it with --build)" >&2
    exit 1
fi

mounts=(-v "$workspace":/wiicompiled -w /wiicompiled)
nuget_cache=${NUGET_PACKAGES:-$HOME/.nuget/packages}
if [[ -d "$nuget_cache" ]]; then
    mounts+=(-v "$nuget_cache":/root/.nuget/packages)
fi

echo "Staging inside $container_cmd (writes Launcher/artifacts/flatpak-build/<arch>/app/):..."
"$container_cmd" run --rm "${mounts[@]}" \
    "$image" bash Launcher/build-flatpak.sh --staged-only "$@"
echo "Staging complete."
cat <<COMMANDS

Finish the bundle on the host, where bwrap/mount can run:

  # NixOS (flatpak-builder is not on the host PATH):
  nix shell nixpkgs#flatpak-builder nixpkgs#flatpak -c \\
      bash Launcher/build-flatpak.sh --skip-stage

  # Ubuntu/Debian host (flatpak + flatpak-builder installed):
  bash Launcher/build-flatpak.sh --skip-stage

The bundle lands at Launcher/dist/WiiCompiled-Setup-<arch>.flatpak.
COMMANDS