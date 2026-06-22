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

var MAX_SESSIONS = 20;
var SUMMARY_LIMIT = 150;
var CONTEXT_LIMIT = 480;
var CONTEXT_MESSAGE_LIMIT = 60;
var ENRICH_CONCURRENCY = 4;
var MAX_APP_MESSAGE_FAILURES = 8;
var BUILD_LABEL = "v0.1.0";
var DEFAULT_BASE_URL = "";
var DEFAULT_USERNAME = "t3code";
var DEFAULT_PASSWORD = "";
var T3_GET_SNAPSHOT = "orchestration.getSnapshot";
var T3_DISPATCH_COMMAND = "orchestration.dispatchCommand";
var PEBBLE_CODEX_MODEL_FALLBACK = "gpt-5.5";
var DEFAULT_SETTINGS = {
  baseUrl: DEFAULT_BASE_URL,
  username: DEFAULT_USERNAME,
  password: DEFAULT_PASSWORD
};

var appMessageQueue = [];
var appMessageBusy = false;
var appMessageFailureCount = 0;
var pendingBySession = {};
var runtimeStatusBySession = {};
var cachedSessions = {};
var cachedProjects = {};
var lastSnapshot = null;

function settings() {
  migrateSettings();
  var raw = localStorage.getItem("t3pebble_settings");
  if (!raw) {
    return copySettings(DEFAULT_SETTINGS);
  }
  try {
    var parsed = JSON.parse(raw);
    return {
      baseUrl: normalizeBaseUrl(parsed.baseUrl),
      username: trim(parsed.username || DEFAULT_USERNAME),
      password: parsed.password || DEFAULT_PASSWORD
    };
  } catch (e) {
    return copySettings(DEFAULT_SETTINGS);
  }
}

function migrateSettings() {
  if (localStorage.getItem("t3pebble_build_label") === BUILD_LABEL) {
    return;
  }
  var raw = localStorage.getItem("t3pebble_settings");
  var shouldReset = !raw;
  if (raw) {
    try {
      var parsed = JSON.parse(raw);
      var baseUrl = trim(parsed.baseUrl || "");
      shouldReset = !baseUrl || /^https?:\/\/wills-macbook-pro-5(?::4096)?\/?$/i.test(baseUrl);
    } catch (e) {
      shouldReset = true;
    }
  }
  localStorage.setItem("t3pebble_build_label", BUILD_LABEL);
  if (shouldReset) {
    localStorage.setItem("t3pebble_settings", JSON.stringify(DEFAULT_SETTINGS));
  }
}

function copySettings(source) {
  return {
    baseUrl: source.baseUrl,
    username: source.username,
    password: source.password
  };
}

