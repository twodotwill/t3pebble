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
  return { context, listeners, sentMessages }
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

  bridge.listeners.appmessage({ payload: { cmd: 7, session_id: "ses_done", index: 2, request_id: "ctx_a" } })
  const contextMessage = await waitFor(() => bridge.sentMessages.find((message) => message.cmd === 7))
  assert.strictEqual(contextMessage.request_id, "ctx_a")
  assert.match(contextMessage.context, /You\nWrite the install notes/)
  assert.match(contextMessage.context, /Agent\nInstall notes are drafted/)
  assert.strictEqual(contextMessage.context_page, 0)
  assert.ok(Buffer.byteLength(contextMessage.context, "utf8") <= 480)

  const doneThread = state.snapshot.threads.find((thread) => thread.id === "ses_done")
  doneThread.messages = []
  for (let i = 0; i < 12; i++) {
    doneThread.messages.push({
      id: `msg_long_user_${i}`,
      role: "user",
      text: `Long prompt ${i} ` + "neon genesis 🚀 ".repeat(25),
      streaming: false,
      createdAt: `2026-01-01T00:${String(i).padStart(2, "0")}:01.000Z`,
      updatedAt: `2026-01-01T00:${String(i).padStart(2, "0")}:01.000Z`,
    })
    doneThread.messages.push({
      id: `msg_long_agent_${i}`,
      role: "assistant",
      text: `Long answer ${i} ` + "cyber signal ".repeat(30),
      streaming: false,
      createdAt: `2026-01-01T00:${String(i).padStart(2, "0")}:02.000Z`,
      updatedAt: `2026-01-01T00:${String(i).padStart(2, "0")}:02.000Z`,
    })
  }
  doneThread.messages.push({
    id: "msg_final_long_agent",
    role: "assistant",
    text: "Final assistant response " + "keeps flowing through pagination ".repeat(45) + "FINAL_CONTEXT_SENTINEL",
    streaming: false,
    createdAt: "2026-01-01T00:20:02.000Z",
    updatedAt: "2026-01-01T00:20:02.000Z",
  })
  bridge.context.lastSnapshot = state.snapshot

  const contextCount = () => bridge.sentMessages.filter((message) => message.cmd === 7).length
  const beforePage0 = contextCount()
  bridge.listeners.appmessage({ payload: { cmd: 7, session_id: "ses_done", index: 2, context_page: 0, request_id: "ctx_p0" } })
  const page0 = await waitFor(() => {
    const messages = bridge.sentMessages.filter((message) => message.cmd === 7)
    return messages.length > beforePage0 && messages[messages.length - 1]
  })
  assert.strictEqual(page0.context_page, 0)
  assert.strictEqual(page0.request_id, "ctx_p0")
  assert.ok(page0.total > 1)
  assert.ok(Buffer.byteLength(page0.context, "utf8") <= 480)

  const beforePage1 = contextCount()
  bridge.listeners.appmessage({ payload: { cmd: 7, session_id: "ses_done", index: 2, context_page: 1, request_id: "ctx_p1" } })
  const page1 = await waitFor(() => {
    const messages = bridge.sentMessages.filter((message) => message.cmd === 7)
    return messages.length > beforePage1 && messages[messages.length - 1]
  })
  assert.strictEqual(page1.context_page, 1)
  assert.strictEqual(page1.request_id, "ctx_p1")
  assert.notStrictEqual(page1.context, page0.context)
  assert.ok(Buffer.byteLength(page1.context, "utf8") <= 480)

  const beforeLast = contextCount()
  bridge.listeners.appmessage({ payload: { cmd: 7, session_id: "ses_done", index: 2, context_page: 999, request_id: "ctx_last" } })
  const lastPage = await waitFor(() => {
    const messages = bridge.sentMessages.filter((message) => message.cmd === 7)
    return messages.length > beforeLast && messages[messages.length - 1]
  })
  assert.strictEqual(lastPage.context_page, lastPage.total - 1)
  assert.strictEqual(lastPage.request_id, "ctx_last")
  assert.ok(Buffer.byteLength(lastPage.context, "utf8") <= 480)

  const beforeTail = contextCount()
  bridge.listeners.appmessage({ payload: { cmd: 7, session_id: "ses_done", index: 2, context_page: -1, request_id: "ctx_tail" } })
  const tailPage = await waitFor(() => {
    const messages = bridge.sentMessages.filter((message) => message.cmd === 7)
    return messages.length > beforeTail && messages[messages.length - 1]
  })
  assert.strictEqual(tailPage.context_page, tailPage.total - 1)
  assert.strictEqual(tailPage.request_id, "ctx_tail")
  assert.strictEqual(tailPage.context, lastPage.context)
  assert.ok(!tailPage.context.includes("..."))
  assert.match(tailPage.context, /FINAL_CONTEXT_SENTINEL/)

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
