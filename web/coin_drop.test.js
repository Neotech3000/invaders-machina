"use strict";

const fs = require("fs");
const path = require("path");

const html = fs.readFileSync(path.join(__dirname, "index.html"), "utf8");
const start = html.indexOf("// COIN_RULES_START");
const end = html.indexOf("// COIN_RULES_END");
if (start < 0 || end < start) {
  console.error("coin rule markers missing from web/index.html");
  process.exit(1);
}

const state = {
  save: { clearedLv: [], clearedGw: [] },
  demo: false,
  R: { level: 1, god: 0, godEntrance: false, godWave: 0 },
  persists: 0,
};

const api = new Function(
  "state",
  "LEVELS",
  "COIN_MAX",
  `let save = state.save;
   let demo = false;
   let R = state.R;
   function persist() { state.persists += 1; }
   ${html.slice(start, end)}
   return {
     bossBounty, payout, markLevel, markGodWave, levelCleared, godWaveCleared, godWaveId, replayCut,
     setDemo(v) { demo = v; },
     setR(r) { R = r; state.R = r; },
     reload(next) { save = next; state.save = next; }
   };`
)(state, 300, 16);

let failures = 0;
function expect(ok, name) {
  if (ok) return;
  console.error("FAIL " + name);
  failures += 1;
}

function fresh(level, god, wave, entrance) {
  state.persists = 0;
  const save = { clearedLv: [], clearedGw: [] };
  api.reload(save);
  api.setDemo(false);
  api.setR({ level, god: god || 0, godEntrance: !!entrance, godWave: wave || 0 });
  return save;
}

fresh(1, 0, 0, false);
expect(api.bossBounty(2) === 2, "level 1 mini boss first clear");
expect(api.payout(1) === 1, "grunt first clear");
expect(api.payout(api.bossBounty(3)) === 3, "level 5-style boss base at level 1");

fresh(25, 1, 0, false);
expect(api.bossBounty(2) === 3, "level 25 scaled mini was 3");
expect(api.payout(5) === 5, "level 25 god mini first clear is 5");
expect(api.payout(api.bossBounty(3)) === 4, "level 25 boss first clear");
expect(api.payout(api.bossBounty(4)) === 5, "level 25 raid first clear");
expect(api.payout(api.bossBounty(10)) === 11, "level 25 god first clear");

api.markGodWave(25, 0);
expect(api.godWaveCleared(25, 0), "Zeus wave 1 is stored");
expect(!api.godWaveCleared(25, 1), "Zeus wave 2 is still open");
expect(state.persists === 1, "marking a wave writes the save");
expect(api.payout(5) === 1, "level 25 god mini replay is 1");
expect(api.payout(1) === 0, "grunt replay is 0");
api.setR({ level: 25, god: 1, godEntrance: false, godWave: 1 });
expect(api.payout(5) === 5, "an open god wave still pays 5");
api.setR({ level: 25, god: 1, godEntrance: true, godWave: 0 });
expect(api.payout(api.bossBounty(10)) === 11, "the god fight ignores a cleared goon wave");

api.markLevel(25);
expect(api.levelCleared(25), "level 25 is stored");
expect(api.payout(api.bossBounty(10)) === 7, "a cleared god level cuts the god fight");
api.setR({ level: 25, god: 1, godEntrance: false, godWave: 1 });
expect(api.payout(5) === 1, "a cleared level cuts every wave");

const kept = state.save;
api.reload({ clearedLv: kept.clearedLv.slice(), clearedGw: kept.clearedGw.slice() });
api.setR({ level: 1, god: 0, godEntrance: false, godWave: 0 });
expect(api.levelCleared(25), "level 25 survives a reload");
expect(api.godWaveCleared(25, 0), "Zeus wave 1 survives a reload");
api.setR({ level: 25, god: 1, godEntrance: false, godWave: 0 });
expect(api.payout(5) === 1, "a checkpoint replay still cuts");

api.setDemo(true);
expect(api.payout(5) === 5, "the attract demo does not take the replay cut");
const before = state.save.clearedLv.length;
api.markLevel(1);
api.markGodWave(25, 1);
expect(state.save.clearedLv.length === before, "demo does not record a level");
expect(!api.godWaveCleared(25, 1), "demo does not record a wave");

fresh(50, 2, 0, false);
expect(api.bossBounty(2) === 4, "level 50 scaled mini was 4");
expect(api.payout(5) === 5, "level 50 god mini is still 5");
fresh(100, 4, 0, false);
expect(api.bossBounty(2) === 6, "level 100 scaled mini was 6");
expect(api.payout(api.bossBounty(3)) === 7, "level 100 boss first clear");
api.markLevel(100);
expect(api.payout(api.bossBounty(3)) === 3, "level 100 boss replay");
fresh(300, 12, 25, false);
expect(api.bossBounty(2) === 14, "level 300 scaled mini was 14");
expect(api.payout(5) === 5, "level 300 god mini first clear is 5");
expect(api.bossBounty(10) === 16, "level 300 god bounty caps at 16");
expect(api.bossBounty(4) === 16, "level 300 raid caps at 16");
api.markLevel(300);
api.setR({ level: 300, god: 12, godEntrance: true, godWave: 25 });
expect(api.payout(api.bossBounty(10)) === 12, "level 300 god replay is 12");
expect(api.payout(api.bossBounty(4)) === 12, "level 300 raid replay is 12");
api.setR({ level: 300, god: 12, godEntrance: false, godWave: 25 });
expect(api.payout(5) === 1, "level 300 god mini replay is 1");

expect(api.godWaveId(25, 0) === 0, "first god wave is id 0");
expect(api.godWaveId(50, 0) === 26, "second god starts at id 26");
expect(api.godWaveId(300, 25) === 311, "last god wave is the last id");
expect(api.godWaveId(24, 0) === -1, "a normal level has no god wave");
expect(api.godWaveId(25, 26) === -1, "wave 27 is out of range");
api.markGodWave(24, 0);
expect(state.save.clearedGw.length === 0, "a normal level cannot store a god wave");
api.markLevel(0);
api.markLevel(301);
expect(!api.levelCleared(0) && !api.levelCleared(301), "only levels 1 through 300 are stored");

const wavesAt300 = 4 + 2 * (Math.floor(300 / 25) - 1);
expect(wavesAt300 === 26, "level 300 has 26 goon waves");

if (failures !== 0) {
  console.error(failures + " failed");
  process.exit(1);
}
console.log("web coin drop ok");
