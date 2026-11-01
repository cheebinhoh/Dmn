#!/usr/bin/env bash

BASHRC="$HOME/.bashrc"
MARKER_START="# === DMN BUILD HOOK START ==="
MARKER_END="# === DMN BUILD HOOK END ==="

# Define the exact text of the function you want to inject
read -r -d '' HOOK_CONTENT << 'EOF'
dmn_build_n_test_all() {
  (
    set -e

    cmake -S . -B build-all -DCMAKE_BUILD_TYPE=Debug \
      -DDMN_BUILD_TESTS=ON \
      -DDMN_ALLOW_DEPENDENCY_FETCH=ON \
      -DBUILD_KAFKA_TEST=ON \
      -DENABLE_APP_BLUEZ_DISCOVERY_CLI=ON \
      -DENABLE_APP_BLUETOOTH_CLI=ON \
      -DENABLE_APP_BLUETOOTH_GTK=ON \
      -DENABLE_APP_BLUETOOTH_COSMIC=ON \
      -DENABLE_VALGRIND=ON \
      -DENABLE_FAULT_INJECTION=ON \
      && \
      cmake --build build-all --parallel 2 \
      && \
      ctest --test-dir build-all -L "^(dmn|dmn-cli|dmn-gui|valgrind|fault-injection)$" --output-on-failure \

    set +e
  )
}

dmn_build_n_install_app() {
  (
    set -e
    INSTALL_PREFIX="$HOME/.local"

    # CMake installs the Cargo-built release executable with the runtime assets.
    cargo build \
      --manifest-path src/app/gui/cosmic/Cargo.toml \
      --release \
      --locked \
      && \
    cmake -S . -B build-app \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
      -DDMN_BUILD_TESTS=OFF \
      -DENABLE_APP_BLUEZ_DISCOVERY_CLI=ON \
      -DENABLE_APP_BLUETOOTH_CLI=ON \
      -DENABLE_APP_BLUETOOTH_GTK=ON \
      -DENABLE_APP_BLUETOOTH_COSMIC=ON \
      "-DDMN_BLUETOOTH_COSMIC_EXECUTABLE=$PWD/src/app/gui/cosmic/target/release/dmn-bluetooth-cosmic" \
      -DENABLE_DMN_DMESGNET=OFF \
      -DCMAKE_COMPILE_WARNING_AS_ERROR=ON \
      && \
      cmake --build build-app --parallel 2 \
      && \
      cmake --install build-app --component Runtime \
      && \
      test -x "$INSTALL_PREFIX/bin/dmn-bluetooth-gtk" \
      && \
      test -f "$INSTALL_PREFIX/share/applications/com.cheebinhoh.dmn-bluetooth.desktop" \
      && \
      test -f "$INSTALL_PREFIX/share/metainfo/com.cheebinhoh.dmn-bluetooth.metainfo.xml" \
      && \
      test -x "$INSTALL_PREFIX/bin/dmn-bluetooth-cosmic" \
      && \
      test -f "$INSTALL_PREFIX/bin/libdmn-bluetooth-cabi.so" \
      && \
      test -f "$INSTALL_PREFIX/share/applications/io.github.cheebinhoh.dmnbluetoothcosmic.desktop" \
      && \
      test -f "$INSTALL_PREFIX/share/metainfo/io.github.cheebinhoh.dmnbluetoothcosmic.metainfo.xml" \
      && \
      printf 'Installed GTK and COSMIC apps under %s/bin\\n' "$INSTALL_PREFIX"

    set +e
  )
}


EOF

# Ensure .bashrc exists
touch "$BASHRC"

# Construct the full block layout with its boundary markers
FULL_BLOCK=$(cat <<EOF
$MARKER_START
$HOOK_CONTENT
$MARKER_END
EOF
)

# 1. Check if the block markers already exist in the file
if grep -qF "$MARKER_START" "$BASHRC"; then
    # Extract the existing block to compare content
    CURRENT_BLOCK=$(sed -n "/$MARKER_START/,/$MARKER_END/p" "$BASHRC")
    
    if [ "$CURRENT_BLOCK" = "$FULL_BLOCK" ]; then
        echo "✅ Hook is already up to date in $BASHRC. No changes needed."
        exit 0
    else
        echo "🔄 Content mismatch detected! Removing old hook and updating..."
        
        # Cross-platform safe block deletion using a temporary file
        TMP_FILE=$(mktemp)
        sed "/$MARKER_START/,/$MARKER_END/d" "$BASHRC" > "$TMP_FILE"
        mv "$TMP_FILE" "$BASHRC"
    fi
else
    echo "📥 Installing dmn_build_all hook into $BASHRC for the first time..."
fi

# 2. Append the updated or brand new block to the end of the file
echo -e "\n$FULL_BLOCK" >> "$BASHRC"
echo "✨ Successfully updated! Run 'source ~/.bashrc' to apply changes."
