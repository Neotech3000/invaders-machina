import { app, BrowserWindow, ipcMain, screen } from "electron";
import fs from "fs";
import path from "path";
import { DesktopHandoff, realClock, type CatPort, type Expr } from "../handoff/desktop_handoff";
import { assertOpenable, BAUD_LABEL, listPorts, preferredPort } from "../transport/ports";
import { SerialByteStream, type ByteStream } from "../transport/serial";
import { SimulatorStream } from "../transport/simulator";

// Transparent frameless windows on Linux need the X11 path. Forcing that
// platform on Windows or macOS stops the window from opening.
if (process.platform === "linux") {
  app.commandLine.appendSwitch("ozone-platform", "x11");
  app.commandLine.appendSwitch("enable-transparent-visuals");
}

let session: DesktopHandoff;
let stream: ByteStream | null = null;
let picker: BrowserWindow;
let catWin: BrowserWindow;
let chipWin: BrowserWindow;
let userHold = false;

function placeBottomRight(win: BrowserWindow) {
  const area = screen.getPrimaryDisplay().workArea;
  const bounds = win.getBounds();
  win.setPosition(
    Math.round(area.x + area.width - bounds.width - 16),
    Math.round(area.y + area.height - bounds.height - 16),
  );
}

function publish() {
  const snap = session.snapshot();
  for (const win of BrowserWindow.getAllWindows()) {
    if (!win.isDestroyed()) win.webContents.send("state", snap);
  }
}

class WinPort implements CatPort {
  visible = false;
  constructor(private readonly win: BrowserWindow) {}

  async show(expression: Expr) {
    if (this.win.isDestroyed()) return;
    this.win.webContents.send("expression", expression);
    publish();
    placeBottomRight(this.win);
    if (!this.win.isVisible()) this.win.showInactive();
    if (!this.win.isVisible()) {
      await new Promise<void>((resolve) => {
        const done = () => resolve();
        this.win.once("show", done);
        setTimeout(done, 500);
      });
    }
    this.visible = this.win.isVisible();
  }

  hide() {
    this.visible = false;
    if (!this.win.isDestroyed() && this.win.isVisible()) this.win.hide();
  }

  isVisible() {
    return this.visible && !this.win.isDestroyed() && this.win.isVisible();
  }

}

function makeWindow(view: string, width: number, height: number) {
  const win = new BrowserWindow({
    width,
    height,
    frame: false,
    transparent: true,
    skipTaskbar: true,
    focusable: false,
    resizable: false,
    hasShadow: false,
    show: false,
    backgroundColor: "#00000000",
    webPreferences: {
      preload: path.join(__dirname, "preload.cjs"),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
    },
  });
  void win.loadFile(path.join(__dirname, "index.html"), { query: { view } });
  return win;
}

async function maybeAutoConnect() {
  const preferred = await preferredPort();
  if (userHold) {
    if (!preferred) userHold = false;
    return;
  }
  if (session.phase !== "disconnected" || stream) return;
  if (!preferred) return;
  try {
    await connect(preferred);
  } catch (err) {
    console.log("JITTERS auto-connect", err instanceof Error ? err.message : err);
  }
}

async function connect(id: string) {
  if (session.phase !== "disconnected") throw new Error("Already connected");
  assertOpenable(id);
  const next: ByteStream =
    id === "simulator://jitters" ? new SimulatorStream() : new SerialByteStream(id);
  next.onLine((line) => {
    console.log("JITTERS <<", line);
    session.onLine(line);
    publish();
  });
  next.onClose((reason) => {
    console.log("JITTERS close", reason);
    session.onClose(reason);
    stream = null;
    publish();
  });
  stream = next;
  session.onOpen(id);
  try {
    await next.open();
  } catch (err) {
    stream = null;
    session.onClose("open-failed");
    publish();
    throw err;
  }
  publish();
}

function waitPhase(phase: string, ms: number) {
  const start = Date.now();
  return new Promise<boolean>((resolve) => {
    const tick = () => {
      if (session.phase === phase) {
        resolve(true);
        return;
      }
      if (Date.now() - start > ms) {
        resolve(false);
        return;
      }
      setTimeout(tick, 50);
    };
    tick();
  });
}

async function handoffOnce(shotPath: string) {
  const trace: string[] = [];
  const orig = session;
  void orig;
  const port = await preferredPort();
  if (!port) {
    throw new Error("No display port. Copy desktop/port.local.json.example to desktop/port.local.json and set port.");
  }
  console.log("JITTERS demo connect", port);
  await connect(port);
  const linked = await waitPhase("linked", 10000);
  if (!linked) throw new Error(session.status);
  session.bring();
  const present = await waitPhase("present", 8000);
  if (!present) throw new Error(session.status);
  await new Promise((r) => setTimeout(r, 700));
  if (shotPath) {
    const png = await catWin.capturePage();
    fs.mkdirSync(path.dirname(shotPath), { recursive: true });
    fs.writeFileSync(shotPath, png.toPNG());
    console.log("JITTERS shot", shotPath);
  }
  await session.returnToDevice();
  const back = await waitPhase("linked", 8000);
  if (!back) throw new Error(session.status);
  const catHidden = !session.snapshot().catVisible;
  session.disconnect();
  const result = { linked: true, present: true, returned: catHidden, status: session.status, trace };
  console.log("JITTERS RESULT", JSON.stringify(result));
  return result;
}

