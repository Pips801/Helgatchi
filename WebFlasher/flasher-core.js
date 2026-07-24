// Shared serial + esptool-js flashing core for the Helgatchi web tools.
//
// Both index.html (manual flasher/console) and factory/index.html (automated
// line) import this so the bricking-sensitive bits — partition offsets, the
// esptool writeFlash data contract, the hard-reset RTS pulse, the DTR/RTS
// signal handling on the S3 USB-Serial-JTAG — live in exactly one place and
// can't drift between the two pages.
//
// The module owns the serial/flash STATE and mechanics; each page owns its own
// UI and wires callbacks (onLine/onNote/onStatus/onProgress/onInfo/onDrop).

// Pinned: the writeFlash data contract (Uint8Array) and reset behavior are
// version-specific and bricking-sensitive. Do not float this to "latest".
import { ESPLoader, Transport } from "https://unpkg.com/esptool-js@0.6.0/bundle.js";

// Partition offsets — must match Software/Helgatchi-Firmware/partitions.csv.
export const OFF = { bootloader: 0x0, partitions: 0x8000, app: 0x10000, fs: 0x510000 };

export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ---- Version helpers -------------------------------------------------------
export function parseSemver(v) {
  const m = /v?(\d+)\.(\d+)\.(\d+)/.exec(v || "");
  return m ? [+m[1], +m[2], +m[3]] : null;
}
export function isNewer(latest, current) {
  const a = parseSemver(latest), b = parseSemver(current);
  if (!a) return false;
  if (!b) return true;                     // dev/unknown build → offer update
  for (let i = 0; i < 3; i++) { if (a[i] !== b[i]) return a[i] > b[i]; }
  return false;
}
// True when the two version strings are the same x.y.z (suffixes ignored).
export function sameVersion(a, b) {
  const x = parseSemver(a), y = parseSemver(b);
  return !!x && !!y && x[0] === y[0] && x[1] === y[1] && x[2] === y[2];
}

// Parse a `led list` / `vibe list` table into an array of pattern names
// (fallback path for firmware without `webinfo`).
export function parseList(lines) {
  const out = [];
  for (const l of lines) { const m = /^\s*\d+\s+(\S.*?)\s*$/.exec(l); if (m) out.push(m[1]); }
  return out;
}

export class Flasher {
  constructor(opts = {}) {
    this.versionsUrl  = opts.versionsUrl  || "versions.json";
    this.firmwareBase = opts.firmwareBase || "firmware";
    this.cb = {
      onLine:     opts.onLine     || (() => {}),   // mute-filtered device output line
      onNote:     opts.onNote     || (() => {}),   // always-shown app notice
      onStatus:   opts.onStatus   || (() => {}),   // transient status text (msg, isErr)
      onProgress: opts.onProgress || (() => {}),   // flash progress 0..100
      onInfo:     opts.onInfo     || (() => {}),   // fired after every loadWebInfo()
      onDrop:     opts.onDrop     || (() => {}),   // read loop ended unexpectedly
    };

    // Serial / flash state.
    this.port = null;
    this.reader = null;
    this.readLoopActive = false;
    this.lineListeners = [];          // {match, resolve} one-shot line matchers
    this.logMuted = 0;                // >0 → suppress onLine (machine chatter)

    // Device / release info.
    this.entries = [];                // versions.json
    this.installed = null;            // { fw, hw, chip, game, ui }
    this.ledNames = [];
    this.vibeNames = [];
    this.rulesCache = [];

    // Link policy flags (read/written by pages too).
    this.linkLive = false;
    this.userDisconnected = false;
    this.flashing = false;
    this.reconnecting = false;
  }

  _log(text)  { if (this.logMuted > 0) return; this.cb.onLine(text); }
  note(text)  { this.cb.onNote(text); }
  status(m, err = false) { this.cb.onStatus(m, err); }

