import type { Phase } from "../handoff/desktop_handoff";

export interface PickerControls {
  connect: boolean;
  bring: boolean;
  hide: boolean;
  show: boolean;
  sendBack: boolean;
  disconnect: boolean;
  waiting: boolean;
}

/** Which picker and cat-window actions are live for this handoff phase. */
export function controlsFor(phase: Phase, asking = false): PickerControls {
  return {
    connect: phase === "disconnected",
    bring: phase === "linked" && !asking,
    hide: phase === "present",
    show: phase === "hidden_hold",
    sendBack: phase === "present" || phase === "hidden_hold",
    disconnect: phase !== "disconnected",
    waiting: phase === "connecting" || phase === "preparing" || phase === "returning",
  };
}
