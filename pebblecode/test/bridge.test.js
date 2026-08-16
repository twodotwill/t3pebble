const assert = require("assert")
const fs = require("fs")
const path = require("path")
const vm = require("vm")

const source = fs.readFileSync(path.join(__dirname, "..", "src", "pkjs", "index.js"), "utf8")

let storedSettings = null
let storedValues = {}
let sentMessages = []
let requests = []
let dispatches = []
let shells = {}

const HOST_A = "100.64.0.10:3773"
const HOST_B = "100.64.0.11:3773"

const DAY_MS = 24 * 60 * 60 * 1000
const NOW = Date.now()
const iso = (msAgo) => new Date(NOW - msAgo).toISOString()

function settingsFor(servers) {
  return JSON.stringify({ servers, nextServerId: servers.length + 1 })
}

/** A thread as the shell route serves it: no bodies, but every lifecycle flag. */
function shellThread(overrides = {}) {
  return {
    id: "ses_1",
    projectId: "proj_1",
    title: "Pebble client",
    modelSelection: { instanceId: "codex", model: "gpt-5.6-sol" },
    runtimeMode: "full-access",
    interactionMode: "default",
    branch: null,
    worktreePath: null,
    latestTurn: { state: "completed", requestedAt: iso(60000), startedAt: iso(59000), completedAt: iso(58000) },
    createdAt: iso(10 * DAY_MS),
    updatedAt: iso(58000),
    archivedAt: null,
    settledOverride: null,
    settledAt: null,
    snoozedUntil: null,
    snoozedAt: null,
    session: { status: "ready", providerName: "codex", updatedAt: iso(58000) },
    latestUserMessageAt: iso(60000),
    hasPendingApprovals: false,
    hasPendingUserInput: false,
    backgroundLiveness: null,
    planProgress: null,
    ...overrides,
  }
}

function shellSnapshot(overrides = {}) {
  return {
    snapshotSequence: 1,
    updatedAt: iso(58000),
    projects: [
      {
        id: "proj_1",
        title: "Pebble",
        workspaceRoot: "/repo/pebblecode",
        defaultModelSelection: { instanceId: "codex", model: "gpt-5-codex" },
        createdAt: iso(10 * DAY_MS),
        updatedAt: iso(10 * DAY_MS),
      },
    ],
    threads: [shellThread()],
    ...overrides,
  }
}