  // Run an app-driven serial transaction without echoing it. A short grace
  // after keeps the trailing command echo / blank line muted too.
  async withMute(fn) {
    this.logMuted++;
    try { return await fn(); }
    finally { await sleep(120); this.logMuted = Math.max(0, this.logMuted - 1); }
  }

  // ---- Serial text phase (talking to the running app) ----------------------
  async beginTextSession() {
    await this.port.open({ baudRate: 115200 });
    // Assert DTR so the firmware console treats us as connected (it gates on
    // (bool)Serial). RTS left low — on the S3 USB-Serial-JTAG neither resets.
    try { await this.port.setSignals({ dataTerminalReady: true, requestToSend: false }); } catch {}
    this.startReadLoop();
  }
  // Prompt the user to pick a port (requires a user gesture).
  async openPort() {
    this.port = await navigator.serial.requestPort();
    await this.beginTextSession();
  }
  // Open the most-recently-granted port with no user gesture — used to
  // auto-advance to the next device when the browser still holds a grant.
  async openGrantedPort() {
    const ps = await navigator.serial.getPorts();
    if (!ps.length) throw new Error("no granted port");
    this.port = ps[ps.length - 1];
    await this.beginTextSession();
  }

  startReadLoop() {
    this.readLoopActive = true;
    const decoder = new TextDecoder();
    let buf = "";
    this.reader = this.port.readable.getReader();
    (async () => {
      try {
        while (this.readLoopActive) {
          const { value, done } = await this.reader.read();
          if (done) break;
          buf += decoder.decode(value, { stream: true });
          let nl;
          while ((nl = buf.indexOf("\n")) >= 0) {
            const line = buf.slice(0, nl).replace(/\r$/, "");
            buf = buf.slice(nl + 1);
            this._log(line);
            this.lineListeners = this.lineListeners.filter((l) => {
              if (l.match(line)) { l.resolve(line); return false; }
              return true;
            });
          }
        }
      } catch (e) {
        if (this.readLoopActive) this._log("[read error] " + e.message);
      } finally {
        try { this.reader.releaseLock(); } catch {}
      }
      // Loop ended while we still thought we were connected → link dropped
      // (device slept/reset). Backstop the navigator.serial event.
      if (this.readLoopActive) this.cb.onDrop();
    })();
  }

  async stopReadLoop() {
    this.readLoopActive = false;
    if (this.reader) { try { await this.reader.cancel(); } catch {} this.reader = null; }
  }
  async closePort() { try { await this.port?.close(); } catch {} }

  async sendLine(text) {
    const writer = this.port.writable.getWriter();
    try { await writer.write(new TextEncoder().encode(text + "\n")); }
    finally { writer.releaseLock(); }
  }

  awaitLine(match, timeoutMs) {
    return new Promise((resolve, reject) => {
      const listener = { match, resolve };
      this.lineListeners.push(listener);
      setTimeout(() => {
        this.lineListeners = this.lineListeners.filter((l) => l !== listener);
        reject(new Error("timed out waiting for device"));
      }, timeoutMs);
    });
  }

  async sendAndCollect(cmd, ms = 500) {
    const lines = [];
    const collector = { match: (l) => { lines.push(l); return false; } };  // never resolves
    this.lineListeners.push(collector);
    try { await this.sendLine(cmd); } catch {}
    await sleep(ms);
    this.lineListeners = this.lineListeners.filter((l) => l !== collector);
    return lines;
  }

  // ---- Version + device info -----------------------------------------------
  async loadVersions() {
    try {
      const res = await fetch(`${this.versionsUrl}?_=${Date.now()}`);
      if (res.ok) this.entries = await res.json();
    } catch {}
    this.entries = Array.isArray(this.entries) ? this.entries : [];
    return this.entries;
  }

