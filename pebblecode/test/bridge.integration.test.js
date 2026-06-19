const assert = require("assert")
const fs = require("fs")
const path = require("path")
const vm = require("vm")

const source = fs.readFileSync(path.join(__dirname, "..", "src", "pkjs", "index.js"), "utf8")

function createSnapshot() {
  return {
    projects: [
      {
        id: "proj_pebble",
        title: "Pebble",
        workspaceRoot: "/repo/pebblecode",
        defaultModelSelection: { provider: "codex", model: "gpt-5-codex" },
        createdAt: "2026-01-01T00:00:00.000Z",
        updatedAt: "2026-01-01T00:00:00.000Z",
      },
    ],
    threads: [
      {
        id: "ses_running",
        projectId: "proj_pebble",
        title: "Pebble Watch",
        modelSelection: { provider: "codex", model: "gpt-5-codex" },
        runtimeMode: "full-access",
        interactionMode: "default",
        session: { status: "running", providerName: "codex" },
        latestTurn: { state: "running" },
        activities: [],
        messages: [
          {
            id: "msg_running",
            role: "assistant",
            text: "Still compiling the watch app.",
            streaming: true,
            createdAt: "2026-01-01T00:00:01.000Z",
            updatedAt: "2026-01-01T00:00:02.000Z",
          },
        ],
        createdAt: "2026-01-01T00:00:00.000Z",
        updatedAt: "2026-01-01T00:00:06.000Z",
      },
      {
        id: "ses_blocked",
        projectId: "proj_pebble",
        title: "API Bridge",
        modelSelection: { provider: "codex", model: "gpt-5-codex" },
        runtimeMode: "full-access",
        interactionMode: "default",
        session: { status: "idle", providerName: "codex" },
        latestTurn: { state: "completed" },
        activities: [
          {
            kind: "approval.requested",
            sequence: 1,
            createdAt: "2026-01-01T00:00:03.000Z",
            payload: {
              requestId: "per_1",
              requestKind: "file-change",
              detail: "src/pkjs/index.js",
            },
          },
        ],
        messages: [
          {
            id: "msg_blocked",
            role: "assistant",
            text: "I need permission to edit the bridge.",
            streaming: false,
            createdAt: "2026-01-01T00:00:04.000Z",
            updatedAt: "2026-01-01T00:00:05.000Z",
          },
        ],
        createdAt: "2026-01-01T00:00:00.000Z",
        updatedAt: "2026-01-01T00:00:05.000Z",
      },
      {
        id: "ses_done",
        projectId: "proj_pebble",
        title: "Docs",
        modelSelection: { provider: "codex", model: "gpt-5-codex" },
        runtimeMode: "full-access",
        interactionMode: "default",
        session: { status: "idle", providerName: "codex" },
        latestTurn: { state: "completed" },
        activities: [],
        messages: [
          {
            id: "msg_user",
            role: "user",
            text: "Write the install notes.",
            streaming: false,
            createdAt: "2026-01-01T00:00:01.000Z",
            updatedAt: "2026-01-01T00:00:01.000Z",
          },
          {
            id: "msg_done",
            role: "assistant",
            text: "Install notes are drafted. They include Tailscale setup.",
            streaming: false,
            createdAt: "2026-01-01T00:00:02.000Z",
            updatedAt: "2026-01-01T00:00:03.000Z",
          },
        ],
        createdAt: "2026-01-01T00:00:00.000Z",
        updatedAt: "2026-01-01T00:00:04.000Z",
      },
    ],
  }
}

