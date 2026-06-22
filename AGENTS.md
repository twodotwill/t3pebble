# Agent Notes

## Build and Test

Run Pebble bridge checks and build from `pebblecode/`:

```sh
node --check src/pkjs/index.js
node test/bridge.test.js
node test/bridge.integration.test.js
pebble build
```

Copy the installable bundle from the repo root:

```sh
cp pebblecode/build/pebblecode.pbw dist/t3pebble.pbw
```

## Watch Install

The local `pebble` command is a Docker wrapper. Use repo-relative PBW paths, not host absolute paths, when calling Pebble SDK commands.

Normal install path:

```sh
pebble install --phone <phone-ip> dist/t3pebble.pbw
```

If `pebble install --phone ...` or `pebble ping --phone ...` times out fetching watch info, but the Core Devices/Pebble app dev server is open on port `9000`, bypass the old Pebble SDK handshake and install directly through the Core Devices WebSocket protocol.

Direct Core Devices install:

```sh
node - <<'NODE'
const fs = require("fs");
const phone = process.env.PEBBLE_PHONE || "100.76.64.6";
const pbwPath = process.env.PBW_PATH || "dist/t3pebble.pbw";
const pbw = fs.readFileSync(pbwPath);
const payload = Buffer.concat([Buffer.from([0x04]), pbw]);
const ws = new WebSocket(`ws://${phone}:9000/`);
const timeout = setTimeout(() => {
  console.error("Timed out waiting for install result");
  try { ws.close(); } catch (e) {}
  process.exit(2);
}, 60000);
let sent = false;

ws.addEventListener("open", () => {
  console.log("connected to Core Devices dev server");
});

ws.addEventListener("message", async (event) => {
  const data = event.data instanceof Blob
    ? Buffer.from(await event.data.arrayBuffer())
    : Buffer.from(event.data);
  const type = data[0];
  if (type === 0x07 && !sent) {
    sent = true;
    console.log("sending PBW bytes", pbw.length);
    ws.send(payload);
    return;
  }
  if (type === 0x05) {
    clearTimeout(timeout);
    const status = data.length >= 5 ? data.readUInt32LE(1) : data[1];
    console.log("install status", status === 0 ? "success" : "failure", `(${status})`);
    ws.close();
    process.exit(status === 0 ? 0 : 1);
  }
});

ws.addEventListener("error", (event) => {
  clearTimeout(timeout);
  console.error("websocket error", event.message || event.type || event);
  process.exit(1);
});
NODE
```

Known working phone Tailscale IP for Will's S23 Ultra: `100.76.64.6`.

Protocol notes:

- Connect to `ws://<phone-ip>:9000/`.
- Wait for server message type `0x07` (`07ff` means watch connected).
- Send one binary frame containing byte `0x04` followed by the PBW bytes.
- Install result is server message type `0x05`; little-endian status `0` means success.

