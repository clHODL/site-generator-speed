// native.cc — v2 ground-up single-pass engine for site-generator-speed.
//
// Branch native-v2. Replaces the v1 Value-tree + tree-walk interpreter
// (native-top10 @ 964c2c5, measured ~250 ms net work, rank ~#100-130) with:
//   - zero-copy JSON: strings are views into the input buffer (UTF-8,
//     WTF-8 for lone surrogates from \u escapes), arena-allocated nodes;
//   - compiled templates: flat op-lists executed by a tight VM, values
//     appended straight into the output (no Value tree, no temporaries);
//   - single-pass Markdown with SIMD special-byte scanning;
//   - SIMD JSON output escaping; arena (bump) allocation, reset per case;
//   - minimal CJS-style driver path, fast exit after the write.
// All strings stay UTF-8 views; UTF-16 code-unit semantics are preserved
// where the reference needs them (sort order, slug cut, JS trim) via
// explicit helpers. Byte-identical output to makefun-site 1.0.0 is the gate.
//
// Build: g++ -O3 -std=c++17 -fPIC -shared -I/usr/include/node
//        -DNAPI_VERSION=8 -march=x86-64-v2 native.cc -o native.node
//        -static-libstdc++ -static-libgcc ; strip native.node
// AVX2 hot paths use function target attributes + runtime cpuid dispatch,
// with SSE2/scalar fallbacks, so the binary loads on any x86-64-v2 CPU.
//
// glibc 2.38 renamed strtoll/strtoul to __isoc23_* (C23 semantics, the only
// difference is recognising 0b prefixes). The static libstdc++ references
// the new names, which would bind this binary to GLIBC_2.38 — newer than the
// grader image may carry. Define the new names here, forwarding to the old
// (2.2.5) symbols, so the binary only needs GLIBC_2.2.5 for them.
extern "C" {
long long __strtoll_225(const char*, char**, int);
unsigned long long __strtoul_225(const char*, char**, int);
__asm__(".symver __strtoll_225,strtoll@GLIBC_2.2.5");
__asm__(".symver __strtoul_225,strtoul@GLIBC_2.2.5");
long long __isoc23_strtoll(const char* nptr, char** endptr, int base) {
  return __strtoll_225(nptr, endptr, base);
}
unsigned long long __isoc23_strtoul(const char* nptr, char** endptr, int base) {
  return __strtoul_225(nptr, endptr, base);
}
}

#include <node_api.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <immintrin.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../dev/unicode_tables.h"

// ---------------------------------------------------------------- basics

using U8 = std::string;        // owned UTF-8 bytes (WTF-8 for lone surrogates)
using U8V = std::string_view;  // view into input/arena (UTF-8 / WTF-8)

// Str: a UTF-8 view (ptr+len). The workhorse string type; no ownership.
struct Str {
  const char* p = nullptr;
  uint32_t n = 0;
  Str() = default;
  Str(const char* p_, uint32_t n_) : p(p_), n(n_) {}
  Str(const char* p_, size_t n_) : p(p_), n((uint32_t)n_) {}
  Str(U8V v) : p(v.data()), n((uint32_t)v.size()) {}
  U8V view() const { return U8V(p, n); }
  bool empty() const { return n == 0; }
  const char* begin() const { return p; }
  const char* end() const { return p + n; }
  char operator[](size_t i) const { return p[i]; }
  bool startsWith(Str pre) const {
    return n >= pre.n && (pre.n == 0 || std::memcmp(p, pre.p, pre.n) == 0);
  }
  bool endsWith(Str suf) const {
    return n >= suf.n && (suf.n == 0 || std::memcmp(p + n - suf.n, suf.p, suf.n) == 0);
  }
  Str substr(size_t a, size_t len) const {
    if (a > n) a = n;
    if (len > n - a) len = n - a;
    return Str(p + a, (uint32_t)len);
  }
  Str substr(size_t a) const { return substr(a, n - a); }
};
static inline Str S(const char* c) { return Str(c, std::strlen(c)); }
static inline Str S(U8V v) { return Str(v); }

// ---- bump arena: one per runAll, reset per case. Never shrinks mid-case;
// capacity is grown between cases only, so arena pointers stay valid.
struct Arena {
  char* base = nullptr;
  char* cur = nullptr;
  char* end = nullptr;
  void init(size_t cap) {
    base = (char*)std::malloc(cap);
    cur = base;
    end = base + cap;
  }
  void reset() { cur = base; }
  void* alloc(size_t n, size_t align = 8) {
    uintptr_t a = ((uintptr_t)cur + align - 1) & ~(align - 1);
    char* r = (char*)a;
    cur = r + n;
    return r;
  }
  bool room(size_t n) const { return (size_t)(end - cur) >= n + 64; }
  template <typename T> T* allocArr(size_t n) {
    return (T*)alloc(sizeof(T) * n, alignof(T));
  }
  // Copy bytes into the arena, return a view of the copy.
  Str dup(Str s) {
    char* d = (char*)alloc(s.n, 1);
    if (s.n) std::memcpy(d, s.p, s.n);
    return Str(d, s.n);
  }
  Str dup(const char* p, size_t n) { return dup(Str(p, (uint32_t)n)); }
  // Concatenate pieces into one arena string.
  Str cat(std::initializer_list<Str> parts) {
    size_t total = 0;
    for (auto& s : parts) total += s.n;
    char* d = (char*)alloc(total, 1);
    char* w = d;
    for (auto& s : parts) {
      if (s.n) std::memcpy(w, s.p, s.n);
      w += s.n;
    }
    return Str(d, (uint32_t)total);
  }
};

// ---- growable byte buffer (scratch: reallocs freely; nothing keeps views
// into it except via an explicit arena copy afterwards).
struct Buf {
  char* data = nullptr;
  size_t len = 0;
  size_t cap = 0;
  ~Buf() { std::free(data); }
  void reset() { len = 0; }
  void reserve(size_t extra) {
    if (len + extra <= cap) return;
    size_t nc = cap ? cap : 256;
    while (nc < len + extra) nc *= 2;
    data = (char*)std::realloc(data, nc);
    cap = nc;
  }
  void append(const char* p, size_t n) {
    reserve(n);
    std::memcpy(data + len, p, n);
    len += n;
  }
  void append(Str s) { append(s.p, s.n); }
  void append(U8V v) { append(v.data(), v.size()); }
  void append(const char* c) { append(c, std::strlen(c)); }
  void push(char c) {
    reserve(1);
    data[len++] = c;
  }
  void appendInt(int64_t v) {
    char tmp[24];
    int n = std::snprintf(tmp, sizeof tmp, "%lld", (long long)v);
    append(tmp, (size_t)n);
  }
  Str view() const { return Str(data, (uint32_t)len); }
};

// ---- profiler: NATIVE_PROFILE=1 prints phase times to stderr.
// Timers are steady_clock reads; ~free when the env var is unset (the
// print only happens at the end of runAll).
struct Prof {
  static std::vector<std::pair<const char*, double>>& acc() {
    static std::vector<std::pair<const char*, double>> m;
    return m;
  }
  static void add(const char* name, double ms) {
    for (auto& kv : acc())
      if (kv.first == name) {
        kv.second += ms;
        return;
      }
    acc().push_back({name, ms});
  }
  static void print() {
    auto v = acc();
    std::sort(v.begin(), v.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    double total = 0;
    for (auto& kv : v) total += kv.second;
    std::fprintf(stderr, "[profile] total %.1f ms\n", total);
    for (auto& kv : v) std::fprintf(stderr, "[profile] %8.1f ms  %s\n", kv.second, kv.first);
  }
};
struct ProfTimer {
  const char* name;
  std::chrono::steady_clock::time_point t0;
  explicit ProfTimer(const char* n) : name(n), t0(std::chrono::steady_clock::now()) {}
  ~ProfTimer() {
    Prof::add(name, std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0)
                        .count());
  }
};

// ---------------------------------------------------------------- UTF-8

// Decode one UTF-8 sequence at p (bytes [p,end)); returns the code point
// and advances p. Invalid bytes become U+FFFD, one byte consumed (as Node's
// UTF-8 decoding does). WTF-8 surrogate halves (ED A0..BF) decode to the
// surrogate code unit itself, so lone surrogates round-trip.
static inline uint32_t utf8Decode(const uint8_t*& p, const uint8_t* end) {
  uint8_t c = *p;
  if (c < 0x80) {
    p++;
    return c;
  }
  auto cont = [&](int k) -> int {
    return k < (end - p) && (p[k] & 0xC0) == 0x80 ? (p[k] & 0x3F) : -1;
  };
  if ((c & 0xE0) == 0xC0) {
    int c1 = cont(1);
    if (c1 < 0) {
      p++;
      return 0xFFFD;
    }
    uint32_t cp = ((c & 0x1F) << 6) | (uint32_t)c1;
    if (cp < 0x80) {
      p++;
      return 0xFFFD;
    }
    p += 2;
    return cp;
  }
  if ((c & 0xF0) == 0xE0) {
    int c1 = cont(1), c2 = cont(2);
    if (c1 < 0 || c2 < 0) {
      p++;
      return 0xFFFD;
    }
    uint32_t cp = ((c & 0x0F) << 12) | ((uint32_t)c1 << 6) | (uint32_t)c2;
    if (cp >= 0xD800 && cp < 0xE000) {
      p += 3;
      return cp;  // WTF-8 surrogate half: keep the code unit
    }
    if (cp < 0x800) {
      p++;
      return 0xFFFD;
    }
    p += 3;
    return cp;
  }
  if ((c & 0xF8) == 0xF0) {
    int c1 = cont(1), c2 = cont(2), c3 = cont(3);
    if (c1 < 0 || c2 < 0 || c3 < 0) {
      p++;
      return 0xFFFD;
    }
    uint32_t cp = ((c & 0x07) << 18) | ((uint32_t)c1 << 12) | ((uint32_t)c2 << 6) | (uint32_t)c3;
    if (cp < 0x10000 || cp > 0x10FFFF) {
      p++;
      return 0xFFFD;
    }
    p += 4;
    return cp;
  }
  p++;
  return 0xFFFD;
}

// Encode cp as UTF-8 (WTF-8 for surrogates) into out.
static inline void utf8Encode(char* out, uint32_t cp, size_t& n) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    n = 1;
  } else if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    n = 2;
  } else if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    n = 3;
  } else {
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    n = 4;
  }
}

// Number of UTF-16 code units for one code point.
static inline size_t u16Len(uint32_t cp) { return cp < 0x10000 ? 1 : 2; }

// JS White_Space + LineTerminator code points (the \s set).
static inline bool isWSCodePoint(uint32_t cp) {
  return cp == 32 || (cp >= 9 && cp <= 13) || cp == 160 || cp == 0x1680 ||
         (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
         cp == 0x202F || cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
}

// Compare two UTF-8 views in UTF-16 code-unit order (JS string <).
// Fast path: memcmp while the differing bytes are both ASCII. Slow path:
// decode both to UTF-16 units and compare lexicographically.
static int cmpStr(Str a, Str b) {
  size_t n = a.n < b.n ? a.n : b.n;
  if (n) {
    int c = std::memcmp(a.p, b.p, n);
    if (c != 0) {
      // Find the first differing byte; if both are ASCII the memcmp
      // result already equals the UTF-16 code-unit order.
      size_t k = 0;
      while (k < n && a.p[k] == b.p[k]) k++;
      if ((uint8_t)a.p[k] < 0x80 && (uint8_t)b.p[k] < 0x80)
        return (uint8_t)a.p[k] < (uint8_t)b.p[k] ? -1 : 1;
      // Fall through to the slow path.
    } else if (a.n == b.n) {
      return 0;
    } else {
      return a.n < b.n ? -1 : 1;
    }
  } else if (a.n == b.n) {
    return 0;
  } else {
    return a.n < b.n ? -1 : 1;
  }
  // Slow path: compare as UTF-16 code-unit sequences.
  const uint8_t* pa = (const uint8_t*)a.p;
  const uint8_t* pb = (const uint8_t*)b.p;
  const uint8_t* ea = pa + a.n;
  const uint8_t* eb = pb + b.n;
  while (pa < ea && pb < eb) {
    uint32_t ca = utf8Decode(pa, ea);
    uint32_t cb = utf8Decode(pb, eb);
    // Compare the (1 or 2) UTF-16 units of ca vs cb.
    uint16_t ua[2], ub[2];
    size_t na = 0, nb = 0;
    if (ca < 0x10000) ua[na++] = (uint16_t)ca;
    else {
      ua[na++] = (uint16_t)(0xD800 + ((ca - 0x10000) >> 10));
      ua[na++] = (uint16_t)(0xDC00 + ((ca - 0x10000) & 0x3FF));
    }
    if (cb < 0x10000) ub[nb++] = (uint16_t)cb;
    else {
      ub[nb++] = (uint16_t)(0xD800 + ((cb - 0x10000) >> 10));
      ub[nb++] = (uint16_t)(0xDC00 + ((cb - 0x10000) & 0x3FF));
    }
    size_t m = na < nb ? na : nb;
    for (size_t i = 0; i < m; i++)
      if (ua[i] != ub[i]) return ua[i] < ub[i] ? -1 : 1;
    if (na != nb) return na < nb ? -1 : 1;
  }
  if (pa < ea) return 1;
  if (pb < eb) return -1;
  return 0;
}
static inline bool strEq(Str a, Str b) {
  return a.n == b.n && (a.n == 0 || std::memcmp(a.p, b.p, a.n) == 0);
}
static inline bool strLess(Str a, Str b) { return cmpStr(a, b) < 0; }

// JS String.trim on a UTF-8 view (strips JS whitespace at both ends).
static Str jsTrim(Str s) {
  const uint8_t* p = (const uint8_t*)s.p;
  const uint8_t* e = p + s.n;
  const uint8_t* a = p;
  const uint8_t* b = e;
  while (a < b) {
    if (a[0] < 0x80) {
      if (!isWSCodePoint(a[0])) break;
      a++;
    } else {
      const uint8_t* q = a;
      uint32_t cp = utf8Decode(q, e);
      if (!isWSCodePoint(cp)) break;
      a = q;
    }
  }
  while (b > a) {
    // Walk back one code point.
    const uint8_t* q = b - 1;
    while (q > a && (*q & 0xC0) == 0x80) q--;
    const uint8_t* r = q;
    uint32_t cp = utf8Decode(r, b);
    if (!isWSCodePoint(cp)) break;
    b = q;
  }
  return Str((const char*)a, (uint32_t)(b - a));
}

// ---------------------------------------------------------------- SIMD
// AVX2 hot paths with function target attributes + runtime cpuid dispatch.
// Every AVX2 routine has a scalar twin; dispatch picks the fastest the CPU
// actually has (the grader CPU is unknown; this VM has AVX2 + AVX512F).

static bool cpuHasAVX2() {
  static int cached = -1;
  if (cached >= 0) return cached == 1;
  uint32_t eax, ebx, ecx, edx;
  __asm__ __volatile__("cpuid"
                       : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                       : "a"(7), "c"(0));
  bool avx2 = (ebx & (1u << 5)) != 0;
  // AVX2 needs OS XSAVE support for YMM state; check XCR0 bits 1,2.
  uint32_t xlo, xhi;
  __asm__ __volatile__("xgetbv" : "=a"(xlo), "=d"(xhi) : "c"(0));
  bool osx = (xlo & 0x6) == 0x6;
  cached = (avx2 && osx) ? 1 : 0;
  return cached == 1;
}

// Set-membership scan: index of the first byte whose value is one of
// `members` (nmem <= 16), or n if none. Direct per-member compares
// (correct by construction, still SIMD: ~2 instructions per member per
// 32-byte chunk). The earlier nibble-shuffle tables were WRONG — the bit
// index (a nibble, 0-15) overflows an 8-bit table entry, so members with
// a nibble >= 8 ('<', '>', '[', '\\', ...) were invisible.
__attribute__((target("avx2"))) static size_t findInSetAVX2(const uint8_t* p, size_t n,
                                                           const uint8_t* members,
                                                           size_t nmem) {
  __m256i vec[16];
  for (size_t m = 0; m < nmem; m++) vec[m] = _mm256_set1_epi8((char)members[m]);
  size_t i = 0;
  size_t lim = n & ~(size_t)31;
  for (; i < lim; i += 32) {
    __m256i x = _mm256_loadu_si256((const __m256i*)(p + i));
    __m256i acc = _mm256_setzero_si256();
    for (size_t m = 0; m < nmem; m++)
      acc = _mm256_or_si256(acc, _mm256_cmpeq_epi8(x, vec[m]));
    uint32_t mask = (uint32_t)_mm256_movemask_epi8(acc);
    if (mask) return i + (size_t)__builtin_ctz(mask);
  }
  for (; i < n; i++) {
    uint8_t c = p[i];
    for (size_t m = 0; m < nmem; m++)
      if (c == members[m]) return i;
  }
  return n;
}

static size_t findInSetScalar(const uint8_t* p, size_t n, const uint8_t* members,
                              size_t nmem) {
  bool table[256] = {false};
  for (size_t m = 0; m < nmem; m++) table[members[m]] = true;
  for (size_t i = 0; i < n; i++)
    if (table[p[i]]) return i;
  return n;
}

static inline size_t findInSet(const uint8_t* p, size_t n, const uint8_t* members,
                               size_t nmem) {
  if (cpuHasAVX2()) return findInSetAVX2(p, n, members, nmem);
  return findInSetScalar(p, n, members, nmem);
}

// Find the next '\n' at or after i (or n). AVX2 memchr-style scan.
__attribute__((target("avx2"))) static size_t findNewlineAVX2(const uint8_t* p, size_t n,
                                                              size_t i) {
  const __m256i nl = _mm256_set1_epi8('\n');
  size_t lim = n & ~(size_t)31;
  size_t j = i;
  for (; j < lim; j += 32) {
    __m256i x = _mm256_loadu_si256((const __m256i*)(p + j));
    uint32_t mask = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(x, nl));
    if (mask) return j + (size_t)__builtin_ctz(mask);
  }
  for (; j < n; j++)
    if (p[j] == '\n') return j;
  return n;
}
static inline size_t findNewline(const uint8_t* p, size_t n, size_t i) {
  if (cpuHasAVX2()) return findNewlineAVX2(p, n, i);
  const void* r = std::memchr(p + i, '\n', n - i);
  return r ? (size_t)((const uint8_t*)r - p) : n;
}

