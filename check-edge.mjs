// difftest-edge.mjs — hand-made nasty inputs: templates, markdown, front matter.
import { isDeepStrictEqual } from 'node:util';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const kit = join(here, '..', 'site-generator-speed-01-run-20261122-kit');
const ref = await import(join(kit, 'baselines/makefun-site/vendor/makefun-site/index.js'));
const mine = await import(join(here, 'solve.js'));

const baseSite = { title: 'T', base_url: 'https://x.example/blog/', per_page: 2, recent: 2 };
let n = 0;
let bad = 0;
function check(name, input) {
  n++;
  let a, b, ae, be;
  try { a = ref.build(structuredClone(input)); } catch (e) { ae = String(e); }
  try { b = mine.run(structuredClone(input)); } catch (e) { be = String(e); }
  const ok = ae === undefined && be === undefined ? isDeepStrictEqual(a, b) : ae === be;
  if (!ok) {
    bad++;
    console.log('DIFF:', name, 'refErr:', ae, 'myErr:', be);
    if (a && b) {
      for (const k of new Set([...Object.keys(a), ...Object.keys(b)])) {
        if (!isDeepStrictEqual(a[k], b[k])) {
          console.log('  path:', k);
          console.log('  ref:', JSON.stringify(String(a[k]).slice(0, 400)));
          console.log('  my :', JSON.stringify(String(b[k]).slice(0, 400)));
          break;
        }
      }
    }
  }
}
function site(files, templates, siteOverride = {}) {
  return { site: { ...baseSite, ...siteOverride }, templates, files };
}
const L = (name, body, extra = '') => `---\ntitle: ${name}\n${extra}---\n${body}`;

// --- templates: nesting, shadowing, includes, depth, weird paths
check('tpl-nested-loops', site(
  [{ path: 'posts/a.md', content: L('A', '# Hi\n\nText.\n', 'date: 2026-01-02\ntags: [X, Y]\n') }],
  {
    post: '{% include "head" %}{{{ page.content }}}{% include "foot" %}',
    head: '<h1>{{ page.title }}</h1>{% for t in page.tags %}[{{ loop.index }}/{{ loop.length }}:{{ t.name }}|{{ t.url }}]{% endfor %}',
    foot: '{% for t in tags %}{% for p in recent %}{{ t.slug }}:{{ p.title }};{% endfor %}{% endfor %}',
    index: '{% for p in posts %}{% include "card" %}{% endfor %}',
    card: '<b>{{ p.title }}</b>{% if p.tags %}T{% else %}F{% endif %}',
    tag: 'tag {{ tag.name }} {{ tag.count }}',
    tags: 'tags: {% for t in tags %}{{ t.name }},{% endfor %}',
  },
));
check('tpl-include-depth', site(
  [{ path: 'a.md', content: L('A', 'body\n') }],
  {
    page: 'P{% include "i1" %}',
    i1: '1{% include "i2" %}', i2: '2{% include "i3" %}', i3: '3{% include "i4" %}',
    i4: '4{% include "i5" %}', i5: '5{% include "i6" %}', i6: '6{% include "i7" %}',
    i7: '7{% include "i8" %}', i8: '8{% include "i9" %}', i9: '9{% include "i1" %}',
    index: 'I', tag: 'T', tags: 'G',
  },
));
check('tpl-include-missing-and-self', site(
  [{ path: 'a.md', content: L('A', 'x\n') }],
  { page: 'A{% include "nope" %}B{% include "page" %}C', index: 'I', tag: 'T', tags: 'G' },
));
check('tpl-loop-var-named-loop', site(
  [{ path: 'posts/a.md', content: L('A', 'x\n', 'date: 2026-01-02\n') }],
  {
    post: '{% for loop in recent %}{{ loop.title }}|{{ loop.index }};{% endfor %}',
    page: 'P', index: '{% for loop in posts %}{{ loop.title }}({{ loop.index }}){% endfor %}', tag: 'T', tags: 'G',
  },
));
check('tpl-unclosed-and-bad', site(
  [{ path: 'a.md', content: L('A', 'x\n') }],
  {
    page: 'ok {{ page.title }}',
    bad1: 'a {{ unclosed',
    bad2: 'a {{{ unclosed',
    bad3: 'a {% unclosed',
    bad4: '{% for x in %}{% endfor %}',
    bad5: '{% if %}{% endif %}',
    bad6: '{% else %}',
    bad7: '{% endfor %}',
    bad8: '{% for x in y %}{% endif %}',
    bad9: '{% for x in y %}',
    bad10: '{{ 1x }}',
    bad11: '{% frobnicate %}',
    bad12: '{% include "UPPER" %}',
    bad13: '{{ a..b }}',
    index: '{% include "bad1" %}{% include "bad2" %}{% include "bad3" %}{% include "bad4" %}{% include "bad5" %}{% include "bad6" %}{% include "bad7" %}{% include "bad8" %}{% include "bad9" %}{% include "bad10" %}{% include "bad11" %}{% include "bad12" %}{% include "bad13" %}',
    tag: 'T', tags: 'G',
  },
));
check('tpl-standalone-lines', site(
  [{ path: 'a.md', content: L('A', 'x\n') }],
  {
    page: 'A\n  {% if page.title %}  \nB\n{% endif %}\nC\n{% for x in tags %} {{ x.name }} {% endfor %}\nD',
    index: 'I', tag: 'T', tags: 'G',
  },
));
check('tpl-values', site(
  [{ path: 'posts/a.md', content: L('A', 'x\n', 'date: 2026-01-02\ntags: [Café, x]\n') }],
  {
    post: '{{ page.reading_minutes }}|{{ page.is_post }}|{{ page.newer }}|{{ page.older }}|{{ page.toc }}|{{ page.tags }}|{{ page.slug }}|{{ site }}|{{ tags }}|{{ recent }}|{{ missing.deep.path }}|{{ page.title }}',
    page: 'P', index: '{{ posts }}|{{ pager.number }}/{{ pager.total }}|{{ pager }}', tag: '{{ tag.posts }}', tags: 'G',
  },
));

