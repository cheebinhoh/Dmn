#!/bin/sh
set -eu

HOOK_PATH=".git/hooks/pre-commit"

mkdir -p "$(dirname "$HOOK_PATH")"

cat > "$HOOK_PATH" <<'EOF'
#!/bin/sh

./scripts/run-tab-check-and-clang-format.sh
EOF

chmod +x "$HOOK_PATH"

echo "Installed pre-commit hook at $HOOK_PATH"
