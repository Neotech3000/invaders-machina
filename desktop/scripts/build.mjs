import esbuild from "esbuild";
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const out = path.join(root, "dist");
fs.mkdirSync(out, { recursive: true });
fs.copyFileSync(path.join(root, "src/renderer/index.html"), path.join(out, "index.html"));
fs.copyFileSync(path.join(root, "src/renderer/app.css"), path.join(out, "app.css"));
fs.cpSync(path.join(root, "assets/placeholder"), path.join(out, "assets"), { recursive: true });

const node = {
  bundle: true,
  platform: "node",
  format: "cjs",
  target: "node22",
  sourcemap: true,
  external: ["electron", "serialport"],
};

await esbuild.build({
  ...node,
  entryPoints: [path.join(root, "src/main/index.ts")],
  outfile: path.join(out, "main.cjs"),
});
await esbuild.build({
  ...node,
  entryPoints: [path.join(root, "src/main/preload.ts")],
  outfile: path.join(out, "preload.cjs"),
});
await esbuild.build({
  ...node,
  entryPoints: [path.join(root, "src/handoff/sequence.test.ts")],
  outfile: path.join(out, "sequence.test.cjs"),
});
await esbuild.build({
  ...node,
  entryPoints: [path.join(root, "src/handoff/edge.test.ts")],
  outfile: path.join(out, "edge.test.cjs"),
});
await esbuild.build({
  entryPoints: [path.join(root, "src/renderer/app.tsx")],
  outfile: path.join(out, "renderer.js"),
  bundle: true,
  format: "iife",
  platform: "browser",
  target: "chrome132",
  jsx: "automatic",
  define: { "process.env.NODE_ENV": '"production"' },
});

console.log("built", out);
