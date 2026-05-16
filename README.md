# ubersdr-dsp

Standalone noise reduction gRPC server extracted from [AetherSDR](https://github.com/AetherSDR/AetherSDR).

Accepts raw **PCM audio** over a bidirectional gRPC stream, applies the selected
noise reduction algorithm, and returns processed PCM on the same stream.

- **Sample rates:** `12000` or `24000` Hz (client declares; server resamples transparently)
- **Channels:** `1` (mono) or `2` (stereo, default)
- **Sample format:** IEEE 754 float32, little-endian, interleaved
- Zero Qt dependency — only the C/C++ standard library, FFTW3, and gRPC/protobuf are required at runtime.

---

## Filters

| ID | Algorithm | Always built | Notes |
|----|-----------|:---:|-------|
| `nr2` | SpectralNR — MMSE-LSA + OSMS spectral subtraction | ✅ | FFTW3 recommended; built-in radix-2 fallback |
| `rn2` | RNNoise — Mozilla/Xiph RNN-based suppressor | ✅ | Compiled from source; best on speech |
| `nr4` | libspecbleach — SPP-MMSE adaptive denoiser | ✅ (default) | Requires `libfftw3f` |
| `dfnr` | DeepFilterNet3 — neural network denoiser | opt-in | Requires pre-built Rust C-ABI library + ONNX model |
| `bnr` | NVIDIA Maxine BNR — cloud/NIM gRPC denoiser | opt-in | Requires gRPC + NVIDIA Maxine NIM server |

---

## Audio pipeline

Every `ProcessAudio` stream goes through the following stages on each chunk:

```
Client PCM (client_rate, client_channels, float32)
        │
        ▼
[Mono → Stereo expansion]          (only when channels=1)
  Each mono sample duplicated to L+R
        │
        ▼
[Upsample to 24 kHz]               (only when sample_rate=12000)
  r8brain stereo resampler: client_rate → 24000 Hz
        │
        ▼
[Noise reduction filter]           (nr2 / rn2 / nr4 / dfnr / bnr)
  All filters operate on 24 kHz interleaved stereo float32
        │
        ▼
[Downsample to client rate]        (only when sample_rate=12000)
  r8brain stereo resampler: 24000 Hz → client_rate
        │
        ▼
[Stereo → Mono collapse]           (only when channels=1)
  Output mono[i] = (L[i] + R[i]) × 0.5
        │
        ▼
Response PCM (client_rate, client_channels, float32)
```

**DFNR** has an additional internal resampling stage inside `DeepFilterFilter`:

```
24 kHz stereo float32 (from pipeline above)
        │
        ▼
[Stereo → Mono mix + upsample to 48 kHz]   r8brain
        │
        ▼
[DeepFilterNet3 model]   df_process_frame() — fixed frame size
        │
        ▼
[Downsample to 24 kHz + Mono → Stereo]     r8brain
        │
        ▼
24 kHz stereo float32 (back to pipeline)
```

---

## Dependencies

### Runtime (dynamic)

| Library | Purpose | Install |
|---------|---------|---------|
| `libgrpc++.so` | gRPC server runtime | `apt install libgrpc++-dev` |
| `libprotobuf.so` | Protobuf serialisation | `apt install libprotobuf-dev` |
| `libfftw3.so.3` | NR2 FFT (double-precision) | `apt install libfftw3-dev` |
| `libfftw3f.so.3` | NR4 FFT (single-precision) | included in `libfftw3-dev` |
| `libstdc++`, `libm`, `libgcc_s`, `libc` | Standard C/C++ runtime | always present |

No Qt. No ONNX runtime.

### Build

- CMake ≥ 3.20
- C++20 compiler (GCC ≥ 11 or Clang ≥ 14)
- `libgrpc++-dev`, `libprotobuf-dev`, `protobuf-compiler-grpc`
- `libfftw3-dev` (Debian/Ubuntu) or `fftw-devel` (Fedora/RHEL)

### Bundled (compiled from source, no install needed)

- **r8brain** — header-only resampler (used by the session pipeline for 12↔24 kHz, and by DFNR internally for 24↔48 kHz)
- **rnnoise** — Mozilla/Xiph RNN noise suppressor
- **libspecbleach** — spectral bleaching denoiser

---

## Building

```bash
# Default build (NR2 + RN2 + NR4, FFTW3 enabled)
make

# With optional filters
make WITH_DFNR=ON
make WITH_BNR=ON

# Custom install prefix
make install PREFIX=/usr

# Full rebuild
make rebuild
```

The binary is placed at `build/ubersdr-dsp`.

### CMake directly

```bash
cmake -S . -B build \
    -DWITH_FFTW3=ON \
    -DWITH_SPECBLEACH=ON \
    -DWITH_DFNR=OFF \
    -DWITH_BNR=OFF
cmake --build build -j$(nproc)
```

### Build options

| Option | Default | Description |
|--------|---------|-------------|
| `WITH_FFTW3` | `ON` | Use FFTW3 for NR2 (falls back to built-in radix-2 if off/not found) |
| `WITH_SPECBLEACH` | `ON` | Build NR4 (libspecbleach) |
| `WITH_DFNR` | `OFF` | Build DFNR (DeepFilterNet3 — requires pre-built C-ABI library) |
| `WITH_BNR` | `OFF` | Build BNR (NVIDIA Maxine via gRPC) |
| `AETHER_THIRD_PARTY` | `./third_party` | Path to third-party library directory |

---

## Docker

A pre-built multi-arch image is available:

```bash
docker pull madpsy/ubersdr-dsp:latest
docker run --rm -p 50051:50051 madpsy/ubersdr-dsp:latest
```

Or use the provided compose file:

```bash
docker compose -f docker/docker-compose.yml up
```

### Building the image locally

```bash
./docker.sh build        # linux/amd64 (default)
./docker.sh arm64        # linux/arm64
./docker.sh multiarch    # amd64 + arm64 combined manifest
./docker.sh push         # build, push to registry, git commit+push
./docker.sh run          # run the locally built image
```

### What the image contains

The Docker build compiles all filters (`WITH_DFNR=ON`, `WITH_BNR=ON`,
`WITH_SPECBLEACH=ON`, `WITH_FFTW3=ON`) and produces a minimal runtime image
containing only:

- `/usr/local/bin/ubersdr-dsp` — the compiled binary
- `/usr/local/lib/` — exact shared library dependencies (collected via `ldd`)
- `/usr/local/share/ubersdr-dsp/DeepFilterNet3_onnx.tar.gz` — DFNR model

The DFNR model is placed at `/usr/local/share/ubersdr-dsp/` which is one of the
automatic search paths checked by `DeepFilterFilter::findModelPath()` at session
initialisation time. No extra flags are needed to use DFNR in the container.

### DFNR in Docker — build notes

The pre-built `libdeepfilter.a` static library (at
`third_party/deepfilter/lib/linux-x86_64/libdeepfilter.a`) must reach the
Docker build context. The `.dockerignore` and `docker.sh` are both configured
to allow this file through while still excluding all other `.a` files and build
artefacts.

---

## Usage

```
ubersdr-dsp [--grpc-port <port>]

  --grpc-port <port>   gRPC listen port (default: 50051)
```

Filter selection and all parameters are configured per-stream via the
`SessionConfig` gRPC message — there are no command-line filter flags.
See the **gRPC Client Protocol** section below.

---

## gRPC Client Protocol

The server exposes a single gRPC service defined in [`proto/ubersdr_dsp.proto`](proto/ubersdr_dsp.proto).
It listens on `0.0.0.0:50051` by default (override with `--grpc-port`).

```
service DspService {
  rpc ProcessAudio(stream AudioRequest) returns (stream AudioResponse);
  rpc GetFilters(GetFiltersRequest)     returns (GetFiltersResponse);
}
```

---

### Discovering available filters — `GetFilters`

Before opening a processing stream, call the unary `GetFilters` RPC to discover
which filters are compiled in and what parameters each one accepts.

**Request:** empty `GetFiltersRequest`

**Response:** `GetFiltersResponse` containing a list of `FilterInfo` entries, one
per filter. Each entry has:

| Field | Description |
|-------|-------------|
| `name` | Filter identifier to use in `SessionConfig.filter` |
| `description` | Human-readable description (includes `[NOT COMPILED IN]` if unavailable) |
| `params[]` | List of `ParamInfo` descriptors |

Each `ParamInfo` has `name`, `type` (`float`/`int`/`bool`/`string`),
`default_val`, `min_val`, `max_val`, `description`, and `runtime_safe`
(whether the param can be changed mid-stream via `ParamUpdate`).

---

### Processing audio — `ProcessAudio`

`ProcessAudio` is a **bidirectional streaming** RPC. The client sends
`AudioRequest` messages and receives `AudioResponse` messages concurrently.
Every response carries the server-assigned `session_id` string.

#### Message flow

```
Client                                    Server
  │                                          │
  │── AudioRequest { config: SessionConfig } ──▶│  ← must be first
  │◀── AudioResponse { ack: ParamAck{} }    ──│  ← empty ack = configured OK
  │                                          │
  │── AudioRequest { audio: AudioChunk }    ──▶│
  │◀── AudioResponse { audio: AudioChunk }  ──│  ← processed PCM
  │                                          │
  │── AudioRequest { param_update: ... }    ──▶│
  │◀── AudioResponse { ack: ParamAck }      ──│  ← applied + rejected maps
  │                                          │
  │  (close send side)                        │
  │◀── gRPC Status OK                       ──│
```

#### Step 1 — Send `SessionConfig` (required, first message only)

```protobuf
message SessionConfig {
  string filter = 1;              // "nr2" | "rn2" | "nr4" | "dfnr" | "bnr"
  int32  block  = 2;              // frames per chunk at client rate (default: 960 = 40 ms at 24 kHz)
  map<string,string> params = 3;  // initial parameter values (optional)
  int32  sample_rate = 4;         // client audio sample rate in Hz (default: 24000)
  int32  channels    = 5;         // 1 = mono, 2 = stereo (default: 2)
}
```

- `filter` is required. If the name is unknown or the filter failed to
  initialise, the server sends an `ErrorResponse` and closes the stream with
  gRPC status `INTERNAL`.
- `block` sets the expected number of frames per chunk at the **client sample
  rate**. Chunks that are not a multiple of `block × channels × 4` bytes are
  rejected with `INVALID_AUDIO`.
- `sample_rate` declares the sample rate of the PCM the client will send.
  Supported values: `12000`, `24000` (default: `24000`). The server
  transparently resamples to/from its internal 24 kHz processing rate using
  r8brain. Response `AudioChunk`s are returned at the same rate. An
  unsupported value is rejected with `INVALID_SAMPLE_RATE` and gRPC status
  `INVALID_ARGUMENT`.
- `channels` declares whether the client sends mono (`1`) or stereo (`2`,
  default). Mono input is expanded to stereo internally (each sample duplicated
  to L and R) before the filter, and collapsed back to mono in the response
  (L+R averaged). All filters process stereo internally. An unsupported value
  is rejected with `INVALID_CHANNELS` and gRPC status `INVALID_ARGUMENT`.
- `params` keys use the same names as shown in the **Filter parameters
  reference** section below. Values are always strings.
- Sending a second `SessionConfig` mid-stream is rejected with
  `FILTER_CHANGE_NOT_ALLOWED`; the stream stays open.

On success the server responds with an empty `ParamAck` (`applied` and
`rejected` both empty).

#### Step 2 — Stream `AudioChunk` messages

```protobuf
message AudioChunk {
  bytes  pcm_data     = 1;  // float32 LE PCM at client sample_rate and channels
  uint64 sequence_num = 2;  // monotonic counter (echoed back)
  uint64 timestamp_us = 3;  // optional capture timestamp
}
```

- `pcm_data` must be exactly `block × channels × 4` bytes
  (`block` frames × channel count × 4 bytes per float32), where `block` and
  `channels` are as declared in `SessionConfig`.
- The server processes the chunk and responds with an `AudioChunk` of the same
  size and channel layout (at the client sample rate), with `sequence_num`
  copied from the request.
- Sending audio before `SessionConfig` is rejected with `NOT_CONFIGURED`.

#### Step 3 — Update parameters at runtime (optional)

```protobuf
message ParamUpdate {
  map<string,string> params = 1;  // only keys you want to change
}
```

The server responds with a `ParamAck`:

```protobuf
message ParamAck {
  map<string,string> applied  = 1;  // key → value actually set
  map<string,string> rejected = 2;  // key → reason string
}
```

All parameters marked `runtime_safe = true` in `GetFilters` can be changed
mid-stream. Unknown keys and out-of-range values appear in `rejected` with a
human-readable reason; the stream continues regardless.

---

### Error responses

The server sends `ErrorResponse` messages in-band on the same stream:

```protobuf
message ErrorResponse {
  string code    = 1;
  string message = 2;
}
```

| `code` | Trigger | Stream stays open? |
|--------|---------|:-----------------:|
| `FILTER_INIT_FAILED` | Unknown filter name, or filter failed to initialise (not compiled in, model missing, BNR server unreachable) | No — server also closes with gRPC `INTERNAL` |
| `INVALID_SAMPLE_RATE` | `sample_rate` is not `12000` or `24000` | No — server also closes with gRPC `INVALID_ARGUMENT` |
| `INVALID_CHANNELS` | `channels` is not `1` or `2` | No — server also closes with gRPC `INVALID_ARGUMENT` |
| `FILTER_CHANGE_NOT_ALLOWED` | Second `SessionConfig` sent after stream is configured | Yes |
| `NOT_CONFIGURED` | `AudioChunk` or `ParamUpdate` sent before `SessionConfig` | Yes |
| `INVALID_AUDIO` | `pcm_data` length is not a multiple of `channels × 4` bytes | Yes |

---

### Filter parameters reference

All parameter values are sent as strings. `runtime_safe = true` means the
parameter can be changed via `ParamUpdate` after the stream is configured.

#### `nr2` — SpectralNR (always available)

| Parameter | Type | Default | Range | Runtime safe | Description |
|-----------|------|---------|-------|:---:|-------------|
| `gain-method` | int | `2` | 0–3 | ✅ | Gain method: 0=Linear 1=Log 2=Gamma 3=Trained |
| `npe-method` | int | `0` | 0–2 | ✅ | NPE method: 0=OSMS 1=MMSE 2=NSTAT |
| `gain-max` | float | `1.0` | 0.0–2.0 | ✅ | Max gain cap |
| `gain-smooth` | float | `0.85` | 0.0–1.0 | ✅ | Temporal gain smoothing |
| `qspp` | float | `0.2` | 0.0–1.0 | ✅ | Speech presence probability prior |
| `ae` | bool | `true` | — | ✅ | Artifact elimination post-processing |

#### `rn2` — RNNoise (always available)

No tunable parameters.

#### `nr4` — libspecbleach (requires `WITH_SPECBLEACH=ON`)

| Parameter | Type | Default | Range | Runtime safe | Description |
|-----------|------|---------|-------|:---:|-------------|
| `reduction` | float | `10` | 0–40 | ✅ | Noise reduction amount in dB |
| `smoothing` | float | `0` | 0–100 | ✅ | Smoothing factor in % |
| `whitening` | float | `0` | 0–100 | ✅ | Whitening factor in % |
| `adaptive` | bool | `true` | — | ✅ | Enable adaptive noise tracking |
| `noise-method` | int | `0` | 0–2 | ✅ | Noise estimator: 0=SPP-MMSE 1=Brandt 2=Martin |

#### `dfnr` — DeepFilterNet3 (requires `WITH_DFNR=ON`)

| Parameter | Type | Default | Range | Runtime safe | Description |
|-----------|------|---------|-------|:---:|-------------|
| `model` | string | _(auto)_ | — | ❌ | Path to `DeepFilterNet3_onnx.tar.gz` — set in `SessionConfig.params` only |
| `atten-limit` | float | `100` | 0–100 | ✅ | Attenuation limit in dB |
| `pf-beta` | float | `0` | 0–0.3 | ✅ | Post-filter beta |

**Model file search order** (when `model` param is not set):

1. Path supplied via the `model` session param
2. Current working directory (`./DeepFilterNet3_onnx.tar.gz`)
3. Directory containing the `ubersdr-dsp` executable
4. `/usr/local/share/ubersdr-dsp/DeepFilterNet3_onnx.tar.gz`
5. `/usr/share/ubersdr-dsp/DeepFilterNet3_onnx.tar.gz`
6. `/usr/local/share/AetherSDR/DeepFilterNet3_onnx.tar.gz`
7. `/usr/share/AetherSDR/DeepFilterNet3_onnx.tar.gz`

The Docker image places the model at path 4 automatically — no extra
configuration is needed when using the container.

The `model` path cannot be changed at runtime; close and reopen the stream
(with a new `SessionConfig`) to use a different model file.

#### `bnr` — NVIDIA Maxine BNR (requires `WITH_BNR=ON`)

| Parameter | Type | Default | Range | Runtime safe | Description |
|-----------|------|---------|-------|:---:|-------------|
| `bnr-address` | string | `localhost:8001` | — | ❌ | NIM gRPC server address — set in `SessionConfig.params` only |
| `intensity` | float | `1.0` | 0.0–1.0 | ✅ | Noise suppression intensity ratio |

Bool parameters accept: `true`, `false`, `1`, `0`, `on`, `off`.

---

### Minimal Python client example

```python
import grpc
import ubersdr_dsp_pb2 as pb
import ubersdr_dsp_pb2_grpc as rpc
import struct, itertools

channel = grpc.insecure_channel("localhost:50051")
stub = rpc.DspServiceStub(channel)

# 1. Discover filters
resp = stub.GetFilters(pb.GetFiltersRequest())
for f in resp.filters:
    print(f.name, f.description)

# 2. Open a processing stream (24 kHz stereo, 40 ms blocks)
def requests():
    yield pb.AudioRequest(config=pb.SessionConfig(
        filter="nr2",
        block=960,
        sample_rate=24000,
        channels=2,
        params={"gain-method": "2", "gain-smooth": "0.9"},
    ))
    seq = itertools.count()
    with open("input.raw", "rb") as f:
        while chunk := f.read(960 * 2 * 4):   # 960 frames × 2 ch × 4 bytes
            yield pb.AudioRequest(audio=pb.AudioChunk(
                pcm_data=chunk,
                sequence_num=next(seq),
            ))

with open("output.raw", "wb") as out:
    for response in stub.ProcessAudio(requests()):
        if response.HasField("audio"):
            out.write(response.audio.pcm_data)
        elif response.HasField("ack"):
            print("ack — applied:", dict(response.ack.applied),
                  "rejected:", dict(response.ack.rejected))
        elif response.HasField("error"):
            print("ERROR", response.error.code, response.error.message)
```

**Mono example** (12 kHz mono, 20 ms blocks, DFNR filter):

```python
yield pb.AudioRequest(config=pb.SessionConfig(
    filter="dfnr",
    block=240,          # 240 frames × 1 ch × 4 bytes = 960 bytes per chunk
    sample_rate=12000,
    channels=1,
    params={"atten-limit": "80"},
))
# Each AudioChunk.pcm_data = 240 × 1 × 4 = 960 bytes
# Server expands mono→stereo, upsamples 12→24 kHz, runs DFNR,
# downsamples 24→12 kHz, collapses stereo→mono, returns 960 bytes.
```

Generate the Python stubs from the proto file:

```bash
python -m grpc_tools.protoc \
    -I proto \
    --python_out=. \
    --grpc_python_out=. \
    proto/ubersdr_dsp.proto
```

---

## Examples

### `examples/mic_to_speaker.py` — real-time mic → noise reduction → speaker

Captures audio from the system microphone, streams it through the ubersdr-dsp
gRPC server for noise reduction, and plays the processed audio on the speaker
output. Uses [sounddevice](https://python-sounddevice.readthedocs.io/) (PortAudio)
for audio I/O.

#### Setup (Ubuntu)

```bash
# 1. Create a virtual environment
python3 -m venv examples/.venv

# 2. Install dependencies
examples/.venv/bin/pip install -r examples/requirements.txt

# 3. Generate gRPC stubs (run once from the repo root)
examples/.venv/bin/python -m grpc_tools.protoc \
    -I proto \
    --python_out=examples \
    --grpc_python_out=examples \
    proto/ubersdr_dsp.proto
```

#### Usage

```bash
# List available audio devices
examples/.venv/bin/python examples/mic_to_speaker.py --list

# Default: NR2 filter, system default mic and speaker
examples/.venv/bin/python examples/mic_to_speaker.py

# DeepFilterNet3 neural denoiser
examples/.venv/bin/python examples/mic_to_speaker.py --filter dfnr

# Specific devices by name substring (from --list output)
examples/.venv/bin/python examples/mic_to_speaker.py \
    --filter nr4 \
    --input "ALC256" \
    --output "pulse"

# 12 kHz mono, custom server, pass filter params
examples/.venv/bin/python examples/mic_to_speaker.py \
    --filter nr2 \
    --rate 12000 \
    --server localhost:50051 \
    --param gain-method=2 \
    --param gain-smooth=0.9
```

#### Options

| Option | Default | Description |
|--------|---------|-------------|
| `--filter` | `nr2` | Filter: `nr2` `rn2` `nr4` `dfnr` `bnr` |
| `--server` | `localhost:50051` | ubersdr-dsp gRPC address |
| `--rate` | `24000` | Sample rate: `12000` or `24000` Hz |
| `--block` | `960` | Frames per chunk (960 = 40 ms at 24 kHz) |
| `--latency` | `low` | sounddevice latency: `low`, `high`, or seconds |
| `--input` | system default | Input device index or name substring |
| `--output` | system default | Output device index or name substring |
| `--list` | — | List audio devices and exit |
| `--param KEY=VALUE` | — | Set a filter parameter (repeatable) |

Audio is always processed as **mono** (mic input is mono; the server expands
to stereo internally, processes, then collapses back to mono for playback).

---

## FFTW3 Wisdom

NR2 uses FFTW3 for FFT computation. On first run without wisdom, FFTW uses
`FFTW_MEASURE` which is fast for the default FFT size (256). For best
performance across all sizes, generate wisdom once:

```bash
# Generate wisdom (takes several minutes — runs FFTW_PATIENT for sizes 64–262144)
# This is done via the AetherSDR GUI or by calling SpectralNR::generateWisdom()
# from a small helper program.

# Use existing wisdom at runtime — pass the path in SessionConfig params:
# params: { "wisdom": "/path/to/wisdom/dir" }
# (NR2 reads this at filter initialisation time)
```

The wisdom file is named `aethersdr_fftw_wisdom` inside the specified directory.
WDSP/Thetis wisdom files (same FFTW3 format) are also compatible on Windows.

---

## Project structure

```
ubersdr-dsp/
├── proto/
│   └── ubersdr_dsp.proto       # gRPC service + message definitions
├── src/
│   ├── server.cpp              # main() — gRPC server startup, signal handling
│   ├── DspServiceImpl.h/.cpp   # ProcessAudio + GetFilters RPC handlers
│   │                           #   (resampling, mono expand/collapse, filter dispatch)
│   ├── FilterFactory.h/.cpp    # createFilter() — maps filter name → IFilter
│   └── filters/
│       ├── IFilter.h           # Abstract filter interface
│       ├── Nr2FilterWrapper.h/.cpp   # NR2 — SpectralNR adapter
│       ├── Rn2FilterWrapper.h/.cpp   # RN2 — RNNoise adapter
│       ├── Nr4FilterWrapper.h/.cpp   # NR4 — libspecbleach adapter
│       ├── DfnrFilterWrapper.h/.cpp  # DFNR — DeepFilterNet3 adapter
│       └── BnrFilterWrapper.h/.cpp   # BNR — NVIDIA Maxine adapter
├── SpectralNR.h/.cpp           # NR2 — MMSE-LSA spectral NR (from AetherSDR)
├── RNNoiseFilter.h/.cpp        # RN2 — RNNoise wrapper (from AetherSDR)
├── SpecbleachFilter.h/.cpp     # NR4 — libspecbleach wrapper (from AetherSDR)
├── DeepFilterFilter.h/.cpp     # DFNR — DeepFilterNet3 wrapper (from AetherSDR)
│                               #   (internal 24↔48 kHz resampling via r8brain)
├── NvidiaBnrFilter.h/.cpp      # BNR — NVIDIA Maxine gRPC wrapper (from AetherSDR)
├── Resampler.h/.cpp            # r8brain wrapper (session-level 12↔24 kHz resampling)
├── CMakeLists.txt              # Build system
├── Makefile                    # Convenience wrapper around CMake
├── docker.sh                   # Docker build/push/run helper
├── docker/
│   ├── Dockerfile              # Multi-stage build (builder + minimal runtime)
│   ├── docker-compose.yml      # Compose file (ubersdr-dsp + optional NVIDIA BNR NIM)
│   └── .dockerignore           # Excludes build artefacts; allows libdeepfilter.a
└── third_party/
    ├── r8brain/                # Header-only resampler
    ├── rnnoise/                # Mozilla/Xiph RNNoise
    ├── libspecbleach/          # Spectral bleaching denoiser
    └── deepfilter/             # DeepFilterNet3 C API header, pre-built lib, model
        ├── include/deep_filter.h
        ├── lib/linux-x86_64/libdeepfilter.a
        └── models/DeepFilterNet3_onnx.tar.gz
```

---

## BNR — NVIDIA Maxine Background Noise Removal

BNR is **not compiled in by default** (`WITH_BNR=OFF`). Requesting `filter: "bnr"`
in a `SessionConfig` without rebuilding returns a `FILTER_INIT_FAILED` error.

BNR has no local model file. It is a gRPC client to NVIDIA's Maxine NIM
microservice, which runs the GPU-accelerated model inside a Docker container.

### Requirements

- NVIDIA GPU (Turing / Ampere or newer recommended)
- Docker with NVIDIA Container Toolkit
- Free NGC account: https://ngc.nvidia.com
- gRPC + protobuf development libraries

### Step 1 — Install gRPC

```bash
apt install libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc
```

### Step 2 — Get the NVIDIA Maxine `.proto` file

The NIM container page is at:
https://catalog.ngc.nvidia.com/orgs/nim/teams/nvidia/containers/maxine-bnr?version=1.0.0

The `.proto` file is included in the container image and also available in the
Maxine SDK at https://developer.nvidia.com/maxine-getting-started (NGC account
required). It is at `maxine-sdk/protos/bnr.proto` and defines the
`nvidia.maxine.bnr.v1` package with the `MaxineBNR` service and
`EnhanceAudioRequest` / `EnhanceAudioResponse` messages.

### Step 3 — Generate C++ stubs

```bash
mkdir -p bnr_stubs
protoc --grpc_out=./bnr_stubs \
       --cpp_out=./bnr_stubs \
       --plugin=protoc-gen-grpc=$(which grpc_cpp_plugin) \
       bnr.proto
# Produces: bnr.pb.h  bnr.pb.cc  bnr.grpc.pb.h  bnr.grpc.pb.cc
```

### Step 4 — Rebuild with BNR enabled

```bash
make WITH_BNR=ON BNR_PROTO_DIR=$(pwd)/bnr_stubs rebuild
```

Or with CMake directly:

```bash
cmake -S . -B build \
    -DWITH_BNR=ON \
    -DBNR_PROTO_DIR=$(pwd)/bnr_stubs
cmake --build build -j$(nproc)
```

### Step 5 — Get an NGC API key

1. Sign up for a free account at https://ngc.nvidia.com
2. Accept the NVIDIA AI Enterprise / NIM terms when prompted
3. Top-right menu → **Setup** → **Generate API Key** — copy it (shown once)

### Step 6 — Pull and run the NIM container

```bash
# Username is literally "$oauthtoken" (not a shell variable)
docker login nvcr.io
# Username: $oauthtoken
# Password: <your NGC API key>

docker pull nvcr.io/nim/nvidia/maxine-bnr:1.0.0

docker run --gpus all --rm -p 8001:8001 \
    nvcr.io/nim/nvidia/maxine-bnr:1.0.0
```

The container exposes a gRPC endpoint on port 8001 and is ready when you see
`Server listening on 0.0.0.0:8001`.

### Step 7 — Use BNR

Connect a gRPC client and send a `SessionConfig` with `filter: "bnr"` and
optionally `params: { "bnr-address": "localhost:8001", "intensity": "0.8" }`.

The `bnr-address` param defaults to `localhost:8001` and can only be set at
session start. The `intensity` param (0.0–1.0) can be changed at runtime via
`ParamUpdate`.

### How it works

BNR streams **480-sample (10 ms) frames of 48 kHz mono float32** to the NIM
server via a bidirectional gRPC stream and receives denoised audio back.
The session-level pipeline handles the 24 kHz stereo ↔ 48 kHz mono conversion
transparently — you feed and receive the same format as all other filters.

---

## Licence

The filter implementations are derived from AetherSDR and their respective
upstream projects:

- **SpectralNR** — GPL-2.0-or-later (derived from WDSP/emnr.c by Warren Pratt NR0V)
- **RNNoise** — BSD-3-Clause (Mozilla/Xiph)
- **libspecbleach** — GPL-2.0-or-later
- **DeepFilterNet3** — MIT
- **r8brain** — MIT
- **NVIDIA Maxine BNR** — proprietary (NVIDIA SDK)
