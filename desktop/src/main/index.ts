import { app, BrowserWindow, ipcMain, screen } from "electron";
import fs from "fs";
import path from "path";
import { DesktopHandoff, realClock, type CatPort, type Expr } from "../handoff/desktop_handoff";
import { assertOpenable, BAUD_LABEL, listPorts, PREFERRED_PORT } from "../transport/ports";
import { SerialByteStream, type ByteStream } from "../transport/serial";
import { SimulatorStream } from "../transport/simulator";

app.commandLine.appendSwitch("ozone-platform", "x11");
app.commandLine.appendSwitch("enable-transparent-visuals");

let session: DesktopHandoff;
let stream: ByteStream | null = null;
let picker: BrowserWindow;
let catWin: BrowserWindow;
let chipWin: BrowserWindow;

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
  console.log("JITTERS demo connect", PREFERRED_PORT);
  await connect(PREFERRED_PORT);
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
  picker = makeWindow("picker", 392, 540);
  catWin = makeWindow("cat", 176, 248);
  chipWin = makeWindow("chip", 120, 48);
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
    firstHelloMs: 8000,
  });

  ipcMain.handle("state", () => session.snapshot());
  ipcMain.handle("ports", () => ({
    ports: listPorts(),
    preferred: PREFERRED_PORT,
    baudLabel: BAUD_LABEL,
  }));
  ipcMain.handle("connect", async (_e, id: string) => {
    await connect(id);
    return session.snapshot();
  });
  ipcMain.handle("disconnect", () => {
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
  ipcMain.handle("pet", () => {
    session.pet();
    for (const win of BrowserWindow.getAllWindows()) {
      if (!win.isDestroyed()) win.webContents.send("pet");
    }
    publish();
    return session.snapshot();
  });
  ipcMain.handle("start-spot", () => {
    session.startSpot();
    publish();
    return session.snapshot();
  });
  ipcMain.handle("stop-spot", () => {
    session.stopSpot();
    publish();
    return session.snapshot();
  });
  ipcMain.handle("spot-hit", () => {
    session.spotHit();
    publish();
    return session.snapshot();
  });
  ipcMain.handle("device-tap", () => {
    session.deviceTap();
    publish();
    return session.snapshot();
  });

  picker.showInactive();

  const previewAt = process.argv.indexOf("--preview-face");
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

  const gameAt = process.argv.indexOf("--preview-game");
  if (gameAt >= 0) {
    const shot = process.argv[gameAt + 1] ?? "";
    if (!picker.isDestroyed()) picker.hide();
    try {
      session.startSpot();
      await catWin.loadFile(path.join(__dirname, "index.html"), { query: { view: "cat", game: "1" } });
      publish();
      placeBottomRight(catWin);
      catWin.showInactive();
      const seen = await catWin.webContents.executeJavaScript(`new Promise((resolve) => {
        const start = Date.now();
        const tick = () => {
          const img = document.querySelector("img");
          const spot = document.querySelector("[data-spot]");
          if (spot && img && img.complete && img.naturalWidth > 0) resolve("spot");
          else if (Date.now() - start > 4000) resolve("timeout");
          else setTimeout(tick, 40);
        };
        tick();
      })`);
      if (seen !== "spot") throw new Error("spot missing");
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

  const once = process.argv.includes("--handoff-once");
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