app.whenReady().then(async () => {
  picker = makeWindow("picker", 400, 560);
  catWin = makeWindow("cat", 208, 248);
  chipWin = makeWindow("chip", 220, 72);
  picker.setPosition(48, 48);
  placeBottomRight(catWin);
  placeBottomRight(chipWin);

  const cat = new WinPort(catWin);
  const chip = new WinPort(chipWin);
  session = new DesktopHandoff({
    cat,
    chip,
    clock: realClock(),
    send: (line) => {
      console.log("JITTERS >>", line);
      stream?.send(line);
    },
    close: () => {
      const current = stream;
      stream = null;
      void current?.close();
    },
    trace: (event) => console.log("JITTERS", event),
    changed: () => publish(),
    firstHelloMs: 8000,
  });

  ipcMain.handle("state", () => session.snapshot());
  ipcMain.handle("ports", async () => ({
    ports: await listPorts(),
    preferred: await preferredPort(),
    baudLabel: BAUD_LABEL,
  }));
  ipcMain.handle("connect", async (_e, id: string) => {
    await connect(id);
    return session.snapshot();
  });
  ipcMain.handle("disconnect", () => {
    userHold = true;
    session.disconnect();
    publish();
    return session.snapshot();
  });
  ipcMain.handle("bring", () => {
    session.bring();
    publish();
    return session.snapshot();
  });
  ipcMain.handle("hide", () => {
    session.hideCat();
    publish();
    return session.snapshot();
  });
  ipcMain.handle("unhide", () => {
    session.unhide();
    publish();
    return session.snapshot();
  });
  ipcMain.handle("return", async () => {
    await session.returnToDevice();
    publish();
    return session.snapshot();
  });

  picker.showInactive();

  const previewAt = process.argv.indexOf("--preview-face");
  const gameAt = process.argv.indexOf("--preview-invaders");
  const once = process.argv.includes("--handoff-once");
  if (previewAt < 0 && gameAt < 0 && !once) {
    void maybeAutoConnect();
    setInterval(() => {
      void maybeAutoConnect();
    }, 1500);
  }

  if (previewAt >= 0) {
    const shot = process.argv[previewAt + 1] ?? "";
    if (!picker.isDestroyed()) picker.hide();
    try {
      await new Promise<void>((resolve) => {
        if (catWin.webContents.isLoading()) catWin.webContents.once("did-finish-load", () => resolve());
        else resolve();
      });
      placeBottomRight(catWin);
      catWin.showInactive();
      await catWin.webContents.executeJavaScript(`new Promise((resolve) => {
        const start = Date.now();
        const tick = () => {
          const img = document.querySelector("img");
          const frame = img && img.getAttribute("data-frame");
          if (frame === "blink" && img.complete && img.naturalWidth > 0) resolve(frame);
          else if (Date.now() - start > 8000) resolve(frame || "timeout");
          else setTimeout(tick, 40);
        };
        tick();
      })`);
      const png = await catWin.capturePage();
      if (shot) {
        fs.mkdirSync(path.dirname(shot), { recursive: true });
        fs.writeFileSync(shot, png.toPNG());
        console.log("JITTERS shot", shot);
      }
      app.exit(0);
    } catch (err) {
      console.error("JITTERS FAIL", err instanceof Error ? err.message : err);
      app.exit(1);
    }
    return;
  }

  if (gameAt >= 0) {
    const shot = process.argv[gameAt + 1] ?? "";
    if (!picker.isDestroyed()) picker.hide();
    try {
      await new Promise<void>((resolve) => {
        if (catWin.webContents.isLoading()) catWin.webContents.once("did-finish-load", () => resolve());
        else resolve();
      });
      catWin.setBackgroundColor("#000000");
      catWin.setContentSize(320, 480);
      await catWin.loadFile(path.join(__dirname, "index.html"), { query: { view: "invaders" } });
      placeBottomRight(catWin);
      catWin.showInactive();
      const seen = await catWin.webContents.executeJavaScript(`new Promise((resolve) => {
        const start = Date.now();
        const tick = () => {
          const board = document.querySelector("[data-invaders]");
          if (board && board.getBoundingClientRect().height > 400) resolve("invaders");
          else if (Date.now() - start > 4000) resolve("timeout");
          else setTimeout(tick, 40);
        };
        tick();
      })`);
      if (seen !== "invaders") throw new Error("invaders missing");
      await new Promise((r) => setTimeout(r, 400));
      let png = await catWin.capturePage();
      for (let attempt = 0; attempt < 4 && png.isEmpty(); attempt += 1) {
        await new Promise((r) => setTimeout(r, 300));
        png = await catWin.capturePage();
      }
      const bytes = png.toPNG();
      if (bytes.length < 1000) throw new Error("empty invaders capture");
      if (shot) {
        fs.mkdirSync(path.dirname(shot), { recursive: true });
        fs.writeFileSync(shot, bytes);
        console.log("JITTERS shot", shot, bytes.length);
      }
      app.exit(0);
    } catch (err) {
      console.error("JITTERS FAIL", err instanceof Error ? err.message : err);
      app.exit(1);
    }
    return;
  }

  if (!once) return;
  const shotFlag = process.argv.indexOf("--shot");
  const shot = shotFlag >= 0 ? process.argv[shotFlag + 1] : "";
  try {
    await new Promise((r) => setTimeout(r, 400));
    await handoffOnce(shot);
    app.exit(0);
  } catch (err) {
    console.error("JITTERS FAIL", err instanceof Error ? err.message : err);
    app.exit(1);
  }
});

app.on("window-all-closed", () => {
  /* keep the process for the frameless windows; they are hidden, not closed */
});
