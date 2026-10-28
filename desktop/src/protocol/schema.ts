import fs from "fs";
import path from "path";
import Ajv from "ajv";

export type Expr = "idle";

export interface Timeouts {
  helloMs: number;
  readyMs: number;
  commitMs: number;
  returnMs: number;
  pingIntervalMs: number;
  silenceMs: number;
  dedupSize: number;
  maxLineBytes: number;
}

export interface Envelope {
  v: number;
  type: string;
  id: string;
  payload: Record<string, unknown>;
}

const schemaDir = path.resolve(__dirname, "../../shared/schema");

export const expressions = (
  JSON.parse(fs.readFileSync(path.join(schemaDir, "expressions.json"), "utf8")) as { ids: Expr[] }
).ids;

export const timeouts = JSON.parse(
  fs.readFileSync(path.join(schemaDir, "timeouts.json"), "utf8"),
) as Timeouts;

const ajv = new Ajv({ allErrors: true, validateSchema: false });
const validate = ajv.compile(
  JSON.parse(fs.readFileSync(path.join(schemaDir, "protocol.schema.json"), "utf8")),
);

export function parseLine(line: string): Envelope | null {
  if (line.length > timeouts.maxLineBytes) return null;
  let value: unknown;
  try {
    value = JSON.parse(line);
  } catch {
    return null;
  }
  if (!validate(value)) return null;
  return value as Envelope;
}

export function encode(type: string, id: string, payload: Record<string, unknown>): string {
  const line = JSON.stringify({ v: 1, type, id, payload });
  if (!validate(JSON.parse(line))) {
    throw new Error(`refusing to send invalid ${type}`);
  }
  return line;
}

export function isExpr(value: unknown): value is Expr {
  return value === "idle";
}
