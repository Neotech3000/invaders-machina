import assert from "assert";
import { choosePreferred, isPlaceholderPort, type PortChoice } from "./ports";

const sim: PortChoice = {
  id: "simulator://jitters",
  label: "Simulator",
  detail: "In-process handoff, no cable",
  blocked: false,
};
const ch340: PortChoice = {
  id: "/dev/ttyUSB0",
  label: "/dev/ttyUSB0",
  detail: "QinHeng Electronics vid:1a86",
  blocked: false,
};
const other: PortChoice = {
  id: "/dev/ttyACM0",
  label: "/dev/ttyACM0",
  detail: "USB serial",
  blocked: false,
};

assert.equal(isPlaceholderPort(""), true);
assert.equal(isPlaceholderPort("REPLACE_WITH_YOUR_PORT"), true);
assert.equal(isPlaceholderPort("COM5"), false);

assert.equal(choosePreferred([sim, ch340, other], ""), "/dev/ttyUSB0");
assert.equal(choosePreferred([sim, ch340, other], "/dev/ttyACM0"), "/dev/ttyACM0");
assert.equal(choosePreferred([sim, other], ""), "/dev/ttyACM0");
assert.equal(choosePreferred([sim, ch340, { ...ch340, id: "COM4" }], ""), "");
assert.equal(choosePreferred([sim], "REPLACE_WITH_YOUR_PORT"), "");

console.log("ports ok");
