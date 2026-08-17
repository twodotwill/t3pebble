// The wire protocol and the build label are generated from protocol.json by
// tools/gen-protocol.js, which writes the same table into appinfo.json's
// appKeys and into main.c. Edit protocol.json, never this block;
// test/protocol.test.js fails the build if the copies disagree.
//
// SCOPE_ACTIVE / SCOPE_SETTLED pick which slice of a host's threads a request
// wants. Settled covers the snoozed ones too: both mean "not asking for
// anything right now".
//
// BUILD_LABEL is load-bearing -- migrateSettings() rewrites stored settings
// whenever it changes -- so it lives here with everything else that has to
// stay in step across the two sides.
// @generated protocol:begin
var KEY_CMD = "cmd";
var KEY_INDEX = "index";
var KEY_TOTAL = "total";
var KEY_SESSION_ID = "session_id";
var KEY_TITLE = "title";
var KEY_DIRECTORY = "directory";
var KEY_AGENT = "agent";
var KEY_STATUS = "status";
var KEY_SUMMARY = "summary";
var KEY_CONTEXT = "context";
var KEY_PROMPT = "prompt";
var KEY_ERROR = "error";
var KEY_REQUEST_ID = "request_id";
var KEY_REQUEST_KIND = "request_kind";
var KEY_PROJECT_ID = "project_id";
var KEY_CONTEXT_PAGE = "context_page";
var KEY_SERVER = "server";
var KEY_STATE = "state";
var KEY_HOST_ID = "host_id";
var KEY_DETAIL = "detail";
var KEY_C_NEEDS = "c_needs";
var KEY_C_RUN = "c_run";
var KEY_C_IDLE = "c_idle";
var KEY_C_SETTLED = "c_settled";
var KEY_SCOPE = "scope";
var KEY_OFFSET = "offset";
var KEY_MATCHED = "matched";
var KEY_OTHER = "other";
var KEY_SETTLED = "settled";
var KEY_ACTION = "action";
var KEY_PATH = "path";
var KEY_NAME = "name";

var CMD_REFRESH = 1;
var CMD_SESSION_ITEM = 2;
var CMD_SESSION_END = 3;
var CMD_DETAIL = 4;
var CMD_PROMPT = 5;
var CMD_ERROR = 6;
var CMD_CONTEXT = 7;
var CMD_STATUS = 8;
var CMD_PROJECT_ITEM = 9;
var CMD_PROJECT_END = 10;
var CMD_NEW_THREAD = 11;
var CMD_HOST_ITEM = 12;
var CMD_HOST_END = 13;
var CMD_SELECT_HOST = 14;
var CMD_THREAD_ACTION = 15;
var CMD_PROJECT_NAME = 16;
var CMD_PROJECT_PREVIEW = 17;
var CMD_PROJECT_CREATE = 18;
var CMD_CONCIERGE = 19;
var CMD_SCREENSHOT_PAGE = 90;

var SCOPE_ACTIVE = 0;
var SCOPE_SETTLED = 1;

var BUILD_LABEL = "v0.7";
// @generated protocol:end

var MAX_SESSIONS = 20;
var SUMMARY_LIMIT = 150;
var CONTEXT_LIMIT = 480;
var CONTEXT_MESSAGE_LIMIT = 60;
var DETAIL_MESSAGE_LIMIT = 10;
// Kept equal to MAX_SERVERS so the host list is always one batch. Laptops sleep,
// and a sleeping host costs a full timeout; batching would make hosts wait
// behind that instead of alongside it.
var SERVER_CONCURRENCY = 6;
var MAX_APP_MESSAGE_FAILURES = 8;
var DEFAULT_BASE_URL = "";
var DEFAULT_TOKEN = "";
var HTTP_TIMEOUT_MS = 20000;
// The host list is one cheap request per machine, so it does not need the full
// budget. A shorter ceiling keeps a sleeping laptop from stalling the first
// paint for every other host.
var HOST_PROBE_TIMEOUT_MS = 8000;
// Why a host did not answer is the one thing worth reading off the wrist, so
// it gets a real sentence. Kept under HostItem.detail on the watch.
var HOST_FAILURE_LIMIT = 88;
var MAX_SERVERS = 6;
// Thread and project ids are namespaced by server so every id the watch sends
// back can be routed to the machine it came from. SessionItem.id is 72 bytes
// on the watch, so the prefix is kept short.
var ID_SEPARATOR = "::";
// Mainline T3 Code serves orchestration reads over REST instead of the old
// `orchestration.getSnapshot` RPC. The snapshot route is cheap but returns
// threads with empty bodies, so thread detail is fetched per thread.
// The shell route carries hasPendingApprovals / hasPendingUserInput /
// settledOverride / latestUserMessageAt, which is everything the settled
// classification needs. One request per host, no per-thread hydration.
var T3_SHELL_PATH = "/api/orchestration/shell";
var T3_THREAD_PATH = "/api/orchestration/threads/";
var T3_DISPATCH_PATH = "/api/orchestration/dispatch";
var T3_SOURCE_LABEL = "t3 http";
// Turn windows requested when hydrating thread bodies. The list only needs
// enough turns to recover a status badge and a summary line; the transcript
// view asks for a much deeper window.
var LIST_TURN_LIMIT = 6;
var CONTEXT_TURN_LIMIT = 40;
// New threads inherit whatever the project's default is on the server. This
// is only used when a project carries no default at all.
var FALLBACK_MODEL_SELECTION = { instanceId: "codex", model: "gpt-5.6-sol" };
var DEFAULT_SETTINGS = {
  servers: [],
  nextServerId: 1
};
// The screenshot storyboard below is phone-side only and unreachable on a
// handset (there is no `process`), so it costs bundle size and nothing else.
// The watch half of the rig -- CMD_SCREENSHOT_PAGE, which let the phone
// rearrange a shipped app's window stack -- is behind SCREENSHOT_BUILD in
// main.c and is not in the released binary.
var SCREENSHOT_FIXTURES = typeof process !== "undefined" && process.env &&
  process.env.T3PEBBLE_SCREENSHOT_FIXTURES === "1";

var appMessageQueue = [];
var appMessageBusy = false;
var appMessageFailureCount = 0;
var pendingBySession = {};
var cachedSessions = {};
var lastSnapshot = null;
var shellByServer = {};
// What was last sent for each host, so an unchanged roll-up costs no radio
// time. A quiet tailnet used to spend one acknowledged AppMessage per host
// every five minutes restating numbers the watch already had.
var lastHostRow = {};

function settings() {
  migrateSettings();
  var raw = localStorage.getItem("t3pebble_settings");
  if (!raw) {
    return copySettings(DEFAULT_SETTINGS);
  }
  try {
    return normalizeSettings(JSON.parse(raw));
  } catch (e) {
    return copySettings(DEFAULT_SETTINGS);
  }
}

function normalizeSettings(parsed) {
  var servers = [];
  var source = (parsed && parsed.servers) || [];
  var nextId = parseInt(parsed && parsed.nextServerId, 10);
  if (isNaN(nextId) || nextId < 1) {
    nextId = 1;
  }
  for (var i = 0; i < source.length && servers.length < MAX_SERVERS; i++) {
    var server = normalizeServer(source[i]);
    if (!server.baseUrl) {
      continue;
    }
    if (!server.id) {
      server.id = "s" + nextId;
      nextId++;
    }
    servers.push(server);
  }
  // Ids must never collide, or the watch would route a reply to the wrong
  // machine. A duplicate arriving from edited settings gets a fresh id.
  var seen = {};
  for (var j = 0; j < servers.length; j++) {
    if (seen[servers[j].id]) {
      servers[j].id = "s" + nextId;
      nextId++;
    }
    seen[servers[j].id] = true;
    var used = parseInt(String(servers[j].id).replace(/^s/, ""), 10);
    if (!isNaN(used) && used >= nextId) {
      nextId = used + 1;
    }
  }
  return { servers: servers, nextServerId: nextId };
}

function normalizeServer(server) {
  server = server || {};
  var baseUrl = normalizeBaseUrl(server.baseUrl);
  return {
    id: trim(server.id),
    label: compact(trim(server.label) || hostShortName(baseUrl), 18),
    baseUrl: baseUrl,
    token: trim(server.token || DEFAULT_TOKEN),
    // Where a project dictated from the watch gets created. Per host, because
    // six machines rarely agree on where work lives. Empty disables the flow.
    projectRoot: trim(server.projectRoot || "").replace(/\/+$/, ""),
    // Optional project whose agent proposes a path when the location is easier
    // described than dictated. Empty disables the concierge route.
    concierge: trim(server.concierge || "")
  };
}

function migrateSettings() {
  if (localStorage.getItem("t3pebble_build_label") === BUILD_LABEL) {
    return;
  }
  var raw = localStorage.getItem("t3pebble_settings");
  var migrated = null;
  if (raw) {
    try {
      var parsed = JSON.parse(raw);
      if (parsed && parsed.servers) {
        migrated = normalizeSettings(parsed);
      } else {
        // Single-server builds stored one `{baseUrl, token}` pair. Promote it
        // to the first entry of the server list so nothing has to be retyped.
        var baseUrl = trim(parsed && parsed.baseUrl);
        if (baseUrl && !/^https?:\/\/wills-macbook-pro-5(?::4096)?\/?$/i.test(baseUrl)) {
          migrated = normalizeSettings({
            servers: [{ label: "", baseUrl: baseUrl, token: trim(parsed.token || "") }]
          });
        }
      }
    } catch (e) {
      migrated = null;
    }
  }
  localStorage.setItem("t3pebble_build_label", BUILD_LABEL);
  localStorage.setItem("t3pebble_settings", JSON.stringify(migrated || DEFAULT_SETTINGS));
}

function copySettings(source) {
  return normalizeSettings(source);
}

function saveSettings(next) {
  // The settings page round-trips ids but not the counter, so carry the old
  // floor forward. Reusing the id of a removed server would let a stale watch
  // id resolve to a different machine.
  var previous = 1;
  try {
    previous = parseInt(JSON.parse(localStorage.getItem("t3pebble_settings")).nextServerId, 10) || 1;
  } catch (e) {
    previous = 1;
  }
  var normalized = normalizeSettings({
    servers: (next && next.servers) || [],
    nextServerId: previous
  });
  localStorage.setItem("t3pebble_settings", JSON.stringify(normalized));
}

function configuredServers() {
  return settings().servers;
}

function serverById(serverId) {
  var servers = configuredServers();
  for (var i = 0; i < servers.length; i++) {
    if (servers[i].id === serverId) {
      return servers[i];
    }
  }
  return null;
}

function compositeId(serverId, nativeId) {
  return serverId + ID_SEPARATOR + nativeId;
}

function splitCompositeId(value) {
  var raw = String(value || "");
  var at = raw.indexOf(ID_SEPARATOR);
  if (at === -1) {
    return { serverId: "", nativeId: raw };
  }
  return { serverId: raw.slice(0, at), nativeId: raw.slice(at + ID_SEPARATOR.length) };
}


function trim(value) {
  return String(value || "").replace(/^\s+|\s+$/g, "");
}

function normalizeBaseUrl(value) {
  var baseUrl = trim(value || DEFAULT_BASE_URL);
  if (/^https?:\/\/wills-macbook-pro-5(?::4096)?\/?$/i.test(baseUrl)) {
    return DEFAULT_BASE_URL;
  }
  return baseUrl.replace(/\/+$/, "");
}

function compact(value, limit) {
  value = String(value || "").replace(/\s+/g, " ");
  if (value.length <= limit) {
    return value;
  }
  return value.slice(0, limit - 1) + "...";
}

function compactTail(value, limit) {
  value = String(value || "").replace(/\s+/g, " ");
  if (value.length <= limit) {
    return value;
  }
  if (limit <= 3) {
    return value.slice(value.length - limit);
  }
  return "..." + value.slice(value.length - limit + 3);
}



function utf8Length(value) {
  value = String(value || "");
  if (typeof Buffer !== "undefined") {
    return Buffer.byteLength(value, "utf8");
  }
  return unescape(encodeURIComponent(value)).length;
}


