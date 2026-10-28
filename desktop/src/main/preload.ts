import { contextBridge, ipcRenderer } from "electron";
import type { Snapshot } from "../handoff/desktop_handoff";
import type { PortChoice } from "../transport/ports";

const api = {
  getState: (): Promise<Snapshot> => ipcRenderer.invoke("state"),
  listPorts: (): Promise<{ ports: PortChoice[]; preferred: string; baudLabel: string }> =>
    ipcRenderer.invoke("ports"),
  connect: (id: string): Promise<Snapshot> => ipcRenderer.invoke("connect", id),
  disconnect: (): Promise<Snapshot> => ipcRenderer.invoke("disconnect"),
  bring: (): Promise<Snapshot> => ipcRenderer.invoke("bring"),
  hide: (): Promise<Snapshot> => ipcRenderer.invoke("hide"),
  unhide: (): Promise<Snapshot> => ipcRenderer.invoke("unhide"),
  returnToDevice: (): Promise<Snapshot> => ipcRenderer.invoke("return"),
  onState: (cb: (snap: Snapshot) => void) => {
    const listener = (_event: unknown, snap: Snapshot) => cb(snap);
    ipcRenderer.on("state", listener);
  },
};

contextBridge.exposeInMainWorld("jitters", api);

export type JittersApi = typeof api;
