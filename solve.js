// solve.js — a fast drop-in for makefun-site 1.0.0.
//
// Written from scratch against makefun-site's README and reference behaviour:
// the same front matter, template dialect, Markdown subset, slug rule and
// report, byte for byte. Speed comes from avoiding the reference's per-char
// loops and per-line regexes: plain-text runs are sliced whole, slugs and
// escapes take ASCII fast paths, the Markdown lexer classifies lines by their
// first character, and templates are parsed once and rendered into arrays.
//
// run(input) returns the output tree { path: text } for one site's input
// { site, templates, files }, exactly as makefun-site 1.0.0's build does.

const REPORT_FILE = 'build-report.txt';
const RESERVED = ['posts', 'tags', 'page'];

const byCodeUnits = (a, b) => (a < b ? -1 : a > b ? 1 : 0);

// ---------------------------------------------------------------- text

const ESCAPES = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };
const ESC_TEST = /[&<>"']/;
const ESC_G = /[&<>"']/g;

/** `text` with & < > " and ' escaped. */
function escapeHtml(text) {
  return ESC_TEST.test(text) ? text.replace(ESC_G, (c) => ESCAPES[c]) : text;
}

const NON_ASCII = /[^\x00-\x7F]/;
const MARK_RE = /\p{M}/gu;
const SLUG_MAX = 64;

/** The slug of ASCII `text` (NFKD is identity on ASCII, no combining marks). */
function slugAscii(text) {
  let out = '';
  let gap = false;
  for (let i = 0; i < text.length; i++) {
    const c = text.charCodeAt(i);
    let keep = -1;
    if (c >= 65 && c <= 90) keep = c + 32;
    else if ((c >= 97 && c <= 122) || (c >= 48 && c <= 57)) keep = c;
    if (keep === -1) {
      gap = out.length > 0;
      continue;
    }
    if (gap) out += '-';
    gap = false;
    out += String.fromCharCode(keep);
  }
  out = out.slice(0, SLUG_MAX);
  let e = out.length;
  while (e > 0 && out.charCodeAt(e - 1) === 45) e--;
  out = out.slice(0, e);
  return out === '' ? 'untitled' : out;
}

/** The slug of `text`: NFKD, combining marks dropped, A-Z lowered, every run of
 *  characters other than a-z and 0-9 one "-", none at either end, at most 64
 *  characters (a "-" left at the cut dropped), "untitled" when nothing is left.
 *  Pure in its input, so repeated texts share one cached answer. */
const slugCache = new Map();
function slugify(text) {
  const hit = slugCache.get(text);
  if (hit !== undefined) return hit;
  const out = slugifyUncached(text);
  if (slugCache.size > 20000) slugCache.clear();
  slugCache.set(text, out);
  return out;
}

function slugifyUncached(text) {
  if (!NON_ASCII.test(text)) return slugAscii(text);
  const plain = text.normalize('NFKD').replace(MARK_RE, '');
  let out = '';
  let gap = false;
  for (const ch of plain) {
    let keep = null;
    if (ch >= 'A' && ch <= 'Z') keep = ch.toLowerCase();
    else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) keep = ch;
    if (keep === null) {
      gap = out.length > 0;
      continue;
    }
    if (gap) out += '-';
    gap = false;
    out += keep;
  }
  out = out.slice(0, SLUG_MAX);
  let e = out.length;
  while (e > 0 && out.charCodeAt(e - 1) === 45) e--;
  out = out.slice(0, e);
  return out === '' ? 'untitled' : out;
}

/** `slug` made unique against `taken` (and added to it). */
function uniqueSlug(slug, taken) {
  let out = slug;
  for (let n = 2; taken.has(out); n++) out = `${slug}-${n}`;
  taken.add(out);
  return out;
}

const MONTHS = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'];

/** A YYYY-MM-DD date in words, from the string alone. */
function longDate(date) {
  const y = date.slice(0, 4);
  const m = date.charCodeAt(5) * 10 + date.charCodeAt(6) - 528; // (d1-48)*10 + (d2-48)
  const d = date.charCodeAt(8) * 10 + date.charCodeAt(9) - 528;
  return `${d} ${MONTHS[m - 1]} ${y}`;
}

// ---------------------------------------------------------------- front matter

const KEYS = ['title', 'date', 'tags', 'draft', 'layout', 'slug', 'summary'];
const LINE_RE = /^([a-z_]+):(?: (.*))?$/;
const DATE_RE = /^(\d{4})-(\d{2})-(\d{2})$/;
const NAME_RE = /^[a-z0-9-]+$/;
const SLUG_RE = /^[a-z0-9]+(?:-[a-z0-9]+)*$/;
const TAG_RE = /^[^"[\]]+$/;
const BLANK_RE = /^[ \t]*$/;

function daysIn(y, m) {
  if (m === 2) return (y % 4 === 0 && y % 100 !== 0) || y % 400 === 0 ? 29 : 28;
  return m === 4 || m === 6 || m === 9 || m === 11 ? 30 : 31;
}

/** A string value: "..." with \" and \\ its only escapes, or bare. */
function stringOf(raw) {
  if (raw.charCodeAt(0) === 34) {
    if (raw.length < 2 || raw.charCodeAt(raw.length - 1) !== 34) return { error: 'a quoted string is not closed' };
    let out = '';
    for (let i = 1; i < raw.length - 1; i++) {
      const c = raw[i];
      if (c === '\\') {
        const n = raw[i + 1];
        if ((n !== '"' && n !== '\\') || i + 1 === raw.length - 1) return { error: 'a quoted string holds an escape other than \\" or \\\\' };
        out += n;
        i++;
      } else if (c === '"') return { error: 'a quoted string holds a stray quote' };
      else out += c;
    }
    return { value: out };
  }
  if (raw.charCodeAt(0) === 91) return { error: 'a bare string opens on [' };
  return { value: raw };
}

/** One key's value, or why it is refused. */
function valueOf(key, raw) {
  switch (key) {
    case 'title':
    case 'summary': {
      const s = stringOf(raw);
      return 'error' in s ? { error: `key "${key}": ${s.error}` } : s;
    }
    case 'date': {
      const m = DATE_RE.exec(raw);
      const y = m ? Number(m[1]) : 0;
      const mo = m ? Number(m[2]) : 0;
      const d = m ? Number(m[3]) : 0;
      if (!m || y < 1970 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > daysIn(y, mo)) return { error: 'key "date": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)' };
      return { value: raw };
    }
    case 'tags': {
      if (!raw.startsWith('[') || !raw.endsWith(']')) return { error: 'key "tags": not a list [a, b]' };
      const inner = raw.slice(1, -1);
      if (inner.trim() === '') return { value: [] };
      const tags = inner.split(',').map((t) => t.trim());
      if (tags.some((t) => t === '' || !TAG_RE.test(t))) return { error: 'key "tags": an empty tag, or one holding " [ or ]' };
      return { value: tags };
    }
    case 'draft':
      return raw === 'true' ? { value: true } : raw === 'false' ? { value: false } : { error: 'key "draft": true or false' };
    case 'layout':
      return NAME_RE.test(raw) ? { value: raw } : { error: 'key "layout": not a template name' };
    case 'slug':
      return SLUG_RE.test(raw) && raw.length <= 64 ? { value: raw } : { error: 'key "slug": not a slug' };
    default:
      return { error: `unknown key "${key}"` };
  }
}

/**
 * A source's front matter and body: { data, body }, or { error } with the
 * first reason it is refused. `text` has "\n" line ends.
 */
function parseFrontMatter(text) {
  if (text.length < 3 || text.charCodeAt(0) !== 45 || text.charCodeAt(1) !== 45 || text.charCodeAt(2) !== 45) return { error: 'no front matter' };
  if (text.length > 3 && text.charCodeAt(3) !== 10) return { error: 'no front matter' };
  if (text.length === 3) return { error: 'the front matter is not closed' };
  // Find the closing line: a line exactly '---'.
  let closeStart = -1;
  let p = 4;
  while (p < text.length) {
    const nl = text.indexOf('\n', p);
    const lineEnd = nl === -1 ? text.length : nl;
    if (lineEnd - p === 3 && text.charCodeAt(p) === 45 && text.charCodeAt(p + 1) === 45 && text.charCodeAt(p + 2) === 45) {
      closeStart = p;
      break;
    }
    if (nl === -1) break;
    p = nl + 1;
  }
  if (closeStart === -1) return { error: 'the front matter is not closed' };
  const fmText = text.slice(4, closeStart);
  const fmLines = fmText === '' ? [] : fmText.split('\n');
  // The split leaves a trailing '' for the '\n' before the closing line; it is
  // a blank line and skipped, exactly as the reference's extra line would be.
  const data = {};
  for (let k = 0; k < fmLines.length; k++) {
    const line = fmLines[k];
    if (BLANK_RE.test(line) || line.charCodeAt(0) === 35) continue;
    const m = LINE_RE.exec(line);
    if (!m) return { error: `line ${k + 2} is not "key: value"` };
    const key = m[1];
    const raw = (m[2] ?? '').replace(/[ \t]+$/, '');
    if (!KEYS.includes(key)) return { error: `unknown key "${key}"` };
    if (Object.hasOwn(data, key)) return { error: `key "${key}" is given twice` };
    if (raw === '') return { error: `key "${key}" has no value` };
    const v = valueOf(key, raw);
    if ('error' in v) return v;
    data[key] = v.value;
  }
  if (!Object.hasOwn(data, 'title')) return { error: 'no title' };
  const bodyStart = closeStart + 3;
  const body = bodyStart < text.length && text.charCodeAt(bodyStart) === 10 ? text.slice(bodyStart + 1) : text.slice(bodyStart);
  return { data, body };
}

// ---------------------------------------------------------------- markdown

const ESCAPABLE = new Set(['\\', '`', '*', '_', '[', ']', '(', ')', '#', '+', '-', '.', '!', '>']);
const WS_RE = /\s/;
const SCHEME_RE = /^[A-Za-z][A-Za-z0-9+.-]*:/;
const CONTINUED_RE = /^ {2,}\S/;

/** Whether `line` continues a list item (two or more spaces, then a non-space). */
function isContinued(line) {
  return CONTINUED_RE.test(line);
}

/** Whether `line` is blank (spaces and tabs only). */
function isBlank(line) {
  for (let i = 0; i < line.length; i++) {
    const c = line.charCodeAt(i);
    if (c !== 32 && c !== 9) return false;
  }
  return true;
}

/** `line` with spaces and tabs dropped at both ends. */
function trimST(line) {
  let a = 0;
  let b = line.length;
  while (a < b && (line.charCodeAt(a) === 32 || line.charCodeAt(a) === 9)) a++;
  while (b > a && (line.charCodeAt(b - 1) === 32 || line.charCodeAt(b - 1) === 9)) b--;
  return a === 0 && b === line.length ? line : line.slice(a, b);
}

/** A fence-open line's language, or null (line of ``` + optional lang + spaces/tabs). */
function fenceLang(line) {
  if (line.charCodeAt(0) !== 96 || line.charCodeAt(1) !== 96 || line.charCodeAt(2) !== 96) return null;
  let i = 3;
  while (i < line.length) {
    const c = line.charCodeAt(i);
    if ((c >= 65 && c <= 90) || (c >= 97 && c <= 122) || (c >= 48 && c <= 57) || c === 95 || c === 43 || c === 45) i++;
    else break;
  }
  const lang = line.slice(3, i);
  while (i < line.length && (line.charCodeAt(i) === 32 || line.charCodeAt(i) === 9)) i++;
  return i === line.length ? lang : null;
}

/** Whether `line` is a fence-close line (``` + spaces/tabs only). */
function isFenceClose(line) {
  if (line.charCodeAt(0) !== 96 || line.charCodeAt(1) !== 96 || line.charCodeAt(2) !== 96) return false;
  for (let i = 3; i < line.length; i++) {
    const c = line.charCodeAt(i);
    if (c !== 32 && c !== 9) return false;
  }
  return true;
}

/** A heading line's depth and text, or null. */
function headingOf(line) {
  let i = 0;
  while (i < 6 && line.charCodeAt(i) === 35) i++;
  if (i === 0 || i === 6 + 1) return null;
  if (i >= 6 && line.charCodeAt(6) === 35) return null; // 7 or more '#'
  if (i > 6) return null;
  const c = line.charCodeAt(i);
  if (c !== 32 && c !== 9) return null;
  let j = i;
  while (j < line.length && (line.charCodeAt(j) === 32 || line.charCodeAt(j) === 9)) j++;
  let text = line.slice(j);
  let e = text.length;
  while (e > 0 && (text.charCodeAt(e - 1) === 32 || text.charCodeAt(e - 1) === 9)) e--;
  text = e === text.length ? text : text.slice(0, e);
  return text === '' ? null : { depth: i, text };
}

/** Whether `line` is a thematic break (--- or *** + spaces/tabs only). */
function isHr(line) {
  const c0 = line.charCodeAt(0);
  if (c0 !== 45 && c0 !== 42) return false;
  if (line.charCodeAt(1) !== c0 || line.charCodeAt(2) !== c0) return false;
  for (let i = 3; i < line.length; i++) {
    const c = line.charCodeAt(i);
    if (c !== 32 && c !== 9) return false;
  }
  return true;
}

// Block kinds, by a line's first character where possible.
const K_BLANK = 0, K_FENCE = 1, K_HEADING = 2, K_HR = 3, K_QUOTE = 4, K_LIST = 5, K_PARA = 6;

/** A line's block kind. */
function lineKind(line) {
  if (line.length === 0) return K_BLANK;
  const c = line.charCodeAt(0);
  if (c === 32 || c === 9) return isBlank(line) ? K_BLANK : K_PARA;
  if (c === 96) return fenceLang(line) !== null ? K_FENCE : K_PARA;
  if (c === 35) return headingOf(line) !== null ? K_HEADING : K_PARA;
  if (c === 45) {
    if (isHr(line)) return K_HR;
    if (line.charCodeAt(1) === 32) return K_LIST;
    return K_PARA;
  }
  if (c === 42) {
    if (isHr(line)) return K_HR;
    if (line.charCodeAt(1) === 32) return K_LIST;
    return K_PARA;
  }
  if (c === 62) return K_QUOTE;
  if (c >= 48 && c <= 57) {
    // ordered list: 1-9 digits, '. ', then anything
    let i = 0;
    while (i < 9 && line.charCodeAt(i) >= 48 && line.charCodeAt(i) <= 57) i++;
    if (i > 0 && line.charCodeAt(i) === 46 && line.charCodeAt(i + 1) === 32) return K_LIST;
    return K_PARA;
  }
  return K_PARA;
}


/** The first index from `from` where `s` holds `what` outside a backslash escape, or -1. */
function findUnescaped(s, what, from) {
  const wl = what.length;
  for (let j = from; j <= s.length - wl; j++) {
    if (s.charCodeAt(j) === 92 && j + 1 < s.length && ESCAPABLE.has(s[j + 1])) {
      j++;
      continue;
    }
    if (s.startsWith(what, j)) return j;
  }
  return -1;
}

/** The first single * from `from` (one whose run is not **), outside an escape, or -1. */
function findSingleStar(s, from) {
  for (let j = from; j < s.length; j++) {
    const c = s.charCodeAt(j);
    if (c === 92 && j + 1 < s.length && ESCAPABLE.has(s[j + 1])) {
      j++;
      continue;
    }
    if (c !== 42) continue;
    if (s.charCodeAt(j + 1) === 42) {
      j++;
      continue;
    }
    return j;
  }
  return -1;
}

/** `s` rendered inline; `env.link(target)` is a link's href. */
function inline(s, env) {
  const re = /[\\`*\[&<>"']/g;
  let out = '';
  let i = 0;
  const n = s.length;
  let m;
  while (i < n) {
    re.lastIndex = i;
    m = re.exec(s);
    if (m === null) {
      out += s.slice(i);
      break;
    }
    const k = m.index;
    if (k > i) out += s.slice(i, k);
    const c = s.charCodeAt(k);
    if (c === 92) {
      if (k + 1 < n && ESCAPABLE.has(s[k + 1])) {
        out += ESCAPES[s[k + 1]] ?? s[k + 1];
        i = k + 2;
      } else {
        out += '\\';
        i = k + 1;
      }
      continue;
    }
    if (c === 96) {
      const j = s.indexOf('`', k + 1);
      if (j > k + 1) {
        out += '<code>' + escapeHtml(s.slice(k + 1, j)) + '</code>';
        i = j + 1;
      } else {
        out += '`';
        i = k + 1;
      }
      continue;
    }
    if (c === 42) {
      if (s.charCodeAt(k + 1) === 42) {
        const j = findUnescaped(s, '**', k + 2);
        if (j > k + 2) {
          out += '<strong>' + inline(s.slice(k + 2, j), env) + '</strong>';
          i = j + 2;
        } else {
          out += '**';
          i = k + 2;
        }
      } else {
        const j = findSingleStar(s, k + 1);
        if (j > k + 1) {
          out += '<em>' + inline(s.slice(k + 1, j), env) + '</em>';
          i = j + 1;
        } else {
          out += '*';
          i = k + 1;
        }
      }
      continue;
    }
    if (c === 91) {
      const k2 = findUnescaped(s, ']', k + 1);
      const mm = k2 !== -1 && s.charCodeAt(k2 + 1) === 40 ? s.indexOf(')', k2 + 2) : -1;
      const target = mm === -1 ? '' : s.slice(k2 + 2, mm);
      if (target !== '' && !WS_RE.test(target)) {
        out += '<a href="' + escapeHtml(env.link(target)) + '">' + inline(s.slice(k + 1, k2), env) + '</a>';
        i = mm + 1;
      } else {
        out += '[';
        i = k + 1;
      }
      continue;
    }
    out += ESCAPES[s[k]];
    i = k + 1;
  }
  return out;
}

/**
 * `src` rendered: its HTML (each block on its own lines, a "\n" after the
 * last), its table of contents (every level 2 and 3 heading) and its excerpt
 * (the first paragraph's inline HTML, or "").
 */
function renderMarkdown(src, env) {
  const lines = src.split('\n');
  const n = lines.length;
  const blocks = [];
  const toc = [];
  let excerpt = null;
  const ids = new Set();
  let i = 0;
  // Each line's kind, classified once: the main loop, the paragraph-end
  // check and the list loop used to re-classify the same line up to 3x.
  const kinds = new Array(n);
  const kindAt = (j) => (kinds[j] === undefined ? (kinds[j] = lineKind(lines[j])) : kinds[j]);
  while (i < n) {
    const line = lines[i];
    const kind = kindAt(i);
    if (kind === K_BLANK) {
      i++;
      continue;
    }
    if (kind === K_FENCE) {
      const lang = fenceLang(line);
      const body = [];
      i++;
      while (i < n && !isFenceClose(lines[i])) body.push(lines[i++]);
      i++;
      const cls = lang === '' ? '' : ` class="language-${escapeHtml(lang)}"`;
      const code = body.length === 0 ? '' : body.map(escapeHtml).join('\n') + '\n';
      blocks.push(`<pre><code${cls}>${code}</code></pre>`);
      continue;
    }
    if (kind === K_HEADING) {
      const h = headingOf(line);
      const id = uniqueSlug(slugify(h.text), ids);
      const html = inline(h.text, env);
      blocks.push(`<h${h.depth} id="${id}">${html}</h${h.depth}>`);
      if (h.depth === 2 || h.depth === 3) toc.push({ level: h.depth, id, html });
      i++;
      continue;
    }
    if (kind === K_HR) {
      blocks.push('<hr>');
      i++;
      continue;
    }
    if (kind === K_QUOTE) {
      const body = [];
      while (i < n && lines[i].charCodeAt(0) === 62) {
        const l = lines[i++];
        body.push(trimST(l.charCodeAt(1) === 32 ? l.slice(2) : l.slice(1)));
      }
      const kept = [];
      for (let q = 0; q < body.length; q++) if (body[q] !== '') kept.push(body[q]);
      blocks.push(kept.length === 0 ? '<blockquote>\n</blockquote>' : `<blockquote>\n<p>${inline(kept.join('\n'), env)}</p>\n</blockquote>`);
      continue;
    }
    if (kind === K_LIST) {
      const ordered = line.charCodeAt(0) >= 48 && line.charCodeAt(0) <= 57;
      let start = 1;
      if (ordered) {
        let d = 0;
        while (line.charCodeAt(d) >= 48 && line.charCodeAt(d) <= 57) d++;
        start = Number(line.slice(0, d));
      }
      const items = [];
      while (i < n) {
        const l = lines[i];
        if (kindAt(i) !== K_LIST) {
          // A continuation line: two or more spaces then a non-space.
          if (isContinued(l)) {
            items[items.length - 1].push(trimST(l));
            i++;
            continue;
          }
          break;
        }
        // Same kind? A bullet line in an ordered list (or the reverse) ends it.
        const lOrdered = l.charCodeAt(0) >= 48 && l.charCodeAt(0) <= 57;
        if (lOrdered !== ordered) break;
        if (ordered) {
          let d = 0;
          while (l.charCodeAt(d) >= 48 && l.charCodeAt(d) <= 57) d++;
          items.push([trimST(l.slice(d + 2))]);
        } else {
          items.push([trimST(l.slice(2))]);
        }
        i++;
      }
      const tag = ordered ? 'ol' : 'ul';
      const open = ordered && start !== 1 ? `<ol start="${start}">` : `<${tag}>`;
      let html = open + '\n';
      for (let q = 0; q < items.length; q++) html += `<li>${inline(items[q].join('\n'), env)}</li>\n`;
      html += `</${tag}>`;
      blocks.push(html);
      continue;
    }
    // Paragraph.
    const body = [trimST(line)];
    i++;
    while (i < n) {
      const k = kindAt(i);
      if (k === K_BLANK || k === K_FENCE || k === K_HEADING || k === K_HR) break;
      body.push(trimST(lines[i++]));
    }
    const text = body.join('\n');
    const html = inline(text, env);
    if (excerpt === null) excerpt = html;
    blocks.push(`<p>${html}</p>`);
  }
  return { html: blocks.length === 0 ? '' : blocks.join('\n') + '\n', toc, excerpt: excerpt ?? '' };
}

// ---------------------------------------------------------------- templates

const PATH_RE = /^[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*$/;
const FOR_RE = /^for ([A-Za-z_][A-Za-z0-9_]*) in (\S+)$/;
const IF_RE = /^if (not )?(\S+)$/;
const INCLUDE_RE = /^include "([a-z0-9-]+)"$/;
const INCLUDE_DEPTH = 8;

/** A template that does not parse: its name and why. */
class TemplateError extends Error {}

/** The path `text` names, split at its dots, or a TemplateError. */
function pathOf(name, text) {
  if (!PATH_RE.test(text)) throw new TemplateError(`template "${name}": ${JSON.stringify(text)} is not a path`);
  return text.split('.');
}

/** `src`'s tokens: text, {{ }}, {{{ }}} and {% %}, a standalone tag's line taken with it. */
function tokenize(name, src) {
  const tokens = [];
  let text = '';
  let i = 0;
  while (i < src.length) {
    const open = src.indexOf('{', i);
    if (open === -1 || open === src.length - 1) {
      text += src.slice(i);
      break;
    }
    const next = src.charCodeAt(open + 1);
    if (next !== 123 && next !== 37) {
      text += src.slice(i, open + 1);
      i = open + 1;
      continue;
    }
    text += src.slice(i, open);
    if (next === 123) {
      const raw = src.charCodeAt(open + 2) === 123;
      const close = src.indexOf(raw ? '}}}' : '}}', open + (raw ? 3 : 2));
      if (close === -1) throw new TemplateError(`template "${name}": a ${raw ? '{{{' : '{{'} is not closed`);
      if (text !== '') tokens.push({ t: 'text', text });
      text = '';
      tokens.push({ t: raw ? 'raw' : 'var', path: pathOf(name, src.slice(open + (raw ? 3 : 2), close).trim()) });
      i = close + (raw ? 3 : 2);
      continue;
    }
    const close = src.indexOf('%}', open + 2);
    if (close === -1) throw new TemplateError(`template "${name}": a {% is not closed`);
    const end = close + 2;
    const lineStart = src.lastIndexOf('\n', open - 1) + 1;
    const nl = src.indexOf('\n', end);
    const lineEnd = nl === -1 ? src.length : nl;
    let p = lineStart;
    let beforeOk = true;
    while (p < open) {
      const c = src.charCodeAt(p);
      if (c !== 32 && c !== 9) { beforeOk = false; break; }
      p++;
    }
    let afterOk = true;
    p = end;
    while (p < lineEnd) {
      const c = src.charCodeAt(p);
      if (c !== 32 && c !== 9) { afterOk = false; break; }
      p++;
    }
    const standalone = beforeOk && afterOk;
    if (standalone) text = text.slice(0, text.length - (open - lineStart));
    if (text !== '') tokens.push({ t: 'text', text });
    text = '';
    tokens.push({ t: 'tag', tag: src.slice(open + 2, close).trim() });
    i = standalone ? (nl === -1 ? src.length : nl + 1) : end;
  }
  if (text !== '') tokens.push({ t: 'text', text });
  return tokens;
}

/** `src` parsed into its tree of nodes (a TemplateError when it does not parse). */
function parseTemplate(name, src) {
  const root = { kind: 'root', body: [] };
  const stack = [root];
  let body = root.body;
  for (const tok of tokenize(name, src)) {
    if (tok.t === 'text') body.push({ kind: 'text', text: tok.text });
    else if (tok.t === 'var' || tok.t === 'raw') body.push({ kind: tok.t, path: tok.path });
    else {
      const tag = tok.tag;
      const top = stack[stack.length - 1];
      let m;
      if ((m = FOR_RE.exec(tag)) !== null) {
        const node = { kind: 'for', name: m[1], path: pathOf(name, m[2]), body: [] };
        body.push(node);
        stack.push(node);
        body = node.body;
      } else if ((m = IF_RE.exec(tag)) !== null) {
        const node = { kind: 'if', not: m[1] !== undefined, path: pathOf(name, m[2]), then: [], else: null };
        body.push(node);
        stack.push(node);
        body = node.then;
      } else if (tag === 'else') {
        if (top.kind !== 'if' || top.else !== null) throw new TemplateError(`template "${name}": an else outside an if`);
        top.else = [];
        body = top.else;
      } else if (tag === 'endfor' || tag === 'endif') {
        if (top.kind !== (tag === 'endfor' ? 'for' : 'if')) throw new TemplateError(`template "${name}": an ${tag} with no open ${tag.slice(3)}`);
        stack.pop();
        const parent = stack[stack.length - 1];
        body = parent.kind === 'root' ? parent.body : parent.kind === 'for' ? parent.body : parent.else ?? parent.then;
      } else if ((m = INCLUDE_RE.exec(tag)) !== null) body.push({ kind: 'include', name: m[1] });
      else throw new TemplateError(`template "${name}": {% ${tag} %} is not a tag`);
    }
  }
  if (stack.length > 1) throw new TemplateError(`template "${name}": a ${stack[stack.length - 1].kind} is not closed`);
  return root.body;
}

/** The value at `path` in the scope chain: its first name from the nearest scope holding it, then own properties. */
function lookup(scope, path) {
  let s = scope;
  while (s !== null && !Object.hasOwn(s.vars, path[0])) s = s.parent;
  if (s === null) return undefined;
  let v = s.vars[path[0]];
  for (let k = 1; k < path.length; k++) {
    if (v === null || typeof v !== 'object' || !Object.hasOwn(v, path[k])) return undefined;
    v = v[path[k]];
  }
  return v;
}

/** A value as text: a string itself, a number in JavaScript's digits, true or false, an array's items joined by ", ", else "". */
function textOf(v) {
  if (typeof v === 'string') return v;
  if (typeof v === 'number') return String(v);
  if (typeof v === 'boolean') return v ? 'true' : 'false';
  if (Array.isArray(v)) return v.map(textOf).join(', ');
  return '';
}

/** Whether a value counts as true in an if. */
function truthy(v) {
  if (Array.isArray(v)) return v.length > 0;
  return v !== undefined && v !== null && v !== false && v !== '' && v !== 0;
}

/** Compile a template's node tree into a function (S, C, D) returning its
 *  rendering. C carries the compiled templates (t), the HTML escape (e), the
 *  scope lookup (L), the value-to-text (T) and the if-test (TR). The generated
 *  code follows the node tree exactly: text, escaped/raw values, if/else, for
 *  with its loop record, and includes to depth 8. */
function compileTemplate(nodes) {
  let code = 'let o=[];\n';
  let v = 0;
  const gen = (ns, sv) => {
    for (const n of ns) {
      if (n.kind === 'text') code += `o.push(${JSON.stringify(n.text)});\n`;
      else if (n.kind === 'var') code += `o.push(C.e(C.T(C.L(${sv},${JSON.stringify(n.path)}))));\n`;
      else if (n.kind === 'raw') code += `o.push(C.T(C.L(${sv},${JSON.stringify(n.path)})));\n`;
      else if (n.kind === 'if') {
        const cond = `C.TR(C.L(${sv},${JSON.stringify(n.path)}))`;
        code += n.not ? `if(!(${cond})){\n` : `if(${cond}){\n`;
        gen(n.then, sv);
        if (n.else !== null) {
          code += `}else{\n`;
          gen(n.else, sv);
        }
        code += `}\n`;
      } else if (n.kind === 'for') {
        v++;
        const sv2 = `S${v}`;
        const xv = `X${v}`;
        const kv = `K${v}`;
        code += `{const ${xv}=C.L(${sv},${JSON.stringify(n.path)});if(Array.isArray(${xv})){for(let ${kv}=0;${kv}<${xv}.length;${kv}++){const ${sv2}={vars:{[${JSON.stringify(n.name)}]:${xv}[${kv}],loop:{index:${kv}+1,first:${kv}===0,last:${kv}===${xv}.length-1,length:${xv}.length}},parent:${sv}};\n`;
        gen(n.body, sv2);
        code += `}}}\n`;
      } else if (n.kind === 'include') {
        code += `{const t=C.t(${JSON.stringify(n.name)});if(t!==undefined&&D<8)o.push(t(${sv},C,D+1));}\n`;
      }
    }
  };
  gen(nodes, 'S');
  code += 'return o.join("");';
  return new Function('S', 'C', 'D', code);
}

// ---------------------------------------------------------------- the build

/** `rel` (a link's path, "/"-separated) from the directory parts `dirParts`, or null when it climbs out of the root. */
function resolvePath(dirParts, rel) {
  const parts = dirParts.slice();
  for (const seg of rel.split('/')) {
    if (seg === '' || seg === '.') continue;
    if (seg === '..') {
      if (parts.length === 0) return null;
      parts.pop();
    } else parts.push(seg);
  }
  return parts.join('/');
}

const dirOf = (path) => (path.includes('/') ? path.slice(0, path.lastIndexOf('/')) : '');

/** JS \s as a table, for the word scan. */
const WS_TABLE = new Uint8Array(65536);
for (const c of [32, 9, 10, 11, 12, 13, 160, 0x1680, 0x2028, 0x2029, 0x202f, 0x205f, 0x3000, 0xfeff]) WS_TABLE[c] = 1;
for (let c = 0x2000; c <= 0x200a; c++) WS_TABLE[c] = 1;

/** The words of a body: its runs of characters other than whitespace (JS \s). */
function wordsOf(text) {
  let n = 0;
  let inWord = false;
  for (let i = 0; i < text.length; i++) {
    if (WS_TABLE[text.charCodeAt(i)] === 1) inWord = false;
    else if (!inWord) {
      inWord = true;
      n++;
    }
  }
  return n;
}

/** One site's output tree: path -> text. */
export function run(input) {
  const templates = new Map();
  const report = [];
  for (const name of Object.keys(input.templates).sort(byCodeUnits)) {
    try {
      templates.set(name, parseTemplate(name, input.templates[name]));
    } catch (err) {
      if (!(err instanceof TemplateError)) throw err;
      report.push(err.message);
    }
  }
  const base = input.site.base_url.replace(/\/+$/, '');

  // 1. Sources, in code-unit order of their paths; .md is a source, else static.
  const files = [...input.files].sort((x, y) => byCodeUnits(x.path, y.path));
  const sources = [];
  const statics = [];
  for (const f of files) {
    if (f.path.endsWith('.md')) sources.push({ path: f.path, text: f.content.indexOf('\r') === -1 ? f.content : f.content.replace(/\r\n?/g, '\n') });
    else statics.push({ path: f.path, content: f.content });
  }

  // 2. Front matter; a refused source is reported and dropped, and so is a draft.
  const kept = [];
  for (const s of sources) {
    const fm = parseFrontMatter(s.text);
    if ('error' in fm) {
      report.push(`refused ${s.path}: ${fm.error}`);
      continue;
    }
    s.data = fm.data;
    s.body = fm.body;
    s.post = s.path.startsWith('posts/');
    let own;
    if (s.path.startsWith('tags/')) own = 'tags';
    else if (s.path.startsWith('page/')) own = 'page';
    if (own !== undefined) {
      report.push(`refused ${s.path}: ${own}/ is the generator's own`);
      continue;
    }
    if (s.post && !Object.hasOwn(s.data, 'date')) {
      report.push(`refused ${s.path}: a post needs a date`);
      continue;
    }
    const layout = s.data.layout ?? (s.post ? 'post' : 'page');
    if (!templates.has(layout)) {
      report.push(`refused ${s.path}: layout "${layout}" is no template that parses`);
      continue;
    }
    s.layout = layout;
    if (s.data.draft === true) {
      report.push(`draft ${s.path}`);
      continue;
    }
    kept.push(s);
  }
  sources.length = 0;
  for (const s of kept) sources.push(s);

  // 3. Permalinks: slug made unique in its directory, in source order.
  const taken = new Map();
  const takenIn = (dir) => {
    let set = taken.get(dir);
    if (set === undefined) {
      set = new Set(dir === '' ? RESERVED : []);
      taken.set(dir, set);
    }
    return set;
  };
  const bySource = new Map();
  for (const s of sources) {
    const dir = s.post ? 'posts' : dirOf(s.path);
    const slug = uniqueSlug(s.data.slug ?? slugify(s.data.title), takenIn(dir));
    s.slug = slug;
    s.outDir = dir === '' ? slug : `${dir}/${slug}`;
    s.url = `${base}/${s.outDir}/`;
    bySource.set(s.path, s);
  }

  // 4. Markdown, links resolved to the pages and static files they name.
  const staticPaths = new Set(statics.map((f) => f.path));
  const broken = new Set();
  for (const s of sources) {
    const dir = dirOf(s.path);
    const dirParts = dir === '' ? [] : dir.split('/');
    // Resolved link targets, cached per source: the same rel repeats
    // from many pages in one directory.
    const resolved = new Map();
    const env = {
      link: (target) => {
        if (SCHEME_RE.test(target) || target.startsWith('/') || target.startsWith('#')) return target;
        const hash = target.indexOf('#');
        const rel = hash === -1 ? target : target.slice(0, hash);
        const frag = hash === -1 ? '' : target.slice(hash);
        let path;
        if (rel === '') path = null;
        else if (resolved.has(rel)) path = resolved.get(rel);
        else {
          path = resolvePath(dirParts, rel);
          resolved.set(rel, path);
        }
        if (path !== null && bySource.has(path)) return `${bySource.get(path).url}${frag}`;
        if (path !== null && staticPaths.has(path)) return `${base}/${path}${frag}`;
        broken.add(`broken-link ${s.path} -> ${target}`);
        return target;
      },
    };
    const md = renderMarkdown(s.body, env);
    s.html = md.html;
    s.toc = md.toc;
    s.excerpt = md.excerpt;
  }

  // 5. Collections: posts newest first, then by slug; newer/older neighbours.
  const posts = sources.filter((s) => s.post);
  posts.sort((x, y) => byCodeUnits(y.data.date, x.data.date) || byCodeUnits(x.slug, y.slug));
  posts.forEach((p, k) => {
    p.newer = k === 0 ? null : posts[k - 1];
    p.older = k === posts.length - 1 ? null : posts[k + 1];
  });

  // 6. Tags, grouped by slug, named as first met in the posts' order.
  const bySlug = new Map();
  const ownTags = (s) => {
    const seen = new Set();
    const out = [];
    for (const name of s.data.tags ?? []) {
      const slug = slugify(name);
      if (seen.has(slug)) continue;
      seen.add(slug);
      out.push({ name, slug });
    }
    return out;
  };
  for (const p of posts) {
    p.tagRefs = [];
    for (const { name, slug } of ownTags(p)) {
      let tag = bySlug.get(slug);
      if (tag === undefined) {
        tag = { name, slug, url: `${base}/tags/${slug}/`, posts: [] };
        bySlug.set(slug, tag);
      }
      tag.posts.push(p);
      p.tagRefs.push(tag);
    }
  }
  for (const s of sources) {
    if (s.post) continue;
    s.tagRefs = ownTags(s).map(({ name, slug }) => bySlug.get(slug) ?? { name, slug, url: '', posts: [] });
  }
  const tags = [...bySlug.values()].sort((x, y) => y.posts.length - x.posts.length || byCodeUnits(x.slug, y.slug));

  // 7. Layouts.
  const linkOf = (s) => (s === null ? null : { title: s.data.title, url: s.url, date: s.data.date ?? '', date_long: s.data.date === undefined ? '' : longDate(s.data.date) });
  const cardOf = (s) => ({
    title: s.data.title,
    url: s.url,
    date: s.data.date ?? '',
    date_long: s.data.date === undefined ? '' : longDate(s.data.date),
    summary: s.data.summary ?? '',
    excerpt: s.excerpt,
    reading_minutes: Math.max(1, Math.ceil(wordsOf(s.body) / 200)),
    tags: s.tagRefs.map((t) => (t.link ??= { name: t.name, slug: t.slug, url: t.url })),
  });
  // One card per source, computed once: pageOf used to recompute cardOf for
  // every source — and wordsOf over its whole body a second time for posts.
  const cards = new Map(sources.map((s) => [s, cardOf(s)]));
  const pageOf = (s) => ({
    ...cards.get(s),
    slug: s.slug,
    content: s.html,
    toc: s.toc.map((h) => ({ level: h.level, id: h.id, html: h.html })),
    newer: linkOf(s.newer ?? null),
    older: linkOf(s.older ?? null),
    is_post: s.post,
  });
  const site = { title: input.site.title, base_url: base };
  const allTags = tags.map((t) => ({ name: t.name, slug: t.slug, url: t.url, count: t.posts.length }));
  const recent = posts.slice(0, input.site.recent).map((p) => cards.get(p));
  // Templates compile lazily on first render: a partial nothing includes
  // never pays the compile cost.
  const compiled = new Map();
  const ctx = {
    t: (name) => {
      let fn = compiled.get(name);
      if (fn === undefined) {
        const nodes = templates.get(name);
        if (nodes === undefined) return undefined;
        fn = compileTemplate(nodes);
        compiled.set(name, fn);
      }
      return fn;
    },
    e: escapeHtml,
    L: lookup,
    T: textOf,
    TR: truthy,
  };
  const output = {};
  const render = (name, vars) => {
    const fn = ctx.t(name);
    if (fn === undefined) return null;
    return fn({ vars: { site, tags: allTags, recent, ...vars }, parent: null }, ctx, 0);
  };
  for (const s of sources) output[`${s.outDir}/index.html`] = render(s.layout, { page: pageOf(s) });
  const per = input.site.per_page;
  const pages = Math.max(1, Math.ceil(posts.length / per));
  const indexUrl = (k) => (k === 1 ? `${base}/` : `${base}/page/${k}/`);
  for (let k = 1; k <= pages; k++) {
    const html = render('index', {
      posts: posts.slice((k - 1) * per, k * per).map((p) => cards.get(p)),
      pager: { number: k, total: pages, url: indexUrl(k), newer_url: k === 1 ? '' : indexUrl(k - 1), older_url: k === pages ? '' : indexUrl(k + 1) },
    });
    if (html !== null) output[k === 1 ? 'index.html' : `page/${k}/index.html`] = html;
  }
  for (const t of tags) {
    const html = render('tag', { tag: { name: t.name, slug: t.slug, url: t.url, count: t.posts.length, posts: t.posts.map((p) => cards.get(p)) } });
    if (html !== null) output[`tags/${t.slug}/index.html`] = html;
  }
  const tagsHtml = render('tags', {});
  if (tagsHtml !== null) output['tags/index.html'] = tagsHtml;
  for (const f of statics) {
    if (Object.hasOwn(output, f.path)) report.push(`shadowed ${f.path}: a built page has its path`);
    else output[f.path] = f.content;
  }

  // 8. The report.
  const lines = [...report, ...broken].sort(byCodeUnits);
  const pageCount = sources.filter((s) => !s.post).length;
  lines.push(`built ${pageCount} pages, ${posts.length} posts, ${tags.length} tags and ${statics.length} static files`);
  output[REPORT_FILE] = `${lines.join('\n')}\n`;
  return output;
}
