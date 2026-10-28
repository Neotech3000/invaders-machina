import { encode, expressions, isExpr, parseLine, timeouts, type Envelope, type Expr } from "../protocol/schema";

export type Phase =
  | "disconnected"
  | "connecting"
  | "linked"
  | "preparing"
  | "present"
  | "hidden_hold"
  | "returning";

export interface CatPort {
  show(expression: Expr): Promise<void>;
  hide(): void;
  isVisible(): boolean;
}

export interface Clock {
  set(key: string, ms: number, fn: () => void): void;
  clear(key: string): void;
  clearAll(): void;
}

export interface Snapshot {
  phase: Phase;
  expression: Expr;
  owner: "device" | "desktop";
  catVisible: boolean;
  chipVisible: boolean;
  status: string;
  portId: string | null;
}

export interface SessionOptions {
  cat: CatPort;
  chip: CatPort;
  send: (line: string) => void;
  close: () => void;
  clock: Clock;
  trace?: (event: string) => void;
  /** Wait for the first device hello after a USB reset. Protocol helloMs still bounds the ack. */
  firstHelloMs?: number;
}

/**
 * Desktop handoff. Does not import Electron.
 * handoff.ready is sent only after the cat port reports visible.
 */
export class DesktopHandoff {
  phase: Phase = "disconnected";
  expression: Expr = "idle";
  owner: "device" | "desktop" = "device";
  status = "Choose a port";
  portId: string | null = null;
  private readonly seen: string[] = [];
  private readonly acks = new Map<string, string>();
  private seq = 1;
  private malformed = 0;
  private readyResend = false;
  private opened = false;
  private closing = false;

  constructor(private readonly opt: SessionOptions) {}

  snapshot(): Snapshot {
    return {
      phase: this.phase,
      expression: this.expression,
      owner: this.owner,
      catVisible: this.opt.cat.isVisible(),
      chipVisible: this.opt.chip.isVisible(),
      status: this.status,
      portId: this.portId,
    };
  }

  onOpen(portId: string) {
    this.portId = portId;
    this.opened = true;
    this.phase = "connecting";
    this.owner = "device";
    this.status = "Waiting for the display";
    this.malformed = 0;
    this.seen.length = 0;
    this.acks.clear();
    this.arm("hello", this.opt.firstHelloMs ?? 8000, () => {
      this.status = "Display did not say hello";
      this.shutdown("hello-timeout");
    });
  }

  onClose(reason: string) {
    if (this.closing || (!this.opened && this.phase === "disconnected")) return;
    this.opt.clock.clearAll();
    this.opt.cat.hide();
    this.opt.chip.hide();
    this.opened = false;
    this.phase = "disconnected";
    this.owner = "device";
    this.portId = null;
    if (!this.status.includes("did not") && !this.status.includes("quiet")) {
      this.status = reason ? `Link closed (${reason})` : "Disconnected";
    }
  }

  onLine(line: string) {
    const trimmed = line.trim();
    if (!trimmed.startsWith("{")) return;
    const msg = parseLine(trimmed);
    if (!msg) {
      this.malformed += 1;
      if (this.malformed >= 5) this.shutdown("malformed");
      return;
    }
    this.malformed = 0;
    if (msg.id.startsWith("d-")) return;
    if (this.seen.includes(msg.id)) {
      const replay = this.acks.get(msg.id);
      if (replay) this.opt.send(replay);
      return;
    }
    this.seen.push(msg.id);
    if (this.seen.length > timeouts.dedupSize) this.seen.shift();
    this.dispatch(msg);
    this.armSilence();
  }

  bring() {
    if (this.phase !== "linked") return;
    this.send("handoff.request", { expression: this.expression });
    this.status = "Asking the display";
  }

  hideCat() {
    if (this.phase !== "present") return;
    this.opt.cat.hide();
    void this.opt.chip.show(this.expression);
    this.phase = "hidden_hold";
    this.status = "Hidden on the laptop";
  }

  unhide() {
    if (this.phase !== "hidden_hold") return;
    this.opt.chip.hide();
    void this.opt.cat.show(this.expression);
    this.phase = "present";
    this.status = "Cat is on the laptop";
  }

  async returnToDevice() {
    if (this.phase !== "present" && this.phase !== "hidden_hold") return;
    if (!this.opt.cat.isVisible()) {
      this.opt.chip.hide();
      await this.opt.cat.show(this.expression);
    }
    this.phase = "returning";
    this.status = "Sending the cat back";
    this.arm("return", timeouts.returnMs, () => {
      this.phase = "present";
      this.owner = "desktop";
      this.status = "Display did not confirm. Try return again.";
    });
    this.send("handoff.return", {});
  }

  disconnect() {
    if (this.opened) {
      try {
        this.send("session.end", { reason: "user" });
      } catch {
        /* the write is best-effort */
      }
    }
    this.shutdown("user");
    this.status = "Disconnected";
  }

