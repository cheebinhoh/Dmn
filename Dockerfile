# Copyright © 2025 Chee Bin HOH. All rights reserved.

FROM ubuntu:24.04

# Avoid prompts during installation
ENV DEBIAN_FRONTEND=noninteractive
ENV KAFKA_VERSION=3.6.1
ENV SCALA_VERSION=2.13

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
      openjdk-17-jre-headless \
      libgtest-dev \
      libgtest-dev \
      libprotobuf-dev \
      libtool \
      net-tools \
      iputils-ping \
      protobuf-compiler \
      tar \
      unzip \
      vim \
      wget \
      && rm -rf /var/lib/apt/lists/*


RUN wget https://archive.apache.org/dist/kafka/${KAFKA_VERSION}/kafka_${SCALA_VERSION}-${KAFKA_VERSION}.tgz && \
    tar -xzf kafka_${SCALA_VERSION}-${KAFKA_VERSION}.tgz -C /opt && \
    mv /opt/kafka_${SCALA_VERSION}-${KAFKA_VERSION} /opt/kafka && \
    rm kafka_${SCALA_VERSION}-${KAFKA_VERSION}.tgz

# Copy everything from the current directory (host) into /app (container)
COPY . /app

# Set working directory inside the container
WORKDIR /app

EXPOSE 9092 2181

# Run build
RUN mkdir build && cmake -B build && cmake --build build/

# Entry point
ENTRYPOINT ["/bin/bash", "-c", "cd /app/build; exec /bin/bash"]
