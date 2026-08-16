const assert = require("assert")
const fs = require("fs")
const path = require("path")
const vm = require("vm")

const source = fs.readFileSync(path.join(__dirname, "..", "src", "pkjs", "index.js"), "utf8")
const BUILD_LABEL = /var BUILD_LABEL = "([^"]+)"/.exec(source)[1]

const DAY_MS = 24 * 60 * 60 * 1000
const NOW = Date.now()
const iso = (msAgo) => new Date(NOW - msAgo).toISOString()

const CMD = {
  refresh: 1, sessionItem: 2, sessionEnd: 3, detail: 4, prompt: 5, error: 6,
  context: 7, status: 8, projectItem: 9, projectEnd: 10, newThread: 11,
  hostItem: 12, hostEnd: 13, selectHost: 14,
}

const KEY = {
  cmd: "cmd", index: "index", sessionId: "session_id", projectId: "project_id",
  prompt: "prompt", requestId: "request_id", requestKind: "request_kind",
  contextPage: "context_page", hostId: "host_id",
}

function baseThread(overrides = {}) {
  return {
    projectId: "proj_pebble",
    modelSelection: { instanceId: "codex", model: "gpt-5.6-sol" },
    runtimeMode: "full-access",
    interactionMode: "default",
    branch: null,
    worktreePath: null,
    createdAt: iso(10 * DAY_MS),
    updatedAt: iso(58000),
    archivedAt: null,
    settledOverride: null,
    settledAt: null,
    snoozedUntil: null,
    snoozedAt: null,
    latestUserMessageAt: iso(60000),
    hasPendingApprovals: false,
    hasPendingUserInput: false,
    backgroundLiveness: null,
    planProgress: null,
    latestTurn: { state: "completed", requestedAt: iso(60000), startedAt: iso(59000), completedAt: iso(58000) },
    session: { status: "ready", providerName: "codex", updatedAt: iso(58000) },
    __activities: [],
    __messages: [],
    ...overrides,
  }
}

function createShell() {
  return {
    snapshotSequence: 1,
    updatedAt: iso(58000),
    projects: [
      {
        id: "proj_pebble",
        title: "Pebble",
        workspaceRoot: "/repo/pebblecode",
        defaultModelSelection: { instanceId: "codex", model: "gpt-5-codex" },
        createdAt: iso(10 * DAY_MS),
        updatedAt: iso(10 * DAY_MS),
      },
    ],
    threads: [
      baseThread({
        id: "ses_running",
        title: "Pebble Watch",
        session: { status: "running", providerName: "codex", updatedAt: iso(1000) },
        latestTurn: { state: "running", requestedAt: iso(5000), startedAt: iso(4000), completedAt: null },
        __messages: [
          { id: "m1", role: "assistant", text: "Still compiling.", streaming: true, createdAt: iso(3000), updatedAt: iso(2000) },
        ],
      }),
      baseThread({
        id: "ses_blocked",
        title: "API Bridge",
        hasPendingApprovals: true,
        __activities: [
          {
            kind: "approval.requested",
            sequence: 1,
            createdAt: iso(30000),
            payload: { requestId: "per_1", requestKind: "file-change", detail: "src/pkjs/index.js" },
          },
        ],
        __messages: [
          { id: "m2", role: "assistant", text: "I need permission to edit the bridge.", streaming: false, createdAt: iso(40000), updatedAt: iso(35000) },
        ],
      }),
      baseThread({
        id: "ses_settled",
        title: "Docs",
        settledOverride: "settled",
        settledAt: iso(2 * DAY_MS),
        __messages: [
          { id: "m3", role: "user", text: "Write the install notes.", streaming: false, createdAt: iso(3 * DAY_MS), updatedAt: iso(3 * DAY_MS) },
          { id: "m4", role: "assistant", text: "Install notes are drafted. They include Tailscale setup.", streaming: false, createdAt: iso(2 * DAY_MS), updatedAt: iso(2 * DAY_MS) },
        ],
      }),
    ],
  }
}

