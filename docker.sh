#!/usr/bin/env bash
# docker.sh — build the ubersdr-dsp Docker image
#
# The gRPC noise-reduction server is built from source inside the Docker image.
# No host binaries are required.
#
# Usage:
#   ./docker.sh [build|push|arm64|multiarch|run]
#
#   build      — build the image for linux/amd64 (default, uses buildx)
#   arm64      — build the image for linux/arm64 only (uses buildx)
#   multiarch  — build & load a multi-arch manifest (amd64 + arm64) locally
#   push       — build multi-arch manifest for amd64+arm64, push to registry,
#                then commit & push the git repository
#   run        — run the image locally (forwards extra args to the server)
#
# Environment variables (build):
#   IMAGE      Docker image name/tag   (default: madpsy/ubersdr-dsp:latest)
#   PLATFORM   Docker --platform flag  (default: linux/amd64)
#   BUILDER    buildx builder name     (default: ubersdr-builder, created if absent)
#
# Environment variables (run):
#   GRPC_PORT  gRPC listen port inside the container (default: 50051)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

IMAGE="${IMAGE:-madpsy/ubersdr-dsp:latest}"
PLATFORM="${PLATFORM:-linux/amd64}"
BUILDER="${BUILDER:-ubersdr-dsp-builder}"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

die() { echo "error: $*" >&2; exit 1; }

check_deps() {
    command -v docker >/dev/null || die "docker not found in PATH"
    docker buildx version >/dev/null 2>&1 || die "docker buildx not available (Docker >= 19.03 required)"
}

# Ensure a buildx builder that supports multi-platform builds exists.
ensure_builder() {
    if ! docker buildx inspect "$BUILDER" >/dev/null 2>&1; then
        echo "Creating buildx builder '$BUILDER'..."
        docker buildx create --name "$BUILDER" --driver docker-container --bootstrap
    else
        docker buildx inspect "$BUILDER" --bootstrap >/dev/null
    fi
}

# Stage the build context into a temp directory, stripping build artefacts.
stage_context() {
    TMPCTX="$(mktemp -d)"
    trap 'rm -rf "$TMPCTX"' EXIT
    echo "Staging build context in $TMPCTX..."
    rsync -a --exclude='/build' \
              --exclude='/build-*' \
              --exclude='.git' \
              --exclude='*.o' \
              --exclude='*.so' \
              --exclude='*.so.*' \
              --include='third_party/deepfilter/lib/**' \
              --exclude='*.a' \
              "$SCRIPT_DIR/" "$TMPCTX/"
}

# ---------------------------------------------------------------------------
# Build targets
# ---------------------------------------------------------------------------

# build [platform] [extra buildx flags...]
#   Builds for a single platform and loads the result into the local daemon.
build() {
    local platform="${1:-$PLATFORM}"
    shift || true
    check_deps
    ensure_builder
    stage_context

    echo "Building image $IMAGE (platform=$platform)..."
    docker buildx build \
        --builder "$BUILDER" \
        --platform "$platform" \
        --tag "$IMAGE" \
        --file "$SCRIPT_DIR/docker/Dockerfile" \
        --load \
        "$@" \
        "$TMPCTX"

    echo "Built and loaded: $IMAGE"
}

# multiarch — build amd64+arm64 and load a combined manifest into the local daemon.
# NOTE: --load with multiple platforms requires containerd image store
# (Docker Desktop or daemon with containerd snapshotter enabled).
# If your daemon does not support it, use 'push' instead.
multiarch() {
    check_deps
    ensure_builder
    stage_context

    echo "Building multi-arch image $IMAGE (linux/amd64,linux/arm64)..."
    docker buildx build \
        --builder "$BUILDER" \
        --platform linux/amd64,linux/arm64 \
        --tag "$IMAGE" \
        --file "$SCRIPT_DIR/docker/Dockerfile" \
        --load \
        "$TMPCTX"

    echo "Built and loaded multi-arch: $IMAGE"
}

# push — build amd64+arm64 and push a multi-arch manifest to the registry,
#        then commit & push the git repository.
push() {
    check_deps
    ensure_builder
    stage_context

    echo "Building and pushing multi-arch image $IMAGE (linux/amd64,linux/arm64)..."
    docker buildx build \
        --builder "$BUILDER" \
        --platform linux/amd64,linux/arm64 \
        --tag "$IMAGE" \
        --file "$SCRIPT_DIR/docker/Dockerfile" \
        --push \
        "$TMPCTX"

    echo "Pushed multi-arch manifest: $IMAGE"

    echo "Committing and pushing git repository..."
    git -C "$SCRIPT_DIR" add -A
    git -C "$SCRIPT_DIR" diff --cached --quiet || git -C "$SCRIPT_DIR" commit -m "Release $IMAGE"
    git -C "$SCRIPT_DIR" push
}

run_image() {
    local port="${GRPC_PORT:-50051}"

    docker run --rm -it \
        --platform "$PLATFORM" \
        -p "${port}:${port}" \
        "$IMAGE" \
        --grpc-port "$port" \
        "$@"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

case "${1:-build}" in
    build)     build "$PLATFORM" ;;
    arm64)     build linux/arm64 ;;
    multiarch) multiarch ;;
    push)      push ;;
    run)       shift; run_image "$@" ;;
    *)
        echo "Usage: $0 [build|arm64|multiarch|push|run [server-args...]]" >&2
        exit 1
        ;;
esac