// ---------------------------------------------------------------- Val
// Arena values. Strings are views (into the input buffer or the arena);
// objects/arrays are arena arrays. Int vs Dbl are separate types so JS
// number formatting (Number#toString) only ever sees integers here.

struct KV;
struct Val {
  enum Type : uint8_t { Null, Bool, Int, Dbl, Str, Arr, Obj, Loop } type = Null;
  bool b = false;
  int64_t i = 0;
  double d = 0;
  ::Str s;
  struct {
    Val* items = nullptr;
    uint32_t n = 0;
  } arr;
  struct {
    KV* items = nullptr;
    uint32_t n = 0;
  } obj;
  struct {
    uint32_t idx = 0, len = 0;
  } loop;
  static Val nullv() { return Val(); }
  static Val boolean(bool v) {
    Val x;
    x.type = Bool;
    x.b = v;
    return x;
  }
  static Val integer(int64_t v) {
    Val x;
    x.type = Int;
    x.i = v;
    return x;
  }
  static Val dbl(double v) {
    Val x;
    x.type = Dbl;
    x.d = v;
    return x;
  }
  static Val str(::Str v) {
    Val x;
    x.type = Str;
    x.s = v;
    return x;
  }
  static Val array(Val* items, uint32_t n) {
    Val x;
    x.type = Arr;
    x.arr.items = items;
    x.arr.n = n;
    return x;
  }
  static Val object(KV* items, uint32_t n) {
    Val x;
    x.type = Obj;
    x.obj.items = items;
    x.obj.n = n;
    return x;
  }
  static Val loopRec(uint32_t idx, uint32_t len) {
    Val x;
    x.type = Loop;
    x.loop.idx = idx;
    x.loop.len = len;
    return x;
  }
  // Linear property lookup on an object (first match). Keys are unique in
  // every object we build; parsed JSON objects overwrite duplicates.
  const Val* find(::Str key) const;
  const Val* find(const char* key) const { return find(S(key)); }
  bool isNum() const { return type == Int || type == Dbl; }
  double asNum() const { return type == Int ? (double)i : d; }
};
struct KV {
  Str key;
  Val val;
};
inline const Val* Val::find(::Str key) const {
  if (type != Obj) return nullptr;
  for (uint32_t k = 0; k < obj.n; k++)
    if (strEq(obj.items[k].key, key)) return &obj.items[k].val;
  return nullptr;
}

// JS String(value): numbers via JS digits, bool via true/false, arrays
// joined with ", ", everything else "".
static void appendJsNum(Buf& out, double v);  // defined below
static void appendTextOf(Buf& out, const Val* v);
static void appendTextOf(Buf& out, const Val* v) {
  if (!v) return;
  switch (v->type) {
    case Val::Str:
      out.append(v->s);
      break;
    case Val::Int:
      out.appendInt(v->i);
      break;
    case Val::Dbl:
      appendJsNum(out, v->d);
      break;
    case Val::Bool:
      out.append(v->b ? "true" : "false");
      break;
    case Val::Arr:
      for (uint32_t k = 0; k < v->arr.n; k++) {
        if (k) out.append(", ");
        appendTextOf(out, &v->arr.items[k]);
      }
      break;
    default:
      break;  // Null, Obj, Loop -> ""
  }
}
static bool truthy(const Val* v) {
  if (!v) return false;
  switch (v->type) {
    case Val::Null:
      return false;
    case Val::Bool:
      return v->b;
    case Val::Str:
      return v->s.n > 0;
    case Val::Int:
      return v->i != 0;
    case Val::Dbl:
      // The reference's truthy is `v !== 0` for numbers: NaN counts as
      // truthy (NaN !== 0), unlike JS's own truthiness.
      return v->d != 0;
    case Val::Arr:
      return v->arr.n > 0;
    default:
      return true;  // Obj, Loop
  }
}

// ---------------------------------------------------------------- numbers

// JS Number#toString for doubles. Values in this program are integers by
// construction (per_page, recent, counts, indices); the double path is
// only a correctness safety net (v1's %.17g behaviour).
static void appendJsNum(Buf& out, double v) {
  if (std::isnan(v)) {
    out.append("NaN");
    return;
  }
  if (std::isinf(v)) {
    out.append(v < 0 ? "-Infinity" : "Infinity");
    return;
  }
  if (v == 0) {
    out.append("0");
    return;
  }
  double av = std::fabs(v);
  double ip;
  if (std::modf(v, &ip) == 0 && av < 1e21) {
    char tmp[32];
    int n = std::snprintf(tmp, sizeof tmp, "%.0f", v);
    out.append(tmp, (size_t)n);
    return;
  }
  char tmp[40];
  int n = std::snprintf(tmp, sizeof tmp, "%.17g", v);
  out.append(tmp, (size_t)n);
}

// ---------------------------------------------------------------- JSON

// Zero-copy recursive-descent JSON parser. Strings become views into the
// input; \u escapes are expanded into a scratch arena buffer (WTF-8 for
// lone surrogates). Objects overwrite duplicate keys (JSON.parse semantics).
struct JsonParser {
  const uint8_t* p;
  const uint8_t* e;
  Arena* ar;
  bool ok = true;

  JsonParser(const uint8_t* p_, const uint8_t* e_, Arena* ar_) : p(p_), e(e_), ar(ar_) {}

  void ws() {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
  }
  // SIMD whitespace skip for long runs (used at the top of the document
  // and between big elements). Returns true if advanced.
  void wsFast() {
    // Scalar loop is fine here: structural whitespace is a handful of
    // bytes between values, and the parser is not the bottleneck.
    ws();
  }

  Val parse() {
    ws();
    Val v = value();
    return v;
  }

  Val value() {
    if (p >= e) {
      ok = false;
      return Val::nullv();
    }
    uint8_t c = *p;
    if (c == '{') return object();
    if (c == '[') return array();
    if (c == '"') return Val::str(string());
    if (c == 't') {
      if (e - p >= 4 && std::memcmp(p, "true", 4) == 0) {
        p += 4;
        return Val::boolean(true);
      }
      ok = false;
      return Val::nullv();
    }
    if (c == 'f') {
      if (e - p >= 5 && std::memcmp(p, "false", 5) == 0) {
        p += 5;
        return Val::boolean(false);
      }
      ok = false;
      return Val::nullv();
    }
    if (c == 'n') {
      if (e - p >= 4 && std::memcmp(p, "null", 4) == 0) {
        p += 4;
        return Val::nullv();
      }
      ok = false;
      return Val::nullv();
    }
    return number();
  }

  Val object() {
    p++;  // {
    ws();
    // Collect into a growable arena vector; duplicate keys overwrite.
    KV* items = nullptr;
    uint32_t n = 0, cap = 0;
    auto push = [&](Str key, Val val) {
      for (uint32_t k = 0; k < n; k++)
        if (strEq(items[k].key, key)) {
          items[k].val = val;
          return;
        }
      if (n == cap) {
        cap = cap ? cap * 2 : 8;
        KV* ni = ar->allocArr<KV>(cap);
        if (items) std::memcpy(ni, items, sizeof(KV) * n);
        items = ni;
      }
      items[n].key = key;
      items[n].val = val;
      n++;
    };
    if (p < e && *p == '}') {
      p++;
      return Val::object(nullptr, 0);
    }
    while (true) {
      ws();
      if (p >= e || *p != '"') {
        ok = false;
        return Val::nullv();
      }
      Str key = string();
      ws();
      if (p >= e || *p != ':') {
        ok = false;
        return Val::nullv();
      }
      p++;
      ws();
      Val v = value();
      push(key, v);
      ws();
      if (p >= e) {
        ok = false;
        return Val::nullv();
      }
      if (*p == ',') {
        p++;
        continue;
      }
      if (*p == '}') {
        p++;
        break;
      }
      ok = false;
      return Val::nullv();
    }
    return Val::object(items, n);
  }

  Val array() {
    p++;  // [
    ws();
    Val* items = nullptr;
    uint32_t n = 0, cap = 0;
    auto push = [&](Val v) {
      if (n == cap) {
        cap = cap ? cap * 2 : 8;
        Val* ni = ar->allocArr<Val>(cap);
        if (items) std::memcpy(ni, items, sizeof(Val) * n);
        items = ni;
      }
      items[n++] = v;
    };
    if (p < e && *p == ']') {
      p++;
      return Val::array(nullptr, 0);
    }
    while (true) {
      ws();
      push(value());
      ws();
      if (p >= e) {
        ok = false;
        return Val::nullv();
      }
      if (*p == ',') {
        p++;
        continue;
      }
      if (*p == ']') {
        p++;
        break;
      }
      ok = false;
      return Val::nullv();
    }
    return Val::array(items, n);
  }

  // Parse a JSON string starting at the opening quote. Fast path: scan for
  // " or \ with the set scanner; copy the whole run at once. Escapes are
  // expanded into a scratch buffer (arena).
  Str string() {
    // p points at '"'.
    const uint8_t* start = p + 1;
    // Set: '"' and '\\' (escape scan); control chars < 0x20 are rejected
    // by the JSON spec but never occur in generated input — the scalar
    // slow path would catch them; here we just treat them as content.
    static const uint8_t members[2] = {'"', '\\'};
    size_t rem = (size_t)(e - start);
    size_t rel = findInSet(start, rem, members, 2);
    if (rel < rem && start[rel] == '"') {
      // Clean string, no escapes: zero-copy view.
      Str r((const char*)start, (uint32_t)rel);
      p = start + rel + 1;
      return r;
    }
    // Escape path: expand into a scratch arena buffer.
    Buf tmp;
    const uint8_t* q = start;
    while (q < e) {
      size_t rem2 = (size_t)(e - q);
      size_t rel2 = findInSet(q, rem2, members, 2);
      if (rel2 < rem2 && q[rel2] == '"') {
        tmp.append((const char*)q, rel2);
        p = q + rel2 + 1;
        Str r = ar->dup(tmp.view());
        return r;
      }
      tmp.append((const char*)q, rel2);
      q += rel2;
      if (q >= e) break;
      // q points at '\\'.
      q++;
      if (q >= e) break;
      uint8_t esc = *q++;
      switch (esc) {
        case '"': tmp.push('"'); break;
        case '\\': tmp.push('\\'); break;
        case '/': tmp.push('/'); break;
        case 'b': tmp.push('\b'); break;
        case 'f': tmp.push('\f'); break;
        case 'n': tmp.push('\n'); break;
        case 'r': tmp.push('\r'); break;
        case 't': tmp.push('\t'); break;
        case 'u': {
          auto hex4 = [&]() -> int {
            if (e - q < 4) return -1;
            int v = 0;
            for (int k = 0; k < 4; k++) {
              uint8_t h = q[k];
              int d;
              if (h >= '0' && h <= '9') d = h - '0';
              else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
              else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
              else return -1;
              v = (v << 4) | d;
            }
            q += 4;
            return v;
          };
          int cp = hex4();
          if (cp < 0) {
            ok = false;
            return Str();
          }
          if (cp >= 0xD800 && cp < 0xDC00) {
            // High surrogate: try a following \uDC00-\uDFFF low half.
            const uint8_t* save = q;
            if (e - q >= 6 && q[0] == '\\' && q[1] == 'u') {
              q += 2;
              int lo2 = hex4();
              if (lo2 >= 0xDC00 && lo2 < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo2 - 0xDC00);
              } else {
                q = save;
              }
            }
          }
          char enc[4];
          size_t en = 0;
          utf8Encode(enc, (uint32_t)cp, en);
          tmp.append(enc, en);
          break;
        }
        default:
          ok = false;
          return Str();
      }
    }
    ok = false;
    return Str();
  }

  Val number() {
    const uint8_t* s = p;
    if (p < e && *p == '-') p++;
    bool isInt = true;
    while (p < e && *p >= '0' && *p <= '9') p++;
    if (p < e && *p == '.') {
      isInt = false;
      p++;
      while (p < e && *p >= '0' && *p <= '9') p++;
    }
    if (p < e && (*p == 'e' || *p == 'E')) {
      isInt = false;
      p++;
      if (p < e && (*p == '+' || *p == '-')) p++;
      while (p < e && *p >= '0' && *p <= '9') p++;
    }
    size_t n = (size_t)(p - s);
    if (n == 0) {
      ok = false;
      return Val::nullv();
    }
    if (isInt) {
      // Parse as int64 (the only numbers that reach templates).
      bool neg = s[0] == '-';
      const uint8_t* d = s + (neg ? 1 : 0);
      uint64_t mag = 0;
      bool over = false;
      for (const uint8_t* q = d; q < p; q++) {
        if (mag > (UINT64_MAX - 9) / 10) over = true;
        else mag = mag * 10 + (uint64_t)(*q - '0');
      }
      if (!over) {
        int64_t v;
        if (neg) {
          if (mag > (uint64_t)INT64_MAX + 1) over = true;
          else v = mag == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)mag;
        } else {
          if (mag > (uint64_t)INT64_MAX) over = true;
          else v = (int64_t)mag;
        }
        if (!over) return Val::integer(v);
      }
      // Overflow: fall through to double.
    }
    char buf[64];
    size_t cn = n < sizeof buf - 1 ? n : sizeof buf - 1;
    std::memcpy(buf, s, cn);
    buf[cn] = 0;
    return Val::dbl(std::strtod(buf, nullptr));
  }
};

// JSON string escaping into out (for answers.json). AVX2 scan for bytes
// needing escape: '"' (0x22), '\\' (0x5C), and < 0x20; clean runs are
// memcpy'd whole. WTF-8 surrogate sequences (ED A0..BF) are re-escaped
// as \uXXXX (JSON.stringify behaviour).
__attribute__((target("avx2"))) static void appendJsonEscapedAVX2(Buf& out, Str s) {
  const uint8_t* p = (const uint8_t*)s.p;
  size_t n = s.n;
  size_t i = 0;
  const __m256i q = _mm256_set1_epi8('"');
  const __m256i bs = _mm256_set1_epi8('\\');
  const __m256i lt = _mm256_set1_epi8(0x20);
  const __m256i bias = _mm256_set1_epi8((char)0x80);
  const __m256i hiED = _mm256_set1_epi8((char)0xED);
  const __m256i zero = _mm256_setzero_si256();
  size_t lim = n & ~(size_t)31;
  for (; i < lim;) {
    __m256i x = _mm256_loadu_si256((const __m256i*)(p + i));
    __m256i m1 = _mm256_cmpeq_epi8(x, q);
    __m256i m2 = _mm256_cmpeq_epi8(x, bs);
    // Unsigned x < 0x20: flip the sign bit, then signed compare vs 0xA0.
    __m256i xb = _mm256_xor_si256(x, bias);
    __m256i m3 = _mm256_cmpgt_epi8(_mm256_set1_epi8((char)0xA0), xb);
    // WTF-8 surrogate lead: byte 0xED followed by 0xA0..0xBF.
    __m256i m4 = _mm256_cmpeq_epi8(x, hiED);
    uint32_t mask = (uint32_t)_mm256_movemask_epi8(_mm256_or_si256(
        _mm256_or_si256(m1, m2), _mm256_or_si256(m3, m4)));
    if (!mask) {
      out.append((const char*)p + i, 32);
      i += 32;
      continue;
    }
    // Slow lane for this chunk: byte by byte.
    for (size_t j = i; j < i + 32; j++) {
      uint8_t c = p[j];
      if (c == '"' || c == '\\') {
        out.push('\\');
        out.push((char)c);
      } else if (c < 0x20) {
        char tmp[8];
        int m = std::snprintf(tmp, sizeof tmp, "\\u%04x", c);
        out.append(tmp, (size_t)m);
      } else if (c == 0xED && j + 2 < n && (p[j + 1] & 0xF0) == 0xA0 &&
                 (p[j + 2] & 0xC0) == 0x80) {
        uint32_t cp = ((c & 0x0F) << 12) | ((p[j + 1] & 0x3F) << 6) | (p[j + 2] & 0x3F);
        char tmp[8];
        int m = std::snprintf(tmp, sizeof tmp, "\\u%04x", cp);
        out.append(tmp, (size_t)m);
        j += 2;
      } else {
        out.push((char)c);
      }
    }
    i += 32;
  }
  // Scalar tail.
  for (; i < n; i++) {
    uint8_t c = p[i];
    if (c == '"' || c == '\\') {
      out.push('\\');
      out.push((char)c);
    } else if (c < 0x20) {
      char tmp[8];
      int m = std::snprintf(tmp, sizeof tmp, "\\u%04x", c);
      out.append(tmp, (size_t)m);
    } else if (c == 0xED && i + 2 < n && (p[i + 1] & 0xF0) == 0xA0 &&
               (p[i + 2] & 0xC0) == 0x80) {
      uint32_t cp = ((c & 0x0F) << 12) | ((p[i + 1] & 0x3F) << 6) | (p[i + 2] & 0x3F);
      char tmp[8];
      int m = std::snprintf(tmp, sizeof tmp, "\\u%04x", cp);
      out.append(tmp, (size_t)m);
      i += 2;
    } else {
      out.push((char)c);
    }
  }
}
static void appendJsonEscapedScalar(Buf& out, Str s) {
  const uint8_t* p = (const uint8_t*)s.p;
  size_t n = s.n;
  for (size_t i = 0; i < n; i++) {
    uint8_t c = p[i];
    if (c == '"' || c == '\\') {
      out.push('\\');
      out.push((char)c);
    } else if (c < 0x20) {
      char tmp[8];
      int m = std::snprintf(tmp, sizeof tmp, "\\u%04x", c);
      out.append(tmp, (size_t)m);
    } else if (c == 0xED && i + 2 < n && (p[i + 1] & 0xF0) == 0xA0 &&
               (p[i + 2] & 0xC0) == 0x80) {
      uint32_t cp = ((c & 0x0F) << 12) | ((p[i + 1] & 0x3F) << 6) | (p[i + 2] & 0x3F);
      char tmp[8];
      int m = std::snprintf(tmp, sizeof tmp, "\\u%04x", cp);
      out.append(tmp, (size_t)m);
      i += 2;
    } else {
      out.push((char)c);
    }
  }
}
static inline void appendJsonEscaped(Buf& out, Str s) {
  if (cpuHasAVX2()) appendJsonEscapedAVX2(out, s);
  else appendJsonEscapedScalar(out, s);
}
static inline void appendJsonString(Buf& out, Str s) {
  out.push('"');
  appendJsonEscaped(out, s);
  out.push('"');
}