function saveSettings(next) {
  localStorage.setItem("t3pebble_settings", JSON.stringify({
    baseUrl: normalizeBaseUrl(next.baseUrl),
    username: trim(next.username || DEFAULT_USERNAME),
    password: next.password || DEFAULT_PASSWORD
  }));
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

function compactLine(value, limit) {
  value = String(value || "").replace(/\s+/g, " ").replace(/^\s+|\s+$/g, "");
  if (value.length <= limit) {
    return value;
  }
  if (limit <= 3) {
    return value.slice(0, limit);
  }
  return value.slice(0, limit - 3) + "...";
}

function compactMultilineTail(value, limit) {
  value = String(value || "").replace(/\r/g, "").replace(/[ \t]+\n/g, "\n").replace(/\n[ \t]+/g, "\n").replace(/\n{3,}/g, "\n\n").replace(/^\s+|\s+$/g, "");
  if (value.length <= limit) {
    return value;
  }
  if (limit <= 3) {
    return value.slice(value.length - limit);
  }
  return "...\n" + value.slice(value.length - limit + 4).replace(/^\s+/, "");
}

function utf8Length(value) {
  value = String(value || "");
  if (typeof Buffer !== "undefined") {
    return Buffer.byteLength(value, "utf8");
  }
  return unescape(encodeURIComponent(value)).length;
}

function trimToUtf8Bytes(value, byteLimit) {
  return trimToUtf8BytesWithIndex(value, byteLimit).text;
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

function responseList(response) {
  if (response && response.data && response.data.length !== undefined) {
    return response.data;
  }
  if (response && response.length !== undefined) {
    return response;
  }
  return [];
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

function sessionDirectory(session) {
  return (session && session.fullDirectory) || (session && session.location && (session.location.directory || session.location.path)) || (session && session.directory) || "";
}

function wsUrl(config) {
  var baseUrl = normalizeBaseUrl(config.baseUrl);
  if (!baseUrl) {
    return "";
  }
  var url = baseUrl.replace(/^http:/i, "ws:").replace(/^https:/i, "wss:");
  url = url.replace(/\/+$/, "") + "/ws";
  if (config.password) {
    url += (url.indexOf("?") === -1 ? "?" : "&") + "token=" + encodeURIComponent(config.password);
  }
  return url;
}

function randomId(prefix) {
  var random = Math.floor(Math.random() * 1000000000).toString(36);
  return prefix + Date.now().toString(36) + random;
}

function rpcRequestId() {
  return String(Date.now()) + String(Math.floor(Math.random() * 1000000));
}

function rpcRequest(method, payload, callback) {
  var config = settings();
  if (!config.baseUrl) {
    callback(new Error("Configure remote URL"));
    return;
  }
  if (typeof WebSocket === "undefined") {
    callback(new Error("Phone WebSocket unavailable"));
    return;
  }

  var socket = null;
  var requestId = rpcRequestId();
  var done = false;
  function finish(error, response) {
    if (done) {
      return;
    }
    done = true;
    clearTimeout(timer);
    try {
      if (socket) {
        socket.close();
      }
    } catch (e) {
      void e;
    }
    callback(error, response);
  }

  var timer = setTimeout(function() {
    finish(new Error("T3 WebSocket timeout"));
  }, 20000);

  function handleMessage(event) {
    var raw = event && event.data !== undefined ? event.data : event;
    var message = null;
    try {
      message = JSON.parse(raw);
    } catch (e) {
      return;
    }
    if (!message || message.requestId !== requestId || message._tag !== "Exit") {
      return;
    }
    var exit = message.exit || {};
    if (exit._tag === "Success") {
      finish(null, exit.value);
      return;
    }
    finish(new Error(rpcFailureMessage(exit)));
  }

  try {
    socket = new WebSocket(wsUrl(config));
  } catch (e) {
    finish(e);
    return;
  }

  socket.onopen = function() {
    socket.send(JSON.stringify({
      _tag: "Request",
      id: requestId,
      tag: method,
      payload: payload || {},
      headers: []
    }));
  };
  socket.onmessage = handleMessage;
  socket.onerror = function() {
    finish(new Error("T3 WebSocket error"));
  };
  socket.onclose = function() {
    if (!done) {
      finish(new Error("T3 WebSocket closed"));
    }
  };
  if (socket.addEventListener) {
    socket.addEventListener("message", handleMessage);
  }
}

function rpcFailureMessage(exit) {
  if (!exit) {
    return "T3 request failed";
  }
  if (exit.cause && exit.cause.pretty) {
    return exit.cause.pretty;
  }
  if (exit.cause && exit.cause.message) {
    return exit.cause.message;
  }
  if (exit.error && exit.error.message) {
    return exit.error.message;
  }
  return "T3 request failed";
}

function getSnapshot(callback) {
  rpcRequest(T3_GET_SNAPSHOT, {}, function(error, snapshot) {
    if (!error) {
      lastSnapshot = snapshot;
    }
    callback(error, snapshot);
  });
}

function dispatchCommand(command, callback) {
  rpcRequest(T3_DISPATCH_COMMAND, command, callback);
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
  return message;
}

function projectById(snapshot) {
  var map = {};
  var projects = snapshot && snapshot.projects ? snapshot.projects : [];
  for (var i = 0; i < projects.length; i++) {
    map[projects[i].id] = projects[i];
  }
  return map;
}

function newestFirst(values) {
  return values.slice().sort(function(left, right) {
    return (Date.parse(right.updatedAt || right.createdAt || "") || 0) - (Date.parse(left.updatedAt || left.createdAt || "") || 0);
  });
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

function projectForThread(snapshot, thread) {
  return projectById(snapshot)[thread.projectId] || null;
}

function projectTitle(project) {
  return compact((project && project.title) || fileName(project && project.workspaceRoot) || "Project", 58);
}

function threadDirectory(snapshot, thread) {
  var project = projectForThread(snapshot, thread);
  return thread.worktreePath || (project && project.workspaceRoot) || "";
}

function modelSelection(selection) {
  if (!selection) {
    return { provider: "codex", model: PEBBLE_CODEX_MODEL_FALLBACK };
  }
  if (selection.provider === "codex") {
    return {
      provider: selection.provider,
      model: PEBBLE_CODEX_MODEL_FALLBACK,
      options: selection.options
    };
  }
  return selection;
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

function statusForThread(thread) {
  if (derivePendingApprovals(thread.activities).length || derivePendingUserInputs(thread.activities).length) {
    return "Needs input";
  }
  if ((thread.session && thread.session.status === "error") || (thread.latestTurn && thread.latestTurn.state === "error")) {
    return "Error";
  }
  if (thread.session && (thread.session.status === "running" || thread.session.status === "starting")) {
    return "Running";
  }
  var messages = newestFirst(thread.messages || []);
  for (var i = 0; i < messages.length; i++) {
    if (messages[i].role === "assistant" && !messages[i].streaming && trim(messages[i].text)) {
      return "Done";
    }
  }
  if (thread.latestTurn && thread.latestTurn.state === "completed") {
    return "Done";
  }
  if (thread.latestTurn && thread.latestTurn.state === "running") {
    return "Running";
  }
  return "Idle";
}

function sessionSummaryFromThread(thread) {
  var approval = derivePendingApprovals(thread.activities)[0];
  if (approval) {
    return pendingSummary({
      kind: "permission",
      request: {
        permission: approval.requestKind,
        patterns: approval.detail ? [approval.detail] : []
      }
    });
  }

  var input = derivePendingUserInputs(thread.activities)[0];
  if (input) {
    return pendingSummary({
      kind: "question",
      request: input
    });
  }

  var errorDetail = latestErrorDetail(thread);
  if (errorDetail) {
    return compact("Error: " + errorDetail, SUMMARY_LIMIT);
  }

  return summaryFromMessages((thread.messages || []).map(toPebbleMessage));
}

function toPebbleSession(snapshot, thread) {
  var directory = threadDirectory(snapshot, thread);
  var approval = derivePendingApprovals(thread.activities)[0];
  var input = derivePendingUserInputs(thread.activities)[0];
  return {
    id: thread.id,
    title: compact(thread.title || fileName(directory) || "Untitled", 58),
    directory: compact(directory, 42),
    fullDirectory: directory,
    agent: compact((thread.session && thread.session.providerName) || (thread.modelSelection && thread.modelSelection.provider) || "agent", 20),
    status: compact(statusForThread(thread), 16),
    summary: sessionSummaryFromThread(thread),
    requestId: approval ? approval.requestId : (input ? input.requestId : ""),
    requestKind: approval ? "permission" : (input ? "question" : "")
  };
}

function toPebbleProject(project) {
  var selection = modelSelection(project.defaultModelSelection);
  return {
    id: project.id,
    title: projectTitle(project),
    directory: project.workspaceRoot || "",
    model: selection.model || "codex"
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

function refreshSessions() {
  pendingBySession = {};
  runtimeStatusBySession = {};
  sendStatus(BUILD_LABEL + " " + urlHost(settings().baseUrl));
  listSessions(function(error, sessions, source, diagnostic) {
    if (error) {
      sendError(error);
      return;
    }

    sessions = sessions.slice(0, MAX_SESSIONS);
    sendStatus(sessionCountStatus(sessions.length, source));
    if (sessions.length === 0) {
      cachedSessions = {};
      send(makeMessage(CMD_SESSION_END, { total: 0 }));
      refreshProjects();
      if (diagnostic) {
        sendStatus(diagnostic);
      }
      return;
    }

    if (Object.keys(cachedSessions).length === 0) {
      sendSessionItems(sessions.map(function(session) {
        return toQuickItem(session);
      }));
    } else {
      send(makeMessage(CMD_SESSION_END, { total: sessions.length }));
    }

    loadRuntimeStatusForSessions(sessions, function() {
      loadPendingForSessions(sessions, function() {
        loadSessionSummaries(sessions, 0, function(items) {
          sendSessionItems(items);
          refreshProjects();
        });
      });
    });
  });
}

function sessionCountStatus(count, source) {
  return "Found " + count + " session" + (count === 1 ? "" : "s") + (source ? " via " + source : "");
}

function listSessions(callback) {
  getSnapshot(function(error, snapshot) {
    if (error) {
      callback(error);
      return;
    }
    var sessions = activeThreads(snapshot).map(function(thread) {
      return toPebbleSession(snapshot, thread);
    });
    callback(null, sessions, "t3 ws", "t3 ws " + sessions.length);
  });
}

function urlHost(url) {
  return String(url || "").replace(/^https?:\/\//i, "").replace(/\/.*$/, "") || "no URL";
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

function refreshProjects() {
  if (!lastSnapshot) {
    send(makeMessage(CMD_PROJECT_END, { total: 0 }));
    return;
  }
  sendProjectItems(activeProjects(lastSnapshot).map(toPebbleProject));
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
      agent: items[i].model || "codex"
    }));
  }
  send(makeMessage(CMD_PROJECT_END, { total: items.length }));
}

function loadRuntimeStatusForSessions(sessions, callback) {
  runtimeStatusBySession = {};
  if (lastSnapshot) {
    var threads = activeThreads(lastSnapshot);
    for (var i = 0; i < threads.length; i++) {
      var status = statusForThread(threads[i]);
      if (status === "Running") {
        runtimeStatusBySession[threads[i].id] = { type: "busy" };
      } else if (status === "Error") {
        runtimeStatusBySession[threads[i].id] = { type: "error", message: latestErrorDetail(threads[i]) || "Error" };
      }
    }
  }
  callback();
}

function loadRuntimeStatusDirectories(directories, callback) {
  void directories;
  if (!lastSnapshot) {
    getSnapshot(function() {
      loadRuntimeStatusForSessions([], callback);
    });
    return;
  }
  loadRuntimeStatusForSessions([], callback);
}

function loadPendingForSessions(sessions, callback) {
  pendingBySession = {};
  if (lastSnapshot) {
    var threads = activeThreads(lastSnapshot);
    for (var i = 0; i < threads.length; i++) {
      var approvals = derivePendingApprovals(threads[i].activities);
      var inputs = derivePendingUserInputs(threads[i].activities);
      if (approvals.length) {
        approvals[0].sessionID = threads[i].id;
        rememberPending(threads[i].id, "permission", approvals[0]);
      } else if (inputs.length) {
        inputs[0].sessionID = threads[i].id;
        rememberPending(threads[i].id, "question", inputs[0]);
      }
    }
  }
  callback();
}

function loadPendingDirectories(directories, callback) {
  void directories;
  if (!lastSnapshot) {
    getSnapshot(function() {
      loadPendingForSessions([], callback);
    });
    return;
  }
  loadPendingForSessions([], callback);
}

function loadPending(directory, callback) {
  void directory;
  loadPendingForSessions([], callback);
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

function loadSessionSummaries(sessions, index, done) {
  void index;
  var items = new Array(sessions.length);
  eachLimit(sessions, ENRICH_CONCURRENCY, function(session, itemIndex, next) {
    loadSessionItem(session, function(item) {
      items[itemIndex] = item;
      next();
    });
  }, function() {
    done(items);
  });
}

function loadSessionItem(session, callback) {
  loadMessages(session.id, 8, sessionDirectory(session), function(error, response) {
    var messages = error ? [] : responseList(response);
    callback(toItem(session, messages));
  });
}

function loadMessages(sessionId, limit, directory, callback) {
  void directory;
  if (!lastSnapshot) {
    getSnapshot(function(error) {
      if (error) {
        callback(error);
        return;
      }
      loadMessages(sessionId, limit, directory, callback);
    });
    return;
  }
  var thread = threadById(lastSnapshot, sessionId);
  if (!thread) {
    callback(new Error("Thread not found"));
    return;
  }
  var messages = (thread.messages || []).slice().sort(function(left, right) {
    return messageTime(toPebbleMessage(right)) - messageTime(toPebbleMessage(left));
  }).slice(0, limit).map(toPebbleMessage);
  callback(null, messages);
}

function toQuickItem(session) {
  var directory = sessionDirectory(session);
  var title = session.title || fileName(directory) || "Untitled";
  return {
    id: session.id,
    title: compact(title, 58),
    directory: compact(directory, 42),
    fullDirectory: directory,
    agent: compact(session.agent || "agent", 20),
    status: compact(session.status || "Idle", 16),
    summary: compact(session.summary || "Awaiting latest signal.", SUMMARY_LIMIT),
    requestId: session.requestId || "",
    requestKind: session.requestKind || ""
  };
}

function toItem(session, messages) {
  var pending = pendingBySession[session.id];
  var runtimeStatus = runtimeStatusBySession[session.id];
  var status = pending ? "Needs input" : (statusFromRuntimeStatus(runtimeStatus) || session.status || statusFromMessages(messages));
  var summary = pending ? pendingSummary(pending) : summaryFromMessages(messages);
  if (!pending && runtimeStatus && summary === "No messages yet.") {
    summary = summaryFromRuntimeStatus(runtimeStatus);
  }
  if (!pending && summary === "No messages yet." && session.summary) {
    summary = compact(session.summary, SUMMARY_LIMIT);
  }
  var directory = sessionDirectory(session);
  var title = session.title || fileName(directory) || "Untitled";
  var requestId = pending ? pending.requestId : (session.requestId || "");
  var requestKind = pending ? pending.kind : (session.requestKind || "");

  return {
    id: session.id,
    title: compact(title, 58),
    directory: compact(directory, 42),
    fullDirectory: directory,
    agent: compact(session.agent || "agent", 20),
    status: compact(status, 16),
    summary: summary,
    requestId: requestId,
    requestKind: requestKind
  };
}

function statusFromRuntimeStatus(status) {
  if (!status) {
    return "";
  }
  if (status.type === "busy" || status.type === "retry") {
    return "Running";
  }
  return "";
}

function summaryFromRuntimeStatus(status) {
  if (!status) {
    return "No messages yet.";
  }
  if (status.type === "retry" && status.message) {
    return compact("Retrying: " + status.message, SUMMARY_LIMIT);
  }
  if (status.type === "busy") {
    return "Agent is running.";
  }
  return "No messages yet.";
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

function statusFromMessages(messages) {
  var sorted = latestFirst(messages);
  if (sorted.length > 0 && isUserMessage(sorted[0])) {
    return "Running";
  }
  for (var i = 0; i < sorted.length; i++) {
    var message = sorted[i];
    var info = messageInfo(message);
    if (info.role === "assistant" || message.type === "assistant") {
      if (messageError(message)) {
        return "Error";
      }
      if (!messageFinished(message)) {
        return "Running";
      }
      if (hasActiveTool(message)) {
        return "Running";
      }
      return "Done";
    }
  }
  return "Idle";
}

function hasActiveTool(message) {
  var content = messageContent(message);
  for (var i = 0; i < content.length; i++) {
    if (content[i].type === "tool" && content[i].state) {
      var status = content[i].state.status;
      if (status === "pending" || status === "running") {
        return true;
      }
    }
  }
  return false;
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

function latestFirst(messages) {
  return messages.slice().sort(function(a, b) {
    return messageTime(b) - messageTime(a);
  });
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

function messageFinished(message) {
  var info = messageInfo(message);
  var time = message.time || info.time || {};
  return Boolean(time.completed || message.finish || info.finish);
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
    directory: item.directory,
    agent: item.agent,
    status: item.status,
    summary: item.summary,
    requestId: item.requestId,
    requestKind: item.requestKind
  };
}

function detail(sessionId, index) {
  var session = cachedSessions[sessionId] || { id: sessionId };
  var directory = sessionDirectory(session);
  loadRuntimeStatusDirectories([directory], function() {
    loadPending(directory, function() {
      loadMessages(sessionId, 10, directory, function(error, response) {
        if (error) {
          sendError(error);
          return;
        }
        var item = toItem(session, responseList(response));
        cachedSessions[sessionId] = item;
        send(makeMessage(CMD_DETAIL, itemFields(item, index, 1)));
      });
    });
  });
}

function context(sessionId, index, page, requestId) {
  var session = cachedSessions[sessionId] || { id: sessionId };
  loadMessages(sessionId, CONTEXT_MESSAGE_LIMIT, sessionDirectory(session), function(error, response) {
    if (error) {
      sendError(error);
      return;
    }
    var messages = oldestFirst(responseList(response));
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

  dispatchCommand({
    type: "thread.approval.respond",
    commandId: randomId("pebble:approval:"),
    threadId: sessionId,
    requestId: requestId,
    decision: decision,
    createdAt: new Date().toISOString()
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

  dispatchCommand({
    type: "thread.user-input.respond",
    commandId: randomId("pebble:user-input:"),
    threadId: sessionId,
    requestId: requestId,
    answers: questionAnswersObject(request, answers),
    createdAt: new Date().toISOString()
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
  dispatchCommand({
    type: "thread.turn.interrupt",
    commandId: randomId("pebble:interrupt:"),
    threadId: sessionId,
    createdAt: new Date().toISOString()
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
    dispatchCommand({
      type: "thread.turn.start",
      commandId: randomId("pebble:turn:"),
      threadId: sessionId,
      message: {
        messageId: randomId("pebble:"),
        role: "user",
        text: prompt,
        attachments: []
      },
      modelSelection: modelSelection(thread.modelSelection),
      runtimeMode: thread.runtimeMode,
      interactionMode: thread.interactionMode,
      createdAt: new Date().toISOString()
    }, function(dispatchError) {
      if (dispatchError) {
        sendError(dispatchError);
        return;
      }
      send(makeMessage(CMD_PROMPT, {}));
      getSnapshot(function() {});
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
    var now = new Date().toISOString();
    var threadId = randomId("pebble-thread-");
    var selection = modelSelection(project.defaultModelSelection);
    dispatchCommand({
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
      bootstrap: {
        createThread: {
          projectId: project.id,
          title: "New thread",
          modelSelection: selection,
          runtimeMode: "full-access",
          interactionMode: "default",
          branch: null,
          worktreePath: null,
          createdAt: now
        }
      },
      createdAt: now
    }, function(dispatchError) {
      if (dispatchError) {
        sendError(dispatchError);
        return;
      }
      send(makeMessage(CMD_PROMPT, {}));
      getSnapshot(function() {});
    });
  });
}

function withThread(threadId, callback) {
  if (lastSnapshot) {
    var cached = threadById(lastSnapshot, threadId);
    if (cached) {
      callback(null, cached);
      return;
    }
  }
  getSnapshot(function(error, snapshot) {
    if (error) {
      callback(error);
      return;
    }
    callback(null, threadById(snapshot, threadId));
  });
}

function withProject(projectId, callback) {
  if (lastSnapshot) {
    var projects = lastSnapshot.projects || [];
    for (var i = 0; i < projects.length; i++) {
      if (projects[i].id === projectId && !projects[i].deletedAt) {
        callback(null, projects[i]);
        return;
      }
    }
  }
  getSnapshot(function(error, snapshot) {
    if (error) {
      callback(error);
      return;
    }
    var projects = snapshot.projects || [];
    for (var i = 0; i < projects.length; i++) {
      if (projects[i].id === projectId && !projects[i].deletedAt) {
        callback(null, projects[i]);
        return;
      }
    }
    callback(null, null);
  });
}

function configurationHtml() {
  var current = settings();
  return [
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>",
    "<style>body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;margin:20px;background:#f7f7f2;color:#111}",
    "label{display:block;margin:16px 0 6px;font-weight:600}input{box-sizing:border-box;width:100%;font-size:16px;padding:10px;border:1px solid #999;border-radius:6px}",
    "button{margin-top:18px;width:100%;font-size:17px;padding:12px;border:0;border-radius:6px;background:#111;color:white}</style></head><body>",
    "<h2>T3 Pebble</h2>",
    "<label>Base URL</label><input id='baseUrl' placeholder='http://100.x.y.z:3773' value='", htmlEscape(current.baseUrl), "'>",
    "<label>Auth token</label><input id='password' type='password' value='", htmlEscape(current.password), "'>",
    "<button onclick='save()'>Save</button>",
    "<script>function save(){var data={baseUrl:document.getElementById('baseUrl').value,password:document.getElementById('password').value};",
    "location.href='pebblejs://close#'+encodeURIComponent(JSON.stringify(data));}</script></body></html>"
  ].join("");
}

function htmlEscape(value) {
  return String(value || "")
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#39;");
}

Pebble.addEventListener("ready", function() {
  refreshSessions();
});

Pebble.addEventListener("appmessage", function(event) {
  var message = event.payload || {};
  var command = message[KEY_CMD];
  if (command === CMD_REFRESH) {
    refreshSessions();
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
    refreshSessions();
  } catch (e) {
    sendError("Settings not saved");
  }
});
