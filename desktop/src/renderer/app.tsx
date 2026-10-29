import React, { useEffect, useRef, useState } from "react";
import { createRoot } from "react-dom/client";
import type { JittersApi } from "../main/preload";
import type { Snapshot } from "../handoff/desktop_handoff";
import type { PortChoice } from "../transport/ports";
import { controlsFor } from "./controls";

declare global {
  interface Window {
    jitters: JittersApi;
  }
}

const view = new URLSearchParams(location.search).get("view") ?? "picker";
type Phase = Snapshot["phase"];

function useSnap(): Snapshot | null {
  const [snap, setSnap] = useState<Snapshot | null>(null);
  useEffect(() => {
    void window.jitters.getState().then(setSnap);
    window.jitters.onState(setSnap);
  }, []);
  return snap;
}

function phaseLabel(phase: Phase | undefined): string {
  switch (phase) {
    case "connecting":
      return "Connecting";
    case "linked":
      return "On the display";
    case "preparing":
      return "Coming over";
    case "present":
      return "On this laptop";
    case "hidden_hold":
      return "Hidden here";
    case "returning":
      return "Going back";
    default:
      return "Not connected";
  }
}

function phaseHint(phase: Phase | undefined): string {
  switch (phase) {
    case "connecting":
      return "The display says hello after it resets. This usually takes a moment.";
    case "linked":
      return "Jitters is walking on the panel. Tap the cat there to pet it, or the MENU bar to open games. Bring the cat here when you want this window.";
    case "preparing":
      return "The panel should say the cat is heading to the laptop. This window opens when the handoff commits.";
    case "present":
      return "Click the cat to pet it. Hide only tucks the window away. Send back when the panel should have Jitters again.";
    case "hidden_hold":
      return "The cat is still on the laptop. The display stays on the away screen until you send Jitters back.";
    case "returning":
      return "Waiting for the display to show Jitters again. The panel says back, then the cat walks.";
    default:
      return "Choose the yellow display. The CH340 connects on its own when it is plugged in. Disconnect stays disconnected until the cable is unplugged.";
  }
}

function railStep(phase: Phase | undefined): number {
  if (phase === "preparing" || phase === "returning") return 1;
  if (phase === "present" || phase === "hidden_hold") return 2;
  if (phase === "linked" || phase === "connecting") return 0;
  return -1;
}

function shortPort(id: string | null | undefined): string {
  if (!id) return "";
  if (id.startsWith("simulator:")) return "Simulator";
  const slash = id.lastIndexOf("/");
  return slash >= 0 ? id.slice(slash + 1) : id;
}

function Picker() {
  const snap = useSnap();
  const [ports, setPorts] = useState<PortChoice[]>([]);
  const [baud, setBaud] = useState("");
  const [selected, setSelected] = useState("");
  const [error, setError] = useState("");

  useEffect(() => {
    let stop = false;
    const load = () => {
      void window.jitters.listPorts().then((info) => {
        if (stop) return;
        setPorts(info.ports);
        setBaud(info.baudLabel);
        const preferred = info.ports.some((port) => port.id === info.preferred) ? info.preferred : "";
        setSelected((current) => current || preferred);
      });
    };
    load();
    const timer = window.setInterval(load, 1500);
    return () => {
      stop = true;
      window.clearInterval(timer);
    };
  }, []);

  useEffect(() => {
    if (snap?.portId) setSelected(snap.portId);
  }, [snap?.portId]);

  async function run(fn: () => Promise<Snapshot>) {
    setError("");
    try {
      await fn();
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err));
    }
  }

  const phase = snap?.phase ?? "disconnected";
  const controls = controlsFor(phase, Boolean(snap?.asking));
  const step = railStep(phase);
  const onlySim = ports.length <= 1;
  const chosen = ports.find((port) => port.id === selected);

  return (
    <main className="picker">
      <h1>Jitters</h1>
      <p className="tag">The cat on the yellow display</p>
      <p className={`pill pill-${phase}`}>{phaseLabel(phase)}</p>
      <ol className="rail">
        <li className={step === 0 ? "on" : ""}>Display</li>
        <li className={step === 1 ? "on" : ""}>Handoff</li>
        <li className={step === 2 ? "on" : ""}>Laptop</li>
      </ol>
      <p className="lead">{phaseHint(phase)}</p>
      <p className="status">{snap?.status ?? "Choose the display port"}</p>
      {snap?.portId ? <p className="baud">Port {shortPort(snap.portId)}</p> : null}
      <ul>
        {ports.map((port) => (
          <li key={port.id} className={selected === port.id ? "picked" : port.blocked ? "blocked" : ""}>
            <label>
              <input
                type="radio"
                name="port"
                value={port.id}
                checked={selected === port.id}
                disabled={port.blocked}
                onChange={() => setSelected(port.id)}
              />
              <span>
                <strong>{port.label}</strong>
                <small>{port.detail}</small>
              </span>
            </label>
          </li>
        ))}
      </ul>
      <p className="baud">
        {onlySim
          ? "Plug in the yellow display. The CH340 connects when that cable is present."
          : "The CH340 connects when it is plugged in. Disconnect stays off until the cable is unplugged."}
      </p>
      <p className="baud">{baud}</p>
      {error ? <p className="error">{error}</p> : null}
      <div className="actions">
        {controls.connect ? (
          <button
            type="button"
            className="primary"
            disabled={!selected || Boolean(chosen?.blocked)}
            onClick={() => run(() => window.jitters.connect(selected))}
          >
            Connect
          </button>
        ) : null}
        {phase === "connecting" ? (
          <button type="button" className="primary" disabled>
            Connecting…
          </button>
        ) : null}
        {phase === "linked" ? (
          <button
            type="button"
            className="primary"
            disabled={!controls.bring}
            onClick={() => run(() => window.jitters.bring())}
          >
            {snap?.asking ? "Asking the display…" : "Bring to laptop"}
          </button>
        ) : null}
        {phase === "preparing" ? (
          <button type="button" className="primary" disabled>
            Coming over…
          </button>
        ) : null}
        {phase === "present" ? (
          <button type="button" className="primary" onClick={() => run(() => window.jitters.returnToDevice())}>
            Send back to display
          </button>
        ) : null}
        {phase === "hidden_hold" ? (
          <button type="button" className="primary" onClick={() => run(() => window.jitters.unhide())}>
            Show cat
          </button>
        ) : null}
        {phase === "returning" ? (
          <button type="button" className="primary" disabled>
            Going back…
          </button>
        ) : null}
        <div className="row">
          {controls.hide ? (
            <button type="button" onClick={() => run(() => window.jitters.hide())}>
              Hide
            </button>
          ) : null}
          {controls.show ? (
            <button type="button" onClick={() => run(() => window.jitters.returnToDevice())}>
              Send back
            </button>
          ) : null}
          {controls.disconnect ? (
            <button type="button" onClick={() => run(() => window.jitters.disconnect())}>
              Disconnect
            </button>
          ) : null}
        </div>
      </div>
    </main>
  );
}