function trimToUtf8BytesWithIndex(value, byteLimit) {
  value = String(value || "");
  var result = "";
  for (var i = 0; i < value.length; i++) {
    var start = i;
    var unit = value.charAt(i);
    var code = value.charCodeAt(i);
    if (code >= 0xD800 && code <= 0xDBFF && i + 1 < value.length) {
      var nextCode = value.charCodeAt(i + 1);
      if (nextCode >= 0xDC00 && nextCode <= 0xDFFF) {
        unit += value.charAt(i + 1);
        i++;
      }
    }
    var next = result + unit;
    if (utf8Length(next) > byteLimit) {
      return { text: result, nextIndex: start };
    }
    result = next;
  }
  return { text: result, nextIndex: value.length };
}

function splitUtf8Page(value, byteLimit) {
  value = String(value || "");
  var hard = trimToUtf8BytesWithIndex(value, byteLimit);
  if (hard.nextIndex >= value.length) {
    return hard;
  }

  var splitIndex = -1;
  var minIndex = Math.floor(hard.text.length * 0.55);
  for (var i = hard.text.length - 1; i >= minIndex; i--) {
    if (/\s/.test(hard.text.charAt(i))) {
      splitIndex = i;
      break;
    }
  }

  if (splitIndex > 0) {
    var nextIndex = splitIndex + 1;
    while (nextIndex < value.length && /\s/.test(value.charAt(nextIndex))) {
      nextIndex++;
    }
    return {
      text: value.slice(0, splitIndex).replace(/\s+$/g, ""),
      nextIndex: nextIndex
    };
  }

  return hard;
}

function transcriptText(value) {
  return String(value || "")
    .replace(/\r/g, "")
    .replace(/[ \t]+/g, " ")
    .replace(/[ \t]+\n/g, "\n")
    .replace(/\n[ \t]+/g, "\n")
    .replace(/\n{3,}/g, "\n\n")
    .replace(/^\s+|\s+$/g, "");
}


function fiveSentences(value) {
  value = String(value || "").replace(/\s+/g, " ");
  var parts = value.match(/[^.!?]+[.!?]+|[^.!?]+$/g);
  if (!parts || parts.length <= 5) {
    return value;
  }
  return parts.slice(parts.length - 5).join(" ").replace(/^\s+|\s+$/g, "");
}

function fileName(path) {
  path = String(path || "");
  var parts = path.split("/");
  return parts[parts.length - 1] || path || "project";
}


function httpUrl(server, path) {
  var baseUrl = normalizeBaseUrl(server.baseUrl);
  if (!baseUrl) {
    return "";
  }
  return baseUrl + path;
}

function randomId(prefix) {
  var random = Math.floor(Math.random() * 1000000000).toString(36);
  return prefix + Date.now().toString(36) + random;
}

// XMLHttpRequest collapses every pre-HTTP failure into status 0 with no
// detail, which is where the useless "T3 unreachable" came from: a sleeping
// laptop, a wrong port, a stopped server and a name that will not resolve all
// arrive looking identical. The phone does know two things it was not saying —
// which address it dialled, and how long the attempt took — and those separate
// the cases. A refusal comes back in milliseconds because something actively
// answered "no"; silence runs to the full timeout because nothing answered.
var FAST_FAILURE_MS = 2500;

// The address as dialled, so a six-host list says which machine and which port
// this was. The scheme rides along only when it is https, because that is the
// `tailscale serve` path and it fails somewhere different from a plain IP.
function serverAddress(server) {
  var baseUrl = normalizeBaseUrl(server && server.baseUrl);
  var host = urlHost(baseUrl);
  return /^https:/i.test(baseUrl) ? "https://" + host : host;
}

function looksLikeIpAddress(host) {
  return /^\d+\.\d+\.\d+\.\d+(:\d+)?$/.test(String(host || ""));
}

// What to check next, keyed on the shape of the URL, because the two supported
// setups break in different places. A tailnet HTTPS name is published by
// `tailscale serve` on the host; a bare tailnet IP goes straight at whatever
// address `t3 serve` bound to, which is the setting that catches people out
// when a desktop build only listens on loopback.
function reachabilityHint(server) {
  var baseUrl = normalizeBaseUrl(server && server.baseUrl);
  if (/^https:/i.test(baseUrl)) {
    return "run tailscale serve there";
  }
  return "is t3 serve bound to that address?";
}

// Whole seconds only. The offline row is suppressed when it is byte-identical
// to the last poll's, so a sentence carrying millisecond jitter would send a
// message every single poll and spend exactly the Bluetooth that suppression
// exists to save. The information in the elapsed time is fast-versus-slow, and
// that is already carried by which sentence gets chosen.
function secondsText(ms) {
  return String(Math.round((ms > 0 ? ms : 0) / 1000)) + "s";
}

function transportFailure(server, kind, elapsedMs, budgetMs) {
  var error = buildTransportFailure(server, kind, elapsedMs, budgetMs);
  // Marks a failure that never reached HTTP, so the caller can tell "this
  // machine is down" from "this machine said no" — one of those generalises
  // across hosts and the other does not.
  error.transport = true;
  return error;
}

function buildTransportFailure(server, kind, elapsedMs, budgetMs) {
  var where = serverAddress(server);
  if (kind === "timeout") {
    // The budget, not the measured elapsed: they agree to within a few
    // milliseconds and only the budget is identical from one poll to the next.
    return new Error(where + " silent for " + secondsText(budgetMs || elapsedMs) +
                     " - asleep or off the tailnet");
  }
  if (elapsedMs <= FAST_FAILURE_MS) {
    // Only a bare IP can be called refused without hedging: there is no name
    // to resolve, so a fast failure is a reset or no route. A hostname might
    // equally never have resolved, which "no connection" covers and "refused"
    // would misreport.
    var verb = looksLikeIpAddress(urlHost(normalizeBaseUrl(server && server.baseUrl)))
      ? " refused - "
      : " no connection - ";
    return new Error(where + verb + reachabilityHint(server));
  }
  return new Error(where + " dropped the connection after " + secondsText(elapsedMs));
}

function httpRequest(server, method, path, body, callback, timeoutMs) {
  if (!server || !server.baseUrl) {
    callback(new Error("No base URL set for " + ((server && server.label) || "this server")));
    return;
  }
  if (!server.token) {
    callback(new Error("Add an access token for " + (server.label || "this server")));
    return;
  }
  if (typeof XMLHttpRequest === "undefined") {
    callback(new Error("Phone HTTP unavailable"));
    return;
  }

  var done = false;
  var request = new XMLHttpRequest();
  var budgetMs = timeoutMs || HTTP_TIMEOUT_MS;
  var startedAt = Date.now();

  function finish(error, value) {
    if (done) {
      return;
    }
    done = true;
    clearTimeout(timer);
    callback(error, value);
  }

  function failTransport(kind) {
    finish(transportFailure(server, kind, Date.now() - startedAt, budgetMs));
  }

  var timer = setTimeout(function() {
    try {
      request.abort();
    } catch (e) {
      void e;
    }
    failTransport("timeout");
  }, budgetMs);

  // Where the runtime implements them, these say which kind of failure it was
  // instead of leaving it to be inferred from a zero status. Both are additive:
  // the readystatechange path below and the timer still catch everything on a
  // runtime that ignores them, and `done` keeps whichever fires first.
  request.ontimeout = function() {
    failTransport("timeout");
  };
  request.onerror = function() {
    failTransport("error");
  };

  request.onreadystatechange = function() {
    if (request.readyState !== 4 || done) {
      return;
    }
    var status = request.status;
    if (!status) {
      failTransport("error");
      return;
    }
    if (status < 200 || status >= 300) {
      finish(new Error(httpFailureMessage(status, request.responseText, server)));
      return;
    }
    var text = trim(request.responseText);
    if (!text) {
      finish(null, null);
      return;
    }
    try {
      finish(null, JSON.parse(text));
    } catch (e) {
      finish(new Error("T3 sent an unreadable response"));
    }
  };

  try {
    request.open(method, httpUrl(server, path), true);
    // Set after open, which is where XHR requires it. Our own timer stays as
    // the backstop for runtimes that ignore this.
    request.timeout = budgetMs;
    request.setRequestHeader("Authorization", "Bearer " + server.token);
    request.setRequestHeader("Accept", "application/json");
    if (body !== null && body !== undefined) {
      request.setRequestHeader("Content-Type", "application/json");
    }
    request.send(body === null || body === undefined ? null : JSON.stringify(body));
  } catch (e) {
    finish(e);
  }
}

function httpFailureMessage(status, responseText, server) {
  var detail = "";
  try {
    var parsed = JSON.parse(responseText || "");
    if (parsed && typeof parsed === "object") {
      detail = parsed.reason || parsed.requiredScope || parsed.code || parsed._tag || "";
    }
  } catch (e) {
    void e;
  }
  if (status === 401) {
    return "T3 rejected the access token" + (detail ? " (" + detail + ")" : "");
  }
  if (status === 403) {
    return "Access token is missing a scope" + (detail ? " (" + detail + ")" : "");
  }
  if (status === 404) {
    if (detail === "thread_not_found") {
      return "Thread not found";
    }
    // A reply arrived, so something is listening — it just is not T3. Naming
    // the address is what separates "wrong port" from "T3 changed its routes".
    return "No T3 route at " + serverAddress(server) + " - wrong port?";
  }
  // The tailnet HTTPS path puts `tailscale serve` in front, and a gateway
  // error from it means the host is reachable but nothing is listening behind
  // the proxy — a different fix from anything else here. Only claimed on that
  // path: over plain HTTP there is no proxy to name.
  if ((status === 502 || status === 503 || status === 504) &&
      /^https:/i.test(normalizeBaseUrl(server && server.baseUrl))) {
    return "tailscale serve is up at " + serverAddress(server) +
           " but t3 serve is not (" + status + ")";
  }
  return serverAddress(server) + " failed with " + status + (detail ? " " + detail : "");
}

// Fans out across every configured server and folds the replies into one
// snapshot shaped exactly like the single-server one, with ids namespaced by
// server. Everything downstream stays server-agnostic.

// Rewrites every id in one server's snapshot into the namespaced form and
// records which server it came from.
function tagSnapshot(server, snapshot) {
  var multiple = configuredServers().length > 1;
  var label = multiple ? server.label : "";
  var projects = ((snapshot && snapshot.projects) || []).map(function(project) {
    var tagged = copyThread(project);
    tagged.id = compositeId(server.id, project.id);
    tagged.nativeId = project.id;
    tagged.serverId = server.id;
    tagged.serverLabel = label;
    return tagged;
  });
  var threads = ((snapshot && snapshot.threads) || []).map(function(thread) {
    return tagThread(server, label, thread);
  });
  return { projects: projects, threads: threads };
}

function tagThread(server, label, thread) {
  var tagged = copyThread(thread);
  tagged.id = compositeId(server.id, thread.id);
  tagged.nativeId = thread.id;
  tagged.projectId = compositeId(server.id, thread.projectId);
  tagged.nativeProjectId = thread.projectId;
  tagged.serverId = server.id;
  tagged.serverLabel = label;
  return tagged;
}



function copyThread(thread) {
  var copy = {};
  for (var key in thread) {
    if (thread.hasOwnProperty(key)) {
      copy[key] = thread[key];
    }
  }
  return copy;
}

function storeHydratedThread(thread) {
  if (!lastSnapshot || !thread) {
    return;
  }
  var threads = lastSnapshot.threads || [];
  for (var i = 0; i < threads.length; i++) {
    if (threads[i].id === thread.id) {
      threads[i] = thread;
      lastSnapshot.threads = threads;
      return;
    }
  }
  lastSnapshot.threads = threads.concat([thread]);
}

