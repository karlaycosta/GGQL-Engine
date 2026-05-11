# Dockerfile — GGQL Engine build & test
#
# Reprodutibilidade: imagem Ubuntu 24.04 com gcc-14 (C++23) + dependências
# de validação (gfatools, vg, hyperfine, gcovr) para rodar localmente ou em CI.
#
# Build:
#   docker build -t ggql-engine .
#
# Run tests:
#   docker run --rm ggql-engine ctest --test-dir build -j4
#
# Run validation suite:
#   docker run --rm -v $(pwd)/data:/work/data ggql-engine \
#     bash tests/validation/r1_determinism.sh tier_b
#
# Tamanho final: ~2 GB com tools de validação; ~600 MB sem (--target build).

FROM ubuntu:24.04 AS build

# Layer 1: pacotes do sistema (cacheável)
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        g++-14 \
        gcc-14 \
        libfmt-dev \
        libjemalloc-dev \
        libomp-dev \
        zlib1g-dev \
        git \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Layer 2: source + build (rebuilda quando código muda)
WORKDIR /work
COPY CMakeLists.txt ./
COPY src/ src/
COPY include/ include/
COPY tests/ tests/
COPY third_party/ third_party/

ENV CXX=g++-14 CC=gcc-14
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j$(nproc) \
    && ctest --test-dir build -j$(nproc) --output-on-failure

# ── Stage 2: imagem para validação (inclui ferramentas externas) ─────────────
FROM build AS validation

# Pacotes adicionais para validation suite
RUN apt-get update && apt-get install -y --no-install-recommends \
        awk \
        coreutils \
        python3 \
        python3-pip \
        hyperfine \
        gcovr \
        wget \
    && rm -rf /var/lib/apt/lists/*

# vg v1.74.0 (binário oficial Linux)
RUN wget -q https://github.com/vgteam/vg/releases/download/v1.74.0/vg \
        -O /usr/local/bin/vg \
    && chmod +x /usr/local/bin/vg

# gfatools (lh3) — build minimal
RUN git clone --depth=1 https://github.com/lh3/gfatools.git /tmp/gfatools \
    && make -C /tmp/gfatools \
    && cp /tmp/gfatools/gfatools /usr/local/bin/ \
    && rm -rf /tmp/gfatools

# Verifica que tools estão disponíveis
RUN bench_gbwt=/work/build/bench_gbwt run_queries=/work/build/tests/run_queries \
    && [ -x "$bench_gbwt" ] && [ -x "$run_queries" ] \
    && vg version | head -1 \
    && gfatools version 2>/dev/null || true \
    && hyperfine --version

# Comando default: ctest
CMD ["ctest", "--test-dir", "build", "-j4", "--output-on-failure"]
