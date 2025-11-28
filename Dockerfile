# Copyright © 2025 Chee Bin HOH. All rights reserved.

FROM ubuntu:24.04

RUN apt-get update && apt-get install -y \
      build-essential \
      cmake \
      git \
      clang \
      clang-format \
      clang-tidy \
      libgtest-dev \ 
      net-tools \
      curl \
      unzip \
      autoconf \
      automake \
      libtool \
      protobuf-compiler \
      libprotobuf-dev \
      vim \
      && rm -rf /var/lib/apt/lists/*

# Copy everything from the current directory (host) into /app (container)
COPY . /app

# Set working directory inside the container
WORKDIR /app

# Run build
RUN mkdir build && cmake -B build && cmake --build build/ && cd build && ctest -L dmn

# Entry point
ENTRYPOINT ["/bin/bash", "-c", "cd /app/build; exec /bin/bash"]
