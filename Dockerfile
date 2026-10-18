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
      libdbus-1-dev \
      libgtest-dev \
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


# Configure and build every optional test group.
RUN cmake -B build \
      -DCMAKE_BUILD_TYPE=Debug \
      -DBUILD_NDEBUG=ON \
      -DBUILD_KAFKA_TEST=ON \
      -DENABLE_APP_BLUEZ_DISCOVERY=ON \
      -DENABLE_APP_BLUETOOTH=ON \
      -DENABLE_VALGRIND=ON \
      -DENABLE_FAULT_INJECTION=ON && \
    cmake --build build/ -j2


ENV CTEST_LABEL="dmn|kafka|valgrind|fault-injection"

 # Entry point
ENTRYPOINT ["/bin/bash", "-c", "export DMN_KAFKA_LISTENERS=\"${DMN_KAFKA_LISTENERS:-PLAINTEXT://0.0.0.0:9092}\"; export DMN_KAFKA_ADVERTISED_LISTENERS=\"${DMN_KAFKA_ADVERTISED_LISTENERS:-PLAINTEXT://${HOSTNAME}:9092}\"; /app/build/scripts/run-bookstrap-dmn.sh; cd /app/build; ctest -L \"${CTEST_LABEL}\" -VV --output-on-failure; exec /bin/bash"]
