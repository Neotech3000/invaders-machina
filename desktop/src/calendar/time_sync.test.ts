import assert from "assert";
import { encode, parseLine } from "../protocol/schema";
import { timeSyncPayload, type SyncEvent } from "./time_sync";

function main() {
  const now = new Date("2026-10-29T15:04:05Z");
  const events: SyncEvent[] = [
    { y: 2026, m: 10, d: 31, title: "Dentist visit extra" },
    { y: 2026, m: 11, d: 2, title: "Lab" },
    { y: 2026, m: 11, d: 3, title: "Walk" },
    { y: 2026, m: 11, d: 4, title: "Ignored" },
  ];
  const payload = timeSyncPayload(now, events);
  assert.equal(payload.unix, Math.floor(now.getTime() / 1000));
  assert.equal(payload.offset, -now.getTimezoneOffset());
  assert.equal(payload.events.length, 3);
  assert.equal(payload.events[0].title, "Dentist visi");
  assert.equal(payload.events[2].title, "Walk");

  const line = encode("time.sync", "d-timesync1", { ...payload });
  const msg = parseLine(line);
  assert.ok(msg);
  assert.equal(msg?.type, "time.sync");
  assert.equal(msg?.payload.unix, payload.unix);
  assert.equal(msg?.payload.offset, payload.offset);
  const parsedEvents = msg?.payload.events as { title: string }[];
  assert.equal(parsedEvents.length, 3);
  assert.equal(parsedEvents[0].title, "Dentist visi");

  const empty = encode("time.sync", "d-timesync2", { ...timeSyncPayload(now) });
  assert.equal((parseLine(empty)?.payload.events as unknown[]).length, 0);

  console.log("time sync ok");
}

main();