// ---------------------------------------------------------------- slug
// Port of v1's proven slug (itself a port of solve-js.mjs slugify):
// NFKD via kNfkdMap/kNfkdSeq (+ algorithmic Hangul), drop \p{M} marks
// (kMarkRanges), lowercase A-Z, runs of non-[a-z0-9] collapse to one '-'
// (gap flag: a dash is emitted only when the next kept char arrives),
// cut at 64 UTF-16 units, drop a trailing '-', empty -> "untitled".
// Input is a UTF-8 view; astral chars behave as in v1 (surrogate pair
// units, both non-marks, both gaps).

static bool isMarkUnit(uint32_t c) {
  size_t lo = 0, hi = sizeof(kMarkRanges) / sizeof(kMarkRanges[0]);
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (c < kMarkRanges[mid][0]) hi = mid;
    else if (c > kMarkRanges[mid][1]) lo = mid + 1;
    else return true;
  }
  return false;
}

// NFKD-decompose one code point into the unit buffer (itself when there
// is no decomposition). Units are UTF-16 code units.
static void nfkdUnit(uint32_t cp, uint16_t* out, size_t& n) {
  if (cp >= 0xAC00 && cp <= 0xD7A3) {
    uint32_t s = cp - 0xAC00;
    uint32_t l = s / 588, v = (s % 588) / 28, t = s % 28;
    out[n++] = (uint16_t)(0x1100 + l);
    out[n++] = (uint16_t)(0x11A1 + v);
    if (t) out[n++] = (uint16_t)(0x11A7 + t);
    return;
  }
  if (cp < 0x10000) {
    size_t lo = 0, hi = sizeof(kNfkdMap) / sizeof(kNfkdMap[0]);
    while (lo < hi) {
      size_t mid = (lo + hi) / 2;
      if (cp < kNfkdMap[mid].cp) hi = mid;
      else if (cp > kNfkdMap[mid].cp) lo = mid + 1;
      else {
        for (int i = 0; i < kNfkdMap[mid].len; i++)
          out[n++] = kNfkdSeq[kNfkdMap[mid].off + i];
        return;
      }
    }
    out[n++] = (uint16_t)cp;
    return;
  }
  out[n++] = (uint16_t)(0xD800 + ((cp - 0x10000) >> 10));
  out[n++] = (uint16_t)(0xDC00 + ((cp - 0x10000) & 0x3FF));
}

static Str slugOf(Arena* ar, Str s) {
  // ASCII fast path: NFKD is identity on ASCII, no combining marks.
  bool ascii = true;
  for (uint32_t i = 0; i < s.n; i++)
    if ((uint8_t)s.p[i] >= 0x80) {
      ascii = false;
      break;
    }
  Buf out;
  if (ascii) {
    bool gap = false;
    for (uint32_t i = 0; i < s.n; i++) {
      char c = s.p[i];
      int keep = -1;
      if (c >= 'A' && c <= 'Z') keep = c + 32;
      else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) keep = c;
      if (keep == -1) {
        gap = out.len > 0;
        continue;
      }
      if (gap) out.push('-');
      gap = false;
      out.push((char)keep);
    }
    if (out.len > 64) out.len = 64;
    while (out.len > 0 && out.data[out.len - 1] == '-') out.len--;
    if (out.len == 0) return S("untitled");
    return ar->dup(out.view());
  }
  // Full path: NFKD-decompose into units, drop marks, fold.
  // (The non-ASCII path is rare — most titles are ASCII — so a heap
  // vector here is fine.)
  std::vector<uint16_t> units;
  units.reserve(s.n + 8);
  const uint8_t* p = (const uint8_t*)s.p;
  const uint8_t* e = p + s.n;
  while (p < e) {
    uint32_t cp = utf8Decode(p, e);
    if (cp >= 0xD800 && cp < 0xE000) continue;  // WTF-8 half: dropped
    uint16_t tmp[4];
    size_t tn = 0;
    nfkdUnit(cp, tmp, tn);
    for (size_t k = 0; k < tn; k++) units.push_back(tmp[k]);
  }
  bool gap = false;
  for (size_t k = 0; k < units.size(); k++) {
    uint32_t ch = units[k];
    if (isMarkUnit(ch)) continue;
    int keep = -1;
    if (ch >= 'A' && ch <= 'Z') keep = (int)ch + 32;
    else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) keep = (int)ch;
    if (keep == -1) {
      gap = out.len > 0;
      continue;
    }
    if (gap) out.push('-');
    gap = false;
    out.push((char)keep);
  }
  if (out.len > 64) out.len = 64;
  while (out.len > 0 && out.data[out.len - 1] == '-') out.len--;
  if (out.len == 0) return S("untitled");
  return ar->dup(out.view());
}

static const char* MONTHS[12] = {"January", "February", "March", "April", "May", "June",
                                  "July", "August", "September", "October", "November",
                                  "December"};

// A YYYY-MM-DD date in words, from the string alone ("2026-11-09" ->
// "9 November 2026"). Dates in the data are always well-formed.
static Str longDate(Arena* ar, Str date) {
  if (date.n < 10) return ar->dup(date);
  int m = (uint8_t)date.p[5] * 10 + (uint8_t)date.p[6] - 528;
  int d = (uint8_t)date.p[8] * 10 + (uint8_t)date.p[9] - 528;
  char db[8];
  int dn = std::snprintf(db, sizeof db, "%d", d);
  const char* mon = (m >= 1 && m <= 12) ? MONTHS[m - 1] : "";
  return ar->cat({Str(db, (uint32_t)dn), S(" "), S(mon), S(" "), date.substr(0, 4)});
}

// ---------------------------------------------------------------- HTML escape

static inline void appendEscaped(Buf& out, Str s) {
  // HTML-escape & < > " ' (escapeHtml). Fast path: scan for the set with
  // the SIMD set scanner; clean runs memcpy whole.
  static const uint8_t members[5] = {'&', '<', '>', '"', '\''};
  const uint8_t* p = (const uint8_t*)s.p;
  size_t n = s.n;
  size_t i = 0;
  while (i < n) {
    size_t rel = findInSet(p + i, n - i, members, 5);
    size_t j = i + rel;
    if (rel) out.append((const char*)p + i, rel);
    if (j >= n) break;
    switch (p[j]) {
      case '&': out.append("&amp;"); break;
      case '<': out.append("&lt;"); break;
      case '>': out.append("&gt;"); break;
      case '"': out.append("&quot;"); break;
      case '\'': out.append("&#39;"); break;
    }
    i = j + 1;
  }
}

// ---------------------------------------------------------------- front matter
// Exact port of solve-js.mjs parseFrontMatter + valueOf + stringOf.
// Error messages are reproduced verbatim (they land in build-report.txt).

struct FmResult {
  Str body;
  Val data;  // Obj
  bool err = false;
  Str errMsg;  // arena string, valid only if err
};

// BLANK_RE = /^[ \t]*$/ — a line of only spaces/tabs.
static bool isFmBlank(Str line) {
  for (uint32_t i = 0; i < line.n; i++)
    if (line.p[i] != ' ' && line.p[i] != '\t') return false;
  return true;
}

// LINE_RE = /^([a-z_]+):(?: (.*))?$/ — key [a-z_]+, then end-of-line
// (no value) or one space + the rest. Returns false when the line does
// not match at all.
static bool matchLineRe(Str line, Str& key, Str& raw, bool& hasValue) {
  size_t k = 0;
  while (k < line.n && ((line.p[k] >= 'a' && line.p[k] <= 'z') || line.p[k] == '_'))
    k++;
  if (k == 0 || k >= line.n || line.p[k] != ':') return false;
  key = line.substr(0, k);
  if (k + 1 == line.n) {
    hasValue = false;
    raw = Str();
    return true;
  }
  if (line.p[k + 1] != ' ') return false;
  hasValue = true;
  raw = line.substr(k + 2);
  return true;
}

static int daysInFm(int y, int m) {
  if (m == 2) return ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : 28;
  return (m == 4 || m == 6 || m == 9 || m == 11) ? 30 : 31;
}

// DATE_RE = /^(\d{4})-(\d{2})-(\d{2})$/ + range 1970-01-01..2099-12-31.
static bool isValidDate(Str raw) {
  if (raw.n != 10 || raw.p[4] != '-' || raw.p[7] != '-') return false;
  for (int i : {0, 1, 2, 3, 5, 6, 8, 9})
    if (raw.p[i] < '0' || raw.p[i] > '9') return false;
  int y = (raw.p[0] - '0') * 1000 + (raw.p[1] - '0') * 100 + (raw.p[2] - '0') * 10 +
          (raw.p[3] - '0');
  int mo = (raw.p[5] - '0') * 10 + (raw.p[6] - '0');
  int d = (raw.p[8] - '0') * 10 + (raw.p[9] - '0');
  if (y < 1970 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > daysInFm(y, mo))
    return false;
  return true;
}

// stringOf: "..." with \" and \\ its only escapes, or bare. On error
// returns false and sets errMsg (the inner message, without the key
// prefix). The value is built into `scratch` (caller-owned) and returned
// as a view of it; the caller must copy it before reusing `scratch`.
static bool stringOfValue(Str raw, Str& value, Str& errMsg, Buf& scratch) {
  if (raw.n > 0 && raw.p[0] == '"') {
    if (raw.n < 2 || raw.p[raw.n - 1] != '"') {
      errMsg = S("a quoted string is not closed");
      return false;
    }
    scratch.reset();
    for (size_t i = 1; i + 1 < raw.n; i++) {
      char c = raw.p[i];
      if (c == '\\') {
        char nx = raw.p[i + 1];
        if ((nx != '"' && nx != '\\') || i + 1 == raw.n - 1) {
          errMsg = S("a quoted string holds an escape other than \\\" or \\\\");
          return false;
        }
        scratch.push(nx);
        i++;
      } else if (c == '"') {
        errMsg = S("a quoted string holds a stray quote");
        return false;
      } else {
        scratch.push(c);
      }
    }
    value = scratch.view();
    return true;
  }
  if (raw.n > 0 && raw.p[0] == '[') {
    errMsg = S("a bare string opens on [");
    return false;
  }
  value = raw;
  return true;
}

// One key's value, or why it is refused. On success fills `out` (a Val)
// and returns true; on failure returns false and sets errMsg (full
// message, key included). `scratch` is a reusable Buf for building values.
static bool valueOfKey(Arena* ar, Str key, Str raw, Val& out, Str& errMsg, Buf& scratch) {
  if (strEq(key, S("title")) || strEq(key, S("summary"))) {
    Str v, em;
    if (!stringOfValue(raw, v, em, scratch)) {
      errMsg = ar->cat({S("key \""), key, S("\": "), em});
      return false;
    }
    out = Val::str(ar->dup(v));
    return true;
  }
  if (strEq(key, S("date"))) {
    if (!isValidDate(raw)) {
      errMsg = S("key \"date\": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)");
      return false;
    }
    out = Val::str(raw);
    return true;
  }
  if (strEq(key, S("tags"))) {
    if (raw.n < 2 || raw.p[0] != '[' || raw.p[raw.n - 1] != ']') {
      errMsg = S("key \"tags\": not a list [a, b]");
      return false;
    }
    Str inner = raw.substr(1, raw.n - 2);
    if (jsTrim(inner).n == 0) {
      out = Val::array(nullptr, 0);
      return true;
    }
    // Split on ',' and trim each tag (JS trim).
    Val* tagsArr = nullptr;
    uint32_t tn = 0, tcap = 0;
    size_t start = 0;
    while (true) {
      size_t comma = start;
      while (comma < inner.n && inner.p[comma] != ',') comma++;
      Str part = jsTrim(inner.substr(start, comma - start));
      // TAG_RE = /^[^"[\]]+$/ — no ", [, or ].
      bool bad = part.n == 0;
      if (!bad)
        for (uint32_t q = 0; q < part.n; q++)
          if (part.p[q] == '"' || part.p[q] == '[' || part.p[q] == ']') {
            bad = true;
            break;
          }
      if (bad) {
        errMsg = S("key \"tags\": an empty tag, or one holding \" [ or ]");
        return false;
      }
      if (tn == tcap) {
        tcap = tcap ? tcap * 2 : 4;
        Val* ni = ar->allocArr<Val>(tcap);
        if (tagsArr) std::memcpy(ni, tagsArr, sizeof(Val) * tn);
        tagsArr = ni;
      }
      tagsArr[tn++] = Val::str(part);
      if (comma >= inner.n) break;
      start = comma + 1;
    }
    out = Val::array(tagsArr, tn);
    return true;
  }
  if (strEq(key, S("draft"))) {
    if (strEq(raw, S("true"))) {
      out = Val::boolean(true);
      return true;
    }
    if (strEq(raw, S("false"))) {
      out = Val::boolean(false);
      return true;
    }
    errMsg = S("key \"draft\": true or false");
    return false;
  }
  if (strEq(key, S("layout"))) {
    // NAME_RE = /^[a-z0-9-]+$/
    bool ok = raw.n > 0;
    for (uint32_t q = 0; ok && q < raw.n; q++) {
      char c = raw.p[q];
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) ok = false;
    }
    if (!ok) {
      errMsg = S("key \"layout\": not a template name");
      return false;
    }
    out = Val::str(raw);
    return true;
  }
  if (strEq(key, S("slug"))) {
    // SLUG_RE = /^[a-z0-9]+(?:-[a-z0-9]+)*$/ and length <= 64.
    bool ok = raw.n > 0 && raw.n <= 64;
    if (ok) {
      // Segments of [a-z0-9]+ joined by single '-'.
      bool lastDash = false;
      for (uint32_t q = 0; q < raw.n; q++) {
        char c = raw.p[q];
        if (c == '-') {
          if (q == 0 || q == raw.n - 1 || lastDash) {
            ok = false;
            break;
          }
          lastDash = true;
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
          lastDash = false;
        } else {
          ok = false;
          break;
        }
      }
    }
    if (!ok) {
      errMsg = S("key \"slug\": not a slug");
      return false;
    }
    out = Val::str(raw);
    return true;
  }
  errMsg = ar->cat({S("unknown key \""), key, S("\"")});
  return false;
}

static const char* FM_KEYS[7] = {"title", "date", "tags", "draft", "layout", "slug", "summary"};

static FmResult parseFrontMatter(Arena* ar, Str content) {
  FmResult r;
  auto fail = [&](Str msg) {
    r.err = true;
    r.errMsg = msg;
    r.body = Str();
    r.data = Val::object(nullptr, 0);
    return r;
  };
  // The marker must be exactly "---" at the start, followed by '\n'
  // (or end of text at length 3, which is "not closed").
  if (content.n < 3 || content.p[0] != '-' || content.p[1] != '-' || content.p[2] != '-')
    return fail(S("no front matter"));
  if (content.n > 3 && content.p[3] != '\n') return fail(S("no front matter"));
  if (content.n == 3) return fail(S("the front matter is not closed"));
  // Find the closing line: a line exactly '---'.
  long closeStart = -1;
  size_t p = 4;
  while (p < content.n) {
    size_t nl = findNewline((const uint8_t*)content.p, content.n, p);
    size_t lineEnd = nl >= content.n ? content.n : nl;
    if (lineEnd - p == 3 && content.p[p] == '-' && content.p[p + 1] == '-' &&
        content.p[p + 2] == '-')
      closeStart = (long)p;
    if (closeStart >= 0 || nl >= content.n) break;
    p = nl + 1;
  }
  if (closeStart < 0) return fail(S("the front matter is not closed"));
  Str fmText = content.substr(4, (size_t)closeStart - 4);
  // Split fmText into lines (JS split('\n'); a trailing '' is a blank
  // line and skipped below).
  KV* items = nullptr;
  uint32_t n = 0, cap = 0;
  auto push = [&](Str key, Val val) {
    if (n == cap) {
      cap = cap ? cap * 2 : 4;
      KV* ni = ar->allocArr<KV>(cap);
      if (items) std::memcpy(ni, items, sizeof(KV) * n);
      items = ni;
    }
    items[n].key = key;
    items[n].val = val;
    n++;
  };
  Buf scratch;
  // fmText === '' yields no lines at all (JS split on '' gives []).
  if (fmText.n > 0) {
    size_t fpos = 0;
    uint32_t k = 0;  // fm line index (file line = k + 2)
    while (true) {
      size_t eol = findNewline((const uint8_t*)fmText.p, fmText.n, fpos);
      Str line = fmText.substr(fpos, eol - fpos);
      bool last = eol >= fmText.n;
      if (!isFmBlank(line) && !(line.n > 0 && line.p[0] == '#')) {
        Str key, raw;
        bool hasValue = false;
        if (!matchLineRe(line, key, raw, hasValue)) {
          char nb[16];
          int nn = std::snprintf(nb, sizeof nb, "%u", k + 2);
          return fail(ar->cat({S("line "), Str(nb, (uint32_t)nn), S(" is not \"key: value\"")}));
        }
        // raw = (m[2] ?? '').replace(/[ \t]+$/, '')
        if (hasValue) {
          size_t e = raw.n;
          while (e > 0 && (raw.p[e - 1] == ' ' || raw.p[e - 1] == '\t')) e--;
          raw = raw.substr(0, e);
        } else {
          raw = Str();
        }
        bool known = false;
        for (auto* kk : FM_KEYS)
          if (strEq(key, S(kk))) {
            known = true;
            break;
          }
        if (!known)
          return fail(ar->cat({S("unknown key \""), key, S("\"")}));
        for (uint32_t q = 0; q < n; q++)
          if (strEq(items[q].key, key))
            return fail(ar->cat({S("key \""), key, S("\" is given twice")}));
        if (raw.n == 0)
          return fail(ar->cat({S("key \""), key, S("\" has no value")}));
        Val v;
        Str em;
        if (!valueOfKey(ar, key, raw, v, em, scratch)) return fail(em);
        push(key, v);
      }
      if (last) break;
      fpos = eol + 1;
      k++;
    }
  }
  bool hasTitle = false;
  for (uint32_t q = 0; q < n; q++)
    if (strEq(items[q].key, S("title"))) {
      hasTitle = true;
      break;
    }
  if (!hasTitle) return fail(S("no title"));
  size_t bodyStart = (size_t)closeStart + 3;
  r.body = (bodyStart < content.n && content.p[bodyStart] == '\n')
               ? content.substr(bodyStart + 1)
               : content.substr(bodyStart);
  r.data = Val::object(items, n);
  return r;
}

