#!/usr/bin/env bash
set -euo pipefail

# Starts stock T3 Code on a Tailscale address and prints the two values the
# Pebble app settings screen needs. No T3 Code fork or source checkout is
# required: this drives the published `t3` CLI.

# Works from a clone and straight from a pipe:
#   curl -fsSL https://raw.githubusercontent.com/twodotwill/t3pebble/main/run-t3code-tailscale.sh | bash
# Piped, there is no checkout to read the shared helpers from, so they are
# fetched from the same ref. Pin one with T3PEBBLE_REF to avoid tracking main.
T3PEBBLE_REPO="${T3PEBBLE_REPO:-twodotwill/t3pebble}"
T3PEBBLE_REF="${T3PEBBLE_REF:-main}"
T3PEBBLE_RAW_BASE="${T3PEBBLE_RAW_BASE:-https://raw.githubusercontent.com/$T3PEBBLE_REPO/$T3PEBBLE_REF}"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-.}")" 2>/dev/null && pwd)" || ROOT_DIR="."

if [[ -f "$ROOT_DIR/lib/t3-runner.sh" ]]; then
  # shellcheck source=lib/t3-runner.sh
  source "$ROOT_DIR/lib/t3-runner.sh"
else
  if ! command -v curl >/dev/null 2>&1; then
    echo "curl is required when running this script outside a checkout." >&2
    exit 1
  fi
  T3_RUNNER_SRC="$(curl -fsSL "$T3PEBBLE_RAW_BASE/lib/t3-runner.sh")" || {
    echo "Could not fetch lib/t3-runner.sh from $T3PEBBLE_RAW_BASE" >&2
    exit 1
  }
  source /dev/stdin <<<"$T3_RUNNER_SRC"
fi

PORT="${PORT:-3773}"
TOKEN_TTL="${T3PEBBLE_TOKEN_TTL:-365d}"
TOKEN_LABEL="${T3PEBBLE_TOKEN_LABEL:-T3 Pebble watch}"
# Opt-in only. Unset, this script behaves exactly as it always has: bind the
# tailnet IP and hand the watch a plain-HTTP base URL.
TAILSCALE_SERVE="${T3PEBBLE_TAILSCALE_SERVE:-0}"
SERVE_PORT="${T3PEBBLE_SERVE_PORT:-443}"

t3_resolve || exit 1

BASE_DIR_ARGS=()
if [[ -n "${T3CODE_BASE_DIR:-}" ]]; then
  BASE_DIR_ARGS=(--base-dir "$T3CODE_BASE_DIR")
fi

if [[ "$TAILSCALE_SERVE" == "1" ]]; then
  # Serve proxies the tailnet to loopback, so the server itself never needs a
  # routable bind address.
  t3_require_tailscale || exit 1
  MAGIC_DNS="$(t3_tailscale_magic_dns)" || exit 1
  HOST="${HOST:-127.0.0.1}"
  BASE_URL="$(t3_tailscale_https_base_url "$MAGIC_DNS" "$SERVE_PORT")"
else
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

  BASE_URL="http://$HOST:$PORT"
fi

# Reuse a token across restarts when one is supplied; otherwise mint a fresh
# bearer access token against the same data directory the server will use.
if [[ -n "${T3PEBBLE_TOKEN:-}" ]]; then
  TOKEN="$T3PEBBLE_TOKEN"
else
  TOKEN="$("${T3[@]}" auth session issue \
    "${BASE_DIR_ARGS[@]}" \
    --ttl "$TOKEN_TTL" \
    --label "$TOKEN_LABEL" \
    --token-only)"
  TOKEN="$(printf '%s' "$TOKEN" | tr -d '[:space:]')"
fi

if [[ -z "$TOKEN" ]]; then
  echo "Failed to issue a T3 Code access token." >&2
  exit 1
fi

if [[ "$TAILSCALE_SERVE" == "1" ]]; then
  t3_tailscale_serve_enable "$SERVE_PORT" "$HOST" "$PORT" || exit 1
fi

WATCH_LABEL="${T3PEBBLE_LABEL:-$(t3_default_label)}"

cat <<EOF
T3 Pebble server
T3 URL:       $BASE_URL

Paste this line into the Pebble app settings, under Quick setup:

$(t3_bundle_line "$WATCH_LABEL" "$BASE_URL" "$TOKEN")

Run this on each machine and paste every line to attach several hosts.

Or enter the fields by hand:
Base URL:     $BASE_URL
Access token: $TOKEN

Revoke it later with: ${T3[*]} auth session list / revoke <session-id>
EOF

# The watch shows 18 characters, and tailnet machine names routinely run longer.
if [[ "${#WATCH_LABEL}" -gt 18 ]]; then
  cat <<EOF

Note: "$WATCH_LABEL" is longer than the 18 characters the watch shows.
Set a shorter one with: T3PEBBLE_LABEL=mac $0
EOF
fi

if [[ "$TAILSCALE_SERVE" == "1" ]]; then
  cat <<EOF

Tailscale Serve maps $BASE_URL to http://$HOST:$PORT and persists across
restarts. Remove it with: tailscale serve --https=$SERVE_PORT off

The first request can lag while Tailscale provisions a certificate.
To keep the server itself running after a reboot: ${T3[*]} service install
EOF
fi

exec "${T3[@]}" serve \
  --host "$HOST" \
  --port "$PORT" \
  --no-browser \
  "${BASE_DIR_ARGS[@]}" \
  "$@"
