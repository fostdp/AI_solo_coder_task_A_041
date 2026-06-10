FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git wget \
    libfftw3-dev libevent-dev \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /tmp/spdlog && cd /tmp/spdlog \
    && git clone --depth 1 --branch v1.14.1 https://github.com/gabime/spdlog.git . \
    && mkdir build && cd build \
    && cmake .. -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_BUILD_TYPE=Release \
    && make -j$(nproc) install \
    && rm -rf /tmp/spdlog

RUN mkdir -p /tmp/prometheus-cpp && cd /tmp/prometheus-cpp \
    && git clone --depth 1 --branch v1.2.4 https://github.com/jupp0r/prometheus-cpp.git . \
    && git submodule init && git submodule update \
    && mkdir build && cd build \
    && cmake .. -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_PULL=ON -DENABLE_PUSH=OFF -DENABLE_COMPRESSION=OFF \
    && make -j$(nproc) install \
    && rm -rf /tmp/prometheus-cpp

WORKDIR /src
COPY backend/ /src/backend/
COPY config/ /src/config/

WORKDIR /src/backend
RUN mkdir build && cd build \
    && cmake .. -DCMAKE_BUILD_TYPE=Release \
    && make -j$(nproc) \
    && echo "Build complete"

FROM ubuntu:22.04 AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
    libfftw3-3 libevent-2.1-7a libgcc-s1 libstdc++6 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /usr/local/lib/libspdlog* /usr/local/lib/
COPY --from=builder /usr/local/lib/libprometheus-cpp* /usr/local/lib/
RUN ldconfig

COPY --from=builder /src/backend/build/bin/ /opt/turbine/bin/
COPY --from=builder /src/config/ /opt/turbine/config/

RUN mkdir -p /var/log/turbine_monitor /tmp/shm_turbine

WORKDIR /opt/turbine

ENV LD_LIBRARY_PATH=/usr/local/lib
