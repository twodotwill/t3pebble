#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
T3CODE_DIR="${T3CODE_DIR:-$ROOT_DIR/t3code}"

if ! command -v bun >/dev/null 2>&1; then
  echo "bun is required to run the T3 Code server." >&2
  exit 1
fi

if [[ ! -d "$T3CODE_DIR" ]]; then
  echo "T3 Code checkout not found at: $T3CODE_DIR" >&2
  echo "Set T3CODE_DIR=/path/to/t3code." >&2
  exit 1
fi

if ! grep -R -- '--auth-token\|auth-token' "$T3CODE_DIR/apps/server/src" >/dev/null 2>&1; then
  echo "This T3 Code checkout does not expose the legacy --auth-token server API." >&2
  echo "Use the t3pebble-auth-token-compatible branch from git@github.com:twodotwill/t3code.git." >&2
  exit 1
fi

PORT="${PORT:-3773}"
if [[ -z "${HOST:-}" ]]; then
  if ! command -v tailscale >/dev/null 2>&1; then
    echo "tailscale is required unless HOST is set explicitly." >&2
    echo "Example: HOST=100.x.y.z $0" >&2
    exit 1
  fi
  HOST="$(tailscale ip -4 | head -n 1)"
fi

if [[ -z "${HOST}" ]]; then
  echo "Could not determine a Tailscale IPv4 address. Set HOST explicitly." >&2
  exit 1
fi

if [[ -n "${T3PEBBLE_TOKEN:-}" ]]; then
  TOKEN="$T3PEBBLE_TOKEN"
elif command -v openssl >/dev/null 2>&1; then
  TOKEN="$(openssl rand -hex 24)"
else
  TOKEN="$(date +%s)-t3pebble-token"
fi

cd "$T3CODE_DIR"

if [[ "${T3PEBBLE_SKIP_BUILD:-0}" != "1" ]]; then
  bun run build
fi

cat <<EOF
T3 Pebble server
T3 URL:     http://$HOST:$PORT
T3 token:   $TOKEN

Use these values in the Pebble app settings:
Base URL:   http://$HOST:$PORT
Auth token: $TOKEN
EOF

exec bun run --cwd apps/server start -- \
  --host "$HOST" \
  --port "$PORT" \
  --auth-token "$TOKEN" \
  --no-browser \
  "$@"
