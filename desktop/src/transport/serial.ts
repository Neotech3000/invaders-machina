import { SerialPort } from "serialport";
import { BAUD_PLACEHOLDER } from "./ports";

export const kMaxSerialBuf = 2048;
export const kMaxSerialLine = 1024;

/** Pull complete NDJSON lines out of a UART byte stream. Overlong input is dropped. */
export function takeLines(buf: string, chunk: string): { buf: string; lines: string[] } {
  let next = buf + chunk;
  const lines: string[] = [];
  if (next.length > kMaxSerialBuf && !next.includes("\n")) {
    return { buf: "", lines };
  }
  let nl = next.indexOf("\n");
  while (nl >= 0) {
    const line = next.slice(0, nl).replace(/\r$/, "");
    next = next.slice(nl + 1);
    if (line.length > 0 && line.length <= kMaxSerialLine) lines.push(line);
    if (next.length > kMaxSerialBuf && !next.includes("\n")) {
      next = "";
      break;
    }
    nl = next.indexOf("\n");
  }
  if (next.length > kMaxSerialBuf) next = "";
  return { buf: next, lines };
}

export interface ByteStream {
  readonly id: string;
  open(): Promise<void>;
  close(): Promise<void>;
  send(line: string): void;
  onLine(cb: (line: string) => void): void;
  onClose(cb: (reason: string) => void): void;
}

/**
 * CH340 UART. Baud is the unconfirmed placeholder.
 * Opening pulses RTS the way this adapter resets the ESP32, then releases it.
 */
export class SerialByteStream implements ByteStream {
  private port: SerialPort | null = null;
  private buf = "";
  private lineCb: (line: string) => void = () => {};
  private closeCb: (reason: string) => void = () => {};
  private opened = false;
  private ended = false;

  constructor(readonly id: string) {}

  onLine(cb: (line: string) => void) {
    this.lineCb = cb;
  }

  onClose(cb: (reason: string) => void) {
    this.closeCb = cb;
  }

  open(): Promise<void> {
    const port = new SerialPort({
      path: this.id,
      baudRate: BAUD_PLACEHOLDER,
      autoOpen: false,
      rtscts: false,
    });
    this.port = port;
    port.on("data", (chunk: Buffer) => {
      if (this.ended) return;
      const taken = takeLines(this.buf, chunk.toString("utf8"));
      this.buf = taken.buf;
      for (const line of taken.lines) this.lineCb(line);
    });
    port.on("close", () => {
      this.fail("closed");
    });
    port.on("error", (err) => {
      this.fail(err.message);
    });
    return new Promise((resolve, reject) => {
      port.open((err) => {
        if (err) {
          reject(err);
          return;
        }
        this.opened = true;
        this.ended = false;
        // RTS high holds this CH340 board in reset. Pulse once, then run.
        port.set({ rts: true, dtr: false }, () => {
          setTimeout(() => {
            port.set({ rts: false, dtr: false }, () => resolve());
          }, 80);
        });
      });
    });
  }

  send(line: string) {
    if (!this.port || !this.opened) throw new Error("port is closed");
    this.port.write(line + "\n");
  }

  close(): Promise<void> {
    const port = this.port;
    this.opened = false;
    this.ended = true;
    if (!port) return Promise.resolve();
    return new Promise((resolve) => {
      port.set({ rts: false, dtr: false }, () => {
        port.close(() => resolve());
      });
    });
  }

  private fail(reason: string) {
    if (this.ended) return;
    this.ended = true;
    this.opened = false;
    this.closeCb(reason);
  }
}