function createBridge(baseUrl, state) {
  let storedSettings = JSON.stringify({ baseUrl, username: "t3code", password: "secret" })
  const storedValues = { t3pebble_build_label: "v0.1.0" }
  const listeners = {}
  const sentMessages = []

  class FakeWebSocket {
    constructor(url) {
      this.url = url
      state.socketUrls.push(url)
      setTimeout(() => {
        if (this.onopen) this.onopen()
      }, 0)
    }

    send(raw) {
      const request = JSON.parse(raw)
      state.rpcRequests.push(request)
      if (request.tag === "orchestration.dispatchCommand") {
        state.dispatches.push(request.payload)
      }
      const value = request.tag === "orchestration.getSnapshot" ? state.snapshot : true
      const event = {
        data: JSON.stringify({
          _tag: "Exit",
          requestId: request.id,
          exit: { _tag: "Success", value },
        }),
      }
      setTimeout(() => {
        if (this.onmessage) this.onmessage(event)
      }, 0)
    }

    close() {}
    addEventListener() {}
  }

  const context = {
    console,
    setTimeout,
    clearTimeout,
    WebSocket: FakeWebSocket,
    localStorage: {
      getItem(key) {
        if (key === "t3pebble_settings") return storedSettings
        return storedValues[key] || null
      },
      setItem(key, value) {
        if (key === "t3pebble_settings") {
          storedSettings = value
          return
        }
        storedValues[key] = value
      },
    },
    Pebble: {
      addEventListener(name, handler) {
        listeners[name] = handler
      },
      openURL() {},
      sendAppMessage(message, success) {
        sentMessages.push(message)
        if (success) success()
      },
    },
    URL,
  }
  vm.createContext(context)
  vm.runInContext(source, context)
  return { listeners, sentMessages }
}

function waitFor(predicate) {
  return new Promise((resolve, reject) => {
    const started = Date.now()
    function tick() {
      const value = predicate()
      if (value) {
        resolve(value)
        return
      }
      if (Date.now() - started > 3000) {
        reject(new Error("timed out waiting for bridge event"))
        return
      }
      setTimeout(tick, 10)
    }
    tick()
  })
}

async function main() {
  const state = {
    snapshot: createSnapshot(),
    socketUrls: [],
    rpcRequests: [],
    dispatches: [],
  }
  const bridge = createBridge("http://127.0.0.1:3773", state)
  bridge.listeners.ready()

  await waitFor(() => bridge.sentMessages.find((message) => message.cmd === 3))
  assert.strictEqual(state.socketUrls[0], "ws://127.0.0.1:3773/ws?token=secret")
  assert.ok(bridge.sentMessages.find((message) => message.session_id === "ses_running"))
  assert.ok(bridge.sentMessages.find((message) => message.cmd === 9 && message.project_id === "proj_pebble"))

  await waitFor(() =>
    bridge.sentMessages.find((message) => message.session_id === "ses_blocked" && message.status === "Needs input"),
  )

  const bySession = (sessionID) => {
    const matches = bridge.sentMessages.filter((message) => message.session_id === sessionID)
    return matches[matches.length - 1]
  }
  const running = bySession("ses_running")
  const blocked = bySession("ses_blocked")
  const done = bySession("ses_done")

  assert.strictEqual(running.status, "Running")
  assert.strictEqual(blocked.status, "Needs input")
  assert.match(blocked.summary, /Needs permission: file-change on src\/pkjs\/index\.js/)
  assert.strictEqual(blocked.request_id, "per_1")
  assert.strictEqual(done.status, "Done")

  bridge.listeners.appmessage({ payload: { cmd: 7, session_id: "ses_done", index: 2 } })
  const contextMessage = await waitFor(() => bridge.sentMessages.find((message) => message.cmd === 7))
  assert.match(contextMessage.context, /You: Write the install notes/)
  assert.match(contextMessage.context, /Agent: Install notes are drafted/)

  bridge.listeners.appmessage({
    payload: {
      cmd: 5,
      session_id: "ses_blocked",
      request_id: "per_1",
      request_kind: "permission",
      prompt: "yes",
    },
  })
  await waitFor(() => state.dispatches.length === 1)
  assert.strictEqual(state.dispatches[0].type, "thread.approval.respond")
  assert.strictEqual(state.dispatches[0].decision, "accept")

  bridge.listeners.appmessage({ payload: { cmd: 5, session_id: "ses_done", prompt: "continue" } })
  await waitFor(() => state.dispatches.length === 2)
  assert.strictEqual(state.dispatches[1].type, "thread.turn.start")
  assert.match(state.dispatches[1].message.text, /sent from the user's Pebble watch through t3pebble/)
  assert.match(state.dispatches[1].message.text, /last five sentences/)
  assert.strictEqual(state.dispatches[1].modelSelection.model, "gpt-5.5")
}

main()
  .then(() => {
    console.log("phone bridge integration tests passed")
  })
  .catch((error) => {
    console.error(error)
    process.exit(1)
  })