// --- markdown edge cases
const mdCases = {
  'md-nesting': '# T *a **b *c* d** e* f\n\n`code *not* em` and `` ` ``\n\n**unclosed\n\n*unclosed\n\n[a](b) [x](y z) [u]() []() [v](w "t")\n\n\\*escaped\\* \\[not a link\\](x) \\\\ backslash\\\n',
  'md-lists': '- one\n- two\n  continued\n  more\n- three\n\n1. first\n2. second\n   cont\n3. third\n\n5. five\n6. six\n\n- a\n* b\n1. c\n',
  'md-blocks': 'para\n---\npara2\n***\n\n> quote\n> \n> more\n\n>\n\n```\ncode <>&\n```\n\n```js\nconst x = 1;\n```\n\n    indented\n\ntext\n===\n\n#nope\n####### seven\n## \n## ok ##\n',
  'md-links': '[a](posts/a.md) [b](../posts/a.md) [c](../../etc.md) [d](./a.md) [e](#frag) [f](/abs) [g](https://e.com/x) [h](mailto:a@b.c) [i](assets/style.css) [j](missing.md) [k](posts/a.md#frag) [l](a.md)\n',
  'md-unicode': '# Café Über Δέλτα 東京\n\nTexte naïve résumé — “quotes” ‘single’ & <tags>.\n',
  'md-empty': '',
  'md-only-fm': '',
  'md-crlf': 'line1\r\nline2\r\n\r\n# head\r\n',
};
for (const [name, body] of Object.entries(mdCases)) {
  check(name, site(
    [
      { path: 'posts/a.md', content: L('A', body, 'date: 2026-01-02\n') },
      { path: 'a.md', content: L('A2', body) },
      { path: 'assets/style.css', content: 'x{}' },
    ],
    { post: '{{{ page.content }}}', page: '{{{ page.content }}}', index: 'I', tag: 'T', tags: 'G' },
  ));
}

