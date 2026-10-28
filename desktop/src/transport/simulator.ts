import { DeviceSim } from "../sim/device_sim";
import type { ByteStream } from "./serial";

export class SimulatorStream implements ByteStream {
  readonly id = "simulator://jitters";
  private sim = new DeviceSim();
  private lineCb: (line: string) => void = () => {};
  private closeCb: (reason: string) => void = () => {};

  onLine(cb: (line: string) => void) {
    this.lineCb = cb;
  }

  onClose(cb: (reason: string) => void) {
    this.closeCb = cb;
  }

  async open() {
    this.sim = new DeviceSim();
    this.sim.onOut = (line) => this.lineCb(line);
    this.sim.start();
  }

  send(line: string) {
    this.sim.feed(line);
  }

  async close() {
    this.closeCb("closed");
  }
}