function routeRequest(method, url, body) {
  const host = (url.match(/^https?:\/\/([^/]+)/) || [])[1] || ""
  const [pathname, query] = url.replace(/^https?:\/\/[^/]+/, "").split("?")
  requests.push({ method, url, host, pathname, query, body })

  if (method === "POST" && pathname === "/api/orchestration/dispatch") {
    dispatches.push(JSON.parse(body))
    return { status: 200, body: JSON.stringify({ sequence: 1 }) }
  }

  const shell = shells[host]
  if (!shell) {
    return { status: 502, body: JSON.stringify({ _tag: "Unreachable" }) }
  }

  if (method === "GET" && pathname === "/api/orchestration/shell") {
    return { status: 200, body: JSON.stringify(shell) }
  }

  if (method === "GET" && pathname.startsWith("/api/orchestration/threads/")) {
    const threadId = decodeURIComponent(pathname.slice("/api/orchestration/threads/".length))
    assert.ok(!threadId.includes("::"), "server must receive a plain thread id, got " + threadId)
    const thread = shell.threads.find((candidate) => candidate.id === threadId)
    if (!thread) {
      return {
        status: 404,
        body: JSON.stringify({ _tag: "EnvironmentResourceNotFoundError", code: "not_found", reason: "thread_not_found" }),
      }
    }
    // The detail route carries bodies but NOT the shell-only lifecycle
    // fields, so strip them here the way the real server does.
    const { hasPendingApprovals, hasPendingUserInput, latestUserMessageAt,
            backgroundLiveness, planProgress, __activities, __messages, ...detailThread } = thread
    return {
      status: 200,
      body: JSON.stringify({
        snapshotSequence: shell.snapshotSequence,
        thread: {
          ...detailThread,
          activities: __activities || [],
          checkpoints: [],
          proposedPlans: [],
          messages: __messages || [],
        },
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
    const response = routeRequest(this.method, this.url, body)
    requests[requests.length - 1].headers = this.headers
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

function waitFor(predicate, label) {
  return new Promise((resolve, reject) => {
    const started = Date.now()
    function tick() {
      const value = predicate()
      if (value) {
        resolve(value)
        return
      }
      if (Date.now() - started > 3000) {
        reject(new Error("timed out waiting for " + (label || "test event")))
        return
      }
      setTimeout(tick, 10)
    }
    tick()
  })
}

const CMD = {
  sessionItem: 2, sessionEnd: 3, detail: 4, prompt: 5, error: 6, context: 7,
  projectItem: 9, projectEnd: 10, hostItem: 12, hostEnd: 13,
}

async function main() {
  // ---------------------------------------------------------------- settings
  assertJsonEqual(context.settings(), { servers: [], nextServerId: 1 })

  assert.match(context.configurationHtml(), /Base URL/)
  assert.match(context.configurationHtml(), /Access token/)
  assert.match(context.configurationHtml(), /Label/)
  assert.match(context.configurationHtml(), /Add server/)

  storedSettings = JSON.stringify({ baseUrl: "http://old-host:4096", token: "keep-me" })
  context.localStorage.setItem("t3pebble_build_label", "v0.2.0")
  assertJsonEqual(context.settings(), {
    servers: [{ id: "s1", label: "old-host:4096", baseUrl: "http://old-host:4096", token: "keep-me" }],
    nextServerId: 2,
  })

  context.localStorage.setItem("t3pebble_build_label", context.BUILD_LABEL)
  storedSettings = JSON.stringify({
    servers: [
      { label: "beta1", baseUrl: "http://a:3773/", token: "ta" },
      { label: "", baseUrl: "http://b:3773", token: "tb" },
      { label: "ignored", baseUrl: "", token: "tc" },
      { id: "s1", label: "dupe", baseUrl: "http://c:3773", token: "td" },
    ],
  })
  const normalized = context.settings()
  assertJsonEqual(normalized.servers.map((s) => s.id), ["s1", "s2", "s3"])
  assertJsonEqual(normalized.servers.map((s) => s.label), ["beta1", "b:3773", "dupe"])

  assertJsonEqual(context.splitCompositeId("s2::thread-9"), { serverId: "s2", nativeId: "thread-9" })
  assert.ok(context.compositeId("s6", "0a09f4ea-bbed-4a44-a49d-2b495d0b57c8").length < 72)

  // Model selection follows the server, with gpt-5.6-sol only as a fallback.
  assertJsonEqual(context.resolveModelSelection(null), { instanceId: "codex", model: "gpt-5.6-sol" })
  assertJsonEqual(context.resolveModelSelection({ provider: "codex", model: "gpt-5.4" }), {
    instanceId: "codex",
    model: "gpt-5.4",
  })

  // ------------------------------------------------ settled classification
  // Mirrors T3 Code's effectiveSettled: activity blockers outrank any override.
  const st = (overrides) => context.threadState(shellThread(overrides), NOW)

  assert.strictEqual(st({ hasPendingApprovals: true }), "needs")
  assert.strictEqual(st({ hasPendingUserInput: true }), "needs")
  assert.strictEqual(st({ session: { status: "running", updatedAt: iso(1000) } }), "run")
  assert.strictEqual(st({ session: { status: "starting", updatedAt: iso(1000) } }), "run")
  assert.strictEqual(st({ backgroundLiveness: "working" }), "run")

  // A blocker beats an explicit settle, exactly as upstream does.
  assert.strictEqual(st({ settledOverride: "settled", hasPendingApprovals: true }), "needs")
  assert.strictEqual(
    st({ settledOverride: "settled", session: { status: "running", updatedAt: iso(1000) } }),
    "run",
  )

  assert.strictEqual(st({ settledOverride: "settled" }), "settled")
  assert.strictEqual(st({ settledOverride: "active" }), "idle")

  // Auto-settle after T3's 3-day default; a 2-day-old thread stays idle.
  assert.strictEqual(
    st({
      latestUserMessageAt: iso(5 * DAY_MS),
      latestTurn: { state: "completed", requestedAt: iso(5 * DAY_MS), startedAt: null, completedAt: iso(5 * DAY_MS) },
    }),
    "settled",
  )
  assert.strictEqual(
    st({
      latestUserMessageAt: iso(2 * DAY_MS),
      latestTurn: { state: "completed", requestedAt: iso(2 * DAY_MS), startedAt: null, completedAt: iso(2 * DAY_MS) },
    }),
    "idle",
  )

  assert.strictEqual(st({ session: { status: "error", updatedAt: iso(1000) } }), "err")
  assert.strictEqual(st({ snoozedUntil: new Date(NOW + 3600000).toISOString(), snoozedAt: iso(1000) }), "snooze")
  // A snoozed thread that raises its hand is not hidden.
  assert.strictEqual(
    st({ snoozedUntil: new Date(NOW + 3600000).toISOString(), snoozedAt: iso(1000), hasPendingApprovals: true }),
    "needs",
  )

  // A just-dispatched message no turn has adopted yet still counts as running.
  assert.strictEqual(context.hasQueuedTurnStart(shellThread({ latestUserMessageAt: iso(5000), latestTurn: null }), NOW), true)
  assert.strictEqual(context.hasQueuedTurnStart(shellThread({ latestUserMessageAt: iso(10 * 60000), latestTurn: null }), NOW), false)

  // ------------------------------------------------------------- roll-ups
  assert.strictEqual(context.hostStateFromCounts({ needs: 1, run: 2, err: 0, idle: 3, settled: 4, total: 10 }), "needs")
  assert.strictEqual(context.hostStateFromCounts({ needs: 0, run: 2, err: 1, idle: 3, settled: 4, total: 10 }), "run")
  assert.strictEqual(context.hostStateFromCounts({ needs: 0, run: 0, err: 1, idle: 3, settled: 4, total: 8 }), "err")
  assert.strictEqual(context.hostStateFromCounts({ needs: 0, run: 0, err: 0, idle: 3, settled: 4, total: 7 }), "idle")
  assert.strictEqual(context.hostStateFromCounts({ needs: 0, run: 0, err: 0, idle: 0, settled: 4, total: 4 }), "settled")
  assert.strictEqual(context.hostStateFromCounts({ needs: 0, run: 0, err: 0, idle: 0, settled: 0, total: 0 }), "empty")

  assert.strictEqual(context.hostDetailLine("settled", { settled: 4, total: 4 }), "all settled")
  assert.strictEqual(context.hostDetailLine("needs", { needs: 2 }), "2 need you")
  assert.strictEqual(context.hostDetailLine("idle", { idle: 3 }), "3 idle")
  assert.strictEqual(context.hostDetailLine("empty", { total: 0 }), "no threads")

  // --------------------------------------------------------- text helpers
  assert.strictEqual(
    context.summaryFromMessages([
      { info: { role: "assistant", time: { created: 1 }, finish: "stop" }, parts: [{ type: "text", text: "One. Two. Three. Four. Five. Six." }] },
    ]),
    "Two. Three. Four. Five. Six.",
  )
  assert.match(
    context.contextFromMessages([
      { info: { role: "user", time: { created: 1 } }, parts: [{ type: "text", text: "Please continue" }] },
    ]),
    /You\nPlease continue/,
  )
  const pages = context.paginateText(["You\n" + "alpha ".repeat(120), "Agent\n" + "beta ".repeat(120)].join("\n\n"), 480)
  assert.ok(pages.length > 1)
  assert.ok(pages.every((page) => Buffer.byteLength(page, "utf8") <= 480))
  const unicodePages = context.paginateText("You\n" + "neon 🚀 ".repeat(180), 480)
  assert.ok(unicodePages.every((page) => Buffer.byteLength(page, "utf8") <= 480))
  assert.ok(unicodePages.every((page) => !/[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(^|[^\uD800-\uDBFF])[\uDC00-\uDFFF]/.test(page)))

  assert.strictEqual(context.httpFailureMessage(401, JSON.stringify({ reason: "invalid_credential" })), "T3 rejected the access token (invalid_credential)")
  assert.strictEqual(context.httpFailureMessage(404, JSON.stringify({ reason: "thread_not_found" })), "Thread not found")

  // ------------------------------------------------------- the host screen
  storedSettings = settingsFor([
    { id: "s1", label: "beta1", baseUrl: "http://" + HOST_A, token: "token-a" },
    { id: "s2", label: "mini", baseUrl: "http://" + HOST_B, token: "token-b" },
  ])
  shells = {
    [HOST_A]: shellSnapshot({ threads: [shellThread({ id: "t_needs", hasPendingApprovals: true })] }),
    [HOST_B]: shellSnapshot({
      threads: [
        shellThread({ id: "t_settled", settledOverride: "settled" }),
        shellThread({ id: "t_settled2", settledOverride: "settled" }),
      ],
    }),
  }
  requests = []
  sentMessages = []
  context.shellByServer = {}
  context.refreshHosts()

  await waitFor(() => sentMessages.find((m) => m.cmd === CMD.hostEnd), "hostEnd-1")
  const hostRows = sentMessages.filter((m) => m.cmd === CMD.hostItem)
  assert.strictEqual(hostRows.length, 2)
  assertJsonEqual(hostRows.map((m) => m.host_id), ["s1", "s2"])
  assertJsonEqual(hostRows.map((m) => m.title), ["beta1", "mini"])
  assertJsonEqual(hostRows.map((m) => m.state), ["needs", "settled"])
  assertJsonEqual(hostRows.map((m) => m.detail), ["1 need you", "all settled"])

  // One shell request per host and nothing more: no per-thread hydration.
  assert.strictEqual(requests.filter((r) => r.pathname === "/api/orchestration/shell").length, 2)
  assert.strictEqual(requests.filter((r) => r.pathname.startsWith("/api/orchestration/threads/")).length, 0)
  const tokensByHost = {}
  for (const request of requests) tokensByHost[request.host] = request.headers.authorization
  assert.strictEqual(tokensByHost[HOST_A], "Bearer token-a")
  assert.strictEqual(tokensByHost[HOST_B], "Bearer token-b")

  // An unreachable host reports itself rather than vanishing.
  delete shells[HOST_B]
  sentMessages = []
  context.shellByServer = {}
  context.refreshHosts()
  await waitFor(() => sentMessages.find((m) => m.cmd === CMD.hostEnd), "hostEnd-offline")
  const offline = sentMessages.filter((m) => m.cmd === CMD.hostItem).find((m) => m.host_id === "s2")
  assert.strictEqual(offline.state, "offline")
  assert.ok(offline.detail.length > 0)

  // ----------------------------------------------------- the thread screen
  shells[HOST_B] = shellSnapshot({
    threads: [shellThread({ id: "t_settled", settledOverride: "settled", title: "Old work" })],
    projects: [
      {
        id: "proj_mini",
        title: "Mini",
        workspaceRoot: "/mini/repo",
        defaultModelSelection: { instanceId: "codex", model: "gpt-5.6-sol" },
        createdAt: iso(DAY_MS),
        updatedAt: iso(DAY_MS),
      },
    ],
  })
  sentMessages = []
  requests = []
  context.shellByServer = {}
  context.selectHost("s2")
  await waitFor(() => sentMessages.find((m) => m.cmd === CMD.projectEnd), "projectEnd-1")
  const threadRows = sentMessages.filter((m) => m.cmd === CMD.sessionItem)
  assert.strictEqual(threadRows.length, 1)
  assert.strictEqual(threadRows[0].session_id, "s2::t_settled")
  assert.strictEqual(threadRows[0].title, "Old work")
  assert.strictEqual(threadRows[0].state, "settled")
  assert.match(threadRows[0].detail, /^settled /)
  const projectRows = sentMessages.filter((m) => m.cmd === CMD.projectItem)
  assert.strictEqual(projectRows[0].project_id, "s2::proj_mini")
  // Still no bodies fetched to render a list.
  assert.strictEqual(requests.filter((r) => r.pathname.startsWith("/api/orchestration/threads/")).length, 0)

  // ------------------------------------------------------------ the detail
  shells[HOST_B].threads[0].__messages = [
    { id: "m1", role: "user", text: "Do the thing", streaming: false, createdAt: iso(90000), updatedAt: iso(90000) },
    { id: "m2", role: "assistant", text: "Did the thing. All done.", streaming: false, createdAt: iso(80000), updatedAt: iso(80000) },
  ]
  sentMessages = []
  requests = []
  context.detail("s2::t_settled", 0)
  const detailMessage = await waitFor(() => sentMessages.find((m) => m.cmd === CMD.detail), "detail-1")
  assert.strictEqual(detailMessage.state, "settled")
  assert.match(detailMessage.summary, /Did the thing/)
  // Opening a thread is the first and only body fetch.
  assert.strictEqual(requests.filter((r) => r.pathname === "/api/orchestration/threads/t_settled").length, 1)

  // The detail card must agree with the row it was opened from. A thread that
  // auto-settled on age with no latestTurn is the case that exposes it: the
  // detail route omits latestUserMessageAt, so classifying from that response
  // alone finds no activity at all and reads "idle" instead of "settled".
  shells[HOST_B] = shellSnapshot({
    threads: [
      shellThread({
        id: "t_aged",
        title: "Aged out",
        settledOverride: null,
        latestTurn: null,
        latestUserMessageAt: iso(47 * DAY_MS),
        __messages: [
          { id: "m1", role: "assistant", text: "Long finished.", streaming: false, createdAt: iso(47 * DAY_MS), updatedAt: iso(47 * DAY_MS) },
        ],
      }),
    ],
    projects: shells[HOST_B].projects,
  })
  sentMessages = []
  context.shellByServer = {}
  context.selectHost("s2")
  await waitFor(() => sentMessages.find((m) => m.cmd === CMD.projectEnd), "projectEnd-aged")
  const agedRow = sentMessages.filter((m) => m.cmd === CMD.sessionItem)[0]
  assert.strictEqual(agedRow.state, "settled")

  sentMessages = []
  context.detail("s2::t_aged", 0)
  const agedDetail = await waitFor(() => sentMessages.find((m) => m.cmd === CMD.detail), "detail-aged")
  assert.strictEqual(agedDetail.state, agedRow.state)
  assert.strictEqual(agedDetail.detail, agedRow.detail)

  // A pending approval surfaces its request id so a reply can answer it.
  shells[HOST_B].threads[0].hasPendingApprovals = true
  shells[HOST_B].threads[0].__activities = [
    {
      kind: "approval.requested",
      sequence: 1,
      createdAt: iso(30000),
      payload: { requestId: "per_1", requestKind: "file-change", detail: "src/pkjs/index.js" },
    },
  ]
  sentMessages = []
  context.shellByServer = {}
  context.detail("s2::t_aged", 0)
  const blocked = await waitFor(() => sentMessages.find((m) => m.cmd === CMD.detail && m.request_id === "per_1"), "detail-blocked")
  assert.strictEqual(blocked.state, "needs")
  assert.strictEqual(blocked.request_kind, "permission")
  assert.match(blocked.summary, /Needs permission: file-change/)

  // ---------------------------------------------------------------- writes
  dispatches = []
  requests = []
  context.replyPermission("s2::t_aged", "per_1", "yes", () => {})
  await waitFor(() => dispatches.length === 1, "dispatch-approval")
  const dispatchRequest = requests.find((r) => r.pathname === "/api/orchestration/dispatch")
  assert.strictEqual(dispatchRequest.host, HOST_B)
  assert.strictEqual(dispatchRequest.headers.authorization, "Bearer token-b")
  assert.strictEqual(dispatches[0].type, "thread.approval.respond")
  assert.strictEqual(dispatches[0].threadId, "t_aged")
  assert.strictEqual(dispatches[0].decision, "accept")

  dispatches = []
  context.interruptSession("s1::t_needs")
  await waitFor(() => dispatches.length === 1, "dispatch-interrupt")
  assert.strictEqual(dispatches[0].type, "thread.turn.interrupt")
  assert.strictEqual(dispatches[0].threadId, "t_needs")

  dispatches = []
  context.shellByServer = {}
  context.promptSession("s1::t_needs", "continue")
  await waitFor(() => dispatches.length === 1, "dispatch-prompt")
  assert.strictEqual(dispatches[0].type, "thread.turn.start")
  assert.strictEqual(dispatches[0].threadId, "t_needs")
  // An existing thread keeps the model the server has it on.
  assert.strictEqual(dispatches[0].modelSelection, undefined)

  dispatches = []
  context.shellByServer = {}
  context.promptNewThread("s2::proj_mini", "start something")
  await waitFor(() => dispatches.length === 1, "dispatch-new")
  assert.strictEqual(dispatches[0].bootstrap.createThread.projectId, "proj_mini")
  assertJsonEqual(dispatches[0].modelSelection, { instanceId: "codex", model: "gpt-5.6-sol" })

  // An id naming a host that is no longer configured cannot be dispatched.
  let unknownError = null
  context.dispatchForThread("s9::x", () => ({}), (error) => {
    unknownError = error
  })
  assert.match(String(unknownError && unknownError.message), /Unknown T3 server/)

  // --- multi-host setup: pasteable bundle lines -------------------------

  const bundle = context.parseServerBundle(
    [
      "T3 Pebble server",
      "t3pebble1|beta1|https://beta1.tail253492.ts.net|tok_one",
      "  t3pebble1|mini|https://mini.tail253492.ts.net/|tok_two  ",
      "t3pebble1|broken|https://nope.ts.net",
      "t3pebble1||https://unlabelled.ts.net|tok_three",
      "not a bundle line",
    ].join("\n")
  )
  assert.strictEqual(bundle.length, 3)
  assertJsonEqual(bundle[0], {
    id: "",
    label: "beta1",
    baseUrl: "https://beta1.tail253492.ts.net",
    token: "tok_one",
  })
  // A trailing slash would otherwise produce a double slash in every path.
  assert.strictEqual(bundle[1].baseUrl, "https://mini.tail253492.ts.net")
  assert.strictEqual(bundle[1].label, "mini")
  // A line without a token is dropped rather than saved half-configured.
  assert.strictEqual(bundle[2].label, "")
  assert.strictEqual(bundle[2].token, "tok_three")
  assertJsonEqual(context.parseServerBundle(""), [])
  assertJsonEqual(context.parseServerBundle(null), [])

  // --- labels derived from the host ------------------------------------

  // Tailnet names share a suffix, so the leading segment is the useful part.
  assert.strictEqual(context.hostShortName("https://williams-macbook-pro.tail253492.ts.net"), "williams-macbook-pro")
  // An IP has no leading segment worth keeping, so it stays whole with its port.
  assert.strictEqual(context.hostShortName("http://100.64.0.10:3773"), "100.64.0.10:3773")
  assert.strictEqual(context.hostShortName(""), "no URL")

  const labelled = context.normalizeSettings({
    servers: [
      { baseUrl: "https://mini.tail253492.ts.net", token: "t" },
      { baseUrl: "http://100.64.0.10:3773", token: "t" },
      { baseUrl: "https://beta1.tail253492.ts.net", token: "t", label: "desk" },
    ],
  })
  assert.strictEqual(labelled.servers[0].label, "mini")
  assert.strictEqual(labelled.servers[1].label, "100.64.0.10:3773")
  assert.strictEqual(labelled.servers[2].label, "desk")

  // --- settings page carries the parser and the paste box ---------------

  const html = context.configurationHtml()
  assert.match(html, /id='bundle'/)
  assert.match(html, /addFromPaste/)
  // The page parses with the bridge's own function source, not a copy.
  assert.match(html, /var parseServerBundle=function parseServerBundle/)

  // --- the paste flow, driven the way the phone webview drives it -------

  // Start from a fresh install so the blank seed row is what the page opens on.
  storedSettings = null
  const freshHtml = context.configurationHtml()
  const pageScript = freshHtml.slice(freshHtml.indexOf("<script>") + 8, freshHtml.indexOf("</script>"))
  const els = {
    bundle: { value: "" },
    pasteMsg: { textContent: "" },
    servers: { innerHTML: "" },
  }
  const page = {
    console,
    document: { getElementById: (id) => els[id], querySelectorAll: () => [] },
    location: { href: "" },
  }
  vm.createContext(page)
  vm.runInContext(pageScript, page)

  els.bundle.value = [
    "t3pebble1|beta1|https://beta1.tail253492.ts.net|tok_beta",
    "t3pebble1|mini|https://mini.tail253492.ts.net|tok_mini",
  ].join("\n")
  page.addFromPaste()
  // The blank seed row is consumed rather than left behind as an empty entry.
  assert.strictEqual(page.servers.length, 2)
  assert.strictEqual(els.bundle.value, "")

  // Re-running the command on a host rotates its token in place.
  els.bundle.value = "t3pebble1|mini|https://mini.tail253492.ts.net|tok_rotated"
  page.addFromPaste()
  assert.strictEqual(page.servers.length, 2)
  assert.strictEqual(page.servers[1].token, "tok_rotated")

  // Junk must not corrupt what is already configured.
  els.bundle.value = "hello world"
  page.addFromPaste()
  assert.strictEqual(page.servers.length, 2)
  assert.match(els.pasteMsg.textContent, /No setup lines found/)

  // What the page saves must survive the bridge's own normalization.
  page.save()
  const saved = JSON.parse(decodeURIComponent(page.location.href.split("#")[1]))
  const round = context.normalizeSettings(saved)
  assert.strictEqual(round.servers.length, 2)
  assert.strictEqual(round.servers[0].label, "beta1")
  assert.strictEqual(round.servers[1].token, "tok_rotated")

  console.log("phone bridge tests passed")
}

main().catch((error) => {
  console.error(error)
  process.exit(1)
})
