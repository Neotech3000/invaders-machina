import assert from "assert";
import { DesktopHandoff, type CatPort, type Clock, type Expr } from "./desktop_handoff";
import { controlsFor } from "../renderer/controls";
import { encode, parseLine } from "../protocol/schema";
import { kMaxSerialBuf, takeLines } from "../transport/serial";

class ManualClock implements Clock {
  timers: { key: string; fn: () => void }[] = [];
  set(key: string, _ms: number, fn: () => void) {
    this.clear(key);
    this.timers.push({ key, fn });
  }
  clear(key: string) {
    this.timers = this.timers.filter((timer) => timer.key !== key);
  }
  clearAll() {
    this.timers = [];
  }
  fire(key: string) {
    const timer = this.timers.find((item) => item.key === key);
    if (!timer) throw new Error(`no timer ${key}`);
    this.clear(key);
    timer.fn();
  }
}

class FakeCat implements CatPort {
  visible = false;
  expression: Expr = "idle";
  fail = false;
  async show(expression: Expr) {
    if (this.fail) throw new Error("show failed");
    this.expression = expression;
    this.visible = true;
  }
  hide() {
    this.visible = false;
  }
  isVisible() {
    return this.visible;
  }
}

function device(type: string, id: string, payload: Record<string, unknown>) {
  return encode(type, id, payload);
}

async function flush() {
  await new Promise((resolve) => setImmediate(resolve));
}

function setup() {
  const cat = new FakeCat();
  const chip = new FakeCat();
  const clock = new ManualClock();
  const sent: string[] = [];
  let closed = 0;
  const session = new DesktopHandoff({
    cat,
    chip,
    clock,
    send: (line) => sent.push(line),
    close: () => {
      closed += 1;
    },
  });
  return { session, cat, chip, clock, sent, closed: () => closed };
}

function link() {
  const ctx = setup();
  ctx.session.onOpen("simulator://jitters");
  ctx.session.onLine(
    device("hello", "e-hello0001", {
      role: "device",
      proto: 1,
      expressions: ["idle"],
      owner: "device",
    }),
  );
  ctx.session.onLine(device("hello.ack", "e-ack000001", { ok: true, proto: 1 }));
  assert.equal(ctx.session.phase, "linked");
  const sync = ctx.sent.find((line) => line.includes("\"time.sync\""));
  assert.ok(sync, "laptop sends the time once the link is up");
  const parsed = parseLine(sync ?? "");
  assert.equal(parsed?.type, "time.sync");
  assert.equal(typeof parsed?.payload.unix, "number");
  assert.equal(typeof parsed?.payload.offset, "number");
  return ctx;
}

