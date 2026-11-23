// difftest-gen.mjs — draw fresh cases with the kit's own generator (fresh seeds),
// compare my run() against the reference build() on each.
// Usage: node difftest-gen.mjs [nSeeds] [seedBase]
import { isDeepStrictEqual } from 'node:util';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const kit = join(here, '..', 'site-generator-speed-01-run-20261122-kit');

const { streamFor } = await import(join(kit, 'program/generator/rng.ts'));
const { generate } = await import(join(kit, 'program/generator/subject/sites.mjs'));
const ref = await import(join(kit, 'baselines/makefun-site/vendor/makefun-site/index.js'));
const mine = await import(join(here, 'solve.js'));

const nSeeds = Number(process.argv[2] ?? 5);
const seedBase = Number(process.argv[3] ?? 1000);
let total = 0;
let bad = 0;
for (let s = 0; s < nSeeds; s++) {
  const seed = createHash('sha256').update(`difftest-${seedBase + s}`).digest('hex');
  for (let i = 0; i < 3; i++) {
    const input = generate(streamFor(seed, 'case', i), i);
    total++;
    const a = ref.build(structuredClone(input));
    const b = mine.run(structuredClone(input));
    if (!isDeepStrictEqual(a, b)) {
      bad++;
      console.log(`DIFF seed=${seedBase + s} case=${i}`);
      const keys = new Set([...Object.keys(a), ...Object.keys(b)]);
      for (const k of keys) {
        if (!isDeepStrictEqual(a[k], b[k])) {
          console.log('  first diff path:', k);
          const av = String(a[k] ?? '');
          const bv = String(b[k] ?? '');
          let d = 0;
          while (d < Math.min(av.length, bv.length) && av[d] === bv[d]) d++;
          console.log('  ref @', d, JSON.stringify(av.slice(Math.max(0, d - 80), d + 120)));
          console.log('  my  @', d, JSON.stringify(bv.slice(Math.max(0, d - 80), d + 120)));
          break;
        }
      }
    }
  }
  console.log(`seed ${seedBase + s} done (${total} cases, ${bad} bad)`);
}
console.log(bad === 0 ? `ALL OK (${total} generated cases)` : `${bad}/${total} CASES DIFFER`);
process.exit(bad === 0 ? 0 : 1);
