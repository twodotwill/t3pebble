const assert = require("assert")
const fs = require("fs")
const path = require("path")
const vm = require("vm")

const source = fs.readFileSync(path.join(__dirname, "..", "src", "pkjs", "index.js"), "utf8")

let storedSettings = null
let storedValues = {}
let sentMessages = []
let sockets = []
let dispatches = []
let snapshot = null

function makeSnapshot(overrides = {}) {
  return {
    projects: [
      {
        id: "proj_1",
        title: "Pebble",
        workspaceRoot: "/repo/pebblecode",
        defaultModelSelection: { provider: "codex", model: "gpt-5-codex" },
        createdAt: "2026-01-01T00:00:00.000Z",
        updatedAt: "2026-01-01T00:00:00.000Z",
      },
    ],
    threads: [
      {
        id: "ses_1",
        projectId: "proj_1",
        title: "Pebble client",
        modelSelection: { provider: "codex", model: "gpt-5-codex" },
        runtimeMode: "full-access",
        interactionMode: "default",
        session: { status: "running", providerName: "codex" },
        latestTurn: { state: "running" },
        activities: [],
        messages: [
          {
            id: "msg_1",
            role: "assistant",
            text: "Built the app. It is ready.",
            streaming: false,
            createdAt: "2026-01-01T00:00:01.000Z",
            updatedAt: "2026-01-01T00:00:02.000Z",
          },
        ],
        createdAt: "2026-01-01T00:00:00.000Z",
        updatedAt: "2026-01-01T00:00:03.000Z",
      },
    ],
    ...overrides,
  }
}

class FakeWebSocket {
  constructor(url) {
    this.url = url
    this.sent = []
    sockets.push(this)
    setTimeout(() => {
      if (this.onopen) this.onopen()
    }, 0)
  }

  send(raw) {
    this.sent.push(raw)
    const request = JSON.parse(raw)
    if (request.tag === "orchestration.getSnapshot") {
      this.respond(request.id, snapshot)
      return
    }
    if (request.tag === "orchestration.dispatchCommand") {
      dispatches.push(request.payload)
      this.respond(request.id, true)
      return
    }
    this.respond(request.id, null)
  }

  close() {}

  addEventListener(type, handler) {
    if (type === "message") this.messageListener = handler
  }

  respond(requestId, value) {
    const event = {
      data: JSON.stringify({
        _tag: "Exit",
        requestId,
        exit: { _tag: "Success", value },
      }),
    }
    setTimeout(() => {
      if (this.onmessage) this.onmessage(event)
      if (this.messageListener) this.messageListener(event)
    }, 0)
  }
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
    removeItem(key) {
      if (key === "t3pebble_settings") {
        storedSettings = null
        return
      }
      delete storedValues[key]
    },
    clear() {
      storedSettings = null
      storedValues = {}
    },
  },
  Pebble: {
    addEventListener() {},
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

function assertJsonEqual(actual, expected) {
  assert.strictEqual(JSON.stringify(actual), JSON.stringify(expected))
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
        reject(new Error("timed out waiting for test event"))
        return
      }
      setTimeout(tick, 10)
    }
    tick()
  })
}

