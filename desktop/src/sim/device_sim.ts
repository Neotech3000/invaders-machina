import { encode, parseLine } from "../protocol/schema";

/** In-process device. Speaks the same NDJSON the firmware uses. No serial port. */
export class DeviceSim {
  onOut: (line: string) => void = () => {};
  visible = true;
  phase: "local" | "linked" | "await_ready" | "away" | "return_shown" = "local";
  private seq = 1;
  private accepted = "idle";

  start() {
    this.visible = true;
    this.phase = "local";
    this.emit("hello", {
      role: "device",
      proto: 1,
      expressions: ["idle"],
      owner: "device",
    });
  }

  feed(line: string) {
    const msg = parseLine(line.trim());
    if (!msg) return;
    if (msg.type === "hello.ack" && msg.payload.ok === true && this.phase === "local") {
      this.phase = "linked";
      return;
    }
    if (msg.type === "hello" && msg.payload.role === "desktop" && msg.payload.owner === "device") {
      this.emit("hello.ack", { ok: true, proto: 1 });
      return;
    }
    if (msg.type === "ping") {
      this.emit("pong", { ref: msg.id });
      return;
    }
    if (msg.type === "handoff.request" && this.phase === "linked") {
      this.accepted = String(msg.payload.expression ?? "idle");
      this.phase = "await_ready";
      this.emit("handoff.accept", { expression: this.accepted });
      return;
    }
    if (msg.type === "handoff.ready" && this.phase === "await_ready") {
      this.visible = false;
      this.phase = "away";
      this.emit("handoff.commit", { expression: this.accepted });
      return;
    }
    if (msg.type === "handoff.return" && this.phase === "away") {
      this.visible = true;
      this.phase = "return_shown";
      this.emit("handoff.shown", { expression: "idle" });
      return;
    }
    if (msg.type === "handoff.ack" && this.phase === "return_shown") {
      this.phase = "linked";
      return;
    }
    if (msg.type === "session.end") {
      this.visible = true;
      this.phase = "local";
    }
  }

  private emit(type: string, payload: Record<string, unknown>) {
    const id = `e-${String(this.seq++).padStart(8, "0")}`;
    this.onOut(encode(type, id, payload));
  }
}