// --- front matter edge cases
const fmCases = [
  ['fm-no-fm', 'hello\n'],
  ['fm-unclosed', '---\ntitle: X\n'],
  ['fm-just-dashes', '---'],
  ['fm-empty', '---\n---\nbody\n'],
  ['fm-no-title', '---\ndate: 2026-01-01\n---\nbody\n'],
  ['fm-dup', '---\ntitle: A\ntitle: B\n---\nbody\n'],
  ['fm-unknown', '---\ntitle: A\nauthor: X\n---\nbody\n'],
  ['fm-cap-key', '---\nTitle: A\n---\nbody\n'],
  ['fm-no-space', '---\ntitle:A\n---\nbody\n'],
  ['fm-two-spaces', '---\ntitle:  A\n---\nbody\n'],
  ['fm-trailing', '---\ntitle: A   \n---\nbody\n'],
  ['fm-quoted-esc', '---\ntitle: "a\\"b\\\\c"\n---\nbody\n'],
  ['fm-quoted-bad-esc', '---\ntitle: "a\\qb"\n---\nbody\n'],
  ['fm-quoted-stray', '---\ntitle: "a"b"\n---\nbody\n'],
  ['fm-quoted-unclosed', '---\ntitle: "abc\n---\nbody\n'],
  ['fm-bare-bracket', '---\ntitle: [x]\n---\nbody\n'],
  ['fm-date-bad', '---\ntitle: A\ndate: 2026-02-30\n---\nbody\n'],
  ['fm-date-leap', '---\ntitle: A\ndate: 2024-02-29\n---\nbody\n'],
  ['fm-date-1900', '---\ntitle: A\ndate: 1969-12-31\n---\nbody\n'],
  ['fm-date-2100', '---\ntitle: A\ndate: 2100-01-01\n---\nbody\n'],
  ['fm-tags-empty', '---\ntitle: A\ntags: []\n---\nbody\n'],
  ['fm-tags-spaces', '---\ntitle: A\ntags: [  ]\n---\nbody\n'],
  ['fm-tags-bad', '---\ntitle: A\ntags: [a, "b"]\n---\nbody\n'],
  ['fm-tags-empty-item', '---\ntitle: A\ntags: [a,, b]\n---\nbody\n'],
  ['fm-tags-no-brackets', '---\ntitle: A\ntags: a, b\n---\nbody\n'],
  ['fm-draft-yes', '---\ntitle: A\ndraft: yes\n---\nbody\n'],
  ['fm-draft-true', '---\ntitle: A\ndraft: true\n---\nbody\n'],
  ['fm-slug-bad', '---\ntitle: A\nslug: Not A Slug\n---\nbody\n'],
  ['fm-slug-long', '---\ntitle: A\nslug: ' + 'a'.repeat(65) + '\n---\nbody\n'],
  ['fm-slug-64', '---\ntitle: A\nslug: ' + 'a'.repeat(64) + '\n---\nbody\n'],
  ['fm-layout-bad', '---\ntitle: A\nlayout: No_Such\n---\nbody\n'],
  ['fm-key-no-value', '---\ntitle: A\nsummary:\n---\nbody\n'],
  ['fm-key-no-value2', '---\ntitle: A\nsummary: \n---\nbody\n'],
  ['fm-comment-blank', '---\n# comment\n\n  \n\t\ntitle: A\n---\nbody\n'],
  ['fm-summary-quoted', '---\ntitle: A\nsummary: "has \\"quote\\" and \\\\ slash"\n---\nbody\n'],
  ['fm-post-no-date', '---\ntitle: A\n---\nbody\n'],
  ['fm-reserved-tags', '---\ntitle: A\n---\nbody\n'],
  ['fm-underscore-key', '---\ntitle: A\n_my: 1\n---\nbody\n'],
  ['fm-line-no-colon', '---\ntitle: A\nhello world\n---\nbody\n'],
];
for (const [name, content] of fmCases) {
  const files = [{ path: 'posts/a.md', content }];
  if (name === 'fm-reserved-tags') files[0] = { path: 'tags/x.md', content };
  if (name === 'fm-post-no-date') files[0] = { path: 'posts/a.md', content };
  check(name, site(files, { post: 'P', page: 'G', index: 'I', tag: 'T', tags: 'G' }));
}

// --- misc: shadowed statics, base_url slashes, per_page edges, duplicate slugs
check('shadow', site(
  [
    { path: 'posts/a.md', content: L('A', 'x\n', 'date: 2026-01-02\n') },
    { path: 'index.html', content: 'static index' },
    { path: 'posts/a/index.html', content: 'static post' },
    { path: 'page/2/index.html', content: 'static page2' },
  ],
  { post: 'P', page: 'G', index: 'I', tag: 'T', tags: 'G' },
  { per_page: 1 },
));
check('slugs-collide', site(
  [
    { path: 'posts/a.md', content: L('Same Title', '1\n', 'date: 2026-01-02\n') },
    { path: 'posts/b.md', content: L('SAME TITLE!!', '2\n', 'date: 2026-01-01\n') },
    { path: 'posts/c.md', content: L('same-title', '3\n', 'date: 2026-01-03\n') },
    { path: 'x.md', content: L('Same Title', '4\n') },
    { path: 'y.md', content: L('posts', '5\n') },
    { path: 'z.md', content: L('!!!', '6\n') },
    { path: 'w.md', content: L('Café', '7\n') },
  ],
  { post: '{{ page.slug }}|{{ page.url }}', page: '{{ page.slug }}|{{ page.url }}', index: 'I', tag: 'T', tags: 'G' },
));
check('base-slashes', site(
  [{ path: 'a.md', content: L('A', '[x](a.md)\n') }],
  { post: 'P', page: '{{ site.base_url }}|{{{ page.content }}}', index: 'I', tag: 'T', tags: 'G' },
  { base_url: 'https://x.example///' },
));
check('no-posts', site(
  [{ path: 'a.md', content: L('A', 'x\n') }],
  { post: 'P', page: 'G', index: 'IDX{% for p in posts %}X{% endfor %}{% if not posts %}EMPTY{% endif %}', tag: 'T', tags: 'G' },
));
check('missing-layouts', site(
  [{ path: 'a.md', content: L('A', 'x\n') }],
  { page: 'G' },
));

console.log(`${n - bad}/${n} edge cases OK`);
process.exit(bad === 0 ? 0 : 1);