function fetchThreadDetail(compositeThreadId, turnLimit, callback) {
  var parts = splitCompositeId(compositeThreadId);
  var server = serverById(parts.serverId);
  if (!server) {
    callback(new Error("Unknown T3 server"));
    return;
  }
  var multiple = configuredServers().length > 1;
  var path = T3_THREAD_PATH + encodeURIComponent(parts.nativeId) + "?turnLimit=" + turnLimit;
  httpRequest(server, "GET", path, null, function(error, response) {
    if (error) {
      callback(error);
      return;
    }
    var thread = response && response.thread;
    if (!thread) {
      callback(new Error("Thread not found"));
      return;
    }
    var hydrated = tagThread(server, multiple ? server.label : "", thread);
    hydrated.hydratedTurns = turnLimit;
    callback(null, hydrated);
  });
}

function ensureThreadHydrated(threadId, turnLimit, callback) {
  var cached = lastSnapshot ? threadById(lastSnapshot, threadId) : null;
  if (cached && cached.hydratedTurns >= turnLimit) {
    callback(null, cached);
    return;
  }
  refreshThread(threadId, turnLimit, callback);
}

function refreshThread(threadId, turnLimit, callback) {
  fetchThreadDetail(threadId, turnLimit, function(error, thread) {
    if (error) {
      callback(error);
      return;
    }
    storeHydratedThread(thread);
    callback(null, thread);
  });
}


function dispatchCommand(server, command, callback) {
  httpRequest(server, "POST", T3_DISPATCH_PATH, command, callback);
}

// Resolves the server that owns a namespaced thread id, then hands the builder
// the plain thread id that server expects to see.
function dispatchForThread(compositeThreadId, build, callback) {
  var parts = splitCompositeId(compositeThreadId);
  var server = serverById(parts.serverId);
  if (!server) {
    callback(new Error("Unknown T3 server"));
    return;
  }
  dispatchCommand(server, build(parts.nativeId), callback);
}

function send(message) {
  appMessageQueue.push(message);
  flushMessages();
}

function flushMessages() {
  if (appMessageBusy || appMessageQueue.length === 0) {
    return;
  }
  appMessageBusy = true;
  var message = appMessageQueue.shift();
  Pebble.sendAppMessage(message, function() {
    appMessageFailureCount = 0;
    appMessageBusy = false;
    flushMessages();
  }, function() {
    appMessageFailureCount++;
    // A host row that was skipped as unchanged is only correct if the watch
    // actually holds the previous one. Once anything has failed to land that
    // is no longer safe to assume, so the next poll resends every row.
    lastHostRow = {};
    if (appMessageFailureCount <= MAX_APP_MESSAGE_FAILURES) {
      appMessageQueue.unshift(message);
    } else {
      appMessageFailureCount = 0;
      sendError("Watch link dropped a message");
    }
    appMessageBusy = false;
    setTimeout(flushMessages, 250);
  });
}

function sendError(message) {
  send(makeMessage(CMD_ERROR, {
    error: compact(message && message.message ? message.message : message, 110)
  }));
}

function sendStatus(message) {
  send(makeMessage(CMD_STATUS, {
    status: compact(message, 56)
  }));
}

function sendScreenshotPage(index) {
  send(makeMessage(CMD_SCREENSHOT_PAGE, { index: index }));
}

function makeMessage(command, fields) {
  var message = {};
  message[KEY_CMD] = command;
  fields = fields || {};
  if (fields.index !== undefined) message[KEY_INDEX] = fields.index;
  if (fields.total !== undefined) message[KEY_TOTAL] = fields.total;
  if (fields.id !== undefined) message[KEY_SESSION_ID] = fields.id;
  if (fields.title !== undefined) message[KEY_TITLE] = fields.title;
  if (fields.directory !== undefined) message[KEY_DIRECTORY] = fields.directory;
  if (fields.agent !== undefined) message[KEY_AGENT] = fields.agent;
  if (fields.status !== undefined) message[KEY_STATUS] = fields.status;
  if (fields.summary !== undefined) message[KEY_SUMMARY] = fields.summary;
  if (fields.context !== undefined) message[KEY_CONTEXT] = fields.context;
  if (fields.prompt !== undefined) message[KEY_PROMPT] = fields.prompt;
  if (fields.error !== undefined) message[KEY_ERROR] = fields.error;
  if (fields.requestId !== undefined) message[KEY_REQUEST_ID] = fields.requestId;
  if (fields.requestKind !== undefined) message[KEY_REQUEST_KIND] = fields.requestKind;
  if (fields.projectId !== undefined) message[KEY_PROJECT_ID] = fields.projectId;
  if (fields.contextPage !== undefined) message[KEY_CONTEXT_PAGE] = fields.contextPage;
  if (fields.server !== undefined) message[KEY_SERVER] = fields.server;
  if (fields.state !== undefined) message[KEY_STATE] = fields.state;
  if (fields.hostId !== undefined) message[KEY_HOST_ID] = fields.hostId;
  if (fields.detail !== undefined) message[KEY_DETAIL] = fields.detail;
  if (fields.scope !== undefined) message[KEY_SCOPE] = fields.scope;
  if (fields.offset !== undefined) message[KEY_OFFSET] = fields.offset;
  if (fields.matched !== undefined) message[KEY_MATCHED] = fields.matched;
  if (fields.other !== undefined) message[KEY_OTHER] = fields.other;
  if (fields.settled !== undefined) message[KEY_SETTLED] = fields.settled;
  if (fields.path !== undefined) message[KEY_PATH] = fields.path;
  if (fields.name !== undefined) message[KEY_NAME] = fields.name;
  if (fields.counts !== undefined) {
    message[KEY_C_NEEDS] = fields.counts.needs;
    message[KEY_C_RUN] = fields.counts.run;
    message[KEY_C_IDLE] = fields.counts.idle;
    message[KEY_C_SETTLED] = fields.counts.settled;
  }
  return message;
}


function newestFirst(values) {
  return values.slice().sort(function(left, right) {
    return (Date.parse(right.updatedAt || right.createdAt || "") || 0) - (Date.parse(left.updatedAt || left.createdAt || "") || 0);
  });
}

// --- thread lifecycle -------------------------------------------------
// Ported from T3 Code's own threadSettled.ts so the watch never disagrees
// with the desktop about what is settled. Keep the ordering: activity
// blockers are checked before any override, exactly as effectiveSettled does.

var QUEUED_TURN_START_GRACE_MS = 2 * 60 * 1000;
var AUTO_SETTLE_AFTER_DAYS = 3; // T3's DEFAULT_SIDEBAR_AUTO_SETTLE_AFTER_DAYS
var DAY_MS = 24 * 60 * 60 * 1000;

function parseTime(value) {
  if (!value) {
    return NaN;
  }
  return Date.parse(value);
}

// A user message no turn has adopted yet: dispatched but not yet picked up,
// so session is still null and the pending work is invisible to the status
// checks. Only counts inside the adoption grace window.
function hasQueuedTurnStart(thread, nowMs) {
  if (!thread.latestUserMessageAt) {
    return false;
  }
  if (thread.session && thread.session.status === "error") {
    return false;
  }
  var messageAt = parseTime(thread.latestUserMessageAt);
  if (isNaN(messageAt) || isNaN(nowMs)) {
    return false;
  }
  if (Math.abs(nowMs - messageAt) > QUEUED_TURN_START_GRACE_MS) {
    return false;
  }
  var turn = thread.latestTurn;
  if (!turn) {
    return true;
  }
  var stamps = [turn.requestedAt, turn.startedAt, turn.completedAt];
  for (var i = 0; i < stamps.length; i++) {
    if (stamps[i] && parseTime(stamps[i]) >= messageAt) {
      return false;
    }
  }
  return true;
}

function threadLastActivityAt(thread) {
  var turn = thread.latestTurn || {};
  var candidates = [thread.latestUserMessageAt, turn.requestedAt, turn.startedAt, turn.completedAt];
  var latest = null;
  var latestMs = -Infinity;
  for (var i = 0; i < candidates.length; i++) {
    if (!candidates[i]) {
      continue;
    }
    var ms = parseTime(candidates[i]);
    if (ms > latestMs) {
      latest = candidates[i];
      latestMs = ms;
    }
  }
  return latest;
}

function threadRaisedHandWhileSnoozed(thread) {
  if (thread.hasPendingApprovals || thread.hasPendingUserInput) {
    return true;
  }
  var session = thread.session;
  if (session && session.status === "error" &&
      (!thread.snoozedAt || parseTime(session.updatedAt) > parseTime(thread.snoozedAt))) {
    return true;
  }
  var turn = thread.latestTurn;
  if (thread.snoozedAt && turn && turn.state === "completed" && turn.completedAt &&
      parseTime(turn.completedAt) > parseTime(thread.snoozedAt)) {
    return true;
  }
  return false;
}

function effectiveSnoozed(thread, nowMs) {
  if (!thread.snoozedUntil) {
    return false;
  }
  var wakeAt = parseTime(thread.snoozedUntil);
  if (isNaN(wakeAt) || wakeAt <= nowMs) {
    return false;
  }
  return !threadRaisedHandWhileSnoozed(thread);
}

function effectiveSettled(thread, nowMs) {
  if (thread.hasPendingApprovals || thread.hasPendingUserInput) {
    return false;
  }
  var status = thread.session && thread.session.status;
  if (status === "starting" || status === "running") {
    return false;
  }
  if (hasQueuedTurnStart(thread, nowMs)) {
    // The queued blocker is forgivable when the server already adjudicated it
    // by accepting a settle after the message.
    var adjudicated = thread.settledOverride === "settled" && thread.settledAt &&
      thread.latestUserMessageAt &&
      parseTime(thread.settledAt) >= parseTime(thread.latestUserMessageAt);
    if (!adjudicated) {
      return false;
    }
  }
  if (thread.settledOverride === "settled") {
    return true;
  }
  if (thread.settledOverride === "active") {
    return false;
  }
  if (AUTO_SETTLE_AFTER_DAYS === null) {
    return false;
  }
  var lastActivityAt = threadLastActivityAt(thread);
  if (!lastActivityAt) {
    return false;
  }
  return parseTime(lastActivityAt) < nowMs - AUTO_SETTLE_AFTER_DAYS * DAY_MS;
}

function threadState(thread, nowMs) {
  if (thread.hasPendingApprovals || thread.hasPendingUserInput) {
    return "needs";
  }
  var status = thread.session && thread.session.status;
  if (status === "starting" || status === "running" || thread.backgroundLiveness === "working" ||
      hasQueuedTurnStart(thread, nowMs)) {
    return "run";
  }
  if (effectiveSettled(thread, nowMs)) {
    return "settled";
  }
  if (effectiveSnoozed(thread, nowMs)) {
    return "snooze";
  }
  if (status === "error" || (thread.latestTurn && thread.latestTurn.state === "error")) {
    return "err";
  }
  return "idle";
}

function agentLabel(thread) {
  var session = thread.session || {};
  var selection = thread.modelSelection || {};
  var provider = session.providerName || selection.instanceId || selection.provider || "";
  var model = selection.model || "";
  if (provider && model) {
    return compact(provider + " " + model, 30);
  }
  return compact(provider || model || "agent", 30);
}

function relativeAge(iso, nowMs) {
  var ms = parseTime(iso);
  if (isNaN(ms)) {
    return "";
  }
  var delta = nowMs - ms;
  if (delta < 60000) {
    return "now";
  }
  if (delta < 3600000) {
    return Math.round(delta / 60000) + "m";
  }
  if (delta < DAY_MS) {
    return Math.round(delta / 3600000) + "h";
  }
  return Math.round(delta / DAY_MS) + "d";
}

function threadDetailLine(thread, state, nowMs, waitingSince) {
  if (state === "needs") {
    var kind = thread.hasPendingApprovals ? "approval" : "answer";
    var waited = waitingSince ? relativeAge(waitingSince, nowMs) : "";
    return waited ? "waiting " + waited + " for " + kind : "needs " + kind;
  }
  if (state === "run") {
    var progress = thread.planProgress;
    if (progress && progress.step) {
      return compact(progress.step, 34);
    }
    return "running";
  }
  if (state === "err") {
    return "error";
  }
  if (state === "snooze") {
    return "snoozed " + relativeAge(thread.snoozedUntil, nowMs);
  }
  var age = relativeAge(threadLastActivityAt(thread) || thread.updatedAt, nowMs);
  if (state === "settled") {
    return age ? "settled " + age : "settled";
  }
  return age ? "idle " + age : "idle";
}

