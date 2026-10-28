import React, { useEffect, useState } from "react";
import { createRoot } from "react-dom/client";
import type { JittersApi } from "../main/preload";
import type { Snapshot } from "../handoff/desktop_handoff";
import type { PortChoice } from "../transport/ports";

declare global {
  interface Window {
    jitters: JittersApi;
  }
}

const view = new URLSearchParams(location.search).get("view") ?? "picker";

function useSnap(): Snapshot | null {
  const [snap, setSnap] = useState<Snapshot | null>(null);
  useEffect(() => {
    void window.jitters.getState().then(setSnap);
    window.jitters.onState(setSnap);
  }, []);
  return snap;
}

function Picker() {
  const snap = useSnap();
  const [ports, setPorts] = useState<PortChoice[]>([]);
  const [baud, setBaud] = useState("");
  const [selected, setSelected] = useState("");
  const [error, setError] = useState("");

  useEffect(() => {
    void window.jitters.listPorts().then((info) => {
      setPorts(info.ports);
      setBaud(info.baudLabel);
      const preferred = info.ports.some((port) => port.id === info.preferred) ? info.preferred : "";
      setSelected(preferred);
    });
  }, []);

  async function run(fn: () => Promise<Snapshot>) {
    setError("");
    try {
      await fn();
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err));
    }
  }

  const phase = snap?.phase ?? "disconnected";
  const connected = phase !== "disconnected";

  return (
    <main className="picker">
      <h1>Jitters</h1>
      <p className="baud">{baud}</p>
      <ul>
        {ports.map((port) => (
          <li key={port.id}>
            <label>
              <input
                type="radio"
                name="port"
                value={port.id}
                checked={selected === port.id}
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
      <p className="status">{snap?.status ?? "Choose a port"}</p>
      {error ? <p className="error">{error}</p> : null}
      <div className="row">
        {!connected ? (
          <button disabled={!selected} onClick={() => run(() => window.jitters.connect(selected))}>
            Connect
          </button>
        ) : (
          <button onClick={() => run(() => window.jitters.disconnect())}>Disconnect</button>
        )}
        <button disabled={phase !== "linked"} onClick={() => run(() => window.jitters.bring())}>
          Bring to laptop
        </button>
      </div>
      <div className="row">
        <button disabled={phase !== "present"} onClick={() => run(() => window.jitters.park())}>
          Park
        </button>
        <button disabled={phase !== "present"} onClick={() => run(() => window.jitters.hide())}>
          Hide
        </button>
        <button
          disabled={phase !== "present" && phase !== "hidden_hold"}
          onClick={() => run(() => window.jitters.returnToDevice())}
        >
          Return
        </button>
      </div>
    </main>
  );
}

function Cat() {
  const snap = useSnap();
  const expression = snap?.expression ?? "idle";
  return (
    <main className="cat">
      <img src={`assets/${expression}.png`} alt={expression} width={128} height={128} />
      <div className="row">
        <button disabled={snap?.phase !== "present"} onClick={() => void window.jitters.park()}>
          Park
        </button>
        <button disabled={snap?.phase !== "present"} onClick={() => void window.jitters.hide()}>
          Hide
        </button>
        <button
          disabled={snap?.phase !== "present" && snap?.phase !== "hidden_hold"}
          onClick={() => void window.jitters.returnToDevice()}
        >
          Return
        </button>
      </div>
    </main>
  );
}

function Chip() {
  return (
    <main className="chip">
      <button onClick={() => void window.jitters.unhide()}>Show cat</button>
    </main>
  );
}

const root = document.getElementById("root");
if (root) {
  createRoot(root).render(view === "cat" ? <Cat /> : view === "chip" ? <Chip /> : <Picker />);
}
