// practice 3/3: driver output vs expected.json cases[i].output (deep-equal)
import { readFileSync } from 'node:fs';
import { isDeepStrictEqual } from 'node:util';
const a = JSON.parse(readFileSync(process.argv[2], 'utf8'));
const b = JSON.parse(readFileSync(process.argv[3], 'utf8')).cases;
let ok = 0;
for (let i = 0; i < b.length; i++) {
  const eq = isDeepStrictEqual(a[i], b[i].output);
  console.log('case', b[i].id, eq ? 'OK' : 'DIFF');
  if (eq) ok++;
  else for (const k of new Set([...Object.keys(a[i]), ...Object.keys(b[i].output)])) {
    if (!isDeepStrictEqual(a[i][k], b[i].output[k])) { console.log('  path:', k); console.log('  mine:', JSON.stringify(String(a[i][k]).slice(0,300))); console.log('  ref :', JSON.stringify(String(b[i].output[k]).slice(0,300))); break; }
  }
}
console.log(ok + '/' + b.length + ' practice cases OK');