// ---------------------------------------------------------------- Markdown
// Single-pass: one line table per body (SIMD newline scan), blocks walked
// once, inline rendered by an SIMD special-byte scanner. Mirrors the JS
// reference's block/inline semantics exactly (see solve-js.mjs).

struct Heading {
  int level = 0;
  Str id;
  Str html;  // rendered inline html of the heading text
};
struct MdOut {
  Str html;        // full rendered HTML (arena)
  Heading* toc = nullptr;
  uint32_t ntoc = 0;
  Str excerpt;     // first non-empty paragraph, plain text (arena)
};

// Inline context: link resolution needs the source directory parts, the
// site base, the bySource map (source path -> page url) and the static
// paths, for the broken-link check. Broken links are recorded here as
// full report lines ("broken-link <source> -> <target>") and deduplicated.
// This is an exact port of the reference's env.link.
struct InlineCtx {
  Str* dirParts = nullptr;  // directory parts of the source file
  uint32_t nDirParts = 0;
  Str base;         // site base (already stripped)
  Str sourcePath;   // path of the source file (for the broken report)
  // bySource: source paths (sorted) with their page urls.
  struct PageLink {
    Str path;  // source path, e.g. "posts/foo.md"
    Str url;   // page url, e.g. "https://x/blog/posts/foo/"
  };
  const PageLink* pages = nullptr;
  uint32_t nPages = 0;
  const Str* staticPaths = nullptr;  // sorted static file paths
  uint32_t nStatics = 0;
  Arena* ar = nullptr;
  // Per-source resolved cache: rel -> resolved path (or null = climbs
  // out of the root). Linear search; few distinct rels per source.
  struct Resolved {
    Str rel;
    Str path;
    bool isNull = false;
  };
  Resolved* resolved = nullptr;
  uint32_t nResolved = 0, capResolved = 0;
  // Broken report lines (deduplicated).
  Str* broken = nullptr;
  uint32_t nbroken = 0, capbroken = 0;
  void recordBroken(Str target) {
    Str line = ar->cat({S("broken-link "), sourcePath, S(" -> "), target});
    for (uint32_t q = 0; q < nbroken; q++)
      if (strEq(broken[q], line)) return;
    if (nbroken == capbroken) {
      capbroken = capbroken ? capbroken * 2 : 8;
      Str* nb = ar->allocArr<Str>(capbroken);
      if (broken) std::memcpy(nb, broken, sizeof(Str) * nbroken);
      broken = nb;
    }
    broken[nbroken++] = line;
  }
  // Binary search over the sorted source paths; returns the page url or
  // an empty Str when the path is not a source.
  Str pageUrl(Str p) const {
    uint32_t lo = 0, hi = nPages;
    while (lo < hi) {
      uint32_t mid = (lo + hi) / 2;
      int c = cmpStr(p, pages[mid].path);
      if (c == 0) return pages[mid].url;
      if (c < 0) hi = mid;
      else lo = mid + 1;
    }
    return Str();
  }
  bool isStatic(Str p) const {
    uint32_t lo = 0, hi = nStatics;
    while (lo < hi) {
      uint32_t mid = (lo + hi) / 2;
      int c = cmpStr(p, staticPaths[mid]);
      if (c == 0) return true;
      if (c < 0) hi = mid;
      else lo = mid + 1;
    }
    return false;
  }
};

// SCHEME_RE = /^[A-Za-z][A-Za-z0-9+.-]*:/ — a scheme prefix.
static bool hasScheme(Str t) {
  if (t.n == 0) return false;
  char c0 = t.p[0];
  if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z'))) return false;
  for (uint32_t i = 1; i < t.n; i++) {
    char c = t.p[i];
    if (c == ':') return true;
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'))
      return false;
  }
  return false;
}

// env.link(target): the reference's link resolution. Returns the href;
// records a broken link (raw target) when the resolved path is neither
// a page nor a static.
static Str resolveLinkTarget(InlineCtx& ctx, Str target, bool& broken) {
  if (hasScheme(target) || target.startsWith(S("/")) || target.startsWith(S("#"))) {
    broken = false;
    return target;
  }
  // Split off the fragment.
  size_t hash = (size_t)-1;
  for (uint32_t i = 0; i < target.n; i++)
    if (target.p[i] == '#') {
      hash = i;
      break;
    }
  Str rel = hash == (size_t)-1 ? target : target.substr(0, hash);
  Str frag = hash == (size_t)-1 ? Str() : target.substr(hash);
  Str path;
  bool isNull = false;
  if (rel.n == 0) {
    isNull = true;
  } else {
    // Per-source resolved cache.
    bool cached = false;
    for (uint32_t q = 0; q < ctx.nResolved; q++)
      if (strEq(ctx.resolved[q].rel, rel)) {
        path = ctx.resolved[q].path;
        isNull = ctx.resolved[q].isNull;
        cached = true;
        break;
      }
    if (!cached) {
      // resolvePath(dirParts, rel).
      Str segs[128];
      uint32_t nseg = 0;
      for (uint32_t q = 0; q < ctx.nDirParts && nseg < 128; q++)
        segs[nseg++] = ctx.dirParts[q];
      size_t sp = 0;
      bool nullOut = false;
      while (sp <= rel.n) {
        size_t slash = sp;
        while (slash < rel.n && rel.p[slash] != '/') slash++;
        Str seg = rel.substr(sp, slash - sp);
        if (seg.empty() || strEq(seg, S("."))) {
          // skip
        } else if (strEq(seg, S(".."))) {
          if (nseg == 0) {
            nullOut = true;
            break;
          }
          nseg--;
        } else {
          if (nseg < 128) segs[nseg++] = seg;
        }
        if (slash >= rel.n) break;
        sp = slash + 1;
      }
      if (nullOut) {
        isNull = true;
      } else {
        isNull = false;
        Buf nb;
        for (uint32_t q = 0; q < nseg; q++) {
          if (q) nb.push('/');
          nb.append(segs[q]);
        }
        path = ctx.ar->dup(nb.view());
      }
      if (ctx.nResolved == ctx.capResolved) {
        ctx.capResolved = ctx.capResolved ? ctx.capResolved * 2 : 8;
        InlineCtx::Resolved* ni = ctx.ar->allocArr<InlineCtx::Resolved>(ctx.capResolved);
        if (ctx.resolved) std::memcpy(ni, ctx.resolved, sizeof(InlineCtx::Resolved) * ctx.nResolved);
        ctx.resolved = ni;
      }
      ctx.resolved[ctx.nResolved].rel = ctx.ar->dup(rel);
      ctx.resolved[ctx.nResolved].path = path;
      ctx.resolved[ctx.nResolved].isNull = isNull;
      ctx.nResolved++;
    }
  }
  if (!isNull) {
    // A page's url is never empty (base + '/' + outDir + '/'), so an
    // empty result means "not a source".
    Str url = ctx.pageUrl(path);
    if (!url.empty()) {
      broken = false;
      return ctx.ar->cat({url, frag});
    }
    if (ctx.isStatic(path)) {
      broken = false;
      return ctx.ar->cat({ctx.base, S("/"), path, frag});
    }
  }
  broken = true;
  ctx.recordBroken(target);
  return target;
}

// Render inline markdown into out. `text` is a single line's content
// (already trimmed where the caller requires it).
static void renderInline(Buf& out, Str text, InlineCtx& ctx);

// Inline special-byte set: \ ` * [ & < > " '
// (backslash, backtick, asterisk, bracket, amp, lt, gt, dquote, squote)
static const uint8_t kInlineMembers[9] = {'\\', '`', '*', '[', '&', '<', '>', '"', '\''};

// ---- inline helpers (exact ports of the JS reference) ----

// ESCAPABLE: chars a backslash can escape.
static inline bool isEscapable(char c) {
  switch (c) {
    case '\\': case '`': case '*': case '_': case '[': case ']': case '(':
    case ')': case '#': case '+': case '-': case '.': case '!': case '>':
      return true;
    default:
      return false;
  }
}
// ESCAPES: the entity for a char, or the char itself.
static inline void appendEscapeEntity(Buf& out, char c) {
  switch (c) {
    case '&': out.append("&amp;"); break;
    case '<': out.append("&lt;"); break;
    case '>': out.append("&gt;"); break;
    case '"': out.append("&quot;"); break;
    case '\'': out.append("&#39;"); break;
    default: out.push(c); break;
  }
}

// The first index >= from where `s` holds `what` outside a backslash
// escape, or -1. An escape pair (\ + ESCAPABLE) is skipped as a unit.
static long findUnescaped(Str s, Str what, size_t from) {
  size_t wl = what.n;
  if (wl == 0) return (long)from;
  for (size_t j = from; j + wl <= s.n; j++) {
    if (s.p[j] == '\\' && j + 1 < s.n && isEscapable(s.p[j + 1])) {
      j++;
      continue;
    }
    if (std::memcmp(s.p + j, what.p, wl) == 0) return (long)j;
  }
  return -1;
}

// The first single * from `from` (one whose run is not **), outside an
// escape, or -1.
static long findSingleStar(Str s, size_t from) {
  for (size_t j = from; j < s.n; j++) {
    char c = s.p[j];
    if (c == '\\' && j + 1 < s.n && isEscapable(s.p[j + 1])) {
      j++;
      continue;
    }
    if (c != '*') continue;
    if (j + 1 < s.n && s.p[j + 1] == '*') {
      j++;
      continue;
    }
    return (long)j;
  }
  return -1;
}

// Plain indexOf for a byte (no escape logic).
static long indexOfByte(Str s, char c, size_t from) {
  for (size_t j = from; j < s.n; j++)
    if (s.p[j] == c) return (long)j;
  return -1;
}

// Does `s` contain a JS \s whitespace code point?
static bool hasWS(Str s) {
  const uint8_t* p = (const uint8_t*)s.p;
  const uint8_t* e = p + s.n;
  while (p < e) {
    if (*p < 0x80) {
      if (isWSCodePoint(*p)) return true;
      p++;
    } else {
      if (isWSCodePoint(utf8Decode(p, e))) return true;
    }
  }
  return false;
}

static void renderInline(Buf& out, Str text, InlineCtx& ctx) {
  size_t n = text.n;
  size_t i = 0;
  while (i < n) {
    size_t rel = findInSet((const uint8_t*)text.p + i, n - i, kInlineMembers, 9);
    size_t k = i + rel;
    if (rel) out.append(text.p + i, rel);
    if (k >= n) break;
    char c = text.p[k];
    if (c == '\\') {
      if (k + 1 < n && isEscapable(text.p[k + 1])) {
        appendEscapeEntity(out, text.p[k + 1]);
        i = k + 2;
      } else {
        out.push('\\');
        i = k + 1;
      }
      continue;
    }
    if (c == '`') {
      long j = indexOfByte(text, '`', k + 1);
      if (j > (long)k + 1) {
        out.append("<code>");
        appendEscaped(out, text.substr(k + 1, (size_t)j - (k + 1)));
        out.append("</code>");
        i = (size_t)j + 1;
      } else {
        out.push('`');
        i = k + 1;
      }
      continue;
    }
    if (c == '*') {
      if (k + 1 < n && text.p[k + 1] == '*') {
        long j = findUnescaped(text, S("**"), k + 2);
        if (j > (long)k + 2) {
          out.append("<strong>");
          renderInline(out, text.substr(k + 2, (size_t)j - (k + 2)), ctx);
          out.append("</strong>");
          i = (size_t)j + 2;
        } else {
          out.append("**");
          i = k + 2;
        }
      } else {
        long j = findSingleStar(text, k + 1);
        if (j > (long)k + 1) {
          out.append("<em>");
          renderInline(out, text.substr(k + 1, (size_t)j - (k + 1)), ctx);
          out.append("</em>");
          i = (size_t)j + 1;
        } else {
          out.push('*');
          i = k + 1;
        }
      }
      continue;
    }
    if (c == '[') {
      long k2 = findUnescaped(text, S("]"), k + 1);
      long mm = -1;
      if (k2 != -1 && (size_t)k2 + 1 < n && text.p[k2 + 1] == '(')
        mm = indexOfByte(text, ')', (size_t)k2 + 2);
      Str target = mm == -1 ? Str() : text.substr((size_t)k2 + 2, (size_t)mm - ((size_t)k2 + 2));
      if (target.n > 0 && !hasWS(target)) {
        Str linkText = text.substr(k + 1, (size_t)k2 - (k + 1));
        bool broken = false;
        Str href = resolveLinkTarget(ctx, target, broken);
        (void)broken;  // resolveLinkTarget already recorded it
        out.append("<a href=\"");
        appendEscaped(out, href);
        out.append("\">");
        renderInline(out, linkText, ctx);
        out.append("</a>");
        i = (size_t)mm + 1;
      } else {
        out.push('[');
        i = k + 1;
      }
      continue;
    }
    // & < > " ' : HTML-escape.
    appendEscapeEntity(out, c);
    i = k + 1;
  }
}

// ---------------------------------------------------------------- Markdown
// (block level, single pass over the line table)

// ---------------------------------------------------------------- Markdown
// (block level, single pass over the line table)

enum LineKind : uint8_t { LK_BLANK = 0, LK_FENCE = 1, LK_HEADING = 2, LK_HR = 3,
                          LK_QUOTE = 4, LK_LIST = 5, LK_PARA = 6, LK_UNKNOWN = 0xFF };

// A line with only spaces/tabs.
static bool isBlankLine(Str line) {
  for (uint32_t i = 0; i < line.n; i++)
    if (line.p[i] != ' ' && line.p[i] != '\t') return false;
  return true;
}
// `line` with spaces and tabs dropped at both ends.
static Str trimST(Str line) {
  size_t a = 0, b = line.n;
  while (a < b && (line.p[a] == ' ' || line.p[a] == '\t')) a++;
  while (b > a && (line.p[b - 1] == ' ' || line.p[b - 1] == '\t')) b--;
  return line.substr(a, b - a);
}
// A fence-open line's language, or false (line of ``` + optional lang +
// spaces/tabs). `lang` is set on success ("" for a bare fence).
static bool fenceLang(Str line, Str& lang) {
  if (line.n < 3 || line.p[0] != '`' || line.p[1] != '`' || line.p[2] != '`')
    return false;
  size_t i = 3;
  while (i < line.n) {
    char c = line.p[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '-')
      i++;
    else break;
  }
  lang = line.substr(3, i - 3);
  while (i < line.n && (line.p[i] == ' ' || line.p[i] == '\t')) i++;
  return i == line.n;
}
// Whether `line` is a fence-close line (``` + spaces/tabs only).
static bool isFenceClose(Str line) {
  if (line.n < 3 || line.p[0] != '`' || line.p[1] != '`' || line.p[2] != '`')
    return false;
  for (size_t i = 3; i < line.n; i++)
    if (line.p[i] != ' ' && line.p[i] != '\t') return false;
  return true;
}
// A heading line's depth and text, or false.
static bool headingOf(Str line, int& depth, Str& text) {
  size_t i = 0;
  while (i < 6 && i < line.n && line.p[i] == '#') i++;
  if (i == 0) return false;
  if (i >= 6 && i < line.n && line.p[6] == '#') return false;
  if (i > 6) return false;
  if (i >= line.n) return false;
  char c = line.p[i];
  if (c != ' ' && c != '\t') return false;
  size_t j = i;
  while (j < line.n && (line.p[j] == ' ' || line.p[j] == '\t')) j++;
  Str t = line.substr(j);
  size_t e = t.n;
  while (e > 0 && (t.p[e - 1] == ' ' || t.p[e - 1] == '\t')) e--;
  t = t.substr(0, e);
  if (t.n == 0) return false;
  depth = (int)i;
  text = t;
  return true;
}
// Whether `line` is a thematic break (--- or *** + spaces/tabs only).
static bool isHr(Str line) {
  if (line.n < 3) return false;
  char c0 = line.p[0];
  if (c0 != '-' && c0 != '*') return false;
  if (line.p[1] != c0 || line.p[2] != c0) return false;
  for (size_t i = 3; i < line.n; i++)
    if (line.p[i] != ' ' && line.p[i] != '\t') return false;
  return true;
}
static LineKind lineKind(Str line) {
  if (line.n == 0) return LK_BLANK;
  unsigned char c = (unsigned char)line.p[0];
  if (c == ' ' || c == '\t') return isBlankLine(line) ? LK_BLANK : LK_PARA;
  if (c == '`') {
    Str lang;
    return fenceLang(line, lang) ? LK_FENCE : LK_PARA;
  }
  if (c == '#') {
    int d;
    Str t;
    return headingOf(line, d, t) ? LK_HEADING : LK_PARA;
  }
  if (c == '-') {
    if (isHr(line)) return LK_HR;
    if (line.n > 1 && line.p[1] == ' ') return LK_LIST;
    return LK_PARA;
  }
  if (c == '*') {
    if (isHr(line)) return LK_HR;
    if (line.n > 1 && line.p[1] == ' ') return LK_LIST;
    return LK_PARA;
  }
  if (c == '>') return LK_QUOTE;
  if (c >= '0' && c <= '9') {
    size_t i = 0;
    while (i < 9 && i < line.n && line.p[i] >= '0' && line.p[i] <= '9') i++;
    if (i > 0 && i < line.n && line.p[i] == '.' && i + 1 < line.n && line.p[i + 1] == ' ')
      return LK_LIST;
    return LK_PARA;
  }
  return LK_PARA;
}
// Whether `line` continues a list item (two or more spaces, then a
// non-whitespace code point).
static bool isContinued(Str line) {
  size_t i = 0;
  while (i < line.n && line.p[i] == ' ') i++;
  if (i < 2 || i >= line.n) return false;
  if ((uint8_t)line.p[i] < 0x80) return !isWSCodePoint((uint8_t)line.p[i]);
  const uint8_t* p = (const uint8_t*)line.p + i;
  const uint8_t* e = (const uint8_t*)line.p + line.n;
  return !isWSCodePoint(utf8Decode(p, e));
}

