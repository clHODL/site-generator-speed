// solve.js — the native entry point.
import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);
let native = null;
try { native = require('./native.node'); } catch { native = null; }
let jsRun = null;
if (!native) {
  const js = await import('./solve-js.mjs');
  jsRun = js.run;
}
export function run(input) {
  if (native) return native.runOne(input);
  return jsRun(input);
}
export const runAll = native ? native.runAll : null;
