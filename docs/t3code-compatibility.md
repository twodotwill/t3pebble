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

- `project.create`
- `thread.create`
- `thread.turn.start`
- `thread.turn.interrupt`
- `thread.settle` / `thread.unsettle`
- `thread.approval.respond`
- `thread.user-input.respond`

`thread.unsettle` requires `reason: "user"`. Settling sets `settledAt` and
`settledOverride: "settled"`; unsettling clears `settledAt` and sets
`settledOverride: "active"`.

`project.create` honours `createWorkspaceRootIfMissing`, and the REST
normalizer resolves `workspaceRoot` before dispatch, so the watch can create a
project and its directory without an agent involved.

A malformed command is rejected with `400 invalid_request`.

#### Starting a thread takes two commands, not one

`thread.turn.start` accepts a `bootstrap.createThread` block that is supposed to
create the thread and start the first turn together. **Do not use it over REST.**
The REST handler validates the block and then ignores it: it calls
`orchestrationEngine.dispatch` directly, bypassing the `dispatchNormalizedCommand`
router that is the only thing routing a bootstrap command into the branch which
creates the thread. That router is wired into the WebSocket RPC path alone.

The visible symptom is a `500 orchestration_dispatch_failed`, because the turn
lands on a thread that does not exist yet:

```json
{"_tag":"OrchestrationCommandInvariantError","commandType":"thread.turn.start",
 "detail":"Thread '...' does not exist for command 'thread.turn.start'."}
```

Note that an invalid field inside the block still returns `400`, so the block
being accepted proves nothing about it being acted on.

So the bridge dispatches `thread.create` first and then `thread.turn.start`
against that thread id, carrying no bootstrap. Thread ids do not have to be
UUIDs; the bridge's own `pebble-thread-...` format is accepted.

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