static MdOut renderMarkdown(Arena* ar, Str body, InlineCtx& ctx) {
  ProfTimer _pt("md.render");
  // Line table: body.split('\n') (JS semantics: a trailing '\n' yields
  // a final empty line; "" yields one empty line).
  Str* lines = nullptr;
  uint32_t nlines = 0, capLines = 0;
  {
    size_t pos = 0;
    while (true) {
      size_t nl = findNewline((const uint8_t*)body.p, body.n, pos);
      if (nlines == capLines) {
        capLines = capLines ? capLines * 2 : 64;
        Str* nl2 = ar->allocArr<Str>(capLines);
        if (lines) std::memcpy(nl2, lines, sizeof(Str) * nlines);
        lines = nl2;
      }
      lines[nlines++] = body.substr(pos, nl - pos);
      if (nl >= body.n) break;
      pos = nl + 1;
    }
  }
  // Kind cache (classified once per line).
  uint8_t* kinds = ar->allocArr<uint8_t>(nlines);
  std::memset(kinds, LK_UNKNOWN, nlines);
  auto kindAt = [&](uint32_t j) -> LineKind {
    if (kinds[j] == LK_UNKNOWN) kinds[j] = (uint8_t)lineKind(lines[j]);
    return (LineKind)kinds[j];
  };

  Buf html;
  Heading* toc = nullptr;
  uint32_t ntoc = 0, capToc = 0;
  Str* usedIds = nullptr;
  uint32_t nUsed = 0, capUsed = 0;
  Str excerpt;
  bool hasExcerpt = false;
  bool anyBlock = false;
  Buf scratch;  // per-block scratch (paragraphs, heading inline)

  size_t i = 0;
  while (i < nlines) {
    Str line = lines[i];
    LineKind kind = kindAt((uint32_t)i);
    if (kind == LK_BLANK) {
      i++;
      continue;
    }
    if (kind == LK_FENCE) {
      Str lang;
      fenceLang(line, lang);
      // Collect the body until a fence-close line (or EOF).
      i++;
      size_t bodyStart = i;
      while (i < nlines && !isFenceClose(lines[i])) i++;
      size_t bodyEnd = i;
      if (i < nlines) i++;  // skip the close line
      if (anyBlock) html.push('\n');
      anyBlock = true;
      html.append("<pre><code");
      if (lang.n > 0) {
        html.append(" class=\"language-");
        appendEscaped(html, lang);
        html.push('"');
      }
      html.push('>');
      if (bodyEnd > bodyStart) {
        for (size_t q = bodyStart; q < bodyEnd; q++) {
          if (q > bodyStart) html.push('\n');
          appendEscaped(html, lines[q]);
        }
        html.push('\n');
      }
      html.append("</code></pre>");
      continue;
    }
    if (kind == LK_HEADING) {
      int depth = 0;
      Str text;
      headingOf(line, depth, text);
      // Unique slug id.
      Str base = slugOf(ar, text);
      Str id = base;
      {
        bool taken = false;
        for (uint32_t q = 0; q < nUsed; q++)
          if (strEq(usedIds[q], id)) {
            taken = true;
            break;
          }
        int n = 2;
        while (taken) {
          char nb[16];
          int nn = std::snprintf(nb, sizeof nb, "-%d", n++);
          id = ar->cat({base, Str(nb, (uint32_t)nn)});
          taken = false;
          for (uint32_t q = 0; q < nUsed; q++)
            if (strEq(usedIds[q], id)) {
              taken = true;
              break;
            }
        }
        if (nUsed == capUsed) {
          capUsed = capUsed ? capUsed * 2 : 8;
          Str* ni = ar->allocArr<Str>(capUsed);
          if (usedIds) std::memcpy(ni, usedIds, sizeof(Str) * nUsed);
          usedIds = ni;
        }
        usedIds[nUsed++] = id;
      }
      scratch.reset();
      renderInline(scratch, text, ctx);
      Str inner = ar->dup(scratch.view());
      if (depth == 2 || depth == 3) {
        if (ntoc == capToc) {
          capToc = capToc ? capToc * 2 : 8;
          Heading* ni = ar->allocArr<Heading>(capToc);
          if (toc) std::memcpy(ni, toc, sizeof(Heading) * ntoc);
          toc = ni;
        }
        toc[ntoc].level = depth;
        toc[ntoc].id = id;
        toc[ntoc].html = inner;
        ntoc++;
      }
      if (anyBlock) html.push('\n');
      anyBlock = true;
      html.append("<h");
      html.appendInt(depth);
      html.append(" id=\"");
      html.append(id);
      html.append("\">");
      html.append(inner);
      html.append("</h");
      html.appendInt(depth);
      html.push('>');
      i++;
      continue;
    }
    if (kind == LK_HR) {
      if (anyBlock) html.push('\n');
      anyBlock = true;
      html.append("<hr>");
      i++;
      continue;
    }
    if (kind == LK_QUOTE) {
      // Collect '>' lines.
      size_t qStart = i;
      while (i < nlines && lines[i].n > 0 && lines[i].p[0] == '>') {
        i++;
      }
      // Build the kept (non-empty, stripped) lines, joined with '\n'.
      scratch.reset();
      bool anyKept = false;
      for (size_t q = qStart; q < i; q++) {
        Str l = lines[q];
        Str stripped = (l.n > 1 && l.p[1] == ' ') ? l.substr(2) : l.substr(1);
        Str t = trimST(stripped);
        if (t.n == 0) continue;
        if (anyKept) scratch.push('\n');
        anyKept = true;
        scratch.append(t);
      }
      if (anyBlock) html.push('\n');
      anyBlock = true;
      if (!anyKept) {
        html.append("<blockquote>\n</blockquote>");
      } else {
        html.append("<blockquote>\n<p>");
        renderInline(html, scratch.view(), ctx);
        html.append("</p>\n</blockquote>");
      }
      continue;
    }
    if (kind == LK_LIST) {
      bool ordered = line.p[0] >= '0' && line.p[0] <= '9';
      int64_t start = 1;
      if (ordered) {
        size_t d = 0;
        while (d < line.n && line.p[d] >= '0' && line.p[d] <= '9') d++;
        int64_t v = 0;
        for (size_t q = 0; q < d; q++) v = v * 10 + (line.p[q] - '0');
        start = v;
      }
      // Items: each item is a list of lines (first + continuations).
      struct Item {
        Str* lines;
        uint32_t n, cap;
      };
      Item* items = nullptr;
      uint32_t nitems = 0, capItems = 0;
      while (i < nlines) {
        Str l = lines[i];
        if (kindAt((uint32_t)i) != LK_LIST) {
          if (isContinued(l) && nitems > 0) {
            Item& it = items[nitems - 1];
            if (it.n == it.cap) {
              it.cap = it.cap ? it.cap * 2 : 4;
              Str* ni = ar->allocArr<Str>(it.cap);
              if (it.lines) std::memcpy(ni, it.lines, sizeof(Str) * it.n);
              it.lines = ni;
            }
            it.lines[it.n++] = trimST(l);
            i++;
            continue;
          }
          break;
        }
        bool lOrdered = l.p[0] >= '0' && l.p[0] <= '9';
        if (lOrdered != ordered) break;
        Str content;
        if (ordered) {
          size_t d = 0;
          while (d < l.n && l.p[d] >= '0' && l.p[d] <= '9') d++;
          content = trimST(l.substr(d + 2));
        } else {
          content = trimST(l.substr(2));
        }
        if (nitems == capItems) {
          capItems = capItems ? capItems * 2 : 8;
          Item* ni = ar->allocArr<Item>(capItems);
          if (items) std::memcpy(ni, items, sizeof(Item) * nitems);
          items = ni;
        }
        Item& it = items[nitems++];
        it.lines = ar->allocArr<Str>(4);
        it.n = 0;
        it.cap = 4;
        it.lines[it.n++] = content;
        i++;
      }
      if (anyBlock) html.push('\n');
      anyBlock = true;
      if (ordered && start != 1) {
        html.append("<ol start=\"");
        html.appendInt(start);
        html.append("\">");
      } else if (ordered) {
        html.append("<ol>");
      } else {
        html.append("<ul>");
      }
      html.push('\n');
      for (uint32_t q = 0; q < nitems; q++) {
        // Join the item's lines with '\n' and render inline.
        scratch.reset();
        for (uint32_t r = 0; r < items[q].n; r++) {
          if (r) scratch.push('\n');
          scratch.append(items[q].lines[r]);
        }
        html.append("<li>");
        renderInline(html, scratch.view(), ctx);
        html.append("</li>\n");
      }
      html.append(ordered ? "</ol>" : "</ul>");
      continue;
    }
    // Paragraph: first line + following lines until blank/fence/heading/hr.
    scratch.reset();
    scratch.append(trimST(line));
    i++;
    while (i < nlines) {
      LineKind k = kindAt((uint32_t)i);
      if (k == LK_BLANK || k == LK_FENCE || k == LK_HEADING || k == LK_HR) break;
      scratch.push('\n');
      scratch.append(trimST(lines[i]));
      i++;
    }
    Str paraHtml;
    {
      Buf pb;
      renderInline(pb, scratch.view(), ctx);
      paraHtml = ar->dup(pb.view());
    }
    if (!hasExcerpt) {
      excerpt = paraHtml;
      hasExcerpt = true;
    }
    if (anyBlock) html.push('\n');
    anyBlock = true;
    html.append("<p>");
    html.append(paraHtml);
    html.append("</p>");
  }
  MdOut out;
  if (anyBlock) html.push('\n');
  out.html = ar->dup(html.view());
  out.toc = toc;
  out.ntoc = ntoc;
  out.excerpt = hasExcerpt ? excerpt : Str();
  return out;
}

// ---------------------------------------------------------------- templates
// Compiled op-list VM. Templates are tokenized once per case (per the
// reference's tokenize/parseTemplate) into flat ops with precomputed
// jumps; the VM executes them with direct appends into the output —
// no Value tree, no per-node allocation in the hot path. Scope frames
// mirror the reference's {vars, parent} chain; the loop record is a
// frame-owned Val.

struct Op {
  enum K : uint8_t { Text, Var, Raw, If, Else, EndIf, For, EndFor, Include } kind;
  Str text;            // Text: literal
  Str* path = nullptr; // Var/Raw/If/For: path segments
  uint8_t npath = 0;
  bool neg = false;    // If: "not"
  int32_t jump = -1;   // If: else_pc or end_pc; Else: end_pc; For: end_pc
  int32_t elsePc = -1; // If: else op index (-1 = no else)
  Str varName;         // For: loop variable name
  int32_t tmpl = -1;   // Include: template index (-1 = missing)
};
struct Template {
  Str name;
  Op* ops = nullptr;
  uint32_t nops = 0;
};

// PATH_RE = /^[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*$/
static bool isPathStr(Str t) {
  if (t.n == 0) return false;
  size_t i = 0;
  while (true) {
    char c = t.p[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) return false;
    i++;
    while (i < t.n) {
      c = t.p[i];
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '_')
        i++;
      else break;
    }
    if (i == t.n) return true;
    if (t.p[i] != '.') return false;
    i++;
    if (i >= t.n) return false;
  }
}
// Split a validated path on '.' into arena segments.
static Str* splitPath(Arena* ar, Str t, uint8_t& nseg) {
  uint8_t n = 1;
  for (uint32_t i = 0; i < t.n; i++)
    if (t.p[i] == '.') n++;
  Str* segs = ar->allocArr<Str>(n);
  size_t start = 0;
  uint8_t k = 0;
  for (uint32_t i = 0; i <= t.n; i++) {
    if (i == t.n || t.p[i] == '.') {
      segs[k++] = t.substr(start, i - start);
      start = i + 1;
    }
  }
  nseg = n;
  return segs;
}

// A compile error: message built in the arena (full "template ...: ...").
struct TplErr {
  bool ok = true;
  Str msg;
};

// FOR_RE = /^for ([A-Za-z_][A-Za-z0-9_]*) in (\S+)$/
// IF_RE  = /^if (not )?(\S+)$/
// INCLUDE_RE = /^include "([a-z0-9-]+)"$/
// (The tag is already trimmed by the tokenizer.)
static bool parseForTag(Str tag, Str& name, Str& path) {
  // "for NAME in PATH"
  if (!tag.startsWith(S("for "))) return false;
  size_t i = 4;
  size_t ns = i;
  if (i >= tag.n) return false;
  char c = tag.p[i];
  if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) return false;
  i++;
  while (i < tag.n) {
    c = tag.p[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '_')
      i++;
    else break;
  }
  name = tag.substr(ns, i - ns);
  if (!tag.substr(i).startsWith(S(" in "))) return false;
  i += 4;
  if (i >= tag.n) return false;
  // PATH must be \S+ (no whitespace) to the end.
  for (size_t q = i; q < tag.n; q++) {
    char ch = tag.p[q];
    if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') return false;
  }
  path = tag.substr(i);
  return true;
}
static bool parseIfTag(Str tag, bool& neg, Str& path) {
  if (!tag.startsWith(S("if "))) return false;
  size_t i = 3;
  neg = false;
  if (tag.substr(i).startsWith(S("not "))) {
    neg = true;
    i += 4;
  }
  if (i >= tag.n) return false;
  for (size_t q = i; q < tag.n; q++) {
    char ch = tag.p[q];
    if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') return false;
  }
  path = tag.substr(i);
  return true;
}
static bool parseIncludeTag(Str tag, Str& name) {
  // include "NAME" where NAME is [a-z0-9-]+
  if (!tag.startsWith(S("include \""))) return false;
  size_t i = 9;
  size_t ns = i;
  while (i < tag.n) {
    char c = tag.p[i];
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') i++;
    else break;
  }
  if (i == ns) return false;
  if (i + 1 != tag.n || tag.p[i] != '"') return false;
  name = tag.substr(ns, i - ns);
  return true;
}

