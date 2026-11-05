FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
ENV OO_PS4_TOOLCHAIN=/opt/OpenOrbis/PS4Toolchain
ENV PATH="${PATH}:${OO_PS4_TOOLCHAIN}/bin/linux"
ENV DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1

# Install build dependencies
RUN apt-get update && apt-get install -y \
    clang-18 \
    lld-18 \
    llvm-18 \
    cmake \
    make \
    git \
    python3 \
    python3-pyelftools \
    wget \
    curl \
    xz-utils \
    zip \
    unzip \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Set clang-18 as default cc/c++/ar/clang/clang++
# Note: On Ubuntu 24.04, /bin is already a symlink to /usr/bin.
RUN update-alternatives --install /usr/bin/cc cc /usr/bin/clang-18 100 && \
    update-alternatives --install /usr/bin/c++ c++ /usr/bin/clang++-18 100 && \
    ln -sf clang-18 /usr/bin/clang && \
    ln -sf clang++-18 /usr/bin/clang++ && \
    ln -sf llvm-ar-18 /usr/bin/ar

# Install libssl1.1 for OpenOrbis .NET Core tools (PkgTool.Core)
RUN wget -q http://security.ubuntu.com/ubuntu/pool/main/o/openssl/libssl1.1_1.1.1f-1ubuntu2.24_amd64.deb -O /tmp/libssl1.1.deb && \
    dpkg -i /tmp/libssl1.1.deb && \
    rm /tmp/libssl1.1.deb

# Download and extract OpenOrbis Toolchain v0.5.4
WORKDIR /opt
RUN mkdir -p ${OO_PS4_TOOLCHAIN} && \
    wget -q https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases/download/v0.5.4/toolchain-llvm-18.tar.gz -O /tmp/toolchain.tar.gz && \
    tar -xzf /tmp/toolchain.tar.gz -C /opt && \
    rm /tmp/toolchain.tar.gz && \
    if [ ! -d "${OO_PS4_TOOLCHAIN}/bin" ]; then \
        FIND_DIR=$(find /opt -maxdepth 2 -name "bin" -type d | head -n 1 | xargs dirname); \
        if [ -n "$FIND_DIR" ] && [ "$FIND_DIR" != "${OO_PS4_TOOLCHAIN}" ]; then \
            cp -r "$FIND_DIR"/* ${OO_PS4_TOOLCHAIN}/; \
        fi \
    fi && \
    chmod +x ${OO_PS4_TOOLCHAIN}/bin/linux/*

# Build and install OpenGNM (commit 4b295ca)
RUN git clone https://github.com/PS4-OpenGNM/opengnm.git /tmp/opengnm && \
    cd /tmp/opengnm && \
    git checkout 4b295ca && \
    cp config.orbis.mak config.mak && \
    make CC=clang-18 AR=llvm-ar-18 && \
    make install DESTDIR=${OO_PS4_TOOLCHAIN} && \
    cp libopengnm.a ${OO_PS4_TOOLCHAIN}/lib/ && \
    rm -rf /tmp/opengnm

WORKDIR /workspace