  // One muted round-trip fetches version + LED/vibe registries + rules. Falls
  // back to the individual commands for firmware that predates `webinfo`.
  async loadWebInfo() {
    return this.withMute(async () => {
      try {
        await this.sendLine("webinfo");
        const line = await this.awaitLine((l) => l.trim().startsWith("{") && l.includes('"fw"'), 3500);
        const info = JSON.parse(line);
        this.installed  = { fw: info.fw, hw: info.hw, chip: info.chip, game: info.game, ui: info.ui };
        this.ledNames   = Array.isArray(info.led)   ? info.led   : [];
        this.vibeNames  = Array.isArray(info.vibe)  ? info.vibe  : [];
        this.rulesCache = Array.isArray(info.rules) ? info.rules : [];
      } catch {
        this.installed = null;
        try {
          await this.sendLine("ver");
          this.installed = JSON.parse(await this.awaitLine((l) => l.trim().startsWith("{") && l.includes('"fw"'), 2000));
        } catch {}
        try { this.ledNames  = parseList(await this.sendAndCollect("led list"));  } catch { this.ledNames = []; }
        try { this.vibeNames = parseList(await this.sendAndCollect("vibe list")); } catch { this.vibeNames = []; }
        try {
          await this.sendLine("rule dump");
          this.rulesCache = JSON.parse(await this.awaitLine((l) => l.trim().startsWith("["), 3000));
        } catch { this.rulesCache = []; }
      }
      this.cb.onInfo(this.installed);
      return this.installed;
    });
  }

  // True when the device reports at least one factory rule — i.e. the LittleFS
  // image is present. A device with no filesystem reports zero factory rules.
  hasFactoryRules() { return this.rulesCache.some((r) => r && r.factory); }

  // ---- Flashing (esptool-js) -----------------------------------------------
  // esptool-js writeFlash consumes each part's `data` as a Uint8Array — it
  // slices / deflates / pads it. Passing a binary string corrupts the image
  // and bricks the device. This MUST stay a Uint8Array.
  async fetchPart(tag, name, address) {
    const res = await fetch(`${this.firmwareBase}/${encodeURIComponent(tag)}/${name}?_=${Date.now()}`);
    if (!res.ok) throw new Error(`missing ${name} for ${tag}`);
    return { data: new Uint8Array(await res.arrayBuffer()), address };
  }

  // After a flash the S3 reboots and re-enumerates its USB. Reopen the port and
  // pull webinfo to confirm the new firmware is running.
  async reconnectAndValidate() {
    for (let i = 0; i < 8; i++) {
      await sleep(800);
      try {
        await this.beginTextSession();
        await this.loadWebInfo();
        if (this.installed?.fw) return true;
        throw new Error("no response");
      } catch {
        await this.stopReadLoop();
        await this.closePort();
        // The re-enumerated device may surface as a new port object.
        try { const ps = await navigator.serial.getPorts(); if (ps.length) this.port = ps[ps.length - 1]; } catch {}
      }
    }
    return false;
  }

  // Hard-reset the chip on its RST pin so it (re)boots the app. Exact sequence
  // esp-web-tools uses and proven on the S3's USB-Serial-JTAG: ASSERT RTS, hold
  // briefly, then release it via after(). The RTS *pulse* is what reboots the
  // chip — a bare after() leaves it parked in the bootloader ("stuck after
  // flash"). Caller owns the Transport/ESPLoader and any reconnect afterwards.
  async hardResetIntoApp(transport, esploader) {
    try {
      await transport.setRTS(true);
      await sleep(100);
      await esploader.after();          // HardReset: releases RTS → chip runs
    } catch { /* the reset re-enumerates USB and may reject; reboot still happened */ }
    try { await transport.disconnect(); } catch {}
  }

