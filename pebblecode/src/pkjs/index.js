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

var MAX_SESSIONS = 20;
var SUMMARY_LIMIT = 150;
var CONTEXT_LIMIT = 480;
var CONTEXT_MESSAGE_LIMIT = 60;
var DETAIL_MESSAGE_LIMIT = 10;
var ENRICH_CONCURRENCY = 4;
var HYDRATE_CONCURRENCY = 4;
// Kept equal to MAX_SERVERS so the host list is always one batch. Laptops sleep,
// and a sleeping host costs a full timeout; batching would make hosts wait
// behind that instead of alongside it.
var SERVER_CONCURRENCY = 6;
var MAX_APP_MESSAGE_FAILURES = 8;
var BUILD_LABEL = "v0.3.0";
var DEFAULT_BASE_URL = "";
var DEFAULT_TOKEN = "";
var HTTP_TIMEOUT_MS = 20000;
// The host list is one cheap request per machine, so it does not need the full
// budget. A shorter ceiling keeps a sleeping laptop from stalling the first
// paint for every other host.
var HOST_PROBE_TIMEOUT_MS = 8000;
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

var appMessageQueue = [];
var appMessageBusy = false;
var appMessageFailureCount = 0;
var pendingBySession = {};
var runtimeStatusBySession = {};
var cachedSessions = {};
var cachedProjects = {};
var lastSnapshot = null;
var lastSnapshotFailures = [];
var shellByServer = {};

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
    token: trim(server.token || DEFAULT_TOKEN)
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