// Compile one template's source into an op-list. Returns false on a
// TemplateError (msg set). `nameToIdx` maps template names to indices
// for include resolution.
static bool compileTemplate(Arena* ar, Str name, Str src, Template& out,
                            const Str* names, const int32_t* nameIdx, uint32_t nnames,
                            TplErr& err) {
  auto fail = [&](Str detail) {
    err.ok = false;
    err.msg = ar->cat({S("template \""), name, S("\": "), detail});
    return false;
  };
  Op* ops = nullptr;
  uint32_t nops = 0, capOps = 0;
  auto emit = [&](Op op) -> uint32_t {
    if (nops == capOps) {
      capOps = capOps ? capOps * 2 : 16;
      Op* ni = ar->allocArr<Op>(capOps);
      if (ops) std::memcpy(ni, ops, sizeof(Op) * nops);
      ops = ni;
    }
    ops[nops] = op;
    return nops++;
  };
  // Compile stack: indices of open For/If ops.
  int32_t stack[64];
  int nstack = 0;
  // Tokenizer state.
  size_t textStart = 0;  // start of the pending text run
  size_t i = 0;
  auto flushText = [&](size_t end) {
    if (end > textStart) {
      Op op;
      op.kind = Op::Text;
      op.text = src.substr(textStart, end - textStart);
      emit(op);
    }
  };
  while (i < src.n) {
    // Find the next '{'.
    size_t open = i;
    while (open < src.n && src.p[open] != '{') open++;
    if (open >= src.n || open == src.n - 1) {
      // Rest is text.
      flushText(src.n);
      i = src.n;
      break;
    }
    char next = src.p[open + 1];
    if (next != '{' && next != '%') {
      // A lone '{': it belongs to the text; keep scanning past it.
      i = open + 1;
      continue;
    }
    if (next == '{') {
      bool raw = open + 2 < src.n && src.p[open + 2] == '{';
      // Find the close.
      const char* closeStr = raw ? "}}}" : "}}";
      size_t closeLen = raw ? 3 : 2;
      size_t from = open + (raw ? 3 : 2);
      size_t close = (size_t)-1;
      for (size_t q = from; q + closeLen <= src.n; q++) {
        if (std::memcmp(src.p + q, closeStr, closeLen) == 0) {
          close = q;
          break;
        }
      }
      if (close == (size_t)-1)
        return fail(raw ? S("a {{{ is not closed") : S("a {{ is not closed"));
      flushText(open);
      Str inner = jsTrim(src.substr(from, close - from));
      if (!isPathStr(inner)) {
        Buf jb;
        appendJsonString(jb, inner);
        return fail(ar->cat({ar->dup(jb.view()), S(" is not a path")}));
      }
      Op op;
      op.kind = raw ? Op::Raw : Op::Var;
      op.path = splitPath(ar, inner, op.npath);
      emit(op);
      i = close + closeLen;
      textStart = i;
      continue;
    }
    // {% ... %} — find '%}'.
    size_t from = open + 2;
    size_t close = (size_t)-1;
    for (size_t q = from; q + 1 < src.n; q++) {
      if (src.p[q] == '%' && src.p[q + 1] == '}') {
        close = q;
        break;
      }
    }
    if (close == (size_t)-1) return fail(S("a {% is not closed"));
    size_t end = close + 2;
    // Standalone check: the tag alone on its line (spaces/tabs only
    // before and after it on the line).
    size_t lineStart = open;
    while (lineStart > 0 && src.p[lineStart - 1] != '\n') lineStart--;
    size_t nl = end;
    while (nl < src.n && src.p[nl] != '\n') nl++;
    size_t lineEnd = nl;
    bool beforeOk = true;
    for (size_t q = lineStart; q < open; q++)
      if (src.p[q] != ' ' && src.p[q] != '\t') {
        beforeOk = false;
        break;
      }
    bool afterOk = true;
    for (size_t q = end; q < lineEnd; q++)
      if (src.p[q] != ' ' && src.p[q] != '\t') {
        afterOk = false;
        break;
      }
    bool standalone = beforeOk && afterOk;
    size_t textEnd = standalone ? lineStart : open;
    flushText(textEnd);
    Str tag = jsTrim(src.substr(open + 2, close - (open + 2)));
    // Dispatch the tag.
    Str fName, fPath;
    bool neg = false;
    Str incName;
    if (parseForTag(tag, fName, fPath)) {
      if (!isPathStr(fPath)) {
        Buf jb;
        appendJsonString(jb, fPath);
        return fail(ar->cat({ar->dup(jb.view()), S(" is not a path")}));
      }
      Op op;
      op.kind = Op::For;
      op.varName = fName;
      op.path = splitPath(ar, fPath, op.npath);
      uint32_t idx = emit(op);
      if (nstack >= 64) return fail(S("a for is not closed"));
      stack[nstack++] = (int32_t)idx;
    } else if (parseIfTag(tag, neg, fPath)) {
      if (!isPathStr(fPath)) {
        Buf jb;
        appendJsonString(jb, fPath);
        return fail(ar->cat({ar->dup(jb.view()), S(" is not a path")}));
      }
      Op op;
      op.kind = Op::If;
      op.neg = neg;
      op.path = splitPath(ar, fPath, op.npath);
      uint32_t idx = emit(op);
      if (nstack >= 64) return fail(S("a if is not closed"));
      stack[nstack++] = (int32_t)idx;
    } else if (strEq(tag, S("else"))) {
      if (nstack == 0 || ops[stack[nstack - 1]].kind != Op::If ||
          ops[stack[nstack - 1]].elsePc >= 0)
        return fail(S("an else outside an if"));
      uint32_t elseIdx = emit(Op{Op::Else});
      ops[stack[nstack - 1]].elsePc = (int32_t)elseIdx;
    } else if (strEq(tag, S("endfor")) || strEq(tag, S("endif"))) {
      bool wantFor = strEq(tag, S("endfor"));
      if (nstack == 0 ||
          ops[stack[nstack - 1]].kind != (wantFor ? Op::For : Op::If))
        return fail(wantFor ? S("an endfor with no open for")
                            : S("an endif with no open if"));
      int32_t top = stack[--nstack];
      uint32_t endIdx;
      if (wantFor) {
        endIdx = emit(Op{Op::EndFor});
        // Empty-array jump must skip PAST the EndFor op: landing ON it
        // would make EndFor pop a frame that belongs to an enclosing
        // loop (or frames[-1] when there is none) and corrupt the stack.
        ops[top].jump = (int32_t)endIdx + 1;
      } else {
        endIdx = emit(Op{Op::EndIf});
        int32_t elsePc = ops[top].elsePc;
        // False jump: the first op of the else branch (past the Else op
        // itself — jumping AT the Else op would make its handler skip
        // the branch entirely). No else: straight to EndIf.
        ops[top].jump = elsePc >= 0 ? elsePc + 1 : (int32_t)endIdx;
        if (elsePc >= 0) ops[elsePc].jump = (int32_t)endIdx;
      }
    } else if (parseIncludeTag(tag, incName)) {
      Op op;
      op.kind = Op::Include;
      op.tmpl = -1;
      // Resolve the target index (binary search over the sorted names).
      uint32_t lo = 0, hi = nnames;
      while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        int c = cmpStr(incName, names[mid]);
        if (c == 0) {
          op.tmpl = nameIdx[mid];
          break;
        }
        if (c < 0) hi = mid;
        else lo = mid + 1;
      }
      emit(op);
    } else {
      return fail(ar->cat({S("{% "), tag, S(" %} is not a tag")}));
    }
    i = standalone ? (nl >= src.n ? src.n : nl + 1) : end;
    textStart = i;
  }
  if (nstack > 0) {
    Op& top = ops[stack[nstack - 1]];
    return fail(top.kind == Op::For ? S("a for is not closed") : S("a if is not closed"));
  }
  out.name = name;
  out.ops = ops;
  out.nops = nops;
  return true;
}

// ---- scope frames + lookup ----

struct Frame {
  const KV* vars = nullptr;
  uint32_t nvars = 0;
  bool isFor = false;
  Str elemName;
  const Val* elem = nullptr;
  Val loopVal;        // Loop record for this frame
  const Val* arr = nullptr;  // the iterated array
  uint32_t idx = 0, len = 0;
  uint32_t bodyPc = 0, endPc = 0;
};

// Property access on a value (path segment k >= 1). Returns false when
// the property does not exist. Computed values (array length, loop
// fields) are written into `scratch`.
static bool propOf(const Val* v, Str key, Val* scratch, const Val** out) {
  if (v->type == Val::Obj) {
    const Val* f = v->find(key);
    if (!f) return false;
    *out = f;
    return true;
  }
  if (v->type == Val::Arr) {
    if (strEq(key, S("length"))) {
      *scratch = Val::integer(v->arr.n);
      *out = scratch;
      return true;
    }
    // Canonical array index: digits, no leading zero unless "0" itself.
    if (key.n > 0) {
      bool digits = true;
      for (uint32_t i = 0; i < key.n; i++)
        if (key.p[i] < '0' || key.p[i] > '9') {
          digits = false;
          break;
        }
      if (digits && !(key.n > 1 && key.p[0] == '0')) {
        uint64_t idx = 0;
        for (uint32_t i = 0; i < key.n; i++) idx = idx * 10 + (uint64_t)(key.p[i] - '0');
        if (idx < v->arr.n) {
          *out = &v->arr.items[idx];
          return true;
        }
      }
    }
    return false;
  }
  if (v->type == Val::Loop) {
    if (strEq(key, S("index"))) {
      *scratch = Val::integer((int64_t)v->loop.idx + 1);
      *out = scratch;
      return true;
    }
    if (strEq(key, S("first"))) {
      *scratch = Val::boolean(v->loop.idx == 0);
      *out = scratch;
      return true;
    }
    if (strEq(key, S("last"))) {
      *scratch = Val::boolean(v->loop.idx + 1 == v->loop.len);
      *out = scratch;
      return true;
    }
    if (strEq(key, S("length"))) {
      *scratch = Val::integer(v->loop.len);
      *out = scratch;
      return true;
    }
    return false;
  }
  return false;
}

// Resolve a path in the scope chain (innermost first). Returns nullptr
// for undefined. `scratch` holds computed property values.
static const Val* resolvePath(Frame* frames, uint32_t nframes, Str* path, uint8_t npath,
                              Val* scratch) {
  if (npath == 0) return nullptr;
  const Val* v = nullptr;
  // First segment: scope lookup.
  for (int32_t f = (int32_t)nframes - 1; f >= 0; f--) {
    Frame& fr = frames[f];
    if (fr.isFor) {
      if (strEq(path[0], S("loop"))) {
        v = &fr.loopVal;
        break;
      }
      if (strEq(path[0], fr.elemName)) {
        v = fr.elem;
        break;
      }
      continue;
    }
    // Root frame: last match wins (JS object spread semantics).
    for (int32_t q = (int32_t)fr.nvars - 1; q >= 0; q--)
      if (strEq(fr.vars[q].key, path[0])) {
        v = &fr.vars[q].val;
        break;
      }
    if (v) break;
  }
  if (!v) return nullptr;
  for (uint8_t k = 1; k < npath; k++) {
    const Val* next = nullptr;
    if (!propOf(v, path[k], scratch, &next)) return nullptr;
    v = next;
  }
  return v;
}

// textOf with HTML escaping (for {{ }}).
static void appendTextOfEscaped(Buf& out, const Val* v) {
  if (!v) return;
  switch (v->type) {
    case Val::Str:
      appendEscaped(out, v->s);
      break;
    case Val::Int:
      out.appendInt(v->i);
      break;
    case Val::Dbl:
      appendJsNum(out, v->d);
      break;
    case Val::Bool:
      out.append(v->b ? "true" : "false");
      break;
    case Val::Arr:
      for (uint32_t k = 0; k < v->arr.n; k++) {
        if (k) out.append(", ");
        appendTextOfEscaped(out, &v->arr.items[k]);
      }
      break;
    default:
      break;  // Null, Obj, Loop -> ""
  }
}

// Execute template `startIdx` into `out`. `frames` holds the root frame
// at index 0 (nframes = 1 on entry); loop frames are pushed/popped here.
static void execTemplate(Template* tpls, int32_t startIdx, Frame* frames, uint32_t& nframes,
                         Buf& out) {
  struct Call {
    int32_t tmpl;
    uint32_t pc;
  };
  Call calls[9];
  int ncalls = 0;
  calls[ncalls++] = {startIdx, 0};
  Val scratch;
  while (ncalls > 0) {
    Call& c = calls[ncalls - 1];
    Template& t = tpls[c.tmpl];
    if (c.pc >= t.nops) {
      ncalls--;
      continue;
    }
    Op& op = t.ops[c.pc];
    switch (op.kind) {
      case Op::Text:
        out.append(op.text);
        c.pc++;
        break;
      case Op::Var: {
        const Val* v = resolvePath(frames, nframes, op.path, op.npath, &scratch);
        appendTextOfEscaped(out, v);
        c.pc++;
        break;
      }
      case Op::Raw: {
        const Val* v = resolvePath(frames, nframes, op.path, op.npath, &scratch);
        appendTextOf(out, v);
        c.pc++;
        break;
      }
      case Op::If: {
        const Val* v = resolvePath(frames, nframes, op.path, op.npath, &scratch);
        bool tr = truthy(v);
        if (op.neg) tr = !tr;
        c.pc = tr ? c.pc + 1 : (uint32_t)op.jump;
        break;
      }
      case Op::Else:
        c.pc = (uint32_t)op.jump;
        break;
      case Op::EndIf:
        c.pc++;
        break;
      case Op::For: {
        const Val* v = resolvePath(frames, nframes, op.path, op.npath, &scratch);
        if (v && v->type == Val::Arr && v->arr.n > 0) {
          Frame& f = frames[nframes++];
          f = Frame();
          f.isFor = true;
          f.elemName = op.varName;
          f.elem = &v->arr.items[0];
          f.arr = v;
          f.idx = 0;
          f.len = v->arr.n;
          f.bodyPc = c.pc + 1;
          f.endPc = (uint32_t)op.jump;
          f.loopVal = Val::loopRec(0, v->arr.n);
          c.pc++;
        } else {
          c.pc = (uint32_t)op.jump;
        }
        break;
      }
      case Op::EndFor: {
        // Defensive: EndFor is only ever reached by falling through from
        // a live loop body. If the stack has no for frame (should not
        // happen), skip instead of reading frames[-1].
        if (nframes == 0 || !frames[nframes - 1].isFor) {
          c.pc++;
          break;
        }
        Frame& f = frames[nframes - 1];
        f.idx++;
        if (f.idx < f.len) {
          f.elem = &f.arr->arr.items[f.idx];
          f.loopVal = Val::loopRec(f.idx, f.len);
          c.pc = f.bodyPc;
        } else {
          nframes--;
          c.pc++;
        }
        break;
      }
      case Op::Include: {
        // Depth D is ncalls-1 (top render is D=0); include only if D<8.
        if (ncalls - 1 < 8 && op.tmpl >= 0) {
          c.pc++;
          calls[ncalls++] = {op.tmpl, 0};
        } else {
          c.pc++;
        }
        break;
      }
    }
  }
}

// ---------------------------------------------------------------- the build

struct Source {
  Str path;
  Str text;  // CR-normalized content
  Val data;  // front-matter Obj
  Str body;
  bool post = false;
  Str layout;
  Str slug, outDir, url;
  Str html, excerpt;
  Heading* toc = nullptr;
  uint32_t ntoc = 0;
  Val* tagLinks = nullptr;  // card's tag link objects
  uint32_t ntagLinks = 0;
  int32_t newer = -1, older = -1;  // indices into the posts array
  Val card;                        // cardOf(s), computed once
};
struct TagObj {
  Str name, slug, url;
  uint32_t* posts = nullptr;  // source indices
  uint32_t nposts = 0, capPosts = 0;
  Val link;       // memoized {name, slug, url}
  bool hasLink = false;
};

static const char* RESERVED_DIRS[3] = {"posts", "tags", "page"};

// JS Array.slice(start, end) range for an array of length n.
static void jsSliceRange(int64_t n, double start, bool hasEnd, double end, int64_t& from,
                         int64_t& to) {
  auto toIdx = [&](double v) -> int64_t {
    if (std::isnan(v)) return 0;
    double t = std::trunc(v);
    if (t < 0) {
      int64_t r = (int64_t)(t + (double)n);
      return r < 0 ? 0 : r;
    }
    int64_t r = (int64_t)t;
    return r > n ? n : r;
  };
  from = toIdx(start);
  to = hasEnd ? toIdx(end) : n;
  if (to < from) to = from;
}

// The words of a body: runs of characters other than JS \s whitespace.
static int64_t wordsOf(Str text) {
  int64_t n = 0;
  bool inWord = false;
  const uint8_t* p = (const uint8_t*)text.p;
  const uint8_t* e = p + text.n;
  while (p < e) {
    uint32_t cp;
    if (*p < 0x80) {
      cp = *p++;
    } else {
      cp = utf8Decode(p, e);
    }
    if (isWSCodePoint(cp)) inWord = false;
    else if (!inWord) {
      inWord = true;
      n++;
    }
  }
  return n;
}

// One case's output: an ordered list of (path, text) pairs.
struct CaseOut {
  struct Entry {
    Str path;
    Str text;
  };
  Entry* items = nullptr;
  uint32_t n = 0, cap = 0;
  void add(Arena* ar, Str path, Str text) {
    if (n == cap) {
      cap = cap ? cap * 2 : 64;
      Entry* ni = ar->allocArr<Entry>(cap);
      if (items) std::memcpy(ni, items, sizeof(Entry) * n);
      items = ni;
    }
    items[n].path = path;
    items[n].text = text;
    n++;
  }
  bool has(Str path) const {
    for (uint32_t i = 0; i < n; i++)
      if (strEq(items[i].path, path)) return true;
    return false;
  }
};

