# One image for both processes; compose decides which one a container runs.
#
#   docker build -f docker/Dockerfile -t pgdemo .

FROM ubuntu:26.04 AS build
ARG DEBIAN_FRONTEND=noninteractive
# ROS 2 Lyrical publishes for this Ubuntu (resolute). The controller and the
# devices link rclcpp; the rest of the image does not use it.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build git ca-certificates curl gnupg patch pkg-config \
      libpq-dev libbotan-3-dev libboost-json-dev libssl-dev \
 && curl -fsSL https://raw.githubusercontent.com/ros/rosdistro/master/ros.key \
      | gpg --dearmor -o /usr/share/keyrings/ros-archive-keyring.gpg \
 && . /etc/os-release \
 && echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/ros-archive-keyring.gpg] http://packages.ros.org/ros2/ubuntu ${VERSION_CODENAME} main" \
      > /etc/apt/sources.list.d/ros2.list \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      ros-lyrical-rclcpp ros-lyrical-std-msgs ros-lyrical-rosidl-default-generators \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY cmake cmake
COPY patches patches
COPY libs libs
COPY controller controller
COPY device device
COPY msgs msgs
COPY server server
RUN . /opt/ros/lyrical/setup.sh \
 && cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPGDEMO_BUILD_TESTS=OFF \
 && cmake --build /build \
 && strip /build/server/pgdemo-server /build/server/oqsprovider.so /build/controller/pgdemo-controller \
          /build/device/pgdemo-device

FROM ubuntu:26.04
ARG DEBIAN_FRONTEND=noninteractive
# Runtime libraries of the -dev packages above (Ubuntu 26.04 names).
# The runtime needs the ROS libraries the binaries were linked against, and
# run-all.sh sources /opt/ros/lyrical/setup.bash so the rmw plugin is found.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates curl gnupg \
      libpq5 libbotan-3-10 libboost-json1.90.0 \
      postgresql-client openssl \
 && curl -fsSL https://raw.githubusercontent.com/ros/rosdistro/master/ros.key \
      | gpg --dearmor -o /usr/share/keyrings/ros-archive-keyring.gpg \
 && . /etc/os-release \
 && echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/ros-archive-keyring.gpg] http://packages.ros.org/ros2/ubuntu ${VERSION_CODENAME} main" \
      > /etc/apt/sources.list.d/ros2.list \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      ros-lyrical-rclcpp ros-lyrical-std-msgs ros-lyrical-rosidl-default-generators \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --home-dir /app --shell /usr/sbin/nologin pgdemo

WORKDIR /app
COPY --from=build /build/server/pgdemo-server build/server/pgdemo-server
COPY --from=build /build/server/oqsprovider.so build/server/oqsprovider.so
COPY --from=build /build/server/openssl-oqs.cnf build/server/openssl-oqs.cnf
COPY --from=build /build/controller/pgdemo-controller build/controller/pgdemo-controller
COPY --from=build /build/device/pgdemo-device build/device/pgdemo-device
COPY --from=build /build/msgs/libpgdemo_msgs__*.so /app/lib/
COPY sql sql
COPY scripts scripts
# The control panel export: README.md only in a fresh clone, so run-all.sh
# serves the API alone until scripts/fetch-webroot.sh or pnpm build fills it.
COPY webroot webroot
# run/ca and run/devices are mount points of the ca-keys and device-keys
# volumes; a new volume copies this ownership and mode.
RUN mkdir -p run/ca run/devices \
 && chown -R pgdemo:pgdemo run \
 && chmod 700 run/ca run/devices
USER pgdemo