function activeThreads(snapshot) {
  var threads = snapshot && snapshot.threads ? snapshot.threads : [];
  return newestFirst(threads.filter(function(thread) {
    return !thread.deletedAt && !thread.archivedAt;
  })).slice(0, MAX_SESSIONS);
}

// True when a thread is not asking for anything: explicitly settled, aged out
// past the auto-settle window, or snoozed until later.
function isRestingState(state) {
  return state === "settled" || state === "snooze";
}

// One scope's worth of threads, newest first and unpaged. Ordering stays by
// activity rather than by settledAt, so a thread that aged out sits where its
// work left it rather than jumping to the moment the server noticed.
function threadsForScope(snapshot, scope, nowMs) {
  var threads = snapshot && snapshot.threads ? snapshot.threads : [];
  var wantResting = scope === SCOPE_SETTLED;
  return newestFirst(threads.filter(function(thread) {
    if (thread.deletedAt || thread.archivedAt) {
      return false;
    }
    return isRestingState(threadState(thread, nowMs)) === wantResting;
  }));
}

function activeProjects(snapshot) {
  var projects = snapshot && snapshot.projects ? snapshot.projects : [];
  return newestFirst(projects.filter(function(project) {
    return !project.deletedAt;
  })).slice(0, MAX_SESSIONS);
}

function threadById(snapshot, threadId) {
  var threads = snapshot && snapshot.threads ? snapshot.threads : [];
  for (var i = 0; i < threads.length; i++) {
    if (threads[i].id === threadId && !threads[i].deletedAt) {
      return threads[i];
    }
  }
  return null;
}


function projectTitle(project) {
  return compact((project && project.title) || fileName(project && project.workspaceRoot) || "Project", 58);
}


// The watch does not pick models. Whatever the server has configured wins;
// the fallback only covers a project that carries no default at all.
function resolveModelSelection(selection) {
  if (!selection || !trim(selection.model)) {
    return {
      instanceId: FALLBACK_MODEL_SELECTION.instanceId,
      model: FALLBACK_MODEL_SELECTION.model
    };
  }
  var resolved = {
    instanceId: selection.instanceId || selection.provider,
    model: selection.model
  };
  if (selection.options !== undefined) {
    resolved.options = selection.options;
  }
  return resolved;
}

function activityPayload(activity) {
  return activity && activity.payload && typeof activity.payload === "object" ? activity.payload : {};
}

function activitySequence(activity) {
  return activity && activity.sequence !== undefined ? activity.sequence : 9007199254740991;
}

function compareActivities(left, right) {
  var sequenceDelta = activitySequence(left) - activitySequence(right);
  if (sequenceDelta !== 0) {
    return sequenceDelta;
  }
  return compareStrings(left.createdAt, right.createdAt);
}

function compareStrings(left, right) {
  left = String(left || "");
  right = String(right || "");
  if (left < right) {
    return -1;
  }
  if (left > right) {
    return 1;
  }
  return 0;
}

function requestKindFromRequestType(requestType) {
  if (requestType === "command_execution_approval" || requestType === "exec_command_approval") {
    return "command";
  }
  if (requestType === "file_read_approval") {
    return "file-read";
  }
  if (requestType === "file_change_approval" || requestType === "apply_patch_approval") {
    return "file-change";
  }
  return null;
}

function isStalePendingFailure(detail) {
  var normalized = String(detail || "").toLowerCase();
  return normalized.indexOf("stale pending approval request") !== -1 ||
    normalized.indexOf("stale pending user-input request") !== -1 ||
    normalized.indexOf("unknown pending approval request") !== -1 ||
    normalized.indexOf("unknown pending permission request") !== -1 ||
    normalized.indexOf("unknown pending user-input request") !== -1;
}

function derivePendingApprovals(activities) {
  var open = {};
  activities = (activities || []).slice().sort(compareActivities);
  for (var i = 0; i < activities.length; i++) {
    var activity = activities[i];
    var payload = activityPayload(activity);
    var requestId = typeof payload.requestId === "string" ? payload.requestId : null;
    var requestKind = payload.requestKind === "command" || payload.requestKind === "file-read" || payload.requestKind === "file-change" ? payload.requestKind : requestKindFromRequestType(payload.requestType);
    var detail = typeof payload.detail === "string" ? payload.detail : "";
    if (activity.kind === "approval.requested" && requestId && requestKind) {
      open[requestId] = {
        id: requestId,
        sessionID: "",
        requestId: requestId,
        requestKind: requestKind,
        permission: requestKind,
        resources: detail ? [detail] : [],
        createdAt: activity.createdAt || "",
        detail: detail
      };
    } else if (activity.kind === "approval.resolved" && requestId) {
      delete open[requestId];
    } else if (activity.kind === "provider.approval.respond.failed" && requestId && isStalePendingFailure(detail)) {
      delete open[requestId];
    }
  }
  return objectValues(open).sort(function(left, right) {
    return compareStrings(left.createdAt, right.createdAt);
  });
}

function parseUserInputQuestions(payload) {
  var questions = payload && payload.questions;
  if (!questions || !questions.length) {
    return null;
  }
  var parsed = [];
  for (var i = 0; i < questions.length; i++) {
    var question = questions[i] || {};
    if (typeof question.id !== "string" || typeof question.question !== "string" || !question.options) {
      continue;
    }
    parsed.push({
      id: question.id,
      header: question.header || "",
      question: question.question,
      options: question.options
    });
  }
  return parsed.length ? parsed : null;
}

function derivePendingUserInputs(activities) {
  var open = {};
  activities = (activities || []).slice().sort(compareActivities);
  for (var i = 0; i < activities.length; i++) {
    var activity = activities[i];
    var payload = activityPayload(activity);
    var requestId = typeof payload.requestId === "string" ? payload.requestId : null;
    var detail = typeof payload.detail === "string" ? payload.detail : "";
    if (activity.kind === "user-input.requested" && requestId) {
      var questions = parseUserInputQuestions(payload);
      if (questions) {
        open[requestId] = {
          id: requestId,
          sessionID: "",
          requestId: requestId,
          createdAt: activity.createdAt || "",
          questions: questions
        };
      }
    } else if (activity.kind === "user-input.resolved" && requestId) {
      delete open[requestId];
    } else if (activity.kind === "provider.user-input.respond.failed" && requestId && isStalePendingFailure(detail)) {
      delete open[requestId];
    }
  }
  return objectValues(open).sort(function(left, right) {
    return compareStrings(left.createdAt, right.createdAt);
  });
}

function objectValues(object) {
  var values = [];
  for (var key in object) {
    if (object.hasOwnProperty(key)) {
      values.push(object[key]);
    }
  }
  return values;
}

function latestErrorDetail(thread) {
  if (!thread || (!thread.session || thread.session.status !== "error") && (!thread.latestTurn || thread.latestTurn.state !== "error")) {
    return "";
  }
  var activities = newestFirst((thread.activities || []).filter(function(activity) {
    return activity.tone === "error" && (activity.kind === "runtime.error" || String(activity.kind || "").indexOf("provider.") === 0);
  }));
  var payload = activities[0] ? activityPayload(activities[0]) : {};
  return parseJsonErrorMessage(thread.session && thread.session.lastError) || parseJsonErrorMessage(payload.message || payload.detail) || "Error";
}

function parseJsonErrorMessage(value) {
  if (!value) {
    return "";
  }
  try {
    var parsed = JSON.parse(value);
    if (parsed && parsed.error && parsed.error.message) {
      return parsed.error.message;
    }
    if (parsed && parsed.message) {
      return parsed.message;
    }
  } catch (e) {
    void e;
  }
  return String(value);
}




function toPebbleProject(project) {
  var selection = resolveModelSelection(project.defaultModelSelection);
  return {
    id: project.id,
    title: projectTitle(project),
    directory: project.workspaceRoot || "",
    model: selection.model || "codex",
    server: project.serverLabel || ""
  };
}

function toPebbleMessage(message) {
  return {
    type: message.role,
    info: {
      role: message.role,
      time: {
        created: message.createdAt,
        updated: message.updatedAt,
        completed: message.streaming ? null : message.updatedAt
      },
      finish: message.streaming ? null : "stop"
    },
    parts: [{ type: "text", text: message.text || "" }]
  };
}

// --- host roll-up -----------------------------------------------------
// The home screen answers one question per machine: does anything there want
// me? Everything it needs comes from the single shell request per host.

function rollupForThreads(threads, nowMs) {
  var counts = { needs: 0, run: 0, err: 0, idle: 0, settled: 0, snooze: 0 };
  for (var i = 0; i < threads.length; i++) {
    var state = threadState(threads[i], nowMs);
    counts[state] = (counts[state] || 0) + 1;
  }
  counts.total = threads.length;
  return counts;
}

function hostStateFromCounts(counts) {
  if (counts.needs > 0) return "needs";
  if (counts.run > 0) return "run";
  if (counts.err > 0) return "err";
  if (counts.idle > 0) return "idle";
  if (counts.total > 0) return "settled";
  return "empty";
}

function hostDetailLine(state, counts) {
  if (state === "needs") return counts.needs + " need you";
  if (state === "run") return counts.run + " running";
  if (state === "err") return counts.err + " in error";
  // "Idle" is the answer to "what needs me to start it": active threads with
  // nothing actually running.
  if (state === "idle") return counts.idle + " idle";
  if (state === "settled") return "all settled";
  return "no threads";
}

function refreshHosts() {
  if (SCREENSHOT_FIXTURES) {
    fixtureRefreshHosts();
    return;
  }
  var servers = configuredServers();
  if (servers.length === 0) {
    sendStatus("Add a server");
    send(makeMessage(CMD_HOST_END, { total: 0 }));
    return;
  }

  var nowMs = Date.now();
  var rows = new Array(servers.length);
  eachLimit(servers, SERVER_CONCURRENCY, function(server, index, next) {
    httpRequest(server, "GET", T3_SHELL_PATH, null, function(error, snapshot) {
      if (error) {
        shellByServer[server.id] = null;
        rows[index] = {
          id: server.id,
          title: server.label,
          state: "offline",
          // The offline host screen gives the reason the whole panel, so send
          // the sentence rather than the fragment that fit beside a readout.
          detail: compact(String(error.message || error), HOST_FAILURE_LIMIT),
          transport: !!error.transport,
          counts: { needs: 0, run: 0, idle: 0, settled: 0 }
        };
        next();
        return;
      }
      shellByServer[server.id] = tagSnapshot(server, snapshot);
      var threads = activeThreads(shellByServer[server.id]);
      var counts = rollupForThreads(threads, nowMs);
      var state = hostStateFromCounts(counts);
      rows[index] = {
        id: server.id,
        title: server.label,
        state: state,
        detail: hostDetailLine(state, counts),
        counts: {
          needs: counts.needs,
          run: counts.run,
          idle: counts.idle + counts.err,
          settled: counts.settled + counts.snooze
        }
      };
      next();
    }, HOST_PROBE_TIMEOUT_MS);
  }, function() {
    // Log the reason, not just the name: "rejected the token" and "no route"
    // need different fixes, and the watch is where that is read. One fault per
    // host, not one joined line: the fault log keeps six entries, so two down
    // machines should cost two of them rather than share a truncated one.
    var unreachable = 0;
    for (var f = 0; f < rows.length; f++) {
      if (rows[f] && rows[f].state === "offline") {
        sendError(rows[f].title + ": " + rows[f].detail);
        if (rows[f].transport) {
          unreachable++;
        }
      }
    }
    // Every machine failing to answer at once is not six coincidences; it is
    // one link. Sent last so it sits at the top of a newest-first fault log,
    // above the six rows that are all really saying this. Only worth stating
    // when there is more than one host to generalise from.
    if (rows.length > 1 && unreachable === rows.length) {
      sendError("All " + rows.length + " hosts unreachable - is Tailscale on for the phone?");
    }
    // Bluetooth is the other big battery consumer, and a poll that restates
    // unchanged numbers spends it for nothing. Skip a host whose row is
    // byte-identical to the one the watch already has; CMD_HOST_END still goes
    // every time, because that is what stamps the sync age and re-arms the
    // watch's refresh timer.
    var next = {};
    for (var i = 0; i < rows.length; i++) {
      var message = makeMessage(CMD_HOST_ITEM, {
        index: i,
        total: rows.length,
        hostId: rows[i].id,
        title: rows[i].title,
        detail: rows[i].detail,
        state: rows[i].state,
        counts: rows[i].counts
      });
      var fingerprint = JSON.stringify(message);
      next[rows[i].id] = fingerprint;
      if (lastHostRow[rows[i].id] !== fingerprint) {
        send(message);
      }
    }
    // Only remember what actually went out this round, so a host that drops
    // out of settings cannot leave a fingerprint that suppresses its own row
    // if it comes back unchanged.
    lastHostRow = next;
    send(makeMessage(CMD_HOST_END, { total: rows.length }));
  });
}