  private dispatch(msg: Envelope) {
    const payload = msg.payload;
    if (msg.type === "hello") {
      if (payload.role !== "device" || payload.owner !== "device" || payload.proto !== 1) {
        this.send("hello.ack", { ok: false, proto: 1, reason: "rejected" });
        this.shutdown("bad-hello");
        return;
      }
      this.opt.clock.clear("hello");
      this.send("hello.ack", { ok: true, proto: 1 });
      this.send("hello", {
        role: "desktop",
        proto: 1,
        expressions: [...expressions],
        owner: "device",
      });
      this.arm("hello", timeouts.helloMs, () => {
        this.status = "Display did not ack";
        this.shutdown("hello-ack-timeout");
      });
      return;
    }
    if (msg.type === "hello.ack") {
      this.opt.clock.clear("hello");
      if (payload.ok !== true) {
        this.status = "Display rejected the hello";
        this.shutdown("hello-rejected");
        return;
      }
      if (this.phase === "connecting") {
        this.phase = "linked";
        this.owner = "device";
        this.status = "Cat is on the display";
        this.armPing();
        this.armSilence();
      }
      return;
    }
    if (msg.type === "ping") {
      this.send("pong", { ref: msg.id });
      return;
    }
    if (msg.type === "pong" || msg.type === "error") return;
    if (msg.type === "handoff.accept") {
      if (this.phase !== "linked") return;
      const expression = isExpr(payload.expression) ? payload.expression : this.expression;
      this.expression = expression;
      void this.onAccept(expression);
      return;
    }
    if (msg.type === "handoff.commit") {
      if (this.phase !== "preparing") return;
      this.opt.clock.clear("commit");
      this.rememberAck(msg.id, "handoff.ack", { ref: msg.id });
      this.phase = "present";
      this.owner = "desktop";
      this.status = "Cat is on the laptop";
      this.opt.trace?.("commit");
      return;
    }
    if (msg.type === "handoff.shown") {
      if (this.phase !== "returning") return;
      this.opt.clock.clear("return");
      this.opt.cat.hide();
      this.opt.chip.hide();
      this.opt.trace?.("window-hidden");
      this.rememberAck(msg.id, "handoff.ack", { ref: msg.id });
      this.phase = "linked";
      this.owner = "device";
      this.status = "Cat is on the display";
      this.opt.trace?.("shown");
      return;
    }
    if (msg.type === "handoff.abort") {
      const reason = String(payload.reason ?? "");
      if (this.phase === "preparing") {
        this.opt.clock.clear("commit");
        this.opt.cat.hide();
        this.phase = "linked";
        this.owner = "device";
        this.status = "Display kept the cat";
        return;
      }
      if ((this.phase === "present" || this.phase === "hidden_hold") && reason === "timeout") {
        this.opt.cat.hide();
        this.opt.chip.hide();
        this.phase = "linked";
        this.owner = "device";
        this.status = "Display took the cat back";
      }
    }
  }

  private async onAccept(expression: Expr) {
    this.phase = "preparing";
    this.readyResend = false;
    this.status = "Opening the cat here";
    await this.opt.cat.show(expression);
    if (this.phase !== "preparing") return;
    if (!this.opt.cat.isVisible()) {
      this.status = "Cat window did not become visible";
      this.phase = "linked";
      return;
    }
    this.opt.trace?.("window-visible");
    this.opt.trace?.("sent-ready");
    this.send("handoff.ready", { expression });
    if (this.phase === "preparing") {
      this.arm("commit", timeouts.commitMs, () => this.onCommitTimeout());
    }
  }

  private onCommitTimeout() {
    if (this.phase !== "preparing") return;
    if (!this.readyResend && this.opt.cat.isVisible()) {
      this.readyResend = true;
      this.send("handoff.ready", { expression: this.expression });
      this.arm("commit", timeouts.commitMs, () => this.onCommitTimeout());
      return;
    }
    this.opt.cat.hide();
    this.phase = "linked";
    this.owner = "device";
    this.status = "Display did not commit";
  }

  private rememberAck(refId: string, type: string, payload: Record<string, unknown>) {
    const line = this.line(type, payload);
    this.acks.set(refId, line);
    this.opt.send(line);
  }

  private send(type: string, payload: Record<string, unknown>) {
    this.opt.send(this.line(type, payload));
  }

  private line(type: string, payload: Record<string, unknown>) {
    const id = `d-${type.replace(/\./g, "").slice(0, 8)}${this.seq++}`;
    return encode(type, id.slice(0, 32), payload);
  }

  private arm(key: string, ms: number, fn: () => void) {
    this.opt.clock.set(key, ms, fn);
  }

  private armPing() {
    this.arm("ping", timeouts.pingIntervalMs, () => {
      if (this.phase === "disconnected" || this.phase === "connecting") return;
      this.send("ping", {});
      this.armPing();
    });
  }

  private armSilence() {
    if (this.phase === "disconnected" || this.phase === "connecting") return;
    this.arm("silence", timeouts.silenceMs, () => this.shutdown("silence"));
  }

  private shutdown(reason: string) {
    if (this.closing) return;
    this.closing = true;
    this.opt.clock.clearAll();
    this.opt.cat.hide();
    this.opt.chip.hide();
    this.phase = "disconnected";
    this.owner = "device";
    this.portId = null;
    const wasOpen = this.opened;
    this.opened = false;
    if (wasOpen) this.opt.close();
    this.closing = false;
    if (reason === "silence") this.status = "Link went quiet";
    if (reason === "user") this.status = "Disconnected";
  }
}

export function realClock(): Clock {
  const ids = new Map<string, ReturnType<typeof setTimeout>>();
  return {
    set(key, ms, fn) {
      const prev = ids.get(key);
      if (prev) clearTimeout(prev);
      ids.set(key, setTimeout(fn, ms));
    },
    clear(key) {
      const prev = ids.get(key);
      if (prev) clearTimeout(prev);
      ids.delete(key);
    },
    clearAll() {
      for (const id of ids.values()) clearTimeout(id);
      ids.clear();
    },
  };
}
