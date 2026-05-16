#!/usr/bin/env bash
# install-dsp.sh — one-shot installer for ubersdr-dsp + NVIDIA Maxine BNR NIM
#
# What this script does:
#   1. Checks prerequisites (docker, docker compose, NVIDIA GPU + toolkit)
#   2. Asks for your NGC API key and logs in to nvcr.io
#   3. Pulls madpsy/ubersdr-dsp:latest from Docker Hub
#   4. Pulls nvcr.io/nim/nvidia/maxine-bnr:1.0.0 from NVIDIA NGC
#   5. Writes a docker-compose.yml to the install directory
#   6. Starts both services
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/madpsy/ubersdr-dsp/main/install-dsp.sh | bash
#   — or —
#   ./install-dsp.sh [--no-bnr] [--port <grpc-port>] [--dir <install-dir>]
#
# Options:
#   --no-bnr          Skip NVIDIA BNR NIM (no GPU required)
#   --port <port>     gRPC port for ubersdr-dsp (default: 50051)
#   --dir  <dir>      Installation directory (default: ~/ubersdr-dsp)
#   --api-key <key>   NGC API key (skips interactive prompt)

set -euo pipefail

# ── Defaults ──────────────────────────────────────────────────────────────────
DSP_IMAGE="madpsy/ubersdr-dsp:latest"
BNR_IMAGE="nvcr.io/nim/nvidia/maxine-bnr:1.0.0"
GRPC_PORT="${GRPC_PORT:-50051}"
BNR_PORT="8001"
INSTALL_DIR="${HOME}/ubersdr-dsp"
WITH_BNR=true
NGC_API_KEY=""

# ── Colours ───────────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; RESET='\033[0m'

info()    { echo -e "${CYAN}[INFO]${RESET}  $*"; }
ok()      { echo -e "${GREEN}[ OK ]${RESET}  $*"; }
warn()    { echo -e "${YELLOW}[WARN]${RESET}  $*"; }
die()     { echo -e "${RED}[FAIL]${RESET}  $*" >&2; exit 1; }
banner()  { echo -e "\n${BOLD}$*${RESET}"; }

# ── Argument parsing ──────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-bnr)   WITH_BNR=false; shift ;;
        --port)     GRPC_PORT="$2"; shift 2 ;;
        --dir)      INSTALL_DIR="$2"; shift 2 ;;
        --api-key)  NGC_API_KEY="$2"; shift 2 ;;
        -h|--help)
            sed -n '/^# Usage:/,/^[^#]/p' "$0" | grep '^#' | sed 's/^# \?//'
            exit 0 ;;
        *) die "Unknown option: $1" ;;
    esac
done

# ── Banner ────────────────────────────────────────────────────────────────────
echo -e "${BOLD}"
echo "  ╔══════════════════════════════════════════╗"
echo "  ║       ubersdr-dsp installer              ║"
echo "  ║  gRPC noise-reduction server + BNR NIM   ║"
echo "  ╚══════════════════════════════════════════╝"
echo -e "${RESET}"

# ── Prerequisite checks ───────────────────────────────────────────────────────
banner "Checking prerequisites..."

command -v docker >/dev/null 2>&1 || die "Docker is not installed. Install from https://docs.docker.com/get-docker/"
ok "Docker found: $(docker --version)"

# docker compose (v2 plugin or standalone)
if docker compose version >/dev/null 2>&1; then
    COMPOSE="docker compose"
elif command -v docker-compose >/dev/null 2>&1; then
    COMPOSE="docker-compose"
else
    die "docker compose not found. Install Docker Compose v2: https://docs.docker.com/compose/install/"
fi
ok "Docker Compose found: $($COMPOSE version)"

if $WITH_BNR; then
    # Check NVIDIA GPU
    if ! command -v nvidia-smi >/dev/null 2>&1; then
        warn "nvidia-smi not found — NVIDIA GPU may not be available."
        warn "BNR requires an NVIDIA GPU (Turing/Ampere or newer)."
        read -rp "Continue without BNR? [y/N] " ans
        [[ "${ans,,}" == "y" ]] || exit 1
        WITH_BNR=false
    else
        GPU_NAME=$(nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | head -1 || echo "unknown")
        ok "NVIDIA GPU found: $GPU_NAME"
    fi

    if $WITH_BNR; then
        # Check NVIDIA Container Toolkit
        if ! docker run --rm --gpus all ubuntu:22.04 nvidia-smi >/dev/null 2>&1; then
            warn "NVIDIA Container Toolkit may not be installed or configured."
            warn "Install from: https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/install-guide.html"
            read -rp "Continue anyway? [y/N] " ans
            [[ "${ans,,}" == "y" ]] || exit 1
        else
            ok "NVIDIA Container Toolkit working"
        fi
    fi
fi