function createBridge(baseUrl, state, buildLabel) {
  let storedSettings = JSON.stringify({
    servers: [{ id: "s1", label: "beta1", baseUrl, token: "t3-access-token" }],
    nextServerId: 2,
  })
  const storedValues = { t3pebble_build_label: buildLabel }
  const listeners = {}
  const sentMessages = []

  function routeRequest(method, url, body, headers) {
    const [pathname, query] = url.replace(/^https?:\/\/[^/]+/, "").split("?")
    state.requests.push({ method, url, pathname, query, headers })

    if (method === "POST" && pathname === "/api/orchestration/dispatch") {
      state.dispatches.push(JSON.parse(body))
      return { status: 200, body: JSON.stringify({ sequence: 1 }) }
    }

    if (method === "GET" && pathname === "/api/orchestration/shell") {
      return {
        status: 200,
        body: JSON.stringify({
          ...state.shell,
          threads: state.shell.threads.map(({ __activities, __messages, ...rest }) => rest),
        }),
      }
    }

    if (method === "GET" && pathname.startsWith("/api/orchestration/threads/")) {
      const threadId = decodeURIComponent(pathname.slice("/api/orchestration/threads/".length))
      assert.ok(!threadId.includes("::"), "server must receive a plain thread id, got " + threadId)
      const thread = state.shell.threads.find((candidate) => candidate.id === threadId)
      if (!thread) {
        return {
          status: 404,
          body: JSON.stringify({ _tag: "EnvironmentResourceNotFoundError", code: "not_found", reason: "thread_not_found" }),
        }
      }
      // The detail route carries bodies but NOT the shell-only lifecycle
      // fields, so strip them here the way the real server does.
      const { __activities, __messages, hasPendingApprovals, hasPendingUserInput,
              latestUserMessageAt, backgroundLiveness, planProgress, ...rest } = thread
      return {
        status: 200,
        body: JSON.stringify({
          snapshotSequence: state.shell.snapshotSequence,
          thread: { ...rest, activities: __activities, messages: __messages, checkpoints: [], proposedPlans: [] },
        }),
      }
    }

    return { status: 404, body: JSON.stringify({ _tag: "RouteNotFound" }) }
  }

  class FakeXMLHttpRequest {
    constructor() {
      this.readyState = 0
      this.status = 0
      this.responseText = ""
      this.headers = {}
    }
    open(method, url) {
      this.method = method
      this.url = url
      this.readyState = 1
    }
    setRequestHeader(name, value) {
      this.headers[String(name).toLowerCase()] = value
    }
    send(body) {
      const response = routeRequest(this.method, this.url, body, this.headers)
      setTimeout(() => {
        this.readyState = 4
        this.status = response.status
        this.responseText = response.body
        if (this.onreadystatechange) this.onreadystatechange()
      }, 0)
    }
    abort() {}
  }

  const context = {
    console,
    setTimeout,
    clearTimeout,
    XMLHttpRequest: FakeXMLHttpRequest,
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

function waitFor(predicate, label) {
  return new Promise((resolve, reject) => {
    const started = Date.now()
    function tick() {
      const value = predicate()
      if (value) {
        resolve(value)
        return
      }
      if (Date.now() - started > 5000) {
        reject(new Error("timed out waiting for " + (label || "bridge event")))
        return
      }
      setTimeout(tick, 10)
    }
    tick()
  })
}

async function main() {
  const state = { shell: createShell(), requests: [], dispatches: [] }
  const bridge = createBridge("http://127.0.0.1:3773", state, BUILD_LABEL)

  // --- the watch boots onto the host list -------------------------------
  bridge.listeners.ready()
  await waitFor(() => bridge.sentMessages.find((m) => m.cmd === CMD.hostEnd), "host end")

  const shellRequest = state.requests[0]
  assert.strictEqual(shellRequest.pathname, "/api/orchestration/shell")
  assert.strictEqual(shellRequest.headers.authorization, "Bearer t3-access-token")

  const host = bridge.sentMessages.find((m) => m.cmd === CMD.hostItem)
  assert.strictEqual(host.host_id, "s1")
  assert.strictEqual(host.title, "beta1")
  // A pending approval outranks everything else on the machine.
  assert.strictEqual(host.state, "needs")
  assert.strictEqual(host.detail, "1 need you")

  // The host screen costs exactly one request and hydrates nothing.
  assert.strictEqual(state.requests.length, 1)

  // --- picking a host lists its threads ---------------------------------
  bridge.listeners.appmessage({ payload: { [KEY.cmd]: CMD.selectHost, [KEY.hostId]: "s1" } })
  await waitFor(() => bridge.sentMessages.find((m) => m.cmd === CMD.projectEnd), "project end")

  const rows = {}
  for (const message of bridge.sentMessages.filter((m) => m.cmd === CMD.sessionItem)) {
    rows[message.session_id] = message
  }
  assert.strictEqual(Object.keys(rows).length, 3)
  assert.strictEqual(rows["s1::ses_running"].state, "run")
  assert.strictEqual(rows["s1::ses_running"].detail, "running")
  assert.strictEqual(rows["s1::ses_blocked"].state, "needs")
  assert.strictEqual(rows["s1::ses_blocked"].detail, "needs approval")
  assert.strictEqual(rows["s1::ses_settled"].state, "settled")
  assert.match(rows["s1::ses_settled"].detail, /^settled /)

  const projectRow = bridge.sentMessages.find((m) => m.cmd === CMD.projectItem)
  assert.strictEqual(projectRow.project_id, "s1::proj_pebble")

  // The thread list is served from the cached shell: still no body fetches.
  assert.strictEqual(state.requests.filter((r) => r.pathname.startsWith("/api/orchestration/threads/")).length, 0)

  // --- opening a thread is the first body fetch -------------------------
  bridge.listeners.appmessage({ payload: { [KEY.cmd]: CMD.detail, [KEY.sessionId]: "s1::ses_blocked", [KEY.index]: 1 } })
  const detail = await waitFor(() => bridge.sentMessages.find((m) => m.cmd === CMD.detail), "detail")
  assert.strictEqual(detail.state, "needs")
  assert.strictEqual(detail.request_id, "per_1")
  assert.strictEqual(detail.request_kind, "permission")
  assert.match(detail.summary, /Needs permission: file-change on src\/pkjs\/index\.js/)
  assert.strictEqual(state.requests.filter((r) => r.pathname === "/api/orchestration/threads/ses_blocked").length, 1)

  // --- transcript paging ------------------------------------------------
  const longThread = state.shell.threads.find((t) => t.id === "ses_settled")
  longThread.__messages = []
  for (let i = 0; i < 12; i++) {
    longThread.__messages.push({
      id: `u${i}`, role: "user", streaming: false,
      text: `Long prompt ${i} ` + "neon genesis 🚀 ".repeat(25),
      createdAt: iso((30 - i) * 60000), updatedAt: iso((30 - i) * 60000),
    })
    longThread.__messages.push({
      id: `a${i}`, role: "assistant", streaming: false,
      text: `Long answer ${i} ` + "cyber signal ".repeat(30),
      createdAt: iso((30 - i) * 60000 - 1000), updatedAt: iso((30 - i) * 60000 - 1000),
    })
  }
  longThread.__messages.push({
    id: "final", role: "assistant", streaming: false,
    text: "Final response " + "keeps flowing through pagination ".repeat(45) + "FINAL_CONTEXT_SENTINEL",
    createdAt: iso(1000), updatedAt: iso(1000),
  })

  const contextCount = () => bridge.sentMessages.filter((m) => m.cmd === CMD.context).length
  const before0 = contextCount()
  bridge.listeners.appmessage({
    payload: { [KEY.cmd]: CMD.context, [KEY.sessionId]: "s1::ses_settled", [KEY.index]: 2, [KEY.contextPage]: 0, [KEY.requestId]: "c0" },
  })
  const page0 = await waitFor(() => {
    const all = bridge.sentMessages.filter((m) => m.cmd === CMD.context)
    return all.length > before0 && all[all.length - 1]
  }, "context page 0")
  assert.strictEqual(page0.context_page, 0)
  assert.strictEqual(page0.request_id, "c0")
  assert.ok(page0.total > 1)
  assert.ok(Buffer.byteLength(page0.context, "utf8") <= 480)

  const before1 = contextCount()
  bridge.listeners.appmessage({
    payload: { [KEY.cmd]: CMD.context, [KEY.sessionId]: "s1::ses_settled", [KEY.index]: 2, [KEY.contextPage]: 1, [KEY.requestId]: "c1" },
  })
  const page1 = await waitFor(() => {
    const all = bridge.sentMessages.filter((m) => m.cmd === CMD.context)
    return all.length > before1 && all[all.length - 1]
  }, "context page 1")
  assert.strictEqual(page1.context_page, 1)
  assert.notStrictEqual(page1.context, page0.context)

  const beforeTail = contextCount()
  bridge.listeners.appmessage({
    payload: { [KEY.cmd]: CMD.context, [KEY.sessionId]: "s1::ses_settled", [KEY.index]: 2, [KEY.contextPage]: -1, [KEY.requestId]: "ct" },
  })
  const tail = await waitFor(() => {
    const all = bridge.sentMessages.filter((m) => m.cmd === CMD.context)
    return all.length > beforeTail && all[all.length - 1]
  }, "context tail")
  assert.strictEqual(tail.context_page, tail.total - 1)
  assert.match(tail.context, /FINAL_CONTEXT_SENTINEL/)
  assert.ok(!tail.context.includes("..."))

  // --- replies route back to the owning host ----------------------------
  bridge.listeners.appmessage({
    payload: {
      [KEY.cmd]: CMD.prompt, [KEY.sessionId]: "s1::ses_blocked",
      [KEY.requestId]: "per_1", [KEY.requestKind]: "permission", [KEY.prompt]: "yes",
    },
  })
  await waitFor(() => state.dispatches.length === 1, "approval dispatch")
  assert.strictEqual(state.dispatches[0].type, "thread.approval.respond")
  assert.strictEqual(state.dispatches[0].threadId, "ses_blocked")
  assert.strictEqual(state.dispatches[0].decision, "accept")

  bridge.listeners.appmessage({
    payload: { [KEY.cmd]: CMD.prompt, [KEY.sessionId]: "s1::ses_settled", [KEY.prompt]: "continue" },
  })
  await waitFor(() => state.dispatches.length === 2, "turn dispatch")
  assert.strictEqual(state.dispatches[1].type, "thread.turn.start")
  assert.strictEqual(state.dispatches[1].threadId, "ses_settled")
  assert.match(state.dispatches[1].message.text, /sent from the user's Pebble watch through t3pebble/)
  // The watch never imposes a model on an existing thread.
  assert.strictEqual(state.dispatches[1].modelSelection, undefined)

  // --- a new thread uses the project's server-side default --------------
  bridge.listeners.appmessage({
    payload: { [KEY.cmd]: CMD.newThread, [KEY.projectId]: "s1::proj_pebble", [KEY.prompt]: "start fresh" },
  })
  await waitFor(() => state.dispatches.length === 3, "new thread dispatch")
  assert.strictEqual(state.dispatches[2].bootstrap.createThread.projectId, "proj_pebble")
  assert.strictEqual(state.dispatches[2].modelSelection.model, "gpt-5-codex")

  assert.deepStrictEqual(
    bridge.sentMessages.filter((m) => m.cmd === CMD.error).map((m) => m.error),
    [],
    "bridge reported errors",
  )
}

main()
  .then(() => {
    console.log("phone bridge integration tests passed")
  })
  .catch((error) => {
    console.error(error)
    process.exit(1)
  })
