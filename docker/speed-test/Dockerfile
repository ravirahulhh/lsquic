# LSQUIC Speed Test Docker Image
# Build: docker build -t lsquic-speed-test .
# 
# Usage (with host network for best performance):
#   Server: docker run --network host lsquic-speed-test server
#   Client: docker run --network host lsquic-speed-test client -s <server_ip>:9331 -b 1G

FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    golang \
    libevent-dev \
    libssl-dev \
    zlib1g-dev \
    perl \
    clang \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# Clone and build BoringSSL with Clang (required for __has_feature support)
RUN git clone https://boringssl.googlesource.com/boringssl && \
    cd boringssl && \
    git checkout 9fc1c33e9c21439ce5f87855a6591a9324e569fd && \
    cmake -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_C_COMPILER=clang \
          -DCMAKE_CXX_COMPILER=clang++ \
          -B build . && \
    cmake --build build -j$(nproc) --target ssl crypto

# Copy lsquic source
COPY . /build/lsquic

# Build lsquic with speed test (clean any cached build files first)
RUN cd /build/lsquic && \
    rm -rf build CMakeCache.txt CMakeFiles && \
    cmake -DCMAKE_BUILD_TYPE=Release \
          -DSSLLIB_INCLUDE=/build/boringssl/include \
          -DLIBSSL_LIB=/build/boringssl/build \
          -DLSQUIC_BIN=ON \
          -B build . && \
    cmake --build build -j$(nproc) --target speed_server speed_client

# Generate test certificates
RUN openssl req -x509 -newkey rsa:2048 \
    -keyout /build/key.pem -out /build/cert.pem \
    -days 365 -nodes -subj "/CN=speedtest"

# Runtime image
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    libevent-2.1-7 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy binaries and certificates
COPY --from=builder /build/lsquic/build/bin/speed_server /app/
COPY --from=builder /build/lsquic/build/bin/speed_client /app/
COPY --from=builder /build/key.pem /app/
COPY --from=builder /build/cert.pem /app/

# Copy entrypoint script
COPY docker/speed-test/entrypoint.sh /app/
RUN chmod +x /app/entrypoint.sh

EXPOSE 9331/udp

ENTRYPOINT ["/app/entrypoint.sh"]
CMD ["server"]