// --- one host's threads -----------------------------------------------

function selectHost(hostId, scope, offset) {
  if (SCREENSHOT_FIXTURES) {
    fixtureSelectHost(hostId, scope, offset);
    return;
  }
  var server = serverById(hostId);
  scope = scope === SCOPE_SETTLED ? SCOPE_SETTLED : SCOPE_ACTIVE;
  offset = offset > 0 ? offset : 0;
  if (!server) {
    sendError("Unknown host");
    send(makeMessage(CMD_SESSION_END, { total: 0, scope: scope, offset: 0, matched: 0 }));
    return;
  }

  function emit(tagged) {
    lastSnapshot = tagged;
    var nowMs = Date.now();
    var matched = threadsForScope(tagged, scope, nowMs);
    // Settled history outgrows the watch's fixed list, so it arrives a page at
    // a time rather than by raising the cap for a list that is rarely opened.
    if (offset >= matched.length) {
      offset = 0;
    }
    var page = matched.slice(offset, offset + MAX_SESSIONS);
    sendSessionItems(page.map(function(thread) {
      var state = threadState(thread, nowMs);
      return {
        id: thread.id,
        title: compact(thread.title || "Untitled", 54),
        detail: threadDetailLine(thread, state, nowMs),
        state: state,
        summary: "",
        requestId: "",
        requestKind: state === "needs" ? (thread.hasPendingApprovals ? "permission" : "question") : "",
        settled: isRestingState(state)
      };
    }), {
      scope: scope,
      offset: offset,
      matched: matched.length,
      // The count for the other scope's footer row, so the watch can label the
      // way out without asking for a list it is not showing.
      other: threadsForScope(tagged, scope === SCOPE_ACTIVE ? SCOPE_SETTLED : SCOPE_ACTIVE, nowMs).length
    });
    sendProjectItems(activeProjects(tagged).map(toPebbleProject));
  }

  var cached = shellByServer[hostId];
  if (cached) {
    emit(cached);
    return;
  }
  httpRequest(server, "GET", T3_SHELL_PATH, null, function(error, snapshot) {
    if (error) {
      sendError(error);
      send(makeMessage(CMD_SESSION_END, { total: 0 }));
      return;
    }
    shellByServer[hostId] = tagSnapshot(server, snapshot);
    emit(shellByServer[hostId]);
  });
}

// --- screenshot fixtures -------------------------------------------------

var FIXTURE_HOSTS = [
  {
    id: "shot-host-main",
    title: "WORKBENCH",
    detail: "2 need you",
    state: "needs",
    counts: { needs: 2, run: 1, idle: 3, settled: 8 }
  },
  {
    id: "shot-host-lab",
    title: "LAB",
    detail: "1 running",
    state: "run",
    counts: { needs: 0, run: 1, idle: 2, settled: 4 }
  },
  // A machine that did not answer, carrying the kind of sentence the real
  // probe produces. The offline screen is the one that has to stay legible
  // when the app is least able to explain itself, so it gets captured too.
  {
    id: "shot-host-remote",
    title: "REMOTE",
    detail: "T3 rejected the access token (session_expired)",
    state: "offline",
    counts: { needs: 0, run: 0, idle: 0, settled: 0 }
  }
];

var FIXTURE_ACTIVE_SESSIONS = [
  {
    id: "shot-host-main::thread-approval",
    title: "Approve screenshot capture",
    detail: "needs permission",
    state: "needs",
    agent: "codex",
    summary: "Needs permission to run the Pebble screenshot tool against the emery emulator. The approval row should show dense but readable wrist text.",
    requestId: "req-permission",
    requestKind: "permission",
    settled: false
  },
  {
    id: "shot-host-main::thread-running",
    title: "Refine Casio dashboard",
    detail: "running now",
    state: "run",
    agent: "gpt-5.6",
    summary: "The design pass is comparing the host dashboard, thread list, detail summary, transcript page, and diagnostics page for Casio LCD consistency.",
    requestId: "",
    requestKind: "",
    settled: false
  },
  {
    id: "shot-host-main::thread-idle",
    title: "Polish response layout",
    detail: "idle 12m",
    state: "idle",
    agent: "codex",
    summary: "The response screen needs enough body copy to prove wrapping, footer placement, and contrast on the warm LCD substrate.",
    requestId: "",
    requestKind: "",
    settled: false
  },
  {
    id: "shot-host-main::thread-error",
    title: "Long filename overflow",
    detail: "error",
    state: "err",
    agent: "codex",
    summary: "Error: fixture deliberately exercises alert coloring, truncation, and tight row spacing.",
    requestId: "",
    requestKind: "",
    settled: false
  }
];

var FIXTURE_SETTLED_SESSIONS = [
  {
    id: "shot-host-main::thread-settled",
    title: "Archived refactor notes",
    detail: "settled 2h",
    state: "settled",
    agent: "codex",
    summary: "Done. The settled list should keep the same LCD row style while feeling lower priority.",
    requestId: "",
    requestKind: "",
    settled: true
  }
];

var FIXTURE_PROJECTS = [
  { id: "shot-host-main::project-watch", title: "t3pebble", directory: "~/Projects/t3pebble" },
  { id: "shot-host-main::project-lab", title: "watch-lab", directory: "~/Projects/watch-lab" }
];

var FIXTURE_CONTEXT = [
  "You\nMake a repeatable screenshot rig for the Pebble Time 2 app so design changes can be judged from real emulator pixels.",
  "Agent\nI added a fixture storyboard that loads representative hosts, active threads, projects, detail content, and transcript text. It advances the watch through the major screens on a timer while the capture script saves PNG files.",
  "You\nKeep the ending readable from the actual watch.",
  "Agent\nThe final five sentences are intentionally short. They state what changed, where the screenshots go, and whether any user input is needed."
].join("\n\n");

function fixtureRefreshHosts() {
  for (var i = 0; i < FIXTURE_HOSTS.length; i++) {
    send(makeMessage(CMD_HOST_ITEM, {
      index: i,
      total: FIXTURE_HOSTS.length,
      hostId: FIXTURE_HOSTS[i].id,
      title: FIXTURE_HOSTS[i].title,
      detail: FIXTURE_HOSTS[i].detail,
      state: FIXTURE_HOSTS[i].state,
      counts: FIXTURE_HOSTS[i].counts
    }));
  }
  send(makeMessage(CMD_HOST_END, { total: FIXTURE_HOSTS.length }));
}

function fixtureSelectHost(hostId, scope, offset) {
  void hostId;
  scope = scope === SCOPE_SETTLED ? SCOPE_SETTLED : SCOPE_ACTIVE;
  offset = offset > 0 ? offset : 0;
  var sessions = scope === SCOPE_SETTLED ? FIXTURE_SETTLED_SESSIONS : FIXTURE_ACTIVE_SESSIONS;
  var other = scope === SCOPE_SETTLED ? FIXTURE_ACTIVE_SESSIONS.length : FIXTURE_SETTLED_SESSIONS.length;
  sendSessionItems(sessions, {
    scope: scope,
    offset: offset,
    matched: sessions.length,
    other: other
  });
  sendProjectItems(FIXTURE_PROJECTS);
}

function fixtureDetail(sessionId, index) {
  var item = fixtureSessionById(sessionId) || FIXTURE_ACTIVE_SESSIONS[index || 0];
  if (!item) {
    sendError("No fixture thread");
    return;
  }
  send(makeMessage(CMD_DETAIL, itemFields(item, index || 0, 1)));
}

function fixtureContext(sessionId, index, page, requestId) {
  var pages = paginateText(FIXTURE_CONTEXT, CONTEXT_LIMIT);
  var pageIndex = clampPage(page, pages.length);
  send(makeMessage(CMD_CONTEXT, {
    index: index || 0,
    id: sessionId || FIXTURE_ACTIVE_SESSIONS[0].id,
    total: pages.length,
    contextPage: pageIndex,
    requestId: requestId || "",
    context: pages[pageIndex]
  }));
}

function fixtureSessionById(sessionId) {
  var all = FIXTURE_ACTIVE_SESSIONS.concat(FIXTURE_SETTLED_SESSIONS);
  for (var i = 0; i < all.length; i++) {
    if (all[i].id === sessionId) {
      return all[i];
    }
  }
  return null;
}

function runScreenshotStoryboard() {
  refreshHosts();
  setTimeout(function() {
    sendScreenshotPage(0);
  }, 1200);
  setTimeout(function() {
    fixtureSelectHost(FIXTURE_HOSTS[0].id, SCOPE_ACTIVE, 0);
  }, 3600);
  setTimeout(function() {
    sendScreenshotPage(1);
  }, 4300);
  setTimeout(function() {
    fixtureDetail(FIXTURE_ACTIVE_SESSIONS[0].id, 0);
  }, 7600);
  setTimeout(function() {
    sendScreenshotPage(2);
  }, 8300);
  setTimeout(function() {
    fixtureContext(FIXTURE_ACTIVE_SESSIONS[0].id, 0, 0, "shot-context");
  }, 11600);
  setTimeout(function() {
    sendScreenshotPage(3);
  }, 12300);
  setTimeout(function() {
    sendError("Fixture fault for diag");
  }, 15600);
  setTimeout(function() {
    sendScreenshotPage(4);
  }, 16300);
  setTimeout(function() {
    sendScreenshotPage(5);
  }, 19600);
}


