/* jsonwriter - fuzz harness.
 *
 * jsonwriter is a writer, not a parser, so the untrusted-input surface is the
 * *data the caller serializes*: arbitrary string/key bytes flowing through the
 * escaper, and arbitrary sequences of writer calls exercising the nesting
 * stacks, separators and number formatting. The single libFuzzer entry point:
 *
 *   1. drives the writer as an opcode "program" (both compact and pretty) into
 *      a discard sink -- crash/overflow detection, best under -fsanitize=...;
 *   2. runs an escaping ORACLE: it writes the raw input as one JSON string
 *      value and asserts the emitted bytes are a well-formed JSON string token
 *      (no unescaped quote or control byte, every backslash a valid escape).
 *      An abort() here is a real find: a string that breaks out of its quotes
 *      is the writer's one security-critical failure mode.
 *
 * Build: `make fuzz` (clang + libFuzzer) or `make fuzz-standalone` for a
 * portable replay driver that runs argv files / stdin once each (any compiler).
 */
#include <jsonwriter.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Discard sink: report nmemb written (success). */
static size_t discard(const void *restrict p, size_t size, size_t nmemb,
                      void *restrict ctx) {
  (void)p; (void)size; (void)ctx;
  return nmemb;
}

/* Growable capture sink for the oracle; flags oom and reports a short write so
 * the writer stops touching it. */
typedef struct { unsigned char *p; size_t len, cap; int oom; } cap_sink;
static size_t cap_write(const void *restrict ptr, size_t size, size_t nmemb,
                        void *restrict ctx) {
  cap_sink *c = (cap_sink *)ctx;
  size_t n = size * nmemb;
  if (c->len + n > c->cap) {
    size_t nc = c->cap ? c->cap : 256;
    unsigned char *nb;
    while (nc < c->len + n)
      nc *= 2;
    nb = (unsigned char *)realloc(c->p, nc);
    if (!nb) { c->oom = 1; return 0; }
    c->p = nb;
    c->cap = nc;
  }
  memcpy(c->p + c->len, ptr, n);
  c->len += n;
  return nmemb;
}

/* ------------------------------------------------------ byte-stream readers */

/* One length byte, clamped to the bytes remaining. */
static size_t take_len(const uint8_t *d, size_t len, size_t *i) {
  size_t n = (*i < len) ? d[(*i)++] : 0;
  if (n > len - *i)
    n = len - *i;
  return n;
}

/* Up to 8 bytes little-endian into an int64 (fewer if input runs out). */
static int64_t take_i64(const uint8_t *d, size_t len, size_t *i) {
  uint64_t v = 0;
  int k;
  for (k = 0; k < 8 && *i < len; k++)
    v |= (uint64_t)d[(*i)++] << (8 * k);
  return (int64_t)v;
}

/* 8 raw bytes reinterpreted as a double (memcpy avoids aliasing UB; NaN/Inf
 * are valid inputs -- the writer must handle them). */
static long double take_dbl(const uint8_t *d, size_t len, size_t *i) {
  unsigned char b[8] = {0};
  double out;
  int k;
  for (k = 0; k < 8 && *i < len; k++)
    b[k] = d[(*i)++];
  memcpy(&out, b, sizeof out);
  return (long double)out;
}

/* --------------------------------------------------------------- driver */

/* Interpret the input as a program of writer operations. */
static void run_driver(const uint8_t *data, size_t len, int compact) {
  jsonwriter_handle h = jsonwriter_new_stream(discard, NULL);
  size_t i = 0;
  if (!h)
    return;
  if (compact)
    jsonwriter_set_option(h, jsonwriter_option_compact);
  while (i < len) {
    uint8_t op = data[i++];
    switch (op % 13) {
    case 0: jsonwriter_start_object(h); break;
    case 1: jsonwriter_start_array(h); break;
    case 2: jsonwriter_end(h); break;
    case 3: jsonwriter_end_object(h); break;
    case 4: jsonwriter_end_array(h); break;
    case 5: { size_t n = take_len(data, len, &i);
              /* object_keyn treats len 0 as "strlen the key", so a zero-length
               * slice must use a terminated empty string, not a raw pointer */
              jsonwriter_object_keyn(h, n ? (const char *)data + i : "", n);
              i += n; break; }
    case 6: { size_t n = take_len(data, len, &i);
              jsonwriter_strn(h, data + i, n); i += n; break; }
    case 7: { size_t n = take_len(data, len, &i);
              jsonwriter_unknown(h, data + i, n, 0); i += n; break; }
    case 8:  jsonwriter_int(h, take_i64(data, len, &i)); break;
    case 9:  jsonwriter_size_t(h, (size_t)take_i64(data, len, &i)); break;
    case 10: jsonwriter_dbl(h, take_dbl(data, len, &i)); break;
    case 11: jsonwriter_bool(h, op & 1); break;
    case 12: jsonwriter_null(h); break;
    }
  }
  jsonwriter_end_all(h);
  jsonwriter_flush(h);
  jsonwriter_delete(h);
}

