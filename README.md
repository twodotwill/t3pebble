# T3 Pebble

Pebble Time / Pebble Time 2 watch app for controlling T3 Code threads from the watch.

The watch app is a normal Pebble C app. The phone-side PebbleKit JS bridge talks directly to T3 Code over its REST orchestration API; there is no external bridge process and no T3 Code fork.

## Repositories

- T3 Pebble app: https://github.com/twodotwill/t3pebble
- Upstream T3 Code: https://github.com/pingdotgg/t3code

## Current Compatibility

This app runs against stock, unmodified T3 Code — the published `t3` CLI (verified against `t3@0.0.33`). No server patch, no compatibility branch.

It uses:

- a bearer access token from `t3 auth session issue`
- `GET /api/orchestration/snapshot` for projects and thread metadata
- `GET /api/orchestration/threads/:threadId` for thread bodies
- `POST /api/orchestration/dispatch` for every write

See [docs/t3code-compatibility.md](docs/t3code-compatibility.md) for the exact API surface this depends on.

## Working With Threads

The watch opens a host on its **active** threads — anything still running,
waiting, or erroring. Finished work is one row away:

- The last row of the list switches scope: `SETTLED 34` opens the settled list,
  `ACTIVE 6` comes back. Settled covers snoozed threads too.
- Settled history arrives a page at a time; a `MORE 20 OF 34` row fetches the
  next page.
- Ordering is by activity, so a thread that aged out sits where its work left
  it rather than where the server noticed.

Holding **Select** on a thread opens its actions. The menu is built per thread,
so an active one offers Settle and Interrupt while a settled one offers
Unsettle. A short press still replies by dictation, which is the common case.

Threads settle themselves after three days of quiet, matching T3 Code's own
sidebar, so most of the settled list is work nobody explicitly closed.

## Creating A Project From The Watch

Set a **Project root** for a host in settings, then pick `New project` under the
project list and dictate a name. The phone resolves it to an absolute path and
shows it; nothing is created until you confirm:

```text
"sparkle renderer"  ->  /home/will/Projects/sparkle-renderer
```

Confirming dispatches `project.create` with `createWorkspaceRootIfMissing`, so
the directory is made for you.

Holding **Select** on that same row instead describes the location out loud. A
**concierge project**, named in settings, has its agent work out the path and
propose it. The agent only proposes — the watch still creates it after you
approve, so the confirmation stays a real gate.

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

Install T3 Code the normal way (requires Node.js 22.16+, 23.11+, or 24.10+):

```sh
npm install -g t3
```

`npx t3@latest` works too; the scripts here fall back to it automatically.

On the machine running T3 Code:

```sh
brew install tailscale   # or your platform's package manager
```

Install the Pebble SDK separately and make sure this works:

```sh
pebble --version
```

### 2. Clone This Repo

```sh
git clone git@github.com:twodotwill/t3pebble.git
cd t3pebble
```

There is no second repo to clone. T3 Code is used as shipped.

### 3. Start T3 Code For Pebble

On any machine you want the watch to reach, with no checkout required:

```sh
curl -fsSL https://raw.githubusercontent.com/twodotwill/t3pebble/main/run-t3code-tailscale.sh \
  | T3PEBBLE_TAILSCALE_SERVE=1 bash
```

The script fetches its own helpers from the same ref, so nothing else is needed
on that machine beyond `t3`, `tailscale`, and `curl`. Pin a specific revision
with `T3PEBBLE_REF=<tag-or-sha>` instead of tracking `main`.

From a clone it behaves identically:

```sh
./run-t3code-tailscale.sh
```

The script binds T3 Code to your Tailscale IP, mints a long-lived bearer access token, and prints the values to enter in the Pebble app settings:

```text
Base URL:     http://100.x.y.z:3773
Access token: <token>
```

Useful environment variables:

- `HOST` — bind address, if you do not want the Tailscale IP.
- `PORT` — defaults to `3773`.
- `T3PEBBLE_TOKEN` — reuse an existing token instead of minting a new one.
- `T3PEBBLE_TOKEN_TTL` — token lifetime, defaults to `365d`.
- `T3CODE_BASE_DIR` — explicit T3 Code data directory.
- `T3_CMD` — override how the `t3` CLI is invoked.
- `T3PEBBLE_TAILSCALE_SERVE` — set to `1` for Tailscale Serve mode (see below).
- `T3PEBBLE_SERVE_PORT` — HTTPS port for that mode, defaults to `443`.
- `T3PEBBLE_LABEL` — watch label for this machine; defaults to its short
  tailnet name. The watch shows 18 characters.
