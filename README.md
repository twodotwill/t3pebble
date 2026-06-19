# T3 Pebble

Pebble Time / Pebble Time 2 watch app for controlling T3 Code threads from the watch.

The watch app is a normal Pebble C app. The phone-side PebbleKit JS bridge talks directly to T3 Code over its WebSocket RPC API; there is no external bridge process.

## Repositories

- T3 Pebble app: https://github.com/twodotwill/t3pebble
- T3 Code fork/reference: https://github.com/twodotwill/t3code
- Upstream T3 Code: https://github.com/pingdotgg/t3code

## Current Compatibility

This Pebble app currently targets the older T3 Code server API from commit `226ed997e1a6493e6b29d5264e0b0f8173e7c630`.

That T3 Code version supports:

- `--auth-token`
- WebSocket auth with `/ws?token=...`
- `orchestration.getSnapshot`
- `orchestration.dispatchCommand`

Current upstream T3 Code `0.0.27` changed the remote API to pairing/session auth and shell/thread subscriptions. This app needs a follow-up migration before it can run against current upstream T3 Code after restart.

See [docs/t3code-compatibility.md](docs/t3code-compatibility.md) for the exact differences and the migration plan.

## What Works

- Lists recent active T3 Code threads on the watch.
- Lists known T3 Code project folders and can start a new thread in a project.
- Shows thread title, project path, provider, status, latest summary, and full context.
- Uses watch dictation to send a prompt to a selected thread.
- Can interrupt a running turn by dictating `stop`, `interrupt`, or `cancel turn`.
- Handles approval and user-input prompts from T3 Code.
- Forces Codex model selections sent from the Pebble app to `gpt-5.5`.

## Setup

### 1. Install Requirements

On the Mac running T3 Code:

```sh
brew install tailscale
```

Install the Pebble SDK separately and make sure this works:

```sh
pebble --version
```

Install Bun for the legacy-compatible T3 Code server:

```sh
curl -fsSL https://bun.sh/install | bash
```

### 2. Clone Both Repos

```sh
git clone git@github.com:twodotwill/t3pebble.git
git clone git@github.com:twodotwill/t3code.git
cd t3code
git checkout t3pebble-auth-token-compatible
```

The `t3pebble-auth-token-compatible` branch points at the older upstream T3 Code commit that still has the token-based WebSocket API this Pebble app uses.

### 3. Build T3 Code

```sh
cd ../t3code
bun install
bun run build
```

### 4. Start T3 Code For Pebble

From the `t3pebble` repo:

```sh
T3CODE_DIR=../t3code HOST="$(tailscale ip -4 | head -n 1)" T3PEBBLE_TOKEN=t3 ./run-t3code-tailscale.sh
```

The script prints the values to enter in the Pebble app settings:

```text
Base URL: http://100.x.y.z:3773
Auth token: t3
```

For repeat launches after the server has already been built:

```sh
T3PEBBLE_SKIP_BUILD=1 T3CODE_DIR=../t3code T3PEBBLE_TOKEN=t3 ./run-t3code-tailscale.sh
```

### 5. Build The Pebble App

```sh
cd t3pebble
T3CODE_DIR=../t3code ./verify-t3pebble.sh
```

The installable bundle is written to:

```text
dist/t3pebble.pbw
```

Install it through the Pebble tooling or the Core Devices/Pebble phone app.

## Pebble App Settings

Open the app settings from the phone app and set:

```text
Base URL: http://<tailscale-ip>:3773
Auth token: <same token used when starting T3 Code>
```

The phone bridge connects to:

```text
ws://<tailscale-ip>:3773/ws?token=<auth-token>
```

## Test Commands

Fast phone bridge tests:

```sh
cd pebblecode
node --check src/pkjs/index.js
node test/bridge.test.js
node test/bridge.integration.test.js
```

Full local verification, including Pebble build and legacy T3 Code smoke test:

```sh
T3CODE_DIR=../t3code ./verify-t3pebble.sh
```

## Why This Exists

Pebble is too constrained to run a full T3 Code client. The phone-side PebbleKit JS bridge gives the watch a compact control surface while the laptop remains the execution environment. Tailscale provides the private network path between phone and laptop.

No T3 Code runtime changes are required for the currently supported legacy-compatible server branch. The Pebble-specific logic lives in this app.