function urlHost(url) {
  return String(url || "").replace(/^https?:\/\//i, "").replace(/\/.*$/, "") || "no URL";
}

// Label shown when the settings entry leaves Label blank. Tailnet hosts are
// `machine.tailnet.ts.net`, and the shared suffix is the part that would get
// truncated away at 18 characters, so keep the leading segment only. An IP has
// no such segment, so it stays whole and keeps its port.
function hostShortName(url) {
  var host = urlHost(url);
  var portMatch = host.match(/:\d+$/);
  var port = portMatch ? portMatch[0] : "";
  var withoutPort = port ? host.slice(0, host.length - port.length) : host;
  // Only the shared DNS suffix is dropped. A port distinguishes one host from
  // another, so it is always kept, and an IP has no suffix worth trimming.
  if (/^\d+\.\d+\.\d+\.\d+$/.test(withoutPort)) {
    return host;
  }
  var dot = withoutPort.indexOf(".");
  if (dot === -1) {
    return host;
  }
  return withoutPort.slice(0, dot) + port;
}

function eachLimit(items, limit, worker, done) {
  if (items.length === 0) {
    done();
    return;
  }

  var next = 0;
  var running = 0;
  var finished = 0;

  function pump() {
    while (running < limit && next < items.length) {
      var index = next++;
      running++;
      worker(items[index], index, function() {
        running--;
        finished++;
        if (finished >= items.length) {
          done();
          return;
        }
        pump();
      });
    }
  }

  pump();
}

function sendSessionItems(items, page) {
  page = page || {};
  cachedSessions = {};
  for (var i = 0; i < items.length; i++) {
    cachedSessions[items[i].id] = items[i];
    send(makeMessage(CMD_SESSION_ITEM, itemFields(items[i], i, items.length)));
  }
  send(makeMessage(CMD_SESSION_END, {
    total: items.length,
    scope: page.scope || SCOPE_ACTIVE,
    offset: page.offset || 0,
    // How many threads the scope holds in all, so the watch knows whether a
    // further page exists without a second round trip.
    matched: page.matched === undefined ? items.length : page.matched,
    other: page.other || 0
  }));
}


function sendProjectItems(items) {
  cachedProjects = {};
  for (var i = 0; i < items.length; i++) {
    cachedProjects[items[i].id] = items[i];
    send(makeMessage(CMD_PROJECT_ITEM, {
      index: i,
      total: items.length,
      projectId: items[i].id,
      title: items[i].title,
      directory: items[i].directory,
      agent: items[i].model || "codex",
      server: items[i].server || ""
    }));
  }
  send(makeMessage(CMD_PROJECT_END, { total: items.length }));
}



function rememberPending(sessionId, kind, request) {
  if (!sessionId) {
    return;
  }
  pendingBySession[sessionId] = {
    kind: kind,
    request: request,
    requestId: request.id
  };
}



function turnLimitForMessageLimit(limit) {
  return limit > DETAIL_MESSAGE_LIMIT ? CONTEXT_TURN_LIMIT : LIST_TURN_LIMIT;
}

function loadMessages(sessionId, limit, directory, callback) {
  void directory;
  ensureThreadHydrated(sessionId, turnLimitForMessageLimit(limit), function(error, thread) {
    if (error) {
      callback(error);
      return;
    }
    var messages = (thread.messages || []).slice().sort(function(left, right) {
      return messageTime(toPebbleMessage(right)) - messageTime(toPebbleMessage(left));
    }).slice(0, limit).map(toPebbleMessage);
    callback(null, messages);
  });
}






function pendingSummary(pending) {
  var request = pending.request || {};
  if (pending.kind === "permission") {
    var resources = (request.resources || request.patterns || request.always || []).join(", ");
    var action = request.action || request.permission || "permission";
    var tool = request.tool && request.tool.callID ? " for tool " + request.tool.callID : "";
    return compact("Needs permission: " + action + tool + (resources ? " on " + resources : "") + ".", SUMMARY_LIMIT);
  }

  var questions = request.questions || [];
  if (questions.length === 0) {
    return "Needs an answer.";
  }

  var first = questions[0];
  var options = (first.options || []).map(function(option) {
    return option.label;
  }).join(", ");
  var prefix = questions.length > 1 ? questions.length + " questions. First: " : "Question: ";
  var suffix = options ? " Options: " + options + "." : "";
  return compact(prefix + first.question + suffix, SUMMARY_LIMIT);
}



function latestFirst(messages) {
  return messages.slice().sort(function(a, b) {
    return messageTime(b) - messageTime(a);
  });
}

function summaryFromMessages(messages) {
  var sorted = latestFirst(messages);
  for (var i = 0; i < sorted.length; i++) {
    var message = sorted[i];
    var info = messageInfo(message);
    if (info.role === "assistant" || message.type === "assistant") {
      var text = assistantText(message);
      if (text) {
        return compactTail(fiveSentences(text), SUMMARY_LIMIT);
      }
      if (messageError(message)) {
        return compact(messageError(message).message || "Assistant reported an error.", SUMMARY_LIMIT);
      }
    }
  }
  for (var j = 0; j < sorted.length; j++) {
    if (isUserMessage(sorted[j])) {
      var userText = userMessageText(sorted[j]);
      if (userText) {
        return compact("Last prompt: " + userText, SUMMARY_LIMIT);
      }
    }
  }
  return "No messages yet.";
}


function oldestFirst(messages) {
  return messages.slice().sort(function(a, b) {
    return messageTime(a) - messageTime(b);
  });
}

function messageTime(message) {
  var info = messageInfo(message);
  var time = message.time || info.time || {};
  return numericTime(time.updated) || numericTime(time.completed) || numericTime(time.created) || 0;
}

function numericTime(value) {
  if (!value) {
    return 0;
  }
  if (typeof value === "number") {
    return value;
  }
  var parsed = Date.parse(value);
  return isNaN(parsed) ? 0 : parsed;
}

function assistantText(message) {
  var content = messageContent(message);
  var chunks = [];
  for (var i = 0; i < content.length; i++) {
    if (content[i].type === "text" && content[i].text) {
      chunks.push(content[i].text);
    }
  }
  return chunks.join(" ");
}

function messageInfo(message) {
  return message.info || message;
}

function messageContent(message) {
  return message.content || message.parts || [];
}

function messageError(message) {
  var info = messageInfo(message);
  return message.error || info.error;
}


function isUserMessage(message) {
  var info = messageInfo(message);
  return message.type === "user" || info.role === "user";
}

function userMessageText(message) {
  if (message.text) {
    return message.text;
  }
  var content = messageContent(message);
  var chunks = [];
  for (var i = 0; i < content.length; i++) {
    if (content[i].type === "text" && content[i].text) {
      chunks.push(content[i].text);
    }
  }
  return chunks.join(" ");
}

function itemFields(item, index, total) {
  return {
    index: index,
    total: total,
    id: item.id,
    title: item.title,
    detail: item.detail,
    state: item.state,
    agent: item.agent || "",
    summary: item.summary || "",
    requestId: item.requestId || "",
    requestKind: item.requestKind || "",
    // Lets the watch build the right action menu without a second lookup.
    settled: item.settled ? 1 : 0
  };
}

function detail(sessionId, index) {
  if (SCREENSHOT_FIXTURES) {
    fixtureDetail(sessionId, index);
    return;
  }
  // Lifecycle and body come from different routes. hasPendingApprovals,
  // hasPendingUserInput, latestUserMessageAt and backgroundLiveness exist only
  // on the shell model, so classifying from the detail thread alone makes the
  // card disagree with the list it was opened from. State comes from the shell
  // record; messages come from the detail record.
  withShellFor(sessionId, function(shellError, shell) {
    var lifecycle = shellError ? null : threadById(shell, sessionId);
    refreshThread(sessionId, LIST_TURN_LIMIT, function(error, thread) {
      if (error) {
        sendError(error);
        return;
      }
      sendThreadDetail(sessionId, index, lifecycle || thread, thread);
    });
  });
}

function sendThreadDetail(sessionId, index, lifecycle, thread) {
  {
    var nowMs = Date.now();
    var approval = derivePendingApprovals(thread.activities)[0];
    var input = derivePendingUserInputs(thread.activities)[0];
    // Activities are fresher than a cached shell, so a request that landed
    // since the last roll-up still reads as needing an answer.
    var state = (approval || input) ? "needs" : threadState(lifecycle, nowMs);
    var summary;
    if (approval) {
      summary = pendingSummary({
        kind: "permission",
        request: { permission: approval.requestKind, patterns: approval.detail ? [approval.detail] : [] }
      });
    } else if (input) {
      summary = pendingSummary({ kind: "question", request: input });
    } else if (state === "err") {
      summary = compact("Error: " + (latestErrorDetail(thread) || "unknown"), SUMMARY_LIMIT);
    } else {
      summary = summaryFromMessages((thread.messages || []).map(toPebbleMessage));
    }

    if (approval) {
      approval.sessionID = sessionId;
      rememberPending(sessionId, "permission", approval);
    } else if (input) {
      input.sessionID = sessionId;
      rememberPending(sessionId, "question", input);
    } else {
      delete pendingBySession[sessionId];
    }

    var waitingSince = approval ? approval.createdAt : (input ? input.createdAt : null);
    var item = {
      id: sessionId,
      title: compact(thread.title || "Untitled", 54),
      agent: agentLabel(lifecycle && lifecycle.session ? lifecycle : thread),
      detail: threadDetailLine(lifecycle, state, nowMs, waitingSince),
      state: state,
      summary: summary,
      requestId: approval ? approval.requestId : (input ? input.requestId : ""),
      requestKind: approval ? "permission" : (input ? "question" : "")
    };
    cachedSessions[sessionId] = item;
    send(makeMessage(CMD_DETAIL, itemFields(item, index, 1)));
  }
}

function context(sessionId, index, page, requestId) {
  if (SCREENSHOT_FIXTURES) {
    fixtureContext(sessionId, index, page, requestId);
    return;
  }
  loadMessages(sessionId, CONTEXT_MESSAGE_LIMIT, "", function(error, response) {
    if (error) {
      sendError(error);
      return;
    }
    var messages = oldestFirst(response || []);
    var pages = paginateText(contextFromMessages(messages), CONTEXT_LIMIT);
    var pageIndex = clampPage(page, pages.length);
    send(makeMessage(CMD_CONTEXT, {
      index: index,
      id: sessionId,
      total: pages.length,
      contextPage: pageIndex,
      requestId: requestId || "",
      context: pages[pageIndex]
    }));
  });
}

function clampPage(page, pageCount) {
  page = parseInt(page, 10);
  if (pageCount < 1) {
    return 0;
  }
  if (page === -1) {
    return pageCount - 1;
  }
  if (isNaN(page) || page < 0) {
    return 0;
  }
  if (page >= pageCount) {
    return pageCount - 1;
  }
  return page;
}

function paginateText(value, limit) {
  value = String(value || "").replace(/\r/g, "").replace(/[ \t]+\n/g, "\n").replace(/\n[ \t]+/g, "\n").replace(/\n{3,}/g, "\n\n").replace(/^\s+|\s+$/g, "");
  if (!value) {
    return ["No context yet."];
  }
  var paragraphs = value.split(/\n\n/g);
  var pages = [];
  var current = "";

  function pushCurrent() {
    if (current) {
      pages.push(current);
      current = "";
    }
  }

  for (var i = 0; i < paragraphs.length; i++) {
    var paragraph = paragraphs[i];
    while (utf8Length(paragraph) > limit) {
      pushCurrent();
      var chunk = splitUtf8Page(paragraph, limit);
      if (!chunk.text) {
        break;
      }
      pages.push(chunk.text);
      paragraph = paragraph.slice(chunk.nextIndex).replace(/^\s+/g, "");
    }

    var next = current ? current + "\n\n" + paragraph : paragraph;
    if (utf8Length(next) > limit) {
      pushCurrent();
      current = paragraph;
    } else {
      current = next;
    }
  }
  pushCurrent();
  return pages.length ? pages : ["No context yet."];
}

function contextFromMessages(messages) {
  var lines = [];
  for (var i = 0; i < messages.length; i++) {
    var message = messages[i];
    if (isUserMessage(message)) {
      var userText = userMessageText(message);
      if (userText) {
        lines.push("You\n" + transcriptText(userText));
      }
    } else if (message.type === "assistant" || messageInfo(message).role === "assistant") {
      var text = assistantText(message);
      if (text) {
        lines.push("Agent\n" + transcriptText(text));
      } else if (messageError(message)) {
        lines.push("Error\n" + transcriptText(messageError(message).message || "unknown"));
      }
    } else if (message.type === "system" && message.text) {
      lines.push("System\n" + transcriptText(message.text));
    } else if (message.type === "synthetic" && message.text) {
      lines.push("Note\n" + transcriptText(message.text));
    }
  }
  return lines.join("\n\n") || "No context yet.";
}

function submitResponse(message) {
  var sessionId = message[KEY_SESSION_ID];
  var text = trim(message[KEY_PROMPT]);
  var requestId = message[KEY_REQUEST_ID];
  var requestKind = message[KEY_REQUEST_KIND];
  var pending = pendingBySession[sessionId];
  if (pending) {
    requestId = requestId || pending.requestId;
    requestKind = requestKind || pending.kind;
  }

  if (!sessionId || !text) {
    sendError("No reply text");
    return;
  }

  if ((requestKind === "permission" || (pending && pending.kind === "permission")) && requestId) {
    replyPermission(sessionId, requestId, text, function(done) {
      if (done) {
        send(makeMessage(CMD_PROMPT, {}));
        return;
      }
      promptSession(sessionId, text);
    });
    return;
  }

  if ((requestKind === "question" || (pending && pending.kind === "question")) && requestId) {
    replyQuestion(sessionId, requestId, text, pending ? pending.request : null);
    return;
  }

  if (shouldInterruptTurn(text)) {
    interruptSession(sessionId);
    return;
  }

  promptSession(sessionId, text);
}

function replyPermission(sessionId, requestId, text, callback) {
  var normalized = text.toLowerCase();
  var decision = null;
  if (/^(yes|yep|approve|allow|ok|okay|once|sure)\b/.test(normalized)) {
    decision = "accept";
  } else if (/^(always|forever|session)\b/.test(normalized)) {
    decision = "acceptForSession";
  } else if (/^(no|deny|reject|decline|stop)\b/.test(normalized)) {
    decision = "decline";
  } else if (/^(cancel|abort)\b/.test(normalized)) {
    decision = "cancel";
  }

  if (!decision) {
    callback(false);
    return;
  }

  dispatchForThread(sessionId, function(threadId) {
    return {
      type: "thread.approval.respond",
      commandId: randomId("pebble:approval:"),
      threadId: threadId,
      requestId: requestId,
      decision: decision,
      createdAt: new Date().toISOString()
    };
  }, function(error) {
    if (error) {
      sendError(error);
      callback(true);
      return;
    }
    callback(true);
  });
}

function replyQuestion(sessionId, requestId, text, request) {
  var answers = buildQuestionAnswers(request, text);
  if (!answers) {
    sendError("Say an option, or use semicolons for multiple questions");
    return;
  }

  dispatchForThread(sessionId, function(threadId) {
    return {
      type: "thread.user-input.respond",
      commandId: randomId("pebble:user-input:"),
      threadId: threadId,
      requestId: requestId,
      answers: questionAnswersObject(request, answers),
      createdAt: new Date().toISOString()
    };
  }, function(error) {
    if (error) {
      sendError(error);
      return;
    }
    send(makeMessage(CMD_PROMPT, {}));
  });
}

function questionAnswersObject(request, answers) {
  var result = {};
  var questions = request && request.questions ? request.questions : [];
  if (!questions.length) {
    result.answer = answers[0] && answers[0].length === 1 ? answers[0][0] : (answers[0] || []);
    return result;
  }
  for (var i = 0; i < questions.length; i++) {
    var value = answers[i] || [];
    result[questions[i].id] = value.length === 1 ? value[0] : value;
  }
  return result;
}

function buildQuestionAnswers(request, text) {
  if (!request || !request.questions || request.questions.length === 0) {
    return [[text]];
  }

  var questions = request.questions;
  var parts = questions.length > 1 ? text.split(/\s*;\s*/g) : [text];
  if (questions.length > 1 && parts.length < questions.length) {
    return null;
  }

  var answers = [];
  for (var i = 0; i < questions.length; i++) {
    var answer = buildSingleQuestionAnswer(questions[i], trim(parts[i] || ""));
    if (!answer) {
      return null;
    }
    answers.push(answer);
  }
  return answers;
}

function buildSingleQuestionAnswer(question, text) {
  var options = question.options || [];
  if (question.multiple) {
    var labels = matchMultipleOptions(options, text);
    if (labels.length > 0) {
      return labels;
    }
    return question.custom === false ? null : [text];
  }

  var label = matchSingleOption(options, text);
  if (label) {
    return [label];
  }
  return question.custom === false ? null : [text];
}

function matchSingleOption(options, text) {
  var normalized = normalize(text);
  for (var i = 0; i < options.length; i++) {
    var label = options[i].label || "";
    var option = normalize(label);
    if (normalized === option || normalized.indexOf(option) === 0 || option.indexOf(normalized) === 0) {
      return label;
    }
  }
  return null;
}

function matchMultipleOptions(options, text) {
  var normalized = normalize(text);
  var labels = [];
  for (var i = 0; i < options.length; i++) {
    var label = options[i].label || "";
    var option = normalize(label);
    if (normalized === option || normalized.indexOf(option) !== -1) {
      labels.push(label);
    }
  }
  return labels;
}

function normalize(text) {
  return String(text || "").toLowerCase().replace(/[^a-z0-9]+/g, " ").replace(/^\s+|\s+$/g, "");
}

function shouldInterruptTurn(text) {
  var normalized = normalize(text);
  return /^(stop|interrupt|cancel|abort|halt)( turn| run| agent| response| task)?$/.test(normalized);
}

function interruptSession(sessionId) {
  dispatchForThread(sessionId, function(threadId) {
    return {
      type: "thread.turn.interrupt",
      commandId: randomId("pebble:interrupt:"),
      threadId: threadId,
      createdAt: new Date().toISOString()
    };
  }, function(error) {
    if (error) {
      sendError(error);
      return;
    }
    send(makeMessage(CMD_PROMPT, {}));
  });
}

// --- creating a project from the watch --------------------------------

// Dictation gives back a phrase, not a directory name. Fold it to something a
// filesystem is happy with, without inventing characters the user did not say.
function projectSlug(text) {
  var slug = String(text || "")
    .toLowerCase()
    .replace(/['`’]/g, "")
    .replace(/[^a-z0-9]+/g, "-")
    .replace(/^-+|-+$/g, "");
  return slug.slice(0, 48);
}

function projectPathFor(server, name) {
  var slug = projectSlug(name);
  if (!server.projectRoot || !slug) {
    return "";
  }
  return server.projectRoot + "/" + slug;
}

// Step one: resolve the dictated name to an absolute path and hand it back for
// confirmation. Nothing is created until the watch sends it to CMD_PROJECT_CREATE.
function previewProject(hostId, name) {
  var server = serverById(hostId);
  if (!server) {
    sendError("Unknown host");
    return;
  }
  if (!server.projectRoot) {
    sendError("Set a project root in settings");
    return;
  }
  var path = projectPathFor(server, name);
  if (!path) {
    sendError("Could not read a project name");
    return;
  }
  send(makeMessage(CMD_PROJECT_PREVIEW, {
    hostId: hostId,
    name: compact(trim(name), 40),
    path: path
  }));
}

// Step two: the user approved the path shown on the glass, so create it. The
// watch dispatches this itself rather than delegating to an agent, so the
// confirmation is a real gate rather than a notification after the fact.
function createProject(hostId, name, path) {
  var server = serverById(hostId);
  if (!server) {
    sendError("Unknown host");
    return;
  }
  var workspaceRoot = trim(path) || projectPathFor(server, name);
  if (!workspaceRoot) {
    sendError("No project path");
    return;
  }
  var title = compact(trim(name) || projectSlug(name), 40);
  dispatchCommand(server, {
    type: "project.create",
    commandId: randomId("pebble:proj:"),
    projectId: randomId("pebble-project-"),
    title: title || "New project",
    workspaceRoot: workspaceRoot,
    createWorkspaceRootIfMissing: true,
    createdAt: new Date().toISOString()
  }, function(error) {
    if (error) {
      sendError(error);
      return;
    }
    shellByServer[hostId] = null;
    send(makeMessage(CMD_PROMPT, {}));
  });
}

// The concierge route: when the location is easier described than dictated,
// ask the designated project's agent to propose an absolute path. It only
// proposes -- createProject above still does the creating, after you approve.
function conciergeProject(hostId, text) {
  var server = serverById(hostId);
  if (!server) {
    sendError("Unknown host");
    return;
  }
  if (!server.concierge) {
    sendError("Set a concierge project in settings");
    return;
  }
  var ask = "The user asked, from their Pebble watch: \"" + trim(text) + "\"\n\n" +
    "They want a new T3 Code project created on this machine. Work out the " +
    "absolute path it should live at, using nearby checkouts for context. Do " +
    "not create anything. Reply with the last line of your response being " +
    "exactly:\n\nPROJECT_PATH: /absolute/path/here";
  promptNewThread(hostId + ID_SEPARATOR + server.concierge, ask);
}

// Settle marks a thread as no longer wanting the user; unsettle reopens it.
// `reason: "user"` is required by the server and is what makes the reopen
// stick as an explicit override rather than an auto-settle candidate.
function settleSession(sessionId, settled) {
  dispatchForThread(sessionId, function(threadId) {
    if (settled) {
      return {
        type: "thread.settle",
        commandId: randomId("pebble:settle:"),
        threadId: threadId
      };
    }
    return {
      type: "thread.unsettle",
      commandId: randomId("pebble:unsettle:"),
      threadId: threadId,
      reason: "user"
    };
  }, function(error) {
    if (error) {
      sendError(error);
      return;
    }
    // The list the watch is holding is now wrong in a way a redraw cannot fix,
    // so drop the cached snapshot and let the next poll refill it.
    invalidateHost(sessionId);
    send(makeMessage(CMD_PROMPT, {}));
  });
}

function promptSession(sessionId, text) {
  var instruction = "This message was sent from the user's Pebble watch through t3pebble. The user can open the full response, but they will mostly read the ending on the watch. End your reply with the last five sentences as a useful Pebble summary of what you did and what, if anything, you need from the user.";
  var prompt = text + "\n\n" + instruction;
  withThread(sessionId, function(error, thread) {
    if (error || !thread) {
      sendError(error || "Thread not found");
      return;
    }
    // modelSelection is deliberately omitted: it is optional on a turn start,
    // and leaving it out keeps whatever model the thread is already set to on
    // the server rather than having the watch impose one.
    dispatchForThread(sessionId, function(threadId) {
      return {
        type: "thread.turn.start",
        commandId: randomId("pebble:turn:"),
        threadId: threadId,
        message: {
          messageId: randomId("pebble:"),
          role: "user",
          text: prompt,
          attachments: []
        },
        runtimeMode: thread.runtimeMode,
        interactionMode: thread.interactionMode,
        createdAt: new Date().toISOString()
      };
    }, function(dispatchError) {
      if (dispatchError) {
        sendError(dispatchError);
        return;
      }
      send(makeMessage(CMD_PROMPT, {}));
      invalidateHost(sessionId);
    });
  });
}

function promptNewThread(projectId, text) {
  if (!projectId || !text) {
    sendError("No new thread prompt");
    return;
  }
  var instruction = "This message was sent from the user's Pebble watch through t3pebble. The user can open the full response, but they will mostly read the ending on the watch. End your reply with the last five sentences as a useful Pebble summary of what you did and what, if anything, you need from the user.";
  var prompt = text + "\n\n" + instruction;
  withProject(projectId, function(error, project) {
    if (error || !project) {
      sendError(error || "Project not found");
      return;
    }
    var server = serverById(project.serverId);
    if (!server) {
      sendError("Unknown T3 server");
      return;
    }
    var now = new Date().toISOString();
    var threadId = randomId("pebble-thread-");
    // A new thread has to name a model, so use the project's default from the
    // server and only fall back when it has none.
    var selection = resolveModelSelection(project.defaultModelSelection);
    // Two commands, not one. `thread.turn.start` accepts a
    // `bootstrap.createThread` block and the REST route validates it, but only
    // the WebSocket path routes such a command through the branch that creates
    // the thread. Over REST the bootstrap is silently ignored and the turn
    // lands on a thread that does not exist, which the server reports as a 500.
    // So create the thread first and start the turn against it.
    dispatchCommand(server, {
      type: "thread.create",
      commandId: randomId("pebble:new:"),
      threadId: threadId,
      projectId: project.nativeId,
      title: "New thread",
      modelSelection: selection,
      runtimeMode: "full-access",
      interactionMode: "default",
      branch: null,
      worktreePath: null,
      createdAt: now
    }, function(createError) {
      if (createError) {
        sendError(createError);
        return;
      }
      dispatchCommand(server, {
        type: "thread.turn.start",
        commandId: randomId("pebble:turn:"),
        threadId: threadId,
        message: {
          messageId: randomId("pebble:"),
          role: "user",
          text: prompt,
          attachments: []
        },
        modelSelection: selection,
        titleSeed: compact(text, 58),
        runtimeMode: "full-access",
        interactionMode: "default",
        createdAt: now
      }, function(dispatchError) {
        if (dispatchError) {
          sendError(dispatchError);
          return;
        }
        send(makeMessage(CMD_PROMPT, {}));
        invalidateHost(projectId);
      });
    });
  });
}

// Fetches (or reuses) the shell snapshot for whichever host owns an id.
function withShellFor(compositeId, callback) {
  var serverId = splitCompositeId(compositeId).serverId;
  var server = serverById(serverId);
  if (!server) {
    callback(new Error("Unknown T3 server"));
    return;
  }
  var cached = shellByServer[serverId];
  if (cached) {
    callback(null, cached);
    return;
  }
  httpRequest(server, "GET", T3_SHELL_PATH, null, function(error, snapshot) {
    if (error) {
      callback(error);
      return;
    }
    shellByServer[serverId] = tagSnapshot(server, snapshot);
    callback(null, shellByServer[serverId]);
  });
}

function withThread(threadId, callback) {
  withShellFor(threadId, function(error, shell) {
    if (error) {
      callback(error);
      return;
    }
    callback(null, threadById(shell, threadId));
  });
}

function withProject(projectId, callback) {
  withShellFor(projectId, function(error, shell) {
    if (error) {
      callback(error);
      return;
    }
    var projects = (shell && shell.projects) || [];
    for (var i = 0; i < projects.length; i++) {
      if (projects[i].id === projectId) {
        callback(null, projects[i]);
        return;
      }
    }
    callback(null, null);
  });
}

// A dispatch changes the host's state, so drop its cached shell and let the
// next screen re-read it rather than showing a stale roll-up.
function invalidateHost(compositeId) {
  var serverId = splitCompositeId(compositeId).serverId;
  if (serverId) {
    delete shellByServer[serverId];
  }
}

// One pasteable line per machine, as printed by run-t3code-tailscale.sh:
//   t3pebble1|<label>|<base URL>|<token>
// Deliberately free of helper calls: configurationHtml() embeds this function's
// own source into the settings page, so the page parses by exactly these rules.
function parseServerBundle(text) {
  var found = [];
  var lines = String(text === null || text === undefined ? "" : text).split(/[\r\n]+/);
  for (var i = 0; i < lines.length; i++) {
    var line = lines[i].replace(/^\s+|\s+$/g, "");
    if (line.indexOf("t3pebble1|") !== 0) {
      continue;
    }
    var parts = line.slice(10).split("|");
    if (parts.length < 3) {
      continue;
    }
    var label = parts[0].replace(/^\s+|\s+$/g, "");
    var baseUrl = parts[1].replace(/^\s+|\s+$/g, "").replace(/\/+$/, "");
    var token = parts[2].replace(/^\s+|\s+$/g, "");
    // Field four is optional, so lines written before it existed still parse.
    var projectRoot = (parts[3] || "").replace(/^\s+|\s+$/g, "").replace(/\/+$/, "");
    if (!baseUrl || !token) {
      continue;
    }
    found.push({ id: "", label: label, baseUrl: baseUrl, token: token, projectRoot: projectRoot });
  }
  return found;
}

// JSON dropped straight into a <script> can close it early: a token or label
// containing "</script>" would end the block and spill the rest of the page as
// markup. Escaping the slash keeps the string identical to the parser and
// inert to the tokenizer. U+2028/9 are legal in JSON but not in JS source.
function embedJson(value) {
  return JSON.stringify(value)
    .replace(/</g, "\\u003c")
    .replace(/\u2028/g, "\\u2028")
    .replace(/\u2029/g, "\\u2029");
}

function configurationHtml() {
  var current = settings();
  var seed = current.servers.length ? current.servers : [{ id: "", label: "", baseUrl: "", token: "", projectRoot: "", concierge: "" }];
  return [
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>",
    "<style>body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;margin:20px;background:#f7f7f2;color:#111}",
    "label{display:block;margin:12px 0 4px;font-weight:600;font-size:14px}",
    "input,textarea{box-sizing:border-box;width:100%;font-size:16px;padding:10px;border:1px solid #999;border-radius:6px}",
    "textarea{font-family:ui-monospace,Menlo,monospace;font-size:13px}",
    "code{font-size:12px;background:#eee;padding:1px 4px;border-radius:3px}",
    ".card{background:#fff;border:1px solid #ddd;border-radius:10px;padding:14px;margin-top:14px}",
    ".card h3{margin:0;font-size:15px}.head{display:flex;justify-content:space-between;align-items:center}",
    ".rm{width:auto;margin:0;padding:6px 10px;font-size:13px;background:#c0392b}",
    "button{margin-top:18px;width:100%;font-size:17px;padding:12px;border:0;border-radius:6px;background:#111;color:white}",
    ".add{background:#2d6cdf}p.hint{margin:6px 0 0;font-size:13px;color:#555}</style></head><body>",
    "<h2>T3 Pebble</h2>",
    "<p class='hint'>Add one entry per machine running T3 Code. The label is what the watch shows.</p>",
    "<div class='card'><h3>Quick setup</h3>",
    "<p class='hint'>Run <code>run-t3code-tailscale.sh</code> on each machine and paste the ",
    "<code>t3pebble1|...</code> lines it prints. One line per machine; pasting a line again ",
    "refreshes that machine's token instead of adding a duplicate.</p>",
    "<textarea id='bundle' rows='4' placeholder='t3pebble1|beta1|https://beta1.tailnet.ts.net|token'></textarea>",
    "<button class='add' onclick='addFromPaste()'>Add from paste</button>",
    "<p class='hint' id='pasteMsg'></p></div>",
    "<div id='servers'></div>",
    "<button class='add' onclick='addServer()'>+ Add server</button>",
    "<button onclick='save()'>Save</button>",
    "<script>",
    "var MAX=", String(MAX_SERVERS), ";",
    "var servers=", embedJson(seed), ";",
    "var parseServerBundle=", String(parseServerBundle), ";",
    "function esc(v){return String(v==null?'':v).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/\"/g,'&quot;').replace(/'/g,'&#39;');}",
    "function render(){var h='';for(var i=0;i<servers.length;i++){var s=servers[i];",
    "h+=\"<div class='card'><div class='head'><h3>Server \"+(i+1)+\"</h3>\";",
    "if(servers.length>1){h+=\"<button class='rm' onclick='removeServer(\"+i+\")'>Remove</button>\";}",
    "h+=\"</div><label>Label</label><input data-i='\"+i+\"' data-f='label' placeholder='laptop' value='\"+esc(s.label)+\"'>\";",
    "h+=\"<label>Base URL</label><input data-i='\"+i+\"' data-f='baseUrl' placeholder='https://host.ts.net or http://100.x.y.z:3773' value='\"+esc(s.baseUrl)+\"'>\";",
    "h+=\"<label>Access token</label><input data-i='\"+i+\"' data-f='token' type='password' value='\"+esc(s.token)+\"'>\";",
    "h+=\"<label>Project root</label><input data-i='\"+i+\"' data-f='projectRoot' placeholder='/home/you/Projects' value='\"+esc(s.projectRoot||'')+\"'>\";",
    "h+=\"<p class='hint'>Where a project dictated from the watch is created. Leave blank to turn that off.</p>\";",
    "h+=\"<label>Concierge project id</label><input data-i='\"+i+\"' data-f='concierge' placeholder='optional' value='\"+esc(s.concierge||'')+\"'>\";",
    "h+=\"<p class='hint'>Optional. An agent in this project proposes a path when you would rather describe the location than dictate it.</p>\";",
    "h+=\"<p class='hint'>Issue one with <code>t3 auth session issue --token-only</code>.</p></div>\";}",
    "document.getElementById('servers').innerHTML=h;",
    "var inputs=document.querySelectorAll('#servers input');",
    "for(var j=0;j<inputs.length;j++){inputs[j].addEventListener('input',function(e){",
    "servers[+e.target.getAttribute('data-i')][e.target.getAttribute('data-f')]=e.target.value;});}}",
    "function addFromPaste(){var ta=document.getElementById('bundle');var msg=document.getElementById('pasteMsg');",
    "var found=parseServerBundle(ta.value);",
    "if(!found.length){msg.textContent='No setup lines found. Paste the t3pebble1|... line.';return;}",
    "if(servers.length===1&&!servers[0].baseUrl&&!servers[0].token){servers=[];}",
    "var added=0,updated=0,skipped=0;",
    "for(var i=0;i<found.length;i++){var f=found[i];var hit=-1;",
    "for(var j=0;j<servers.length;j++){if(servers[j].baseUrl===f.baseUrl){hit=j;break;}}",
    "if(hit>=0){if(f.label){servers[hit].label=f.label;}servers[hit].token=f.token;",
    "if(f.projectRoot){servers[hit].projectRoot=f.projectRoot;}updated++;continue;}",
    "if(servers.length>=MAX){skipped++;continue;}",
    "servers.push({id:'',label:f.label,baseUrl:f.baseUrl,token:f.token,projectRoot:f.projectRoot||'',concierge:''});added++;}",
    "ta.value='';",
    "msg.textContent=added+' added, '+updated+' updated'+(skipped?', '+skipped+' skipped (max '+MAX+')':'');",
    "render();}",
    "function addServer(){if(servers.length>=MAX){return;}servers.push({id:'',label:'',baseUrl:'',token:'',projectRoot:'',concierge:''});render();}",
    "function removeServer(i){servers.splice(i,1);if(!servers.length){servers.push({id:'',label:'',baseUrl:'',token:'',projectRoot:'',concierge:''});}render();}",
    "function save(){location.href='pebblejs://close#'+encodeURIComponent(JSON.stringify({servers:servers}));}",
    "render();",
    "</script></body></html>"
  ].join("");
}

Pebble.addEventListener("ready", function() {
  if (SCREENSHOT_FIXTURES) {
    runScreenshotStoryboard();
    return;
  }
  refreshHosts();
});

Pebble.addEventListener("appmessage", function(event) {
  var message = event.payload || {};
  var command = message[KEY_CMD];
  if (command === CMD_REFRESH) {
    refreshHosts();
  } else if (command === CMD_SELECT_HOST) {
    selectHost(message[KEY_HOST_ID], message[KEY_SCOPE] || 0, message[KEY_OFFSET] || 0);
  } else if (command === CMD_THREAD_ACTION) {
    var action = message[KEY_ACTION];
    if (action === "settle" || action === "unsettle") {
      settleSession(message[KEY_SESSION_ID], action === "settle");
    } else if (action === "interrupt") {
      interruptSession(message[KEY_SESSION_ID]);
    } else {
      sendError("Unknown action");
    }
  } else if (command === CMD_PROJECT_NAME) {
    previewProject(message[KEY_HOST_ID], trim(message[KEY_NAME]));
  } else if (command === CMD_PROJECT_CREATE) {
    createProject(message[KEY_HOST_ID], trim(message[KEY_NAME]), trim(message[KEY_PATH]));
  } else if (command === CMD_CONCIERGE) {
    conciergeProject(message[KEY_HOST_ID], trim(message[KEY_PROMPT]));
  } else if (command === CMD_DETAIL) {
    detail(message[KEY_SESSION_ID], message[KEY_INDEX] || 0);
  } else if (command === CMD_CONTEXT) {
    context(message[KEY_SESSION_ID], message[KEY_INDEX] || 0, message[KEY_CONTEXT_PAGE] || 0, message[KEY_REQUEST_ID]);
  } else if (command === CMD_PROMPT) {
    submitResponse(message);
  } else if (command === CMD_NEW_THREAD) {
    promptNewThread(message[KEY_PROJECT_ID], trim(message[KEY_PROMPT]));
  }
});

Pebble.addEventListener("showConfiguration", function() {
  Pebble.openURL("data:text/html," + encodeURIComponent(configurationHtml()));
});

Pebble.addEventListener("webviewclosed", function(event) {
  if (!event || !event.response) {
    return;
  }
  try {
    saveSettings(JSON.parse(decodeURIComponent(event.response)));
    shellByServer = {};
    // A host may have been added, removed or relabelled, so nothing the watch
    // is holding can be assumed still current.
    lastHostRow = {};
    refreshHosts();
  } catch (e) {
    sendError("Settings not saved");
  }
});



//////////////////
// WEBPACK FOOTER
// ./src/pkjs/index.js
// module id = 2
// module chunks = 0
