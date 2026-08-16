# T3 Code Compatibility

This document records the T3 Code API surface T3 Pebble depends on.

## Target

Stock, unmodified T3 Code. The published `t3` CLI, installed with `npm install -g t3` or run through `npx t3@latest`.

Verified against `t3@0.0.33` and upstream `main` at `277322933` (`v0.0.34-nightly.20260816`).

There is no compatibility branch and no server patch. The earlier fork — which re-added `--auth-token`, legacy `?token=` WebSocket auth, and an `orchestration.getSnapshot` RPC — is no longer used.

## Required API Surface

### Auth

A bearer access token issued by the CLI:

```sh
t3 auth session issue --ttl 365d --label "T3 Pebble watch" --token-only
```

That grants `AuthAdministrativeScopes`, which includes the two scopes the app needs: `orchestration:read` and `orchestration:operate`. The token must be issued against the same data directory the server runs with (`--base-dir`, or the default).

Every request carries it as `Authorization: Bearer <token>`. There is no pairing exchange, no WebSocket ticket, and no session cookie involved.

### Reads

| Route | Used for |
| --- | --- |
| `GET /api/orchestration/snapshot` | Projects and thread metadata |
| `GET /api/orchestration/threads/:threadId?turnLimit=N` | Thread bodies: messages, activities, session |

The snapshot route serves the *command read model*: every thread arrives with `messages`, `activities`, and `checkpoints` empty, but with `session` and `latestTurn` populated. That is enough for a first paint of the list, and the app then hydrates each thread over the thread detail route.

Turn windows requested by the app:

- `turnLimit=6` for list rows and the detail card
- `turnLimit=40` for the transcript view

The thread detail response is `{ snapshotSequence, thread, page }`.

### Writes

`POST /api/orchestration/dispatch`, carrying a `ClientOrchestrationCommand`:

- `thread.turn.start` (including the `bootstrap.createThread` form used to start a thread in a project)
- `thread.turn.interrupt`
- `thread.approval.respond`
- `thread.user-input.respond`

A malformed command is rejected with `400 invalid_request`.

### Error shapes

Errors are tagged JSON, for example:

```json
{"_tag":"EnvironmentAuthInvalidError","code":"auth_invalid","reason":"missing_credential","traceId":"..."}
```

The bridge reads `reason` / `requiredScope` for its status line. Relevant statuses: `401` (bad or missing token), `403` (missing scope), `404` with `reason: "thread_not_found"`.

## Notes

- `access-control-allow-origin` is `*`, so the PebbleKit JS sandbox can call the API directly.
- The WebSocket RPC endpoint still exists on mainline, but the app no longer uses it. Its read side is subscription-based (`orchestration.subscribeShell`, `orchestration.subscribeThread`), which is a poor fit for a watch that polls; the REST routes carry the same data in one request each.
- `t3 serve` supports `--host`, `--port`, `--base-dir`, and `--no-browser`, which is all the launch script needs. Mainline also ships `--tailscale-serve` if you would rather it manage Tailscale itself.
