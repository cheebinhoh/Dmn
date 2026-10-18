#!/usr/bin/env bash
#
# Copyright © 2026 Chee Bin HOH. All rights reserved.
#
# Start a local Kafka broker for Dmn's integration tests. The Kafka
# distribution is kept beside this script's build/install tree when writable;
# otherwise it is cached in the current user's cache directory.

set -euo pipefail

readonly KAFKA_VERSION="2.8.0"
readonly SCALA_VERSION="2.13"
readonly KAFKA_DIST="kafka_${SCALA_VERSION}-${KAFKA_VERSION}"
readonly KAFKA_ARCHIVE="${KAFKA_DIST}.tgz"
readonly KAFKA_BASE_URL="https://archive.apache.org/dist/kafka/${KAFKA_VERSION}"

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
PREFIX_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"
COMMAND="${1:-start}"

fail() {
  printf 'Kafka bootstrap error: %s\n' "$*" >&2

  exit 1
}

find_java() {
  if [[ -n "${JAVA_HOME:-}" && -x "${JAVA_HOME}/bin/java" ]]; then
    printf '%s\n' "${JAVA_HOME}/bin/java"
  else
    command -v java || true
  fi
}

find_kafka_home() {
  local candidate

  if [[ -n "${DMN_KAFKA_HOME:-}" ]]; then
    candidate="${DMN_KAFKA_HOME}"
    [[ -x "${candidate}/bin/kafka-server-start.sh" ]] ||
      fail "DMN_KAFKA_HOME does not contain bin/kafka-server-start.sh: ${candidate}"
    printf '%s\n' "$candidate"

    return
  fi

  for candidate in \
    "${PREFIX_DIR}/${KAFKA_DIST}" \
    "${PREFIX_DIR}/kafka" \
    "${PREFIX_DIR}/share/dmn/${KAFKA_DIST}" \
    "/opt/kafka"; do
    if [[ -x "${candidate}/bin/kafka-server-start.sh" ]]; then
      printf '%s\n' "$candidate"

      return
    fi
  done
}

download_kafka() {
  local install_root archive checksum checksum_file expected actual
  if [[ -w "${PREFIX_DIR}" ]]; then
    install_root="${PREFIX_DIR}"
  else
    install_root="${XDG_CACHE_HOME:-${HOME}/.cache}/dmn"
  fi

  archive="${install_root}/${KAFKA_ARCHIVE}"
  checksum_file="${archive}.sha512"
  mkdir -p -- "$install_root"

  if [[ ! -f "${install_root}/${KAFKA_DIST}/bin/kafka-server-start.sh" ]]; then
    command -v curl >/dev/null || fail "curl is required to download Kafka"
    command -v tar >/dev/null || fail "tar is required to unpack Kafka"
    command -v sha512sum >/dev/null ||
      fail "sha512sum is required to verify the Kafka archive"

    printf 'Downloading Kafka %s to %s\n' "$KAFKA_VERSION" "$install_root" >&2
    curl --fail --location --retry 3 --output "${archive}.part" \
      "${KAFKA_BASE_URL}/${KAFKA_ARCHIVE}" ||
      fail "failed to download ${KAFKA_ARCHIVE}"
    curl --fail --location --retry 3 --output "${checksum_file}.part" \
      "${KAFKA_BASE_URL}/${KAFKA_ARCHIVE}.sha512" ||
      fail "failed to download the Kafka SHA-512 checksum"

    checksum="$(awk -F ': ' 'NR == 1 { printf "%s", $2; next } { printf "%s", $0 }' \
      "${checksum_file}.part" | tr -d '[:space:]')"
    expected="${checksum^^}"
    actual="$(sha512sum "${archive}.part" | awk '{print toupper($1)}')"
    [[ -n "$expected" && "$actual" == "$expected" ]] || {
      rm -f -- "${archive}.part" "${checksum_file}.part"
      fail "Kafka archive checksum verification failed"
    }

    mv -- "${archive}.part" "$archive"
    mv -- "${checksum_file}.part" "$checksum_file"
    tar --extract --gzip --file "$archive" --directory "$install_root"
  fi

  printf '%s\n' "${install_root}/${KAFKA_DIST}"
}

resolve_kafka_home() {
  local home
  home="$(find_kafka_home)"

  if [[ -z "$home" ]]; then
    home="$(download_kafka)"
  fi

  [[ -x "${home}/bin/kafka-topics.sh" ]] ||
    fail "Kafka distribution is missing kafka-topics.sh: ${home}"

  printf '%s\n' "$home"
}

prepare_runtime() {
  local runtime_root

  if [[ -w "${PREFIX_DIR}" ]]; then
    runtime_root="${PREFIX_DIR}/kafka-runtime"
  else
    runtime_root="${XDG_STATE_HOME:-${HOME}/.local/state}/dmn-kafka"
  fi

  mkdir -p -- "${runtime_root}/logs" "${runtime_root}/data"

  printf '%s\n' "$runtime_root"
}

process_matches_config() {
  local pid="$1" config="$2" command_line
  command_line="$(ps -p "$pid" -o args= 2>/dev/null || true)"
  [[ -n "$command_line" && "$command_line" == *"$config"* ]]
}

