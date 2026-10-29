export interface SyncEvent {
  y: number;
  m: number;
  d: number;
  title: string;
}

export interface TimeSyncPayload {
  unix: number;
  offset: number;
  events: { y: number; m: number; d: number; title: string }[];
}

/** Local offset is minutes east of UTC, the opposite of Date.getTimezoneOffset(). */
export function timeSyncPayload(now: Date, events: SyncEvent[] = []): TimeSyncPayload {
  return {
    unix: Math.floor(now.getTime() / 1000),
    offset: -now.getTimezoneOffset(),
    events: events.slice(0, 3).map((event) => ({
      y: event.y,
      m: event.m,
      d: event.d,
      title: event.title.slice(0, 12),
    })),
  };
}
