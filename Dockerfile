# Copyright © 2025 Chee Bin HOH. All rights reserved.

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


# Setup Kafka local network
ENV DEBIAN_FRONTEND=noninteractive
ENV KAFKA_VERSION=2.8.0
ENV SCALA_VERSION=2.13

RUN wget --retry-connrefused --waitretry=3 --read-timeout=20 --timeout=15 -t 5 \
    https://archive.apache.org/dist/kafka/${KAFKA_VERSION}/kafka_${SCALA_VERSION}-${KAFKA_VERSION}.tgz && \
    tar -xzf kafka_${SCALA_VERSION}-${KAFKA_VERSION}.tgz -C /opt && \
    mv /opt/kafka_${SCALA_VERSION}-${KAFKA_VERSION} /opt/kafka && \
    rm kafka_${SCALA_VERSION}-${KAFKA_VERSION}.tgz


# 3. Create a Startup Script
# This script configures Kafka to use its own container ID/name as its network address
RUN echo '#!/bin/bash \n\
# Update listeners to allow external traffic on the docker network \n\
sed -i "s|#listeners=PLAINTEXT://:9092|listeners=PLAINTEXT://0.0.0.0:9092|g" /opt/kafka/config/server.properties \n\
# Set the advertised listener to the current container hostname \n\
echo "advertised.listeners=PLAINTEXT://$HOSTNAME:9092" >> /opt/kafka/config/server.properties \n\
\n\
# Start Zookeeper in the background \n\
/opt/kafka/bin/zookeeper-server-start.sh /opt/kafka/config/zookeeper.properties & \n\
# Start Kafka \n\
exec /opt/kafka/bin/kafka-server-start.sh /opt/kafka/config/server.properties' > /usr/bin/start-kafka.sh && \
chmod +x /usr/bin/start-kafka.sh


# Copy everything from the current directory (host) into /app (container)
COPY . /app


# Set working directory inside the container
WORKDIR /app

EXPOSE 9092 2181


# Run build
RUN mkdir build && cmake -B build && cmake --build build/


# Entry point
ENTRYPOINT ["/bin/bash", "-c", "/app/scripts/run-bookstrap-dmn.sh; cd /app/build; ctest -L dmn && exec /bin/bash"]