stop_recorded_process() {
  local pid_file="$1" config="$2" label="$3" pid
  [[ -f "$pid_file" ]] || return 0
  read -r pid < "$pid_file"

  if [[ "$pid" =~ ^[0-9]+$ ]] && process_matches_config "$pid" "$config"; then
    kill -TERM "$pid"

    for _ in {1..20}; do
      kill -0 "$pid" 2>/dev/null || break
      sleep 1
    done

    if kill -0 "$pid" 2>/dev/null; then
      printf 'Kafka bootstrap warning: %s process %s did not stop within 20 seconds\n' \
        "$label" "$pid" >&2

      return 1
    fi

    printf 'Stopped %s (pid %s)\n' "$label" "$pid"
  fi

  rm -f -- "$pid_file"
}

if [[ "$COMMAND" != "start" && "$COMMAND" != "stop" ]]; then
  fail "usage: ${BASH_SOURCE[0]} [start|stop]"
fi

RUNTIME_DIR="$(prepare_runtime)"
ZOOKEEPER_CONFIG="${RUNTIME_DIR}/zookeeper.properties"
BROKER_CONFIG="${RUNTIME_DIR}/server.properties"
ZOOKEEPER_PID_FILE="${RUNTIME_DIR}/zookeeper.pid"
BROKER_PID_FILE="${RUNTIME_DIR}/kafka.pid"
KAFKA_LISTENERS="${DMN_KAFKA_LISTENERS:-PLAINTEXT://127.0.0.1:9092}"
KAFKA_ADVERTISED_LISTENERS="${DMN_KAFKA_ADVERTISED_LISTENERS:-${KAFKA_LISTENERS}}"

if [[ "$COMMAND" == "stop" ]]; then
  stop_recorded_process "$BROKER_PID_FILE" "$BROKER_CONFIG" "Kafka broker"
  stop_recorded_process "$ZOOKEEPER_PID_FILE" "$ZOOKEEPER_CONFIG" "ZooKeeper"

  exit 0
fi

JAVA="$(find_java)"
[[ -n "$JAVA" ]] ||
  fail "a Java runtime is required; set JAVA_HOME or install a JRE"
export JAVA_HOME="${JAVA%/bin/java}"
KAFKA_HOME="$(resolve_kafka_home)"

broker_ready() {
  "$KAFKA_HOME/bin/kafka-topics.sh" \
    --bootstrap-server 127.0.0.1:9092 --list >/dev/null 2>&1
}

if ! broker_ready; then
  cp -- "${KAFKA_HOME}/config/zookeeper.properties" "$ZOOKEEPER_CONFIG"
  printf '\ndataDir=%s\n' "${RUNTIME_DIR}/data/zookeeper" >> "$ZOOKEEPER_CONFIG"
  mkdir -p -- "${RUNTIME_DIR}/data/zookeeper"

  cp -- "${KAFKA_HOME}/config/server.properties" "$BROKER_CONFIG"
  cat >> "$BROKER_CONFIG" <<EOF

listeners=${KAFKA_LISTENERS}
advertised.listeners=${KAFKA_ADVERTISED_LISTENERS}
log.dirs=${RUNTIME_DIR}/data/kafka
zookeeper.connect=127.0.0.1:2181
EOF
  mkdir -p -- "${RUNTIME_DIR}/data/kafka"

  if [[ -f "$ZOOKEEPER_PID_FILE" ]] &&
     process_matches_config "$(cat "$ZOOKEEPER_PID_FILE")" "$ZOOKEEPER_CONFIG"; then
    fail "ZooKeeper is already running but the Kafka broker is unavailable; inspect ${RUNTIME_DIR}/logs"
  fi

  if [[ -f "$BROKER_PID_FILE" ]] &&
     process_matches_config "$(cat "$BROKER_PID_FILE")" "$BROKER_CONFIG"; then
    fail "Kafka broker is already starting but not yet ready; inspect ${RUNTIME_DIR}/logs"
  fi

  printf 'Starting ZooKeeper and Kafka using %s\n' "$KAFKA_HOME"
  nohup "$KAFKA_HOME/bin/zookeeper-server-start.sh" "$ZOOKEEPER_CONFIG" \
    > "${RUNTIME_DIR}/logs/zookeeper.log" 2>&1 < /dev/null &
  echo "$!" > "$ZOOKEEPER_PID_FILE"

  nohup "$KAFKA_HOME/bin/kafka-server-start.sh" "$BROKER_CONFIG" \
    > "${RUNTIME_DIR}/logs/kafka.log" 2>&1 < /dev/null &
  echo "$!" > "$BROKER_PID_FILE"
fi

printf 'Waiting for Kafka broker'
for _ in {1..90}; do
  if broker_ready; then
    printf '\n'

    break
  fi

  printf '.'

  sleep 1
done

if ! broker_ready; then
  printf '\n' >&2
  printf 'Kafka bootstrap error: broker did not become ready; inspect %s/logs\n' \
    "$RUNTIME_DIR" >&2
  tail -n 40 "${RUNTIME_DIR}/logs/zookeeper.log" \
    "${RUNTIME_DIR}/logs/kafka.log" >&2 || true

  exit 1
fi

"$KAFKA_HOME/bin/kafka-topics.sh" --create --if-not-exists \
  --topic timer_counter --bootstrap-server 127.0.0.1:9092
printf 'Kafka broker is ready; topic timer_counter is available\n'
