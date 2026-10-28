import fs from "fs";
import path from "path";

export const PREFERRED_PORT = "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0";
export const BAUD_PLACEHOLDER = 115200;
export const BAUD_LABEL = "115200 baud (unconfirmed placeholder)";
const BLOCKED = new Set(["/dev/ttyACM0", "/dev/ttyACM1"]);

export interface PortChoice {
  id: string;
  label: string;
  detail: string;
}

/** Directory listing only. Does not open any serial device. */
export function listPorts(): PortChoice[] {
  const ports: PortChoice[] = [
    { id: "simulator://jitters", label: "Simulator", detail: "No board" },
  ];
  const dir = "/dev/serial/by-id";
  let names: string[] = [];
  try {
    names = fs.readdirSync(dir);
  } catch {
    names = [];
  }
  names.sort();
  for (const name of names) {
    const id = path.join(dir, name);
    let real = "";
    try {
      real = fs.realpathSync(id);
    } catch {
      real = "";
    }
    const blocked = BLOCKED.has(real);
    ports.push({
      id,
      label: name,
      detail: blocked ? `${real} — Framework LED matrix, not the display` : real,
    });
  }
  return ports;
}

export function assertOpenable(id: string) {
  if (id === "simulator://jitters") return;
  let real = id;
  try {
    real = fs.realpathSync(id);
  } catch {
    throw new Error("That port is not connected");
  }
  if (BLOCKED.has(real) || BLOCKED.has(id)) {
    throw new Error("Refusing the Framework LED matrix port");
  }
}
