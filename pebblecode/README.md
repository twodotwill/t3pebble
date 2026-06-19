# T3 Pebble Watch App

Pebble C app plus PebbleKit JS phone bridge for controlling T3 Code threads from a Pebble watch.

The phone bridge talks directly to T3 Code over WebSocket RPC. There is no external bridge server.

## Compatibility

This app currently targets the legacy-compatible T3 Code branch:

```text
twodotwill/t3code:t3pebble-auth-token-compatible
```

That branch supports:

- `/ws?token=...`
- `orchestration.getSnapshot`
- `orchestration.dispatchCommand`

Current upstream T3 Code uses pairing/session auth and shell/thread subscriptions instead. See the parent repo's `docs/t3code-compatibility.md` before trying to use this app with current upstream.

## Build

From the parent `t3pebble` repo:

```sh
T3CODE_DIR=../t3code ./verify-t3pebble.sh
```

Or just build the Pebble app:

```sh
pebble build
```

The full verification writes:

```text
../dist/t3pebble.pbw
```

## Configure

Open app settings from the phone app and set:

- Base URL: `http://<tailscale-host-or-ip>:3773`
- Auth token: the same token passed to the compatible T3 Code server with `--auth-token`

The bridge connects to:

```text
ws://<tailscale-host-or-ip>:3773/ws?token=<auth-token>
```

## Features

- Lists recent active T3 Code threads.
- Lists T3 Code projects and can create a new thread in a selected project.
- Shows status, summaries, and full context.
- Sends dictated prompts to existing or new threads.
- Responds to approvals and user-input prompts.
- Interrupts running turns from dictation.
- Forces Codex model selections sent from the watch to `gpt-5.5`.

## Tests

```sh
node --check src/pkjs/index.js
node test/bridge.test.js
node test/bridge.integration.test.js
pebble build
```

## Real Device Verification

1. Install `../dist/t3pebble.pbw`.
2. Start Tailscale on the phone and laptop.
3. Start the compatible T3 Code server with `../run-t3code-tailscale.sh`.
4. Configure Base URL and Auth token in the Pebble app settings.
5. Confirm the watch lists T3 Code threads.
6. Create a new thread from a known project.
7. Dictate `ls` and confirm the T3 Code thread runs it.
8. Confirm running, done, error, approval, and user-input statuses appear correctly.
