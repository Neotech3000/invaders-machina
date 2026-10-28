import { SerialPort } from "serialport";
import { BAUD_PLACEHOLDER } from "./ports";

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
      this.buf += chunk.toString("utf8");
      let nl = this.buf.indexOf("\n");
      while (nl >= 0) {
        const line = this.buf.slice(0, nl).replace(/\r$/, "");
        this.buf = this.buf.slice(nl + 1);
        if (line.length > 0) this.lineCb(line);
        nl = this.buf.indexOf("\n");
      }
    });
    port.on("close", () => {
      this.opened = false;
      this.closeCb("closed");
    });
    port.on("error", (err) => {
      this.closeCb(err.message);
    });
    return new Promise((resolve, reject) => {
      port.open((err) => {
        if (err) {
          reject(err);
          return;
        }
        this.opened = true;
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
    if (!port) return Promise.resolve();
    return new Promise((resolve) => {
      port.set({ rts: false, dtr: false }, () => {
        port.close(() => resolve());
      });
    });
  }
}
