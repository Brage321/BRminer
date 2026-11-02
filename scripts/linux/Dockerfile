FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential clang cmake ninja-build git ca-certificates curl python3 \
    libboost-dev libssl-dev pkg-config rsync python3-pyte qemu-user && rm -rf /var/lib/apt/lists/*
WORKDIR /work
