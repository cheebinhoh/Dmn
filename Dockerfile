FROM ubuntu:24.04

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    clang \
    clang-format \
    clang-tidy \
    libgtest-dev \
    libgmock-dev \
    curl \
    unzip \
    autoconf \
    automake \
    libtool \
    protobuf-compiler \
    libprotobuf-dev \
    && rm -rf /var/lib/apt/lists/*

# Copy everything from the current directory (host) into /app (container)
COPY . /app

# Set working directory inside the container
WORKDIR /app

# Run build
RUN mkdir build && cmake -B build && cmake --build build/

# Entry point
ENTRYPOINT ["/bin/bash", "-c", "echo \"enter entry point\"; exec /bin/bash"]