// Run one case. Returns false on a structural problem (never for valid
// input). The output is appended to `out`.
static bool runCase(Arena* ar, const Val& input, CaseOut& out) {
  const Val* siteV = input.find("site");
  const Val* templatesV = input.find("templates");
  const Val* filesV = input.find("files");
  if (!siteV || siteV->type != Val::Obj || !templatesV || templatesV->type != Val::Obj ||
      !filesV || filesV->type != Val::Arr)
    return false;

  // ---- templates: sorted by name, compiled; failures are reported.
  uint32_t ntplNames = templatesV->obj.n;
  Str* tplNames = ar->allocArr<Str>(ntplNames ? ntplNames : 1);
  for (uint32_t i = 0; i < ntplNames; i++) tplNames[i] = templatesV->obj.items[i].key;
  std::sort(tplNames, tplNames + ntplNames,
            [](Str a, Str b) { return cmpStr(a, b) < 0; });
  Template* tpls = ar->allocArr<Template>(ntplNames ? ntplNames : 1);
  bool* tplOk = ar->allocArr<bool>(ntplNames ? ntplNames : 1);
  // name -> index map (sorted names, identity here since tpls is in
  // sorted order).
  auto findTpl = [&](Str name) -> int32_t {
    uint32_t lo = 0, hi = ntplNames;
    while (lo < hi) {
      uint32_t mid = (lo + hi) / 2;
      int c = cmpStr(name, tplNames[mid]);
      if (c == 0) return (int32_t)mid;
      if (c < 0) hi = mid;
      else lo = mid + 1;
    }
    return -1;
  };
  int32_t* nameIdx = ar->allocArr<int32_t>(ntplNames ? ntplNames : 1);
  for (uint32_t i = 0; i < ntplNames; i++) nameIdx[i] = (int32_t)i;

  // Report lines (arena strings).
  Str* report = nullptr;
  uint32_t nreport = 0, capReport = 0;
  auto pushReport = [&](Str line) {
    if (nreport == capReport) {
      capReport = capReport ? capReport * 2 : 16;
      Str* ni = ar->allocArr<Str>(capReport);
      if (report) std::memcpy(ni, report, sizeof(Str) * nreport);
      report = ni;
    }
    report[nreport++] = line;
  };

  for (uint32_t i = 0; i < ntplNames; i++) {
    const Val* srcV = templatesV->find(tplNames[i]);
    Str src = (srcV && srcV->type == Val::Str) ? srcV->s : Str();
    TplErr err;
    if (compileTemplate(ar, tplNames[i], src, tpls[i], tplNames, nameIdx, ntplNames, err)) {
      tplOk[i] = true;
    } else {
      tplOk[i] = false;
      pushReport(err.msg);
    }
  }
  // Includes pointing at a template that failed to parse render nothing.
  for (uint32_t i = 0; i < ntplNames; i++) {
    if (!tplOk[i]) continue;
    for (uint32_t q = 0; q < tpls[i].nops; q++) {
      Op& op = tpls[i].ops[q];
      if (op.kind == Op::Include && op.tmpl >= 0 && !tplOk[op.tmpl]) op.tmpl = -1;
    }
  }

  const Val* baseV = siteV->find("base_url");
  Str base = (baseV && baseV->type == Val::Str) ? baseV->s : Str();
  while (base.n > 0 && base.p[base.n - 1] == '/') base = base.substr(0, base.n - 1);
  const Val* siteTitleV = siteV->find("title");
  Str siteTitle = (siteTitleV && siteTitleV->type == Val::Str) ? siteTitleV->s : Str();
  const Val* perPageV = siteV->find("per_page");
  const Val* recentV = siteV->find("recent");

  // ---- 1. Sources, in code-unit order of their paths.
  uint32_t nfiles = filesV->arr.n;
  // Index-sort the files by path (the input order is already sorted in
  // practice, but the reference sorts explicitly).
  uint32_t* fidx = ar->allocArr<uint32_t>(nfiles ? nfiles : 1);
  for (uint32_t i = 0; i < nfiles; i++) fidx[i] = i;
  std::sort(fidx, fidx + nfiles, [&](uint32_t a, uint32_t b) {
    const Val* fa = &filesV->arr.items[a];
    const Val* fb = &filesV->arr.items[b];
    Str pa = fa->find("path") && fa->find("path")->type == Val::Str ? fa->find("path")->s : Str();
    Str pb = fb->find("path") && fb->find("path")->type == Val::Str ? fb->find("path")->s : Str();
    return cmpStr(pa, pb) < 0;
  });
  Source* sources = ar->allocArr<Source>(nfiles ? nfiles : 1);
  uint32_t nsources = 0;
  Str* staticPaths = ar->allocArr<Str>(nfiles ? nfiles : 1);
  Str* staticContents = ar->allocArr<Str>(nfiles ? nfiles : 1);
  uint32_t nstatics = 0;
  for (uint32_t fi = 0; fi < nfiles; fi++) {
    const Val* f = &filesV->arr.items[fidx[fi]];
    const Val* pathV = f->find("path");
    const Val* contentV = f->find("content");
    if (!pathV || pathV->type != Val::Str || !contentV || contentV->type != Val::Str)
      continue;
    Str path = pathV->s;
    Str content = contentV->s;
    if (path.endsWith(S(".md"))) {
      // CR-normalize: \r\n? -> \n (only when a '\r' is present).
      bool hasCR = false;
      for (uint32_t q = 0; q < content.n; q++)
        if (content.p[q] == '\r') {
          hasCR = true;
          break;
        }
      Str text = content;
      if (hasCR) {
        Buf nb;
        for (uint32_t q = 0; q < content.n; q++) {
          if (content.p[q] == '\r') {
            nb.push('\n');
            if (q + 1 < content.n && content.p[q + 1] == '\n') q++;
          } else {
            nb.push(content.p[q]);
          }
        }
        text = ar->dup(nb.view());
      }
      Source& s = sources[nsources++];
      s = Source();
      s.path = path;
      s.text = text;
    } else {
      staticPaths[nstatics] = path;
      staticContents[nstatics] = content;
      nstatics++;
    }
  }

  // ---- 2. Front matter + refusal checks.
  Source* kept = ar->allocArr<Source>(nsources ? nsources : 1);
  uint32_t nkept = 0;
  for (uint32_t i = 0; i < nsources; i++) {
    Source& s = sources[i];
    FmResult fm = parseFrontMatter(ar, s.text);
    if (fm.err) {
      pushReport(ar->cat({S("refused "), s.path, S(": "), fm.errMsg}));
      continue;
    }
    s.data = fm.data;
    s.body = fm.body;
    s.post = s.path.startsWith(S("posts/"));
    Str own;
    bool hasOwn = false;
    if (s.path.startsWith(S("tags/"))) {
      own = S("tags");
      hasOwn = true;
    } else if (s.path.startsWith(S("page/"))) {
      own = S("page");
      hasOwn = true;
    }
    if (hasOwn) {
      pushReport(ar->cat({S("refused "), s.path, S(": "), own, S("/ is the generator's own")}));
      continue;
    }
    if (s.post && !s.data.find("date")) {
      pushReport(ar->cat({S("refused "), s.path, S(": a post needs a date")}));
      continue;
    }
    const Val* layoutV = s.data.find("layout");
    Str layout = layoutV && layoutV->type == Val::Str
                     ? layoutV->s
                     : (s.post ? S("post") : S("page"));
    int32_t li = findTpl(layout);
    if (li < 0 || !tplOk[li]) {
      pushReport(ar->cat({S("refused "), s.path, S(": layout \""), layout,
                          S("\" is no template that parses")}));
      continue;
    }
    s.layout = layout;
    const Val* draftV = s.data.find("draft");
    if (draftV && draftV->type == Val::Bool && draftV->b) {
      pushReport(ar->cat({S("draft "), s.path}));
      continue;
    }
    kept[nkept++] = s;
  }
  sources = kept;
  nsources = nkept;

  // ---- 3. Permalinks.
  // taken sets per directory (root starts with RESERVED).
  struct TakenSet {
    Str dir;
    Str* slugs = nullptr;
    uint32_t n = 0, cap = 0;
  };
  TakenSet* taken = ar->allocArr<TakenSet>(nsources ? nsources : 1);
  uint32_t ntaken = 0;
  auto takenIn = [&](Str dir) -> TakenSet* {
    for (uint32_t q = 0; q < ntaken; q++)
      if (strEq(taken[q].dir, dir)) return &taken[q];
    TakenSet* ts = &taken[ntaken++];
    ts->dir = dir;
    ts->slugs = nullptr;
    ts->n = 0;
    ts->cap = 0;
    if (dir.n == 0) {
      for (auto* r : RESERVED_DIRS) {
        if (ts->n == ts->cap) {
          ts->cap = ts->cap ? ts->cap * 2 : 4;
          Str* ni = ar->allocArr<Str>(ts->cap);
          if (ts->slugs) std::memcpy(ni, ts->slugs, sizeof(Str) * ts->n);
          ts->slugs = ni;
        }
        ts->slugs[ts->n++] = S(r);
      }
    }
    return ts;
  };
  auto uniqueSlug = [&](Str baseSlug, TakenSet* ts) -> Str {
    auto has = [&](Str x) {
      for (uint32_t q = 0; q < ts->n; q++)
        if (strEq(ts->slugs[q], x)) return true;
      return false;
    };
    Str out = baseSlug;
    if (!has(out)) {
      if (ts->n == ts->cap) {
        ts->cap = ts->cap ? ts->cap * 2 : 4;
        Str* ni = ar->allocArr<Str>(ts->cap);
        if (ts->slugs) std::memcpy(ni, ts->slugs, sizeof(Str) * ts->n);
        ts->slugs = ni;
      }
      ts->slugs[ts->n++] = out;
      return out;
    }
    for (int nn = 2;; nn++) {
      char nb[16];
      int nbl = std::snprintf(nb, sizeof nb, "-%d", nn);
      out = ar->cat({baseSlug, Str(nb, (uint32_t)nbl)});
      if (!has(out)) {
        if (ts->n == ts->cap) {
          ts->cap = ts->cap ? ts->cap * 2 : 4;
          Str* ni = ar->allocArr<Str>(ts->cap);
          if (ts->slugs) std::memcpy(ni, ts->slugs, sizeof(Str) * ts->n);
          ts->slugs = ni;
        }
        ts->slugs[ts->n++] = out;
        return out;
      }
    }
  };
  auto dirOf = [](Str path) -> Str {
    size_t slash = (size_t)-1;
    for (uint32_t q = 0; q < path.n; q++)
      if (path.p[q] == '/') slash = q;
    return slash == (size_t)-1 ? Str() : path.substr(0, slash);
  };
  for (uint32_t i = 0; i < nsources; i++) {
    Source& s = sources[i];
    Str dir = s.post ? S("posts") : dirOf(s.path);
    const Val* slugV = s.data.find("slug");
    const Val* titleV = s.data.find("title");
    Str baseSlug = (slugV && slugV->type == Val::Str)
                       ? slugV->s
                       : slugOf(ar, titleV && titleV->type == Val::Str ? titleV->s : Str());
    s.slug = uniqueSlug(baseSlug, takenIn(dir));
    s.outDir = dir.n == 0 ? s.slug : ar->cat({dir, S("/"), s.slug});
    s.url = ar->cat({base, S("/"), s.outDir, S("/")});
  }

  // ---- 4. Markdown (links resolved against the source paths + statics).
  InlineCtx::PageLink* pageLinks = ar->allocArr<InlineCtx::PageLink>(nsources ? nsources : 1);
  for (uint32_t i = 0; i < nsources; i++) {
    pageLinks[i].path = sources[i].path;
    pageLinks[i].url = sources[i].url;
  }
  // pageLinks is already sorted by path (sources are in path order).
  // staticPaths is sorted too (statics were collected in path order).
  Str* allBroken = nullptr;
  uint32_t nBroken = 0, capBroken = 0;
  auto pushBroken = [&](Str line) {
    if (nBroken == capBroken) {
      capBroken = capBroken ? capBroken * 2 : 16;
      Str* ni = ar->allocArr<Str>(capBroken);
      if (allBroken) std::memcpy(ni, allBroken, sizeof(Str) * nBroken);
      allBroken = ni;
    }
    allBroken[nBroken++] = line;
  };
  for (uint32_t i = 0; i < nsources; i++) {
    Source& s = sources[i];
    Str dir = dirOf(s.path);
    // dirParts: split dir on '/'.
    Str dirPartsArr[32];
    uint32_t nDirParts = 0;
    if (dir.n > 0) {
      size_t sp = 0;
      while (sp <= dir.n) {
        size_t slash = sp;
        while (slash < dir.n && dir.p[slash] != '/') slash++;
        if (nDirParts < 32) dirPartsArr[nDirParts++] = dir.substr(sp, slash - sp);
        if (slash >= dir.n) break;
        sp = slash + 1;
      }
    }
    InlineCtx ctx;
    ctx.dirParts = dirPartsArr;
    ctx.nDirParts = nDirParts;
    ctx.base = base;
    ctx.sourcePath = s.path;
    ctx.pages = pageLinks;
    ctx.nPages = nsources;
    ctx.staticPaths = staticPaths;
    ctx.nStatics = nstatics;
    ctx.ar = ar;
    MdOut md = renderMarkdown(ar, s.body, ctx);
    s.html = md.html;
    s.toc = md.toc;
    s.ntoc = md.ntoc;
    s.excerpt = md.excerpt;
    for (uint32_t q = 0; q < ctx.nbroken; q++) pushBroken(ctx.broken[q]);
  }

  // ---- 5. Collections: posts newest first, then by slug.
  uint32_t* postIdx = ar->allocArr<uint32_t>(nsources ? nsources : 1);
  uint32_t nposts = 0;
  for (uint32_t i = 0; i < nsources; i++)
    if (sources[i].post) postIdx[nposts++] = i;
  std::sort(postIdx, postIdx + nposts, [&](uint32_t a, uint32_t b) {
    const Val* da = sources[a].data.find("date");
    const Val* db = sources[b].data.find("date");
    Str sa = da && da->type == Val::Str ? da->s : Str();
    Str sb = db && db->type == Val::Str ? db->s : Str();
    int c = cmpStr(sb, sa);  // date DESC
    if (c) return c < 0;
    return cmpStr(sources[a].slug, sources[b].slug) < 0;
  });
  for (uint32_t k = 0; k < nposts; k++) {
    uint32_t si = postIdx[k];
    sources[si].newer = k == 0 ? -1 : (int32_t)postIdx[k - 1];
    sources[si].older = k + 1 == nposts ? -1 : (int32_t)postIdx[k + 1];
  }

  // ---- 6. Tags, grouped by slug, named as first met in the posts' order.
  // Each source keeps its card's tag links directly (a TagObj's memoized
  // link, or a fresh {name, slug, url:''} for a synthetic tag).
  // NOTE: capacity is NOT nsources — one source can hold many distinct
  // tags, so the array grows on demand (writing past a fixed nsources
  // capacity corrupts the arena and the tag links).
  uint32_t capTags = 16;
  TagObj* tags = ar->allocArr<TagObj>(capTags);
  uint32_t ntags = 0;
  auto findTagBySlug = [&](Str slug) -> int32_t {
    for (uint32_t q = 0; q < ntags; q++)
      if (strEq(tags[q].slug, slug)) return (int32_t)q;
    return -1;
  };
  auto tagLinkVal = [&](Arena* a, Str name, Str slug, Str url) -> Val {
    KV* kv = a->allocArr<KV>(3);
    kv[0] = KV{S("name"), Val::str(name)};
    kv[1] = KV{S("slug"), Val::str(slug)};
    kv[2] = KV{S("url"), Val::str(url)};
    return Val::object(kv, 3);
  };
  // ownTags(s): the source's tags, deduplicated by slug (first name kept).
  auto ownTags = [&](const Source& s, std::pair<Str, Str>* out) -> uint32_t {
    const Val* tagsV = s.data.find("tags");
    uint32_t n = 0;
    if (!tagsV || tagsV->type != Val::Arr) return 0;
    for (uint32_t q = 0; q < tagsV->arr.n; q++) {
      const Val* tv = &tagsV->arr.items[q];
      if (tv->type != Val::Str) continue;
      Str slug = slugOf(ar, tv->s);
      bool seen = false;
      for (uint32_t r = 0; r < n; r++)
        if (strEq(out[r].second, slug)) {
          seen = true;
          break;
        }
      if (seen) continue;
      out[n++] = {tv->s, slug};
    }
    return n;
  };
  for (uint32_t k = 0; k < nposts; k++) {
    Source& p = sources[postIdx[k]];
    std::pair<Str, Str> own[64];
    uint32_t nown = ownTags(p, own);
    p.tagLinks = ar->allocArr<Val>(nown ? nown : 1);
    p.ntagLinks = 0;
    for (uint32_t q = 0; q < nown; q++) {
      Str tname = own[q].first, tslug = own[q].second;
      int32_t ti = findTagBySlug(tslug);
      if (ti < 0) {
        if (ntags == capTags) {
          capTags *= 2;
          TagObj* nt = ar->allocArr<TagObj>(capTags);
          if (tags) std::memcpy(nt, tags, sizeof(TagObj) * ntags);
          tags = nt;
        }
        ti = (int32_t)ntags++;
        tags[ti] = TagObj();
        tags[ti].name = tname;
        tags[ti].slug = tslug;
        tags[ti].url = ar->cat({base, S("/tags/"), tslug, S("/")});
      }
      TagObj& t = tags[ti];
      if (t.nposts == t.capPosts) {
        t.capPosts = t.capPosts ? t.capPosts * 2 : 4;
        uint32_t* ni = ar->allocArr<uint32_t>(t.capPosts);
        if (t.posts) std::memcpy(ni, t.posts, sizeof(uint32_t) * t.nposts);
        t.posts = ni;
      }
      t.posts[t.nposts++] = postIdx[k];
      if (!t.hasLink) {
        t.link = tagLinkVal(ar, t.name, t.slug, t.url);
        t.hasLink = true;
      }
      p.tagLinks[p.ntagLinks++] = t.link;
    }
  }
  for (uint32_t i = 0; i < nsources; i++) {
    Source& s = sources[i];
    if (s.post) continue;
    std::pair<Str, Str> own[64];
    uint32_t nown = ownTags(s, own);
    s.tagLinks = ar->allocArr<Val>(nown ? nown : 1);
    s.ntagLinks = 0;
    for (uint32_t q = 0; q < nown; q++) {
      Str tname = own[q].first, tslug = own[q].second;
      int32_t ti = findTagBySlug(tslug);
      if (ti >= 0) {
        TagObj& t = tags[ti];
        if (!t.hasLink) {
          t.link = tagLinkVal(ar, t.name, t.slug, t.url);
          t.hasLink = true;
        }
        s.tagLinks[s.ntagLinks++] = t.link;
      } else {
        // Synthetic tag (not in bySlug): url '', its own link.
        s.tagLinks[s.ntagLinks++] = tagLinkVal(ar, tname, tslug, Str());
      }
    }
  }
  // The tags list: only bySlug tags, sorted by count desc then slug asc.
  std::sort(tags, tags + ntags, [](const TagObj& a, const TagObj& b) {
    if (a.nposts != b.nposts) return a.nposts > b.nposts;
    return cmpStr(a.slug, b.slug) < 0;
  });

  // ---- 7. Layouts.
  // cardOf(s): one card per source, computed once.
  auto linkOf = [&](int32_t srcIdx) -> Val {
    if (srcIdx < 0) return Val::nullv();
    const Source& s = sources[srcIdx];
    const Val* dateV = s.data.find("date");
    Str date = dateV && dateV->type == Val::Str ? dateV->s : Str();
    KV* kv = ar->allocArr<KV>(4);
    const Val* titleV = s.data.find("title");
    kv[0] = KV{S("title"), Val::str(titleV && titleV->type == Val::Str ? titleV->s : Str())};
    kv[1] = KV{S("url"), Val::str(s.url)};
    kv[2] = KV{S("date"), Val::str(date)};
    kv[3] = KV{S("date_long"), Val::str(dateV ? longDate(ar, date) : Str())};
    return Val::object(kv, 4);
  };
  for (uint32_t i = 0; i < nsources; i++) {
    Source& s = sources[i];
    const Val* dateV = s.data.find("date");
    const Val* titleV = s.data.find("title");
    const Val* summaryV = s.data.find("summary");
    Str date = dateV && dateV->type == Val::Str ? dateV->s : Str();
    int64_t minutes = wordsOf(s.body);
    minutes = (minutes + 199) / 200;
    if (minutes < 1) minutes = 1;
    KV* kv = ar->allocArr<KV>(8);
    kv[0] = KV{S("title"), Val::str(titleV && titleV->type == Val::Str ? titleV->s : Str())};
    kv[1] = KV{S("url"), Val::str(s.url)};
    kv[2] = KV{S("date"), Val::str(date)};
    kv[3] = KV{S("date_long"), Val::str(dateV ? longDate(ar, date) : Str())};
    kv[4] = KV{S("summary"),
               Val::str(summaryV && summaryV->type == Val::Str ? summaryV->s : Str())};
    kv[5] = KV{S("excerpt"), Val::str(s.excerpt)};
    kv[6] = KV{S("reading_minutes"), Val::integer(minutes)};
    Val* tagArr = ar->allocArr<Val>(s.ntagLinks ? s.ntagLinks : 1);
    for (uint32_t q = 0; q < s.ntagLinks; q++) tagArr[q] = s.tagLinks[q];
    kv[7] = KV{S("tags"), Val::array(tagArr, s.ntagLinks)};
    s.card = Val::object(kv, 8);
  }
  // site / allTags / recent (JS slice semantics).
  KV* siteKv = ar->allocArr<KV>(2);
  siteKv[0] = KV{S("title"), Val::str(siteTitle)};
  siteKv[1] = KV{S("base_url"), Val::str(base)};
  Val siteObj = Val::object(siteKv, 2);
  Val* allTagsArr = ar->allocArr<Val>(ntags ? ntags : 1);
  for (uint32_t q = 0; q < ntags; q++) {
    KV* kv = ar->allocArr<KV>(4);
    kv[0] = KV{S("name"), Val::str(tags[q].name)};
    kv[1] = KV{S("slug"), Val::str(tags[q].slug)};
    kv[2] = KV{S("url"), Val::str(tags[q].url)};
    kv[3] = KV{S("count"), Val::integer(tags[q].nposts)};
    allTagsArr[q] = Val::object(kv, 4);
  }
  Val allTags = Val::array(allTagsArr, ntags);
  int64_t rFrom = 0, rTo = 0;
  {
    double recentNum = recentV && recentV->type == Val::Int
                           ? (double)recentV->i
                           : (recentV && recentV->type == Val::Dbl ? recentV->d
                                                                    : std::nan(""));
    bool hasRecent = recentV && recentV->isNum();
    jsSliceRange((int64_t)nposts, 0, hasRecent, recentNum, rFrom, rTo);
  }
  Val* recentArr = ar->allocArr<Val>((rTo - rFrom) > 0 ? (uint32_t)(rTo - rFrom) : 1);
  for (int64_t q = rFrom; q < rTo; q++) recentArr[q - rFrom] = sources[postIdx[q]].card;
  Val recent = Val::array(recentArr, (uint32_t)(rTo - rFrom));

  // Render helper: find a template by name; false when it does not exist
  // or did not parse.
  Frame frames[64];
  Buf renderOut;
  auto render = [&](Str name, KV* extraVars, uint32_t nExtra, Str& htmlOut) -> bool {
    int32_t ti = findTpl(name);
    if (ti < 0 || !tplOk[ti]) return false;
    KV* vars = ar->allocArr<KV>(3 + nExtra);
    vars[0] = KV{S("site"), siteObj};
    vars[1] = KV{S("tags"), allTags};
    vars[2] = KV{S("recent"), recent};
    for (uint32_t q = 0; q < nExtra; q++) vars[3 + q] = extraVars[q];
    uint32_t nframes = 0;
    frames[nframes] = Frame();
    frames[nframes].vars = vars;
    frames[nframes].nvars = 3 + nExtra;
    nframes++;
    renderOut.reset();
    execTemplate(tpls, ti, frames, nframes, renderOut);
    htmlOut = ar->dup(renderOut.view());
    return true;
  };

  // Source pages (in source order).
  for (uint32_t i = 0; i < nsources; i++) {
    Source& s = sources[i];
    // pageOf(s): the card + slug/content/toc/newer/older/is_post.
    KV* pageKv = ar->allocArr<KV>(14);
    for (uint32_t q = 0; q < 8; q++) pageKv[q] = s.card.obj.items[q];
    pageKv[8] = KV{S("slug"), Val::str(s.slug)};
    pageKv[9] = KV{S("content"), Val::str(s.html)};
    Val* tocArr = ar->allocArr<Val>(s.ntoc ? s.ntoc : 1);
    for (uint32_t q = 0; q < s.ntoc; q++) {
      KV* hkv = ar->allocArr<KV>(3);
      hkv[0] = KV{S("level"), Val::integer(s.toc[q].level)};
      hkv[1] = KV{S("id"), Val::str(s.toc[q].id)};
      hkv[2] = KV{S("html"), Val::str(s.toc[q].html)};
      tocArr[q] = Val::object(hkv, 3);
    }
    pageKv[10] = KV{S("toc"), Val::array(tocArr, s.ntoc)};
    pageKv[11] = KV{S("newer"), linkOf(s.newer)};
    pageKv[12] = KV{S("older"), linkOf(s.older)};
    pageKv[13] = KV{S("is_post"), Val::boolean(s.post)};
    Val page = Val::object(pageKv, 14);
    KV extra[1] = {KV{S("page"), page}};
    Str html;
    if (render(s.layout, extra, 1, html)) {
      Str outPath = ar->cat({s.outDir, S("/index.html")});
      out.add(ar, outPath, html);
    }
  }
  // Index pages.
  double perNum = perPageV && perPageV->type == Val::Int
                      ? (double)perPageV->i
                      : (perPageV && perPageV->type == Val::Dbl ? perPageV->d : std::nan(""));
  bool hasPer = perPageV && perPageV->isNum();
  int64_t pages = 0;
  if (hasPer && !std::isnan(perNum) && perNum >= 1) {
    double p = std::ceil((double)nposts / perNum);
    pages = (int64_t)(p < 1 ? 1 : p);
  } else if (hasPer && !std::isnan(perNum) && perNum < 1) {
    pages = 1;  // JS: Math.max(1, ceil(n/per)) with per<0 is 1; per==0
                // would loop forever in the reference — never in the data.
  } else {
    pages = 0;  // per missing/NaN: Math.max(1, NaN) is NaN — no pages.
  }
  auto indexUrl = [&](int64_t k) -> Str {
    if (k == 1) return ar->cat({base, S("/")});
    char nb[24];
    int nn = std::snprintf(nb, sizeof nb, "%lld", (long long)k);
    return ar->cat({base, S("/page/"), Str(nb, (uint32_t)nn), S("/")});
  };
  for (int64_t k = 1; k <= pages; k++) {
    int64_t from = 0, to = 0;
    jsSliceRange((int64_t)nposts, (double)(k - 1) * perNum, true, (double)k * perNum, from, to);
    Val* postsArr = ar->allocArr<Val>((to - from) > 0 ? (uint32_t)(to - from) : 1);
    for (int64_t q = from; q < to; q++) postsArr[q - from] = sources[postIdx[q]].card;
    Val postsVal = Val::array(postsArr, (uint32_t)(to - from));
    KV* pagerKv = ar->allocArr<KV>(5);
    pagerKv[0] = KV{S("number"), Val::integer(k)};
    pagerKv[1] = KV{S("total"), Val::integer(pages)};
    pagerKv[2] = KV{S("url"), Val::str(indexUrl(k))};
    pagerKv[3] = KV{S("newer_url"), Val::str(k == 1 ? Str() : indexUrl(k - 1))};
    pagerKv[4] = KV{S("older_url"), Val::str(k == pages ? Str() : indexUrl(k + 1))};
    Val pager = Val::object(pagerKv, 5);
    KV extra[2] = {KV{S("posts"), postsVal}, KV{S("pager"), pager}};
    Str html;
    if (render(S("index"), extra, 2, html)) {
      if (k == 1) {
        out.add(ar, S("index.html"), html);
      } else {
        char nb[24];
        int nn = std::snprintf(nb, sizeof nb, "%lld", (long long)k);
        out.add(ar, ar->cat({S("page/"), Str(nb, (uint32_t)nn), S("/index.html")}), html);
      }
    }
  }
  // Tag pages (in tags order).
  for (uint32_t q = 0; q < ntags; q++) {
    TagObj& t = tags[q];
    Val* postsArr = ar->allocArr<Val>(t.nposts ? t.nposts : 1);
    for (uint32_t r = 0; r < t.nposts; r++) postsArr[r] = sources[t.posts[r]].card;
    Val postsVal = Val::array(postsArr, t.nposts);
    KV* tagKv = ar->allocArr<KV>(5);
    tagKv[0] = KV{S("name"), Val::str(t.name)};
    tagKv[1] = KV{S("slug"), Val::str(t.slug)};
    tagKv[2] = KV{S("url"), Val::str(t.url)};
    tagKv[3] = KV{S("count"), Val::integer(t.nposts)};
    tagKv[4] = KV{S("posts"), postsVal};
    Val tag = Val::object(tagKv, 5);
    KV extra[1] = {KV{S("tag"), tag}};
    Str html;
    if (render(S("tag"), extra, 1, html)) {
      out.add(ar, ar->cat({S("tags/"), t.slug, S("/index.html")}), html);
    }
  }
  // Tags index page.
  {
    Str html;
    if (render(S("tags"), nullptr, 0, html)) out.add(ar, S("tags/index.html"), html);
  }
  // Statics (shadowed when a built page already has the path; a static
  // named "__proto__" is dropped silently, as the JS assignment is).
  for (uint32_t q = 0; q < nstatics; q++) {
    Str path = staticPaths[q];
    if (strEq(path, S("__proto__"))) continue;
    if (out.has(path)) {
      pushReport(ar->cat({S("shadowed "), path, S(": a built page has its path")}));
    } else {
      out.add(ar, path, staticContents[q]);
    }
  }

  // ---- 8. The report.
  Str* lines = ar->allocArr<Str>(nreport + nBroken + 1);
  uint32_t nlines = 0;
  for (uint32_t q = 0; q < nreport; q++) lines[nlines++] = report[q];
  for (uint32_t q = 0; q < nBroken; q++) lines[nlines++] = allBroken[q];
  std::sort(lines, lines + nlines, [](Str a, Str b) { return cmpStr(a, b) < 0; });
  int64_t pageCount = 0;
  for (uint32_t q = 0; q < nsources; q++)
    if (!sources[q].post) pageCount++;
  char nb[64];
  int nn = std::snprintf(nb, sizeof nb, "built %lld pages, %u posts, %u tags and %u static files",
                         (long long)pageCount, nposts, ntags, nstatics);
  lines[nlines++] = Str(nb, (uint32_t)nn);
  Buf rb;
  for (uint32_t q = 0; q < nlines; q++) {
    rb.append(lines[q]);
    rb.push('\n');
  }
  out.add(ar, S("build-report.txt"), ar->dup(rb.view()));
  return true;
}