  // Flash mechanics only. Returns { ok:true } on success (installed re-read via
  // reconnectAndValidate), or { ok:false, error } — on failure the chip is left
  // in USB-recoverable download mode (never a battery pull). Emits status +
  // progress via callbacks; the caller owns all surrounding UI.
  async doFlash({ tag, wantFw, wantFs, erase }) {
    if (!tag) return { ok: false, error: "no firmware version selected" };
    const fw = erase || wantFw, fs = erase || wantFs;
    if (!fw && !fs) return { ok: false, error: "nothing selected to flash" };

    this.flashing = true;             // the reset re-enumerates USB — pages ignore connect/disconnect
    let transport = null, esploader = null;
    try {
      // 1) Assemble the parts to write.
      this.status("Downloading firmware…");
      const parts = [];
      if (fw) {
        parts.push(await this.fetchPart(tag, "bootloader.bin", OFF.bootloader));
        parts.push(await this.fetchPart(tag, "partitions.bin", OFF.partitions));
        parts.push(await this.fetchPart(tag, "firmware.bin",   OFF.app));
      }
      if (fs) parts.push(await this.fetchPart(tag, "littlefs.bin", OFF.fs));

      // 2) Tell the RUNNING app to paint its "updating" screen before we reset
      //    into the bootloader (older firmware without `update`: proceed anyway).
      this.status("Preparing device…");
      try { await this.sendLine("update"); await this.awaitLine((l) => l.includes('"updating"'), 3000); }
      catch {}

      // 3) Release the text port and hand it to esptool-js.
      await this.stopReadLoop();
      await this.port.close();

      this.status("Connecting to bootloader…");
      transport = new Transport(this.port, true);
      esploader = new ESPLoader({
        transport, baudrate: 115200,
        terminal: { clean() {}, writeLine: (d) => this._log(d), write: (d) => this._log(d) },
      });
      await esploader.main();

      if (erase) { this.status("Erasing flash…"); await esploader.eraseFlash(); }

      this.status("Writing…");
      await esploader.writeFlash({
        fileArray: parts,
        flashMode: "keep", flashFreq: "keep", flashSize: "keep",
        compress: true,
        reportProgress: (_i, written, total) => this.cb.onProgress(Math.round((written / total) * 100)),
      });

      // Reboot into the freshly-written firmware via a hard reset on RST.
      this.status("Rebooting device…");
      await this.hardResetIntoApp(transport, esploader);

      // Reconnect + re-read the version to confirm the new firmware runs.
      this.status("Flashed — reconnecting to verify…");
      const ok = await this.reconnectAndValidate();
      return { ok };
    } catch (e) {
      try { await transport?.disconnect(); } catch {}
      await this.closePort();
      return { ok: false, error: e.message };
    } finally {
      this.flashing = false;
    }
  }

  // Reboot the running device without reflashing — the same RST pulse esptool
  // does at the end of a flash (raw setSignals RTS alone doesn't reset this
  // USB-JTAG board; the bootloader handshake is what makes the RST pulse land).
  // Returns { ok } like doFlash. Caller owns the surrounding UI.
  async restart() {
    this.flashing = true;
    let transport = null, esploader = null;
    try {
      await this.stopReadLoop();
      await this.port.close();
      this.status("Resetting device…");
      transport = new Transport(this.port, true);
      esploader = new ESPLoader({
        transport, baudrate: 115200,
        terminal: { clean() {}, writeLine: (d) => this._log(d), write: (d) => this._log(d) },
      });
      await esploader.main();
      await this.hardResetIntoApp(transport, esploader);
      this.status("Restarted — reconnecting…");
      const ok = await this.reconnectAndValidate();
      return { ok };
    } catch (e) {
      try { await transport?.disconnect(); } catch {}
      await this.closePort();
      return { ok: false, error: e.message };
    } finally {
      this.flashing = false;
    }
  }

  // Send a `power <sub>` command and wait for the "OK" ack. Used for shipping
  // (wipe + deep sleep), wipe (wipe + reboot), reboot, off. The device tears
  // down on its own afterwards; the caller handles the link drop.
  async doPower(sub) {
    await this.sendLine("power " + sub);
    try { await this.awaitLine((l) => l.trim() === "OK", 3000); } catch { /* older fw / already gone */ }
  }
}
