# ── Stage 1: builder ──────────────────────────────────────────────────────────
# Full build environment with all compile-time dependencies.
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    # Build tools
    cmake \
    ninja-build \
    g++ \
    gcc \
    pkg-config \
    # gRPC + protobuf (compile-time)
    libgrpc++-dev \
    libprotobuf-dev \
    protobuf-compiler \
    protobuf-compiler-grpc \
    # FFTW3 (NR2 + NR4)
    libfftw3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Copy the full source tree
COPY . .

# Build the gRPC server — all filters enabled
RUN cmake -S . -B build -GNinja \
        -DCMAKE_BUILD_TYPE=Release \
        -DWITH_GRPC_SERVER=ON \
        -DWITH_FFTW3=ON \
        -DWITH_SPECBLEACH=ON \
        -DWITH_DFNR=ON \
        -DWITH_BNR=ON \
        -DBNR_PROTO_DIR=/src/third_party/bnr \
    && cmake --build build -j"$(nproc)"

# Collect all shared library dependencies of the binary into /deps,
# resolving symlinks so the real .so files are copied (not just the symlinks).
RUN mkdir -p /deps \
    && ldd /src/build/ubersdr-dsp \
       | awk '/=>/ { print $3 }' \
       | grep -v '^$' \
       | while read lib; do \
           real=$(readlink -f "$lib"); \
           cp "$real" /deps/; \
           # Also copy the versioned symlink name so the dynamic linker finds it \
           base=$(basename "$lib"); \
           [ "$base" != "$(basename $real)" ] && ln -sf "$(basename $real)" /deps/"$base" || true; \
         done

# ── Stage 2: runtime ──────────────────────────────────────────────────────────
# Minimal image — only the binary and its exact shared library dependencies.
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

# Copy the resolved shared libraries from the builder
COPY --from=builder /deps/ /usr/local/lib/

# Update the dynamic linker cache so the libraries are found
RUN ldconfig

# Copy only the compiled binary
COPY --from=builder /src/build/ubersdr-dsp /usr/local/bin/ubersdr-dsp

# Copy the DeepFilterNet3 ONNX model to a standard search path.
# DeepFilterFilter::findModelPath() checks /usr/local/share/ubersdr-dsp/ at runtime.
RUN mkdir -p /usr/local/share/ubersdr-dsp
COPY --from=builder /src/third_party/deepfilter/models/DeepFilterNet3_onnx.tar.gz \
     /usr/local/share/ubersdr-dsp/DeepFilterNet3_onnx.tar.gz

# gRPC port
EXPOSE 50051

# Default: listen on all interfaces, port 50051.
# Override with: docker run ... ubersdr-dsp --grpc-port 9090
ENTRYPOINT ["ubersdr-dsp"]
CMD ["--grpc-port", "50051"]