/* --------------------------------------------------------------- oracle */

static int is_hex(unsigned char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

/* True iff p[0..n) is a well-formed RFC 8259 string token: opening and closing
 * quotes, and between them only unescaped chars >= 0x20 (excluding ") or
 * well-formed escapes. A superset acceptor for valid tokens, so it never
 * rejects legal output -- only a genuine breakout fails it. */
static int valid_json_string_token(const unsigned char *p, size_t n) {
  size_t i, end;
  if (n < 2 || p[0] != '"' || p[n - 1] != '"')
    return 0;
  end = n - 1;                 /* content is p[1 .. end) */
  for (i = 1; i < end; ) {
    unsigned char c = p[i];
    if (c == '"' || c < 0x20)
      return 0;
    if (c == '\\') {
      if (i + 1 >= end)
        return 0;              /* lone backslash escaping the closing quote */
      switch (p[i + 1]) {
      case '"': case '\\': case '/': case 'b':
      case 'f': case 'n': case 'r': case 't':
        i += 2; break;
      case 'u':
        if (i + 6 > end || !is_hex(p[i + 2]) || !is_hex(p[i + 3]) ||
            !is_hex(p[i + 4]) || !is_hex(p[i + 5]))
          return 0;
        i += 6; break;
      default:
        return 0;
      }
    } else {
      i += 1;
    }
  }
  return i == end;
}

static void run_escape_oracle(const uint8_t *data, size_t len) {
  cap_sink c = {0, 0, 0, 0};
  jsonwriter_handle h = jsonwriter_new_stream(cap_write, &c);
  if (!h) { free(c.p); return; }
  jsonwriter_set_option(h, jsonwriter_option_compact);
  jsonwriter_strn(h, data, len);     /* root-level string value */
  jsonwriter_flush(h);
  if (!c.oom && c.len && !valid_json_string_token(c.p, c.len))
    abort();                         /* escaping breakout -- a real bug */
  jsonwriter_delete(h);
  free(c.p);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t len) {
  run_driver(data, len, 1);   /* compact */
  run_driver(data, len, 0);   /* pretty  */
  run_escape_oracle(data, len);
  return 0;
}

#ifdef JSW_FUZZ_STANDALONE
/* Portable libFuzzer-compatible driver: run each argv file (or stdin if none)
 * through the entry point exactly once. Builds with any toolchain. */
#include <stdio.h>

static int run_stream(FILE *f) {
  size_t cap = 4096, len = 0;
  unsigned char *buf = (unsigned char *)malloc(cap);
  if (!buf)
    return -1;
  for (;;) {
    size_t got;
    if (len == cap) {
      unsigned char *nb = (unsigned char *)realloc(buf, cap * 2);
      if (!nb) { free(buf); return -1; }
      buf = nb;
      cap *= 2;
    }
    got = fread(buf + len, 1, cap - len, f);
    len += got;
    if (got == 0)
      break;
  }
  LLVMFuzzerTestOneInput(buf, len);
  free(buf);
  return 0;
}

int main(int argc, char **argv) {
  int i;
  if (argc < 2)
    return run_stream(stdin) == 0 ? 0 : 1;
  for (i = 1; i < argc; i++) {
    FILE *f = fopen(argv[i], "rb");
    if (!f) {
      fprintf(stderr, "fuzz: cannot open '%s'\n", argv[i]);
      return 1;
    }
    if (run_stream(f) != 0) { fclose(f); return 1; }
    fclose(f);
  }
  return 0;
}
#endif /* JSW_FUZZ_STANDALONE */