// ---------------------------------------------------------------- N-API bridge

#include <sys/syscall.h>
#include <unistd.h>

static void napiThrow(napi_env env, const char* msg) {
  napi_throw_error(env, nullptr, msg);
}

// JS value -> arena Val (for runOne, the dev-harness path). Strings are
// UTF-8 encoded by N-API (WTF-8 for lone surrogates), matching the
// zero-copy parser's view of the same text.
static Val valueFromJs(napi_env env, napi_value v, Arena* ar) {
  napi_valuetype t;
  napi_typeof(env, v, &t);
  switch (t) {
    case napi_undefined:
    case napi_null:
      return Val::nullv();
    case napi_boolean: {
      bool b = false;
      napi_get_value_bool(env, v, &b);
      return Val::boolean(b);
    }
    case napi_number: {
      double d = 0;
      napi_get_value_double(env, v, &d);
      if (std::trunc(d) == d && std::fabs(d) < 9.0e15) return Val::integer((int64_t)d);
      return Val::dbl(d);
    }
    case napi_string: {
      size_t len = 0;
      napi_get_value_string_utf8(env, v, nullptr, 0, &len);
      Buf tmp;
      tmp.reserve(len + 1);
      size_t got = 0;
      napi_get_value_string_utf8(env, v, tmp.data, len + 1, &got);
      tmp.len = got;
      return Val::str(ar->dup(tmp.view()));
    }
    case napi_object: {
      bool isArr = false;
      napi_is_array(env, v, &isArr);
      if (isArr) {
        uint32_t n = 0;
        napi_get_array_length(env, v, &n);
        Val* items = ar->allocArr<Val>(n ? n : 1);
        for (uint32_t i = 0; i < n; i++) {
          napi_value el;
          napi_get_element(env, v, i, &el);
          items[i] = valueFromJs(env, el, ar);
        }
        return Val::array(items, n);
      }
      napi_value keys;
      napi_get_property_names(env, v, &keys);
      uint32_t n = 0;
      napi_get_array_length(env, keys, &n);
      KV* items = ar->allocArr<KV>(n ? n : 1);
      for (uint32_t i = 0; i < n; i++) {
        napi_value kv;
        napi_get_element(env, keys, i, &kv);
        size_t klen = 0;
        napi_get_value_string_utf8(env, kv, nullptr, 0, &klen);
        Buf kb;
        kb.reserve(klen + 1);
        size_t got = 0;
        napi_get_value_string_utf8(env, kv, kb.data, klen + 1, &got);
        kb.len = got;
        Str key = ar->dup(kb.view());
        napi_value pv;
        napi_get_property(env, v, kv, &pv);
        items[i].key = key;
        items[i].val = valueFromJs(env, pv, ar);
      }
      return Val::object(items, n);
    }
    default:
      return Val::nullv();
  }
}

// runOne(input): the dev-harness path (native-edge.mjs, native-gen.mjs).
// Returns the case output object.
static napi_value RunOne(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1) {
    napiThrow(env, "runOne(input) needs one argument");
    return nullptr;
  }
  Arena ar;
  ar.init(64u << 20);
  Val input = valueFromJs(env, argv[0], &ar);
  CaseOut out;
  if (!runCase(&ar, input, out)) {
    napiThrow(env, "runCase failed (malformed input)");
    return nullptr;
  }
  napi_value result;
  napi_create_object(env, &result);
  for (uint32_t i = 0; i < out.n; i++) {
    napi_value ks, vs;
    napi_create_string_utf8(env, out.items[i].path.p, out.items[i].path.n, &ks);
    napi_create_string_utf8(env, out.items[i].text.p, out.items[i].text.n, &vs);
    napi_set_property(env, result, ks, vs);
  }
  return result;
}

// Serialize one case output into the answers buffer.
static void appendCaseJson(Buf& out, const CaseOut& co) {
  out.push('{');
  for (uint32_t i = 0; i < co.n; i++) {
    if (i) out.push(',');
    appendJsonString(out, co.items[i].path);
    out.push(':');
    appendJsonString(out, co.items[i].text);
  }
  out.push('}');
}

// runAll([inputDir, outputDir]): the driver path. Reads cases.json,
// builds every case, writes answers.json, then exits fast (skipping
// Node teardown) unless NATIVE_NO_FAST_EXIT is set.
static napi_value RunAll(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  // Args (as main.ts passes them) are FILE paths: cases.json in,
  // answers.json out. With no args, fall back to the env dirs.
  std::string inputDir = "/input", outputDir = "/output";
  if (const char* e = std::getenv("MAKEFUN_INPUT_DIR")) inputDir = e;
  if (const char* e = std::getenv("MAKEFUN_OUTPUT_DIR")) outputDir = e;
  std::string casesPath = inputDir + "/cases.json";
  std::string outPath = outputDir + "/answers.json";
  if (argc >= 1) {
    char buf[1024];
    size_t len = 0;
    if (napi_get_value_string_utf8(env, argv[0], buf, sizeof buf, &len) == napi_ok && len > 0)
      casesPath.assign(buf, len);
  }
  if (argc >= 2) {
    char buf[1024];
    size_t len = 0;
    if (napi_get_value_string_utf8(env, argv[1], buf, sizeof buf, &len) == napi_ok && len > 0)
      outPath.assign(buf, len);
  }
  FILE* f = std::fopen(casesPath.c_str(), "rb");
  if (!f) {
    napiThrow(env, ("cannot open " + casesPath).c_str());
    return nullptr;
  }
  std::fseek(f, 0, SEEK_END);
  long fsize = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  char* buf = (char*)std::malloc(fsize > 0 ? (size_t)fsize : 1);
  size_t got = std::fread(buf, 1, (size_t)fsize, f);
  std::fclose(f);
  if ((long)got != fsize) {
    std::free(buf);
    napiThrow(env, "short read on cases.json");
    return nullptr;
  }
  {
    ProfTimer _pt("json_parse");
    Arena inputAr;
    inputAr.init(64u << 20);
    JsonParser parser((const uint8_t*)buf, (const uint8_t*)buf + fsize, &inputAr);
    Val cases = parser.parse();
    if (!parser.ok || cases.type != Val::Arr) {
      std::free(buf);
      napiThrow(env, "cases.json is not a JSON array");
      return nullptr;
    }
    Arena buildAr;
    buildAr.init(128u << 20);
    Buf answers;
    answers.reserve(8u << 20);
    answers.push('[');
    for (uint32_t ci = 0; ci < cases.arr.n; ci++) {
      const Val* c = &cases.arr.items[ci];
      const Val* input = c->find("input");
      if (!input || input->type != Val::Obj) {
        std::free(buf);
        napiThrow(env, "a case has no input object");
        return nullptr;
      }
      buildAr.reset();
      CaseOut out;
      {
        ProfTimer _pt2("runCase");
        if (!runCase(&buildAr, *input, out)) {
          std::free(buf);
          napiThrow(env, "runCase failed");
          return nullptr;
        }
      }
      if (ci) answers.push(',');
      appendCaseJson(answers, out);
    }
    answers.push(']');
    FILE* of = std::fopen(outPath.c_str(), "wb");
    if (!of) {
      std::free(buf);
      napiThrow(env, ("cannot write " + outPath).c_str());
      return nullptr;
    }
    std::fwrite(answers.data, 1, answers.len, of);
    std::fclose(of);
  }
  std::free(buf);
  if (std::getenv("NATIVE_PROFILE")) Prof::print();
  // Fast exit: skip Node teardown (the winners' SYS_exit_group). The
  // answers file is already written and closed.
  if (!std::getenv("NATIVE_NO_FAST_EXIT")) {
    std::fflush(nullptr);
    syscall(SYS_exit_group, 0);
  }
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value Init(napi_env env, napi_value exports) {
  napi_property_descriptor desc[] = {
      {"runOne", nullptr, RunOne, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"runAll", nullptr, RunAll, nullptr, nullptr, nullptr, napi_default, nullptr},
  };
  napi_define_properties(env, exports, 2, desc);
  return exports;
}
NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