- `T3PEBBLE_PROJECT_ROOT` — where projects dictated from the watch are created
  on this machine. Adds it to the paste line so it arrives with the token.

#### Tailscale Serve mode (optional)

```sh
T3PEBBLE_TAILSCALE_SERVE=1 ./run-t3code-tailscale.sh
```

Instead of binding the Tailscale IP, this leaves the server on loopback and
publishes it over tailnet HTTPS, printing a base URL like
`https://<machine>.<tailnet>.ts.net`. Useful because:

- it works against a T3 Code desktop app, which only listens on `127.0.0.1`
- the watch talks HTTPS with a real certificate instead of cleartext HTTP
- no host binding to get right, and no firewall prompt

It needs MagicDNS and HTTPS certificates enabled for the tailnet. The mapping is
created with `tailscale serve --bg`, so it survives reboots; remove it with
`tailscale serve --https=443 off`. To keep the server itself running across
reboots, install it as a service with `t3 service install`.

The Pebble app needs no rebuild for this — the phone bridge already accepts any
`http://` or `https://` base URL. Only the settings field changes.

#### "tailscale is required" on macOS

Tailscale can be running while its CLI is absent from your shell's `PATH`. The
script checks with `command -v tailscale` and stops when that finds nothing,
which is what this message means — not that Tailscale is down.

On macOS the CLI ships inside the app bundle:

```text
/Applications/Tailscale.app/Contents/MacOS/tailscale
```

The clean fix is to install the CLI integration, which puts it on `PATH` for
good. Open Tailscale, go to **Settings → CLI integration → Show me how →
Install Now**, then check:

```sh
command -v tailscale
tailscale status
```

Rerun the script afterwards.

To run it right now without installing the integration, point `PATH` at the
bundle for the one command:

```sh
curl -fsSL https://raw.githubusercontent.com/twodotwill/t3pebble/main/run-t3code-tailscale.sh \
  | PATH="/Applications/Tailscale.app/Contents/MacOS:$PATH" \
    TAILSCALE_BE_CLI=1 \
    T3PEBBLE_TAILSCALE_SERVE=1 bash
```

Tailscale's [macOS CLI documentation](https://tailscale.com/docs/reference/tailscale-cli?tab=macos)
covers the bundle location and recommends the CLI integration.

To issue a token by hand instead:

```sh
t3 auth session issue --ttl 365d --label "T3 Pebble watch" --token-only
```

Tokens are listed and revoked with `t3 auth session list` and `t3 auth session revoke <session-id>`.

### 4. Build The Pebble App

```sh
./verify-t3pebble.sh
```

The installable bundle is written to:

```text
dist/t3pebble.pbw
```

Install it through the Pebble tooling or the Core Devices/Pebble phone app.

## Pebble App Settings

Open the app settings from the phone app. Under **Quick setup**, paste the
`t3pebble1|...` line the launch script printed and press **Add from paste**:

```text
t3pebble1|beta1|https://beta1.tailnet.ts.net|<token>
```

To attach several machines, run the script on each one and paste every line —
together or one at a time. Pasting a machine's line again refreshes its token in
place instead of adding a duplicate, so re-running the script after a token
expires is all it takes.

Up to 6 machines can be configured. Each appears as its own row on the watch,
they are queried in parallel, and a machine that is asleep or unreachable shows
as `offline` without holding up the others.

The fields can still be filled in by hand:

```text
Base URL:     http://<tailscale-ip>:3773
Access token: <token from t3 auth session issue>
```

The phone bridge sends every request to that base URL with an `Authorization: Bearer <token>` header.

Labels are what the watch shows, and it displays 18 characters. With no label
set, one is derived from the host — `beta1.tailnet.ts.net` becomes `beta1`.
Machine names longer than that are worth shortening with `T3PEBBLE_LABEL`.

## Test Commands

Fast phone bridge tests:

```sh
cd pebblecode
node --check src/pkjs/index.js
node test/bridge.test.js
node test/bridge.integration.test.js
```

Full local verification, including the Pebble build and a smoke test against a real stock T3 Code server:

```sh
./verify-t3pebble.sh
```

The smoke test starts `t3 serve` on a throwaway data directory, checks that `/api/orchestration/snapshot` refuses an unauthenticated read, and checks that it answers a bearer token.

## Why This Exists

Pebble is too constrained to run a full T3 Code client. The phone-side PebbleKit JS bridge gives the watch a compact control surface while the laptop remains the execution environment. Tailscale provides the private network path between phone and laptop.

No T3 Code runtime changes are required. All Pebble-specific logic lives in this app.