# ── NGC API key ───────────────────────────────────────────────────────────────
if $WITH_BNR; then
    banner "NVIDIA NGC authentication..."

    if [[ -z "$NGC_API_KEY" ]]; then
        echo ""
        echo "  You need a free NGC API key to pull the NVIDIA Maxine BNR NIM container."
        echo "  Get one at: https://ngc.nvidia.com → top-right menu → Setup → Generate API Key"
        echo ""
        read -rsp "  Enter your NGC API key: " NGC_API_KEY
        echo ""
        [[ -n "$NGC_API_KEY" ]] || die "NGC API key cannot be empty"
    fi

    info "Logging in to nvcr.io..."
    echo "$NGC_API_KEY" | docker login nvcr.io --username '$oauthtoken' --password-stdin \
        || die "Login to nvcr.io failed. Check your API key."
    ok "Logged in to nvcr.io"
fi

# ── Pull images ───────────────────────────────────────────────────────────────
banner "Pulling Docker images..."

info "Pulling $DSP_IMAGE..."
docker pull "$DSP_IMAGE" || die "Failed to pull $DSP_IMAGE"
ok "Pulled $DSP_IMAGE"

if $WITH_BNR; then
    info "Pulling $BNR_IMAGE (this may take a while — ~4 GB)..."
    docker pull "$BNR_IMAGE" || die "Failed to pull $BNR_IMAGE. Check your NGC API key and NGC terms acceptance."
    ok "Pulled $BNR_IMAGE"
fi

# ── Write docker-compose.yml ──────────────────────────────────────────────────
banner "Installing to $INSTALL_DIR..."
mkdir -p "$INSTALL_DIR"

COMPOSE_FILE="$INSTALL_DIR/docker-compose.yml"

if $WITH_BNR; then
    cat > "$COMPOSE_FILE" <<EOF
# ubersdr-dsp + NVIDIA Maxine BNR NIM
# Generated by install-dsp.sh on $(date -u +"%Y-%m-%dT%H:%M:%SZ")
#
# Start:  docker compose up -d
# Stop:   docker compose down
# Logs:   docker compose logs -f

services:

  ubersdr-dsp:
    image: ${DSP_IMAGE}
    container_name: ubersdr-dsp
    ports:
      - "${GRPC_PORT}:${GRPC_PORT}"
    command: ["--grpc-port", "${GRPC_PORT}"]
    restart: unless-stopped
    networks:
      - dsp-net
    depends_on:
      maxine-bnr:
        condition: service_started

  maxine-bnr:
    image: ${BNR_IMAGE}
    container_name: maxine-bnr
    ports:
      - "${BNR_PORT}:${BNR_PORT}"
    networks:
      - dsp-net
    deploy:
      resources:
        reservations:
          devices:
            - driver: nvidia
              count: 1
              capabilities: [gpu]
    restart: unless-stopped

networks:
  dsp-net:
    driver: bridge
EOF
else
    cat > "$COMPOSE_FILE" <<EOF
# ubersdr-dsp (without NVIDIA BNR NIM)
# Generated by install-dsp.sh on $(date -u +"%Y-%m-%dT%H:%M:%SZ")
#
# Start:  docker compose up -d
# Stop:   docker compose down
# Logs:   docker compose logs -f

services:

  ubersdr-dsp:
    image: ${DSP_IMAGE}
    container_name: ubersdr-dsp
    ports:
      - "${GRPC_PORT}:${GRPC_PORT}"
    command: ["--grpc-port", "${GRPC_PORT}"]
    restart: unless-stopped
EOF
fi

ok "Wrote $COMPOSE_FILE"

# ── Start services ────────────────────────────────────────────────────────────
banner "Starting services..."

cd "$INSTALL_DIR"
$COMPOSE up -d || die "Failed to start services"

# Wait a moment and check
sleep 3
$COMPOSE ps

# ── Done ──────────────────────────────────────────────────────────────────────
echo ""
echo -e "${GREEN}${BOLD}Installation complete!${RESET}"
echo ""
echo "  ubersdr-dsp gRPC server: localhost:${GRPC_PORT}"
if $WITH_BNR; then
    echo "  NVIDIA Maxine BNR NIM:   localhost:${BNR_PORT} (internal: maxine-bnr:${BNR_PORT})"
    echo ""
    echo "  To use BNR, send a SessionConfig with:"
    echo "    filter_id: \"bnr\""
    echo "    params { key: \"bnr-address\"  value: \"maxine-bnr:${BNR_PORT}\" }"
    echo "    params { key: \"intensity\"    value: \"0.8\" }"
fi
echo ""
echo "  Manage with:"
echo "    cd $INSTALL_DIR"
echo "    docker compose logs -f       # view logs"
echo "    docker compose down          # stop"
echo "    docker compose up -d         # start"
echo ""
