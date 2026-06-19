# T3 Code Compatibility

This document records the T3 Code API assumptions used by T3 Pebble.

## Current T3 Pebble Target

T3 Pebble currently targets T3 Code commit:

```text
226ed997e1a6493e6b29d5264e0b0f8173e7c630
```

The T3 Code fork publishes this commit as:

```text
t3pebble-auth-token-compatible
```

That commit is upstream T3 Code before the newer pairing/session remote-access rewrite. It is not a custom T3 Code runtime patch.

## Required Old API Surface

The Pebble phone bridge currently requires:

- Server startup with `--auth-token`.
- WebSocket RPC auth via `/ws?token=<token>`.
- `orchestration.getSnapshot` to fetch projects and threads.
- `orchestration.dispatchCommand` to start turns, create threads, respond to approvals, answer user-input prompts, and interrupt turns.

## Current Upstream T3 Code Differences

Current upstream T3 Code `0.0.27` has these important differences:

- `--auth-token` is gone.
- Headless `t3 serve` prints a one-time pairing token and `/pair#token=...` URL.
- HTTP auth uses browser sessions, bearer access tokens, DPoP tokens, and WebSocket tickets.
- WebSocket auth uses `wsTicket`, not the old `token` query parameter.
- `orchestration.getSnapshot` is gone.
- The read side moved to:
  - `orchestration.subscribeShell`
  - `orchestration.subscribeThread`
  - `orchestration.getArchivedShellSnapshot`

The write command names used by T3 Pebble are still broadly recognizable:

- `orchestration.dispatchCommand`
- `thread.turn.start`
- `thread.turn.interrupt`
- `thread.approval.respond`
- `thread.user-input.respond`

## Why T3 Code Was Not Patched

The goal is to keep T3 Code vanilla and put Pebble-specific behavior in the Pebble app. That avoids maintaining a server fork for watch support.

The only reason the fork exists is to:

- pin the last known T3 Code commit that supports the current Pebble app;
- publish a compatibility branch for repeatable setup;
- document the Pebble integration and migration requirements.

## Migration Needed For Current Upstream

To run against current upstream T3 Code, T3 Pebble needs these app-side changes:

1. Accept a T3 pairing URL or pairing token in settings.
2. Exchange the one-time pairing credential for an access token.
3. Request a WebSocket ticket before opening each WebSocket RPC session.
4. Connect to `/ws?wsTicket=<ticket>`.
5. Replace `orchestration.getSnapshot` with `orchestration.subscribeShell`.
6. Use `orchestration.subscribeThread` for richer per-thread detail updates.
7. Keep `orchestration.dispatchCommand` for write operations where the current command schemas still match.

The optional legacy `?token=` mode can remain as a transition path while old compatible T3 Code servers are still running.
