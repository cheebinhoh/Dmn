# Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.

FROM ubuntu:24.04

RUN apt-get update && apt-get install -y \
      automake \
      autoconf \
      build-essential \
      cmake \
      clang \
      clang-format \
      clang-tidy \
      curl \
      git \
      gdb \
      openjdk-17-jre-headless \
      libatomic1 \
      dbus-daemon \
      desktop-file-utils \
      appstream \
      libdbus-1-dev \
      libgtk-4-dev \
      libadwaita-1-dev \
      libgtest-dev \
      libfontconfig-dev \
      libfreetype-dev \
      libexpat1-dev \
      libwayland-dev \
      libxkbcommon-dev \
      pkg-config \
      libicu-dev \
      libprotobuf-dev \
      libtool \
      net-tools \
      iputils-ping \
      procps \
      python3 \
      protobuf-compiler \
      tar \
      unzip \
      valgrind \
      vim \
      && rm -rf /var/lib/apt/lists/*

# Copy everything from the current directory (host) into /app (container)
COPY . /app


# Set working directory inside the container
WORKDIR /app

EXPOSE 9092 2181


# Configure and build all optional non-hardware test groups.
RUN cmake -B build \
      -DCMAKE_BUILD_TYPE=Debug \
      -DDMN_BUILD_TESTS=ON \
      -DDMN_ALLOW_DEPENDENCY_FETCH=ON \
      -DBUILD_KAFKA_TEST=ON \
      -DENABLE_APP_BLUEZ_DISCOVERY_CLI=ON \
      -DENABLE_APP_BLUETOOTH_CLI=ON \
      -DENABLE_APP_BLUETOOTH_GTK=ON \
      -DENABLE_VALGRIND=ON \
      -DENABLE_FAULT_INJECTION=ON && \
    cmake --build build/ -j2

# Optional COSMIC frontend. Cargo is the entry point: its build script runs
# CMake (ENABLE_APP_BLUETOOTH_COSMIC=ON) to build the C++ core, then links it.
# Set --build-arg ENABLE_APP_BLUETOOTH_COSMIC=OFF to skip the Rust toolchain.
ARG ENABLE_APP_BLUETOOTH_COSMIC=ON
ENV ENABLE_APP_BLUETOOTH_COSMIC=${ENABLE_APP_BLUETOOTH_COSMIC} \
    CARGO_HOME=/opt/cargo \
    RUSTUP_HOME=/opt/rustup \
    CARGO_TARGET_DIR=/opt/cosmic-target \
    PATH=/opt/cargo/bin:${PATH}
RUN if [ "${ENABLE_APP_BLUETOOTH_COSMIC}" = "ON" ]; then \
      curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | \
        sh -s -- -y --profile minimal --default-toolchain stable && \
      rustup component add rustfmt && \
      cargo fmt --manifest-path src/app/gui/cosmic/Cargo.toml --all --check && \
      cargo test --manifest-path src/app/gui/cosmic/Cargo.toml --workspace --locked --no-run -j2 ; \
    fi


ENV CTEST_LABEL="^(dmn|dmn-cli|dmn-gui|kafka|valgrind|fault-injection)$"

# Entry point
ENTRYPOINT ["/bin/bash", "-c", "set -e; export DMN_KAFKA_LISTENERS=\"${DMN_KAFKA_LISTENERS:-PLAINTEXT://0.0.0.0:9092}\"; export DMN_KAFKA_ADVERTISED_LISTENERS=\"${DMN_KAFKA_ADVERTISED_LISTENERS:-PLAINTEXT://${HOSTNAME}:9092}\"; /app/build/scripts/run-bookstrap-dmn.sh; cd /app/build; ctest -L \"${CTEST_LABEL}\" -VV --output-on-failure; if [ \"${ENABLE_APP_BLUETOOTH_COSMIC}\" = \"ON\" ]; then cargo test --manifest-path /app/src/app/gui/cosmic/Cargo.toml --workspace --locked; fi; exec /bin/bash"]