async function main() {
  {
    const buttons = ["disconnected", "connecting", "linked", "preparing", "present", "hidden_hold", "returning"] as const;
    assert.deepEqual(
      buttons.map((phase) => controlsFor(phase).connect),
      [true, false, false, false, false, false, false],
    );
    assert.equal(controlsFor("linked").bring, true);
    assert.equal(controlsFor("linked", true).bring, false);
    assert.equal(controlsFor("present").hide, true);
    assert.equal(controlsFor("present").sendBack, true);
    assert.equal(controlsFor("hidden_hold").show, true);
    assert.equal(controlsFor("hidden_hold").sendBack, true);
    assert.equal(controlsFor("hidden_hold").hide, false);
    assert.equal(controlsFor("returning").waiting, true);
    assert.equal(controlsFor("returning").sendBack, false);
    assert.equal(controlsFor("preparing").disconnect, true);
    assert.equal(controlsFor("disconnected").disconnect, false);
  }

  {
    const first = takeLines("", '{"v":1}\r\npartial');
    assert.deepEqual(first.lines, ['{"v":1}']);
    const second = takeLines(first.buf, " line\n");
    assert.deepEqual(second.lines, ["partial line"]);
    assert.equal(second.buf, "");
    const huge = takeLines("", "x".repeat(kMaxSerialBuf + 1));
    assert.deepEqual(huge.lines, []);
    assert.equal(huge.buf, "");
    const long = takeLines("", `${"y".repeat(1025)}\n{"ok":1}\n`);
    assert.deepEqual(long.lines, ['{"ok":1}']);
  }

  {
    const ctx = link();
    ctx.session.bring();
    ctx.session.bring();
    assert.equal(ctx.sent.filter((line) => line.includes("handoff.request")).length, 1);
    assert.equal(ctx.session.snapshot().asking, true);
    ctx.clock.fire("ask");
    assert.equal(ctx.session.snapshot().asking, false);
    assert.match(ctx.session.status, /did not answer/);
    assert.equal(ctx.session.phase, "linked");
    ctx.session.bring();
    assert.equal(ctx.sent.filter((line) => line.includes("handoff.request")).length, 2);
  }

  {
    const ctx = link();
    ctx.session.bring();
    ctx.session.onLine(device("error", "e-err000001", { code: "bad_expression", message: "unknown expression" }));
    assert.equal(ctx.session.phase, "linked");
    assert.equal(ctx.session.snapshot().asking, false);
    assert.match(ctx.session.status, /refused/);
  }

  {
    const ctx = link();
    ctx.session.bring();
    ctx.session.onLine(device("handoff.accept", "e-accept001", { expression: "idle" }));
    await flush();
    assert.equal(ctx.session.phase, "preparing");
    assert.equal(ctx.cat.visible, true);
    const commit = device("handoff.commit", "e-commit001", { expression: "idle" });
    ctx.session.onLine(commit);
    assert.equal(ctx.session.phase, "present");
    const acks = () => ctx.sent.filter((line) => line.includes("handoff.ack")).length;
    const afterFirst = acks();
    ctx.session.onLine(commit);
    assert.equal(acks(), afterFirst + 1);
    assert.equal(ctx.session.phase, "present");
    assert.equal(ctx.session.owner, "desktop");
    ctx.session.onLine(device("handoff.commit", "e-commit002", { expression: "idle" }));
    assert.equal(ctx.session.phase, "present");
  }

  {
    const ctx = link();
    ctx.session.bring();
    ctx.session.onLine(device("handoff.accept", "e-accept002", { expression: "idle" }));
    await flush();
    ctx.session.onLine(device("handoff.abort", "e-abort0001", { reason: "timeout" }));
    assert.equal(ctx.session.phase, "linked");
    assert.equal(ctx.cat.visible, false);
    assert.match(ctx.session.status, /kept the cat/);
  }

  {
    const ctx = link();
    ctx.cat.fail = true;
    ctx.session.bring();
    ctx.session.onLine(device("handoff.accept", "e-accept003", { expression: "idle" }));
    await flush();
    assert.equal(ctx.session.phase, "linked");
    assert.match(ctx.session.status, /did not open/);
    assert.equal(ctx.sent.some((line) => line.includes("handoff.ready")), false);
  }

  {
    const ctx = link();
    ctx.session.bring();
    ctx.session.onLine(device("handoff.accept", "e-accept004", { expression: "idle" }));
    await flush();
    ctx.clock.fire("commit");
    assert.equal(ctx.session.phase, "preparing");
    assert.equal(ctx.sent.filter((line) => line.includes("handoff.ready")).length, 2);
    ctx.clock.fire("commit");
    assert.equal(ctx.session.phase, "linked");
    assert.equal(ctx.cat.visible, false);
    assert.match(ctx.session.status, /did not commit/);
  }

  {
    const ctx = link();
    ctx.session.bring();
    ctx.session.onLine(device("handoff.accept", "e-accept005", { expression: "idle" }));
    await flush();
    ctx.session.onLine(device("handoff.commit", "e-commit003", { expression: "idle" }));
    ctx.session.hideCat();
    assert.equal(ctx.session.phase, "hidden_hold");
    assert.equal(ctx.chip.visible, true);
    assert.equal(ctx.cat.visible, false);
    ctx.session.unhide();
    assert.equal(ctx.session.phase, "present");
    assert.equal(ctx.cat.visible, true);
    assert.equal(ctx.chip.visible, false);
    await ctx.session.returnToDevice();
    assert.equal(ctx.session.phase, "returning");
    ctx.session.onLine(device("handoff.abort", "e-abort0002", { reason: "return_timeout" }));
    assert.equal(ctx.session.phase, "present");
    assert.equal(ctx.cat.visible, true);
    assert.match(ctx.session.status, /did not confirm/);
    assert.equal(ctx.clock.timers.some((timer) => timer.key === "return"), false);
  }

  {
    const ctx = link();
    ctx.session.bring();
    ctx.session.onLine(device("handoff.accept", "e-accept006", { expression: "idle" }));
    await flush();
    ctx.session.onLine(device("handoff.commit", "e-commit004", { expression: "idle" }));
    const returned = ctx.session.returnToDevice();
    ctx.session.onLine(device("handoff.shown", "e-shown0001", { expression: "idle" }));
    await returned;
    assert.equal(ctx.session.phase, "linked");
    assert.equal(ctx.cat.visible, false);
    ctx.session.onLine(device("handoff.shown", "e-shown0001", { expression: "idle" }));
    assert.equal(ctx.session.phase, "linked");
  }

  {
    const ctx = link();
    ctx.session.bring();
    ctx.session.onLine(device("handoff.accept", "e-accept007", { expression: "idle" }));
    await flush();
    ctx.session.onLine(device("handoff.commit", "e-commit005", { expression: "idle" }));
    ctx.session.onClose("unplugged");
    assert.equal(ctx.session.phase, "disconnected");
    assert.equal(ctx.cat.visible, false);
    assert.equal(ctx.chip.visible, false);
    assert.match(ctx.session.status, /unplugged/);
    ctx.session.onLine(
      device("hello", "e-hello0002", { role: "device", proto: 1, expressions: ["idle"], owner: "device" }),
    );
    assert.equal(ctx.session.phase, "disconnected");
  }

  {
    const ctx = link();
    ctx.clock.fire("silence");
    assert.equal(ctx.session.phase, "disconnected");
    assert.match(ctx.session.status, /quiet/);
    assert.equal(ctx.closed(), 1);
  }

  {
    const ctx = link();
    for (let i = 0; i < 5; i += 1) ctx.session.onLine("{");
    assert.equal(ctx.session.phase, "disconnected");
    assert.match(ctx.session.status, /malformed/);
  }

  {
    const ctx = link();
    ctx.session.bring();
    assert.equal(ctx.session.phase, "linked");
    ctx.session.onLine(device("handoff.commit", "e-stale0001", { expression: "idle" }));
    assert.equal(ctx.session.phase, "linked");
    assert.equal(ctx.cat.visible, false);
  }

  console.log("edge ok");
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