function httpRequest(server, method, path, body, callback, timeoutMs) {
  if (!server || !server.baseUrl) {
    callback(new Error("Configure remote URL"));
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

  function finish(error, value) {
    if (done) {
      return;
    }
    done = true;
    clearTimeout(timer);
    callback(error, value);
  }

  var timer = setTimeout(function() {
    try {
      request.abort();
    } catch (e) {
      void e;
    }
    finish(new Error("T3 request timeout"));
  }, timeoutMs || HTTP_TIMEOUT_MS);

  request.onreadystatechange = function() {
    if (request.readyState !== 4 || done) {
      return;
    }
    var status = request.status;
    if (!status) {
      finish(new Error("T3 unreachable"));
      return;
    }
    if (status < 200 || status >= 300) {
      finish(new Error(httpFailureMessage(status, request.responseText)));
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

function httpFailureMessage(status, responseText) {
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
    return detail === "thread_not_found" ? "Thread not found" : "T3 route not found";
  }
  return "T3 request failed (" + status + (detail ? " " + detail : "") + ")";
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

function threadDetailLine(thread, state, nowMs) {
  if (state === "needs") {
    return thread.hasPendingApprovals ? "needs approval" : "needs an answer";
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
          detail: compact(String(error.message || error), 34),
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
    // need different fixes, and the watch is where that is read.
    var down = [];
    for (var f = 0; f < rows.length; f++) {
      if (rows[f] && rows[f].state === "offline") {
        down.push(rows[f].title + ": " + rows[f].detail);
      }
    }
    if (down.length) {
      sendError(compact(down.join(" / "), 60));
    }
    for (var i = 0; i < rows.length; i++) {
      send(makeMessage(CMD_HOST_ITEM, {
        index: i,
        total: rows.length,
        hostId: rows[i].id,
        title: rows[i].title,
        detail: rows[i].detail,
        state: rows[i].state,
        counts: rows[i].counts
      }));
    }
    send(makeMessage(CMD_HOST_END, { total: rows.length }));
  });
}

// --- one host's threads -----------------------------------------------

function selectHost(hostId) {
  var server = serverById(hostId);
  if (!server) {
    sendError("Unknown host");
    send(makeMessage(CMD_SESSION_END, { total: 0 }));
    return;
  }

  function emit(tagged) {
    lastSnapshot = tagged;
    var nowMs = Date.now();
    var threads = activeThreads(tagged);
    sendSessionItems(threads.map(function(thread) {
      var state = threadState(thread, nowMs);
      return {
        id: thread.id,
        title: compact(thread.title || "Untitled", 54),
        detail: threadDetailLine(thread, state, nowMs),
        state: state,
        summary: "",
        requestId: "",
        requestKind: state === "needs" ? (thread.hasPendingApprovals ? "permission" : "question") : ""
      };
    }));
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

function sendSessionItems(items) {
  cachedSessions = {};
  for (var i = 0; i < items.length; i++) {
    cachedSessions[items[i].id] = items[i];
    send(makeMessage(CMD_SESSION_ITEM, itemFields(items[i], i, items.length)));
  }
  send(makeMessage(CMD_SESSION_END, { total: items.length }));
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
    summary: item.summary || "",
    requestId: item.requestId || "",
    requestKind: item.requestKind || ""
  };
}

function detail(sessionId, index) {
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

    var item = {
      id: sessionId,
      title: compact(thread.title || "Untitled", 54),
      detail: threadDetailLine(lifecycle, state, nowMs),
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
    if (!baseUrl || !token) {
      continue;
    }
    found.push({ id: "", label: label, baseUrl: baseUrl, token: token });
  }
  return found;
}

function configurationHtml() {
  var current = settings();
  var seed = current.servers.length ? current.servers : [{ id: "", label: "", baseUrl: "", token: "" }];
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
    "var servers=", JSON.stringify(seed), ";",
    "var parseServerBundle=", String(parseServerBundle), ";",
    "function esc(v){return String(v==null?'':v).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/\"/g,'&quot;').replace(/'/g,'&#39;');}",
    "function render(){var h='';for(var i=0;i<servers.length;i++){var s=servers[i];",
    "h+=\"<div class='card'><div class='head'><h3>Server \"+(i+1)+\"</h3>\";",
    "if(servers.length>1){h+=\"<button class='rm' onclick='removeServer(\"+i+\")'>Remove</button>\";}",
    "h+=\"</div><label>Label</label><input data-i='\"+i+\"' data-f='label' placeholder='laptop' value='\"+esc(s.label)+\"'>\";",
    "h+=\"<label>Base URL</label><input data-i='\"+i+\"' data-f='baseUrl' placeholder='https://host.ts.net or http://100.x.y.z:3773' value='\"+esc(s.baseUrl)+\"'>\";",
    "h+=\"<label>Access token</label><input data-i='\"+i+\"' data-f='token' type='password' value='\"+esc(s.token)+\"'>\";",
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
    "if(hit>=0){if(f.label){servers[hit].label=f.label;}servers[hit].token=f.token;updated++;continue;}",
    "if(servers.length>=MAX){skipped++;continue;}",
    "servers.push({id:'',label:f.label,baseUrl:f.baseUrl,token:f.token});added++;}",
    "ta.value='';",
    "msg.textContent=added+' added, '+updated+' updated'+(skipped?', '+skipped+' skipped (max '+MAX+')':'');",
    "render();}",
    "function addServer(){if(servers.length>=MAX){return;}servers.push({id:'',label:'',baseUrl:'',token:''});render();}",
    "function removeServer(i){servers.splice(i,1);if(!servers.length){servers.push({id:'',label:'',baseUrl:'',token:''});}render();}",
    "function save(){location.href='pebblejs://close#'+encodeURIComponent(JSON.stringify({servers:servers}));}",
    "render();",
    "</script></body></html>"
  ].join("");
}

Pebble.addEventListener("ready", function() {
  refreshHosts();
});

Pebble.addEventListener("appmessage", function(event) {
  var message = event.payload || {};
  var command = message[KEY_CMD];
  if (command === CMD_REFRESH) {
    refreshHosts();
  } else if (command === CMD_SELECT_HOST) {
    selectHost(message[KEY_HOST_ID]);
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