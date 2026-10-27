# Build environment for the Schwung 6W6 module.
# ubuntu:22.04 pinned deliberately: glibc 2.35, matching Ableton Move exactly.
# Building on the VPS host itself (Ubuntu 26.04 / glibc 2.43) produces a .so
# the Move cannot load.
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc-aarch64-linux-gnu g++-aarch64-linux-gnu build-essential \
        cmake ninja-build make file git ca-certificates pkg-config python3 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build
ENV CROSS_PREFIX=aarch64-linux-gnu-