type FaceFrame = "idle" | "blink" | "mouth" | "happy";

// Matches the panel: blink every 3.4s for 140ms, mouth every 7.6s for 380ms.
function useFaceFrame(): FaceFrame {
  const [frame, setFrame] = useState<FaceFrame>("idle");
  useEffect(() => {
    const started = performance.now();
    let timer = 0;
    const schedule = () => {
      const elapsed = performance.now() - started;
      const blinkPos = elapsed % 3400;
      const mouthPos = elapsed % 7600;
      let next: FaceFrame = "idle";
      let wait = 200;
      if (blinkPos >= 1600 && blinkPos < 1740) {
        next = "blink";
        wait = 1740 - blinkPos;
      } else if (mouthPos >= 2200 && mouthPos < 2580) {
        next = "mouth";
        wait = 2580 - mouthPos;
      } else {
        const blinkIn = blinkPos < 1600 ? 1600 - blinkPos : 3400 - blinkPos + 1600;
        const mouthIn = mouthPos < 2200 ? 2200 - mouthPos : 7600 - mouthPos + 2200;
        wait = Math.min(blinkIn, mouthIn);
      }
      setFrame(next);
      timer = window.setTimeout(schedule, Math.max(30, wait));
    };
    schedule();
    return () => window.clearTimeout(timer);
  }, []);
  return frame;
}

function catWhere(phase: Phase | undefined): string {
  switch (phase) {
    case "present":
      return "On this laptop";
    case "hidden_hold":
      return "Hidden here";
    case "returning":
      return "Going back";
    case "preparing":
      return "Coming over";
    default:
      return "Jitters";
  }
}

function Cat() {
  const snap = useSnap();
  const blink = useFaceFrame();
  const until = useRef(0);
  const [happy, setHappy] = useState(false);
  const frame: FaceFrame = happy ? "happy" : blink;

  function pet(event: React.MouseEvent) {
    event.preventDefault();
    until.current = performance.now() + 700;
    setHappy(true);
    window.setTimeout(() => {
      if (performance.now() >= until.current) setHappy(false);
    }, 720);
  }

  const phase = snap?.phase ?? "disconnected";
  const controls = controlsFor(phase, Boolean(snap?.asking));
  return (
    <main className="cat">
      <button type="button" className={happy ? "pet happy" : "pet"} onMouseDown={pet}>
        <img
          src={`assets/${frame}.png`}
          alt="Jitters, click to pet"
          data-frame={frame}
          width={128}
          height={128}
          draggable={false}
        />
      </button>
      <div className="plate">
        <p className="where">{catWhere(phase)}</p>
        <p className="caption">{happy ? "purr" : "click to pet"}</p>
        <div className="row">
          <button type="button" disabled={!controls.hide} onClick={() => void window.jitters.hide()}>
            Hide
          </button>
          <button type="button" disabled={!controls.sendBack} onClick={() => void window.jitters.returnToDevice()}>
            Send back
          </button>
        </div>
      </div>
    </main>
  );
}

function InvadersStill() {
  const aliens = [true, true, true, false, true, true, true, true];
  return (
    <main className="invaders" data-invaders="1">
      <p className="inv-score">LV 1/100</p>
      <p className="inv-points">SCORE 20</p>
      <span className="link-dot" />
      <div className="inv-row">
        {aliens.map((alive, index) => (
          <span key={index} className={alive ? "alien" : "alien gone"} />
        ))}
      </div>
      <span className="shot" />
      <span className="ship" />
      <div className="bar">
        <span>LEFT</span>
        <span>FIRE</span>
        <span>RIGHT</span>
      </div>
    </main>
  );
}

function Chip() {
  return (
    <main className="chip">
      <div>
        <strong>Hidden</strong>
        <small>Still on the laptop</small>
      </div>
      <button type="button" onClick={() => void window.jitters.unhide()}>
        Show
      </button>
    </main>
  );
}

const root = document.getElementById("root");
if (root) {
  createRoot(root).render(
    view === "cat" ? <Cat /> : view === "chip" ? <Chip /> : view === "invaders" ? <InvadersStill /> : <Picker />,
  );
}
