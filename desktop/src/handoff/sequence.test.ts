import assert from "assert";
import { DesktopHandoff, type CatPort, type Clock, type Expr } from "./desktop_handoff";
import { DeviceSim } from "../sim/device_sim";

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
}

class FakeCat implements CatPort {
  visible = false;
  expression: Expr = "idle";
  async show(expression: Expr) {
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

async function flush() {
  await new Promise((resolve) => setImmediate(resolve));
}

async function main() {
  const cat = new FakeCat();
  const chip = new FakeCat();
  const sim = new DeviceSim();
  const trace: string[] = [];
  const session = new DesktopHandoff({
    cat,
    chip,
    clock: new ManualClock(),
    send: (line) => {
      sim.feed(line);
    },
    close: () => {},
    trace: (event) => trace.push(event),
    firstHelloMs: 8000,
  });
  sim.onOut = (line) => session.onLine(line);

  session.onOpen("simulator://jitters");
  sim.start();
  assert.equal(session.phase, "linked");
  assert.equal(cat.visible, false);
  assert.equal(sim.visible, true);

  session.bring();
  await flush();
  assert.equal(trace[0], "window-visible");
  assert.equal(trace[1], "sent-ready");
  assert.ok(trace.indexOf("window-visible") < trace.indexOf("sent-ready"));
  assert.equal(cat.visible, true);
  assert.equal(sim.visible, false);
  assert.equal(session.phase, "present");
  assert.equal(session.owner, "desktop");

  session.hideCat();
  assert.equal(cat.visible, false);
  assert.equal(chip.visible, true);
  assert.equal(session.phase, "hidden_hold");
  assert.equal(sim.visible, false);

  await session.returnToDevice();
  await flush();
  assert.equal(cat.visible, false);
  assert.equal(sim.visible, true);
  assert.equal(session.phase, "linked");
  assert.equal(session.owner, "device");
  assert.ok(trace.includes("window-hidden"));
  assert.ok(trace.includes("shown"));
  const hiddenAt = trace.indexOf("window-hidden");
  const shownAt = trace.indexOf("shown");
  assert.ok(hiddenAt < shownAt);

  console.log("sequence ok", trace.join(" "));
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