async function main() {
  assertJsonEqual(context.settings(), {
    baseUrl: "",
    username: "t3code",
    password: "",
  })

  assert.match(context.configurationHtml(), /Base URL/)
  assert.match(context.configurationHtml(), /Auth token/)
  assert.doesNotMatch(context.configurationHtml(), /Username/)

  storedSettings = JSON.stringify({
    baseUrl: "http://old-host:4096",
    username: "bad",
    password: "bad",
  })
  context.localStorage.setItem("t3pebble_build_label", "v0.1.0")
  assertJsonEqual(context.settings(), {
    baseUrl: "http://old-host:4096",
    username: "bad",
    password: "bad",
  })

  storedSettings = JSON.stringify({
    baseUrl: "http://wills-macbook-pro-5:4096",
    username: "",
    password: "",
  })
  assertJsonEqual(context.settings(), {
    baseUrl: "",
    username: "t3code",
    password: "",
  })

  assertJsonEqual(
    context.buildQuestionAnswers(
      {
        questions: [
          {
            question: "Deploy now?",
            options: [
              { label: "Deploy", description: "Deploy now" },
              { label: "Skip", description: "Do not deploy" },
            ],
            custom: false,
          },
        ],
      },
      "deploy please",
    ),
    [["Deploy"]],
  )

  assertJsonEqual(
    context.buildQuestionAnswers(
      {
        questions: [
          {
            question: "Which checks?",
            options: [
              { label: "Tests", description: "Run tests" },
              { label: "Lint", description: "Run lint" },
            ],
            multiple: true,
            custom: false,
          },
        ],
      },
      "tests and lint",
    ),
    [["Tests", "Lint"]],
  )

  assert.strictEqual(
    context.buildQuestionAnswers(
      {
        questions: [
          { question: "First?", options: [], custom: true },
          { question: "Second?", options: [], custom: true },
        ],
      },
      "one answer only",
    ),
    null,
  )

  assert.match(
    context.pendingSummary({
      kind: "permission",
      request: { permission: "edit", patterns: ["src/pkjs/index.js"] },
    }),
    /Needs permission: edit on src\/pkjs\/index\.js/,
  )

  assert.match(
    context.pendingSummary({
      kind: "question",
      request: {
        questions: [
          {
            question: "Continue?",
            options: [{ label: "Yes", description: "Continue" }],
          },
        ],
      },
    }),
    /Question: Continue\?/,
  )

  assert.strictEqual(
    context.statusFromMessages([
      {
        info: { role: "assistant", time: { created: 1 } },
        parts: [{ type: "text", text: "Working" }],
      },
    ]),
    "Running",
  )
  assert.strictEqual(context.statusFromRuntimeStatus({ type: "busy" }), "Running")
  assert.strictEqual(context.summaryFromRuntimeStatus({ type: "retry", message: "rate limited" }), "Retrying: rate limited")
  assert.strictEqual(context.compactTail("Alpha Beta Gamma Delta", 12), "...mma Delta")

  assert.strictEqual(
    context.summaryFromMessages([
      {
        info: { role: "assistant", time: { created: 1 }, finish: "stop" },
        parts: [{ type: "text", text: "One. Two. Three. Four. Five. Six." }],
      },
    ]),
    "Two. Three. Four. Five. Six.",
  )

  assert.match(
    context.contextFromMessages([
      {
        info: { role: "user", time: { created: 1 } },
        parts: [{ type: "text", text: "Please continue" }],
      },
      {
        info: { role: "assistant", time: { created: 2 }, finish: "stop" },
        parts: [{ type: "text", text: "Continuing now." }],
      },
    ]),
    /You\nPlease continue/,
  )

  const longTranscript = [
    "You\n" + "alpha ".repeat(120),
    "Agent\n" + "beta ".repeat(120),
    "System\n" + "gamma ".repeat(120),
  ].join("\n\n")
  const pages = context.paginateText(longTranscript, 480)
  assert.ok(pages.length > 1)
  assert.ok(pages.every((page) => Buffer.byteLength(page, "utf8") <= 480))
  assertJsonEqual(context.paginateText(" \n\n ", 480), ["No context yet."])

  const unicodePages = context.paginateText("You\n" + "neon 🚀 ".repeat(180), 480)
  assert.ok(unicodePages.length > 1)
  assert.ok(unicodePages.every((page) => Buffer.byteLength(page, "utf8") <= 480))
  assert.ok(unicodePages.every((page) => !/[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(^|[^\uD800-\uDBFF])[\uDC00-\uDFFF]/.test(page)))
  assert.ok(unicodePages.every((page) => !page.includes("...")))

  const longFinalSentence = "Agent\n" + "This is a long final assistant message ".repeat(40) + "FINAL_SENTINEL_WORD"
  const fullTranscript = context.contextFromMessages([
    {
      info: { role: "assistant", time: { created: 3 }, finish: "stop" },
      parts: [{ type: "text", text: longFinalSentence.slice("Agent\n".length) }],
    },
  ])
  assert.ok(fullTranscript.includes("FINAL_SENTINEL_WORD"))
  assert.ok(!fullTranscript.includes("..."))
  const finalPages = context.paginateText(fullTranscript, 480)
  assert.ok(finalPages.length > 1)
  assert.ok(finalPages.every((page) => Buffer.byteLength(page, "utf8") <= 480))
  assert.ok(finalPages.every((page) => !page.includes("...")))
  assert.ok(finalPages[finalPages.length - 1].includes("FINAL_SENTINEL_WORD"))

  storedSettings = JSON.stringify({ baseUrl: "http://100.64.0.10:3773", username: "t3code", password: "secret" })
  snapshot = makeSnapshot()
  sockets = []
  sentMessages = []
  context.cachedSessions = {}
  context.refreshSessions()

  await waitFor(() => sentMessages.find((message) => message.cmd === 3))
  assert.ok(sockets[0].url === "ws://100.64.0.10:3773/ws?token=secret")
  const snapshotRequest = JSON.parse(sockets[0].sent[0])
  assert.match(snapshotRequest.id, /^\d+$/)
  assert.match(snapshotRequest.traceId, /^[a-f0-9]{32}$/)
  assert.match(snapshotRequest.spanId, /^[a-f0-9]{16}$/)
  assert.strictEqual(snapshotRequest.sampled, true)
  assert.ok(sentMessages.some((message) => message.cmd === 8 && message.status === "v0.1.0 100.64.0.10:3773"))
  assert.ok(sentMessages.some((message) => message.cmd === 8 && message.status === "Found 1 session via t3 ws"))
  assert.ok(sentMessages.some((message) => message.summary === "Built the app. It is ready."))
  assert.ok(sentMessages.some((message) => message.status === "Running"))
  assert.ok(sentMessages.some((message) => message.cmd === 9 && message.project_id === "proj_1"))

  snapshot = makeSnapshot({
    threads: [
      {
        id: "ses_1",
        projectId: "proj_1",
        title: "Pebble client",
        modelSelection: { provider: "codex", model: "gpt-5-codex" },
        runtimeMode: "full-access",
        interactionMode: "default",
        session: { status: "idle", providerName: "codex" },
        latestTurn: { state: "completed" },
        activities: [
          {
            kind: "user-input.requested",
            sequence: 1,
            createdAt: "2026-01-01T00:00:04.000Z",
            payload: {
              requestId: "question_1",
              questions: [
                {
                  id: "sandbox",
                  header: "Sandbox",
                  question: "Which sandbox?",
                  options: [{ label: "workspace-write", description: "Allow workspace writes" }],
                },
              ],
            },
          },
        ],
        messages: [],
        createdAt: "2026-01-01T00:00:00.000Z",
        updatedAt: "2026-01-01T00:00:05.000Z",
      },
    ],
  })
  sockets = []
  sentMessages = []
  context.cachedSessions = {}
  context.refreshSessions()
  await waitFor(() => sentMessages.find((message) => message.request_id === "question_1"))
  assert.ok(sentMessages.some((message) => message.status === "Needs input"))
  assert.ok(sentMessages.some((message) => /Which sandbox/.test(message.summary)))

  dispatches = []
  sentMessages = []
  context.replyQuestion(
    "ses_1",
    "question_1",
    "workspace",
    {
      questions: [
        {
          id: "sandbox",
          question: "Which sandbox?",
          options: [{ label: "workspace-write", description: "Allow workspace writes" }],
        },
      ],
    },
  )
  await waitFor(() => dispatches.length === 1)
  assert.strictEqual(dispatches[0].type, "thread.user-input.respond")
  assertJsonEqual(dispatches[0].answers, { sandbox: "workspace-write" })
  assert.ok(sentMessages.some((message) => message.cmd === 5))

  dispatches = []
  sentMessages = []
  let permissionDone = false
  context.replyPermission("ses_1", "perm_1", "yes", (done) => {
    permissionDone = done
  })
  await waitFor(() => dispatches.length === 1)
  assert.strictEqual(permissionDone, true)
  assert.strictEqual(dispatches[0].type, "thread.approval.respond")
  assert.strictEqual(dispatches[0].decision, "accept")

  dispatches = []
  sentMessages = []
  context.pendingBySession = {}
  context.interruptSession("ses_1")
  await waitFor(() => dispatches.length === 1)
  assert.strictEqual(dispatches[0].type, "thread.turn.interrupt")
  assert.ok(sentMessages.some((message) => message.cmd === 5))

  dispatches = []
  sentMessages = []
  context.promptSession("ses_1", "continue")
  await waitFor(() => dispatches.length === 1)
  assert.strictEqual(dispatches[0].type, "thread.turn.start")
  assert.match(dispatches[0].message.text, /sent from the user's Pebble watch through t3pebble/)
  assert.match(dispatches[0].message.text, /last five sentences/)
  assert.strictEqual(dispatches[0].modelSelection.model, "gpt-5.5")
  assert.ok(sentMessages.some((message) => message.cmd === 5))

  dispatches = []
  sentMessages = []
  context.promptNewThread("proj_1", "start new work")
  await waitFor(() => dispatches.length === 1)
  assert.strictEqual(dispatches[0].type, "thread.turn.start")
  assert.strictEqual(dispatches[0].bootstrap.createThread.projectId, "proj_1")
  assert.strictEqual(dispatches[0].modelSelection.model, "gpt-5.5")

  snapshot = makeSnapshot({
    projects: [
      {
        id: "proj_54",
        title: "Pebble 5.4",
        workspaceRoot: "/repo/pebble54",
        defaultModelSelection: { provider: "codex", model: "gpt-5.4" },
        createdAt: "2026-01-01T00:00:00.000Z",
        updatedAt: "2026-01-01T00:00:00.000Z",
      },
    ],
    threads: [],
  })
  context.lastSnapshot = snapshot
  dispatches = []
  context.promptNewThread("proj_54", "start new 5.4 work")
  await waitFor(() => dispatches.length === 1)
  assert.strictEqual(dispatches[0].modelSelection.model, "gpt-5.5")
  assert.strictEqual(dispatches[0].bootstrap.createThread.modelSelection.model, "gpt-5.5")

  snapshot = makeSnapshot({
    projects: [
      {
        id: "proj_instance",
        title: "Pebble Instance",
        workspaceRoot: "/repo/pebble-instance",
        defaultModelSelection: { instanceId: "codex", model: "gpt-5.4" },
        createdAt: "2026-01-01T00:00:00.000Z",
        updatedAt: "2026-01-01T00:00:00.000Z",
      },
    ],
    threads: [
      {
        id: "ses_instance",
        projectId: "proj_instance",
        title: "Canonical thread",
        modelSelection: { instanceId: "codex", model: "gpt-5.5" },
        runtimeMode: "full-access",
        interactionMode: "default",
        session: { status: "idle" },
        latestTurn: { state: "completed" },
        activities: [],
        messages: [],
        createdAt: "2026-01-01T00:00:00.000Z",
        updatedAt: "2026-01-01T00:00:03.000Z",
      },
    ],
  })
  context.lastSnapshot = snapshot
  dispatches = []
  context.promptNewThread("proj_instance", "start canonical work")
  await waitFor(() => dispatches.length === 1)
  assert.strictEqual(dispatches[0].modelSelection.provider, "codex")
  assert.strictEqual(dispatches[0].modelSelection.instanceId, "codex")
  assert.strictEqual(dispatches[0].modelSelection.model, "gpt-5.5")
  assert.strictEqual(dispatches[0].bootstrap.createThread.modelSelection.instanceId, "codex")

  const canonicalSession = context.toPebbleSession(snapshot, snapshot.threads[0])
  assert.strictEqual(canonicalSession.agent, "codex")

  console.log("phone bridge tests passed")
}

main().catch((error) => {
  console.error(error)
  process.exit(1)
})
