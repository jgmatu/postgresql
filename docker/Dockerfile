# One image for both processes; compose decides which one a container runs.
#
#   docker build -f docker/Dockerfile -t pgdemo .

FROM ubuntu:26.04 AS build
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build git ca-certificates patch pkg-config \
      libpq-dev libbotan-3-dev libboost-json-dev libssl-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY cmake cmake
COPY patches patches
COPY libs libs
COPY controller controller
COPY server server
RUN cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPGDEMO_BUILD_TESTS=OFF \
 && cmake --build /build \
 && strip /build/server/pgdemo-server /build/server/oqsprovider.so /build/controller/pgdemo-controller

FROM ubuntu:26.04
ARG DEBIAN_FRONTEND=noninteractive
# Runtime libraries of the -dev packages above (Ubuntu 26.04 names).
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      libpq5 libbotan-3-10 libboost-json1.90.0 \
      postgresql-client openssl ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --home-dir /app --shell /usr/sbin/nologin pgdemo

WORKDIR /app
COPY --from=build /build/server/pgdemo-server build/server/pgdemo-server
COPY --from=build /build/server/oqsprovider.so build/server/oqsprovider.so
COPY --from=build /build/server/openssl-oqs.cnf build/server/openssl-oqs.cnf
COPY --from=build /build/controller/pgdemo-controller build/controller/pgdemo-controller
COPY sql sql
COPY scripts scripts
RUN mkdir -p run && chown pgdemo:pgdemo run
USER pgdemo
