# Phase 9 "Docker" deliverable (docs/rtmp_promot.md). Two-stage build: a
# builder image with the full toolchain (cmake, ninja, clang, liburing-dev,
# libssl-dev, libsqlite3-dev), and a slim runtime image that only carries the
# built binary plus its shared-library dependencies. Linux-only, same as the
# io_uring transport itself (see CMakeLists.txt's RTMP_SERVER_CORE_ONLY
# guard) — this Dockerfile is not meant to build the core-only subset, it
# builds the real Linux transport target (apps/rtmp_server).
#
# NOT BUILT OR RUN IN THIS REPOSITORY'S DEVELOPMENT ENVIRONMENT: there is no
# Docker daemon available on the macOS host this phase was implemented on
# (see docs/phase9-checklist.md "Known limitations" — "Docker deployment
# works" is consequently unverified here). The Dockerfile follows the
# project's own documented build commands (docs/*-checklist.md "Build
# commands") exactly, so it should build correctly on a Linux host with
# Docker installed, but that has not been exercised as part of this phase.

FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    clang \
    liburing-dev \
    libssl-dev \
    libsqlite3-dev \
    git \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

ENV CC=clang CXX=clang++

WORKDIR /build
COPY . .

RUN cmake --preset debug -DCMAKE_BUILD_TYPE=Release -DRTMP_SERVER_BUILD_TESTS=OFF \
    && cmake --build build/debug -j"$(nproc)"

FROM ubuntu:24.04 AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
    liburing2 \
    libssl3 \
    libsqlite3-0 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --no-create-home --shell /usr/sbin/nologin rtmp_server

COPY --from=builder /build/build/debug/apps/rtmp_server/rtmp_server /usr/local/bin/rtmp_server
COPY config/server.example.yaml /etc/rtmp_server/server.yaml

RUN mkdir -p /var/lib/rtmp_server/recordings /var/lib/rtmp_server/data \
    && chown -R rtmp_server:rtmp_server /var/lib/rtmp_server /etc/rtmp_server

USER rtmp_server
WORKDIR /var/lib/rtmp_server

EXPOSE 1935 8080

# Startup validation (ServerConfig::validate(), see core/config.cpp) fails
# fast on a missing/default token_signing_secret/api_authentication_secret —
# the example config's placeholders MUST be overridden (env vars or a
# mounted config) before this container will actually start serving.
ENTRYPOINT ["/usr/local/bin/rtmp_server", "--config", "/etc/rtmp_server/server.yaml"]
