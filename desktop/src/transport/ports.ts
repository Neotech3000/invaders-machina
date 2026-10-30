import fs from "fs";
import path from "path";

export const BAUD_PLACEHOLDER = 115200;
export const BAUD_LABEL = "115200 baud (unconfirmed placeholder)";

const SIMULATOR_ID = "simulator://jitters";
const PORT_PLACEHOLDER = "REPLACE_WITH_YOUR_PORT";

export interface PortChoice {
  id: string;
  label: string;
  detail: string;
  blocked: boolean;
}

const SIMULATOR: PortChoice = {
  id: SIMULATOR_ID,
  label: "Simulator",
  detail: "In-process handoff, no cable",
  blocked: false,
};

export function isPlaceholderPort(port: string): boolean {
  const value = port.trim();
  return value.length === 0 || value === PORT_PLACEHOLDER;
}

/** Prefer an explicit local port, otherwise the only display-like port. */
export function choosePreferred(ports: PortChoice[], configured: string): string {
  const serial = ports.filter((port) => port.id !== SIMULATOR_ID);
  if (!isPlaceholderPort(configured) && serial.some((port) => port.id === configured)) {
    return configured;
  }
  const display = serial.filter((port) =>
    /1a86|ch340|ch341|qinheng|wchusb/i.test(`${port.id} ${port.detail}`),
  );
  if (display.length === 1) return display[0].id;
  if (serial.length === 1) return serial[0].id;
  return "";
}

export function configuredPort(cwd = process.cwd()): string {
  const file = path.join(cwd, "port.local.json");
  try {
    const parsed = JSON.parse(fs.readFileSync(file, "utf8")) as { port?: unknown };
    if (parsed && typeof parsed.port === "string" && !isPlaceholderPort(parsed.port)) {
      return parsed.port.trim();
    }
  } catch {
    /* no local file yet */
  }
  return "";
}

/** Directory and driver listing only. Does not open any serial device. */
export async function listPorts(): Promise<PortChoice[]> {
  const ports: PortChoice[] = [SIMULATOR];
  let found: Array<{
    path: string;
    manufacturer?: string;
    vendorId?: string;
    serialNumber?: string;
  }> = [];
  try {
    const { SerialPort } = await import("serialport");
    found = await SerialPort.list();
  } catch {
    found = [];
  }
  found.sort((a, b) => a.path.localeCompare(b.path));
  for (const item of found) {
    const detail = [
      item.manufacturer,
      item.vendorId ? `vid:${item.vendorId}` : "",
      item.serialNumber ? `usb:${item.serialNumber}` : "",
    ]
      .filter(Boolean)
      .join(" ");
    ports.push({
      id: item.path,
      label: item.path,
      detail: detail || "serial port",
      blocked: false,
    });
  }
  return ports;
}

export async function preferredPort(): Promise<string> {
  return choosePreferred(await listPorts(), configuredPort());
}

export function assertOpenable(id: string) {
  if (id === SIMULATOR_ID) return;
  if (isPlaceholderPort(id)) {
    throw new Error("Set the display port in desktop/port.local.json");
  }
  if (process.platform === "win32") {
    if (!/^COM\d+$/i.test(id)) {
      throw new Error("That port is not connected");
    }
    return;
  }
  try {
    fs.realpathSync(id);
  } catch {
    throw new Error("That port is not connected");
  }
}
