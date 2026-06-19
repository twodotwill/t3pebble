#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PEBBLE_DIR="$ROOT_DIR/pebblecode"
T3CODE_DIR="${T3CODE_DIR:-$ROOT_DIR/t3code}"
DIST_DIR="$ROOT_DIR/dist"
SMOKE_TOKEN="${SMOKE_TOKEN:-t3pebble-smoke-token}"
SMOKE_BASE_DIR="${SMOKE_BASE_DIR:-$(mktemp -d /tmp/t3pebble-smoke.XXXXXX)}"

SERVER_PID=""

cleanup() {
  if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" >/dev/null 2>&1; then
    kill "$SERVER_PID" >/dev/null 2>&1 || true
    wait "$SERVER_PID" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "$1 is required." >&2
    exit 1
  fi
}

require_command bun
require_command curl
require_command node
require_command pebble

if [[ ! -d "$T3CODE_DIR" ]]; then
  echo "T3 Code checkout not found at: $T3CODE_DIR" >&2
  echo "Set T3CODE_DIR=/path/to/t3code." >&2
  exit 1
fi

if ! grep -R -- '--auth-token\|auth-token' "$T3CODE_DIR/apps/server/src" >/dev/null 2>&1; then
  echo "This verification script targets the legacy T3 Code --auth-token API." >&2
  echo "Use T3CODE_DIR pointing at the t3pebble-auth-token-compatible branch." >&2
  exit 1
fi

if [[ -z "${SMOKE_PORT:-}" ]]; then
  SMOKE_PORT="$(
    node -e '
const net = require("node:net");
const server = net.createServer();
server.listen(0, "127.0.0.1", () => {
  console.log(server.address().port);
  server.close();
});
'
  )"
fi

cd "$PEBBLE_DIR"
node --check src/pkjs/index.js
node test/bridge.test.js
node test/bridge.integration.test.js
pebble build

mkdir -p "$DIST_DIR"
cp "$PEBBLE_DIR/build/pebblecode.pbw" "$DIST_DIR/t3pebble.pbw"

cd "$T3CODE_DIR"
bun run --cwd apps/server typecheck
bun run --cwd apps/server build

bun run --cwd apps/server start -- \
  --host 127.0.0.1 \
  --port "$SMOKE_PORT" \
  --auth-token "$SMOKE_TOKEN" \
  --no-browser \
  --base-dir "$SMOKE_BASE_DIR" \
  >/tmp/t3pebble-smoke.log 2>&1 &
SERVER_PID="$!"

ready=0
for _ in $(seq 1 40); do
  if curl --max-time 2 -sS \
    "http://127.0.0.1:$SMOKE_PORT/ws?token=$SMOKE_TOKEN" >/tmp/t3pebble-smoke-ws.txt 2>/dev/null; then
    ready=1
    break
  fi
  sleep 0.25
done

if [[ "$ready" != "1" ]]; then
  echo "T3 Code smoke server did not become ready on 127.0.0.1:$SMOKE_PORT." >&2
  cat /tmp/t3pebble-smoke.log >&2 || true
  exit 1
fi

SMOKE_WS_URL="ws://127.0.0.1:$SMOKE_PORT/ws?token=$SMOKE_TOKEN" \
node <<'NODE'
const url = process.env.SMOKE_WS_URL;
const socket = new WebSocket(url);
const requestId = String(Date.now()) + String(Math.floor(Math.random() * 1000000));
const timeout = setTimeout(() => {
  console.error(`Timed out waiting for ${url}`);
  process.exit(1);
}, 5000);

socket.addEventListener("open", () => {
  socket.send(JSON.stringify({
    _tag: "Request",
    id: requestId,
    tag: "orchestration.getSnapshot",
    payload: {},
    headers: [],
  }));
});

socket.addEventListener("message", (event) => {
  const message = JSON.parse(event.data);
  if (message.requestId !== requestId || message._tag !== "Exit") return;
  clearTimeout(timeout);
  if (message.exit?._tag !== "Success") {
    console.error("Expected orchestration.getSnapshot to succeed.");
    process.exit(1);
  }
  const snapshot = message.exit.value;
  if (!snapshot || !Array.isArray(snapshot.projects) || !Array.isArray(snapshot.threads)) {
    console.error("Snapshot did not include projects and threads arrays.");
    process.exit(1);
  }
  socket.close();
});

socket.addEventListener("error", () => {
  console.error(`Unable to connect to ${url}`);
  process.exit(1);
});
NODE

PBW_PATH="$DIST_DIR/t3pebble.pbw" \
MANIFEST_PATH="$DIST_DIR/t3pebble.manifest.json" \
APPINFO_PATH="$PEBBLE_DIR/appinfo.json" \
node <<'NODE'
const crypto = require("node:crypto");
const fs = require("node:fs");

const pbw = fs.readFileSync(process.env.PBW_PATH);
const appinfo = JSON.parse(fs.readFileSync(process.env.APPINFO_PATH, "utf8"));
const manifest = {
  name: appinfo.longName,
  shortName: appinfo.shortName,
  uuid: appinfo.uuid,
  version: appinfo.versionLabel,
  generatedAt: new Date().toISOString(),
  artifact: {
    path: process.env.PBW_PATH,
    sizeBytes: pbw.length,
    sha256: crypto.createHash("sha256").update(pbw).digest("hex"),
  },
  verification: {
    checks: [
      "node --check src/pkjs/index.js",
      "node test/bridge.test.js",
      "node test/bridge.integration.test.js",
      "pebble build",
      "bun run --cwd apps/server typecheck",
      "bun run --cwd apps/server build",
      "stock T3 Code /ws accepts token",
      "stock T3 Code orchestration.getSnapshot succeeds over websocket RPC",
    ],
  },
};

fs.writeFileSync(process.env.MANIFEST_PATH, `${JSON.stringify(manifest, null, 2)}\n`);
NODE

echo "Verified T3 Pebble."
echo "Installable PBW: $DIST_DIR/t3pebble.pbw"
echo "Manifest: $DIST_DIR/t3pebble.manifest.json"
