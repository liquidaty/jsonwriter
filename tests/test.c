/* jsonwriter - unit tests.
 *
 * Each test drives the public API into an in-memory sink and compares the
 * emitted bytes against an exact expected string. Compact mode is used for the
 * bulk of the assertions (deterministic, separator-free); a handful of pretty
 * cases pin the indentation/newline behavior. Return-code paths (end mismatch,
 * nesting limit, variant misconfiguration) are checked directly.
 *
 * Build/run via `make test` (optionally ASAN=1). Exit status is nonzero iff a
 * test failed. */
#include <jsonwriter.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------- sink buffer */

typedef struct {
  unsigned char *p;
  size_t len, cap;
  int oom;
} sink;

/* fwrite-like: jsonwriter calls write(buf, nbytes, 1, ctx). Append nbytes,
 * keep a trailing NUL for convenient printing, return nmemb on success. */
static size_t sink_write(const void *restrict ptr, size_t size, size_t nmemb,
                         void *restrict ctx) {
  sink *b = (sink *)ctx;
  size_t n = size * nmemb;
  if (b->len + n + 1 > b->cap) {
    size_t nc = b->cap ? b->cap : 256;
    unsigned char *np;
    while (nc < b->len + n + 1)
      nc *= 2;
    np = (unsigned char *)realloc(b->p, nc);
    if (!np) { b->oom = 1; return 0; }
    b->p = np;
    b->cap = nc;
  }
  memcpy(b->p + b->len, ptr, n);
  b->len += n;
  b->p[b->len] = '\0';
  return nmemb;
}

/* ----------------------------------------------------------------- harness */

static int g_pass = 0, g_fail = 0;

/* Fresh writer over `s` (zeroed here). compact!=0 selects compact output. */
static jsonwriter_handle begin(sink *s, int compact) {
  jsonwriter_handle h;
  s->p = NULL; s->len = 0; s->cap = 0; s->oom = 0;
  h = jsonwriter_new_stream(sink_write, s);
  if (h && compact)
    jsonwriter_set_option(h, jsonwriter_option_compact);
  return h;
}

/* Flush `h`, assert its accumulated output equals `expect` exactly, then
 * destroy the writer and free the sink. */
static void done(const char *name, jsonwriter_handle h, sink *s,
                 const char *expect) {
  size_t elen = strlen(expect);
  if (!h) {
    g_fail++;
    printf("FAIL %s (writer alloc failed)\n", name);
    return;
  }
  jsonwriter_flush(h);
  if (s->oom || s->len != elen || (s->len && memcmp(s->p, expect, elen) != 0)) {
    g_fail++;
    printf("FAIL %s\n  expected: %s\n  got:      %s\n", name, expect,
           s->p ? (char *)s->p : "(null)");
  } else {
    g_pass++;
  }
  jsonwriter_delete(h);
  free(s->p);
}

static void ok(const char *name, int cond) {
  if (cond) g_pass++;
  else { g_fail++; printf("FAIL %s\n", name); }
}

/* identity variant handler: the data pointer is a jsonwriter_variant. */
static int g_variant_cleanups;
static struct jsonwriter_variant id_variant(void *p) {
  return *(struct jsonwriter_variant *)p;
}
static void count_cleanup(void *data, struct jsonwriter_variant *v) {
  (void)data; (void)v;
  g_variant_cleanups++;
}

/* ----------------------------------------------------------- structure */

static void run_structure_tests(void) {
  sink s;
  jsonwriter_handle h;

  h = begin(&s, 1);
  jsonwriter_start_object(h);
  jsonwriter_end(h);
  done("empty-object", h, &s, "{}");

  h = begin(&s, 1);
  jsonwriter_start_array(h);
  jsonwriter_end(h);
  done("empty-array", h, &s, "[]");

  h = begin(&s, 1);
  jsonwriter_start_object(h);
  jsonwriter_object_key(h, "name");
  jsonwriter_cstr(h, "Alice");
  jsonwriter_object_key(h, "age");
  jsonwriter_int(h, 30);
  jsonwriter_end(h);
  done("flat-object", h, &s, "{\"name\":\"Alice\",\"age\":30}");

  /* convenience macros */
  h = begin(&s, 1);
  jsonwriter_start_object(h);
  jsonwriter_object_cstr(h, "k", "v");
  jsonwriter_object_int(h, "n", 7);
  jsonwriter_object_bool(h, "b", 1);
  jsonwriter_object_null(h, "z");
  jsonwriter_end(h);
  done("object-macros", h, &s, "{\"k\":\"v\",\"n\":7,\"b\":true,\"z\":null}");

  h = begin(&s, 1);
  jsonwriter_start_array(h);
  jsonwriter_int(h, 1);
  jsonwriter_int(h, 2);
  jsonwriter_int(h, 3);
  jsonwriter_end(h);
  done("array-ints", h, &s, "[1,2,3]");

  h = begin(&s, 1);
  jsonwriter_start_array(h);
  jsonwriter_cstr(h, "a");
  jsonwriter_int(h, 2);
  jsonwriter_bool(h, 1);
  jsonwriter_bool(h, 0);
  jsonwriter_null(h);
  jsonwriter_end(h);
  done("array-mixed", h, &s, "[\"a\",2,true,false,null]");

  /* nested object + array */
  h = begin(&s, 1);
  jsonwriter_start_object(h);
  jsonwriter_object_key(h, "user");
  jsonwriter_start_object(h);
  jsonwriter_object_cstr(h, "name", "Ann");
  jsonwriter_end(h);
  jsonwriter_object_key(h, "tags");
  jsonwriter_start_array(h);
  jsonwriter_cstr(h, "x");
  jsonwriter_cstr(h, "y");
  jsonwriter_end(h);
  jsonwriter_end(h);
  done("nested", h, &s, "{\"user\":{\"name\":\"Ann\"},\"tags\":[\"x\",\"y\"]}");

  /* object_keyn truncates the key to the given length */
  h = begin(&s, 1);
  jsonwriter_start_object(h);
  jsonwriter_object_keyn(h, "abcdef", 3);
  jsonwriter_cstr(h, "v");
  jsonwriter_end(h);
  done("object-keyn", h, &s, "{\"abc\":\"v\"}");

  /* end_all closes every open container */
  h = begin(&s, 1);
  jsonwriter_start_object(h);
  jsonwriter_object_key(h, "a");
  jsonwriter_start_array(h);
  jsonwriter_int(h, 1);
  jsonwriter_end_all(h);
  done("end-all", h, &s, "{\"a\":[1]}");
}

/* ----------------------------------------------------------- strings */

static void run_string_tests(void) {
  sink s;
  jsonwriter_handle h;

  /* empty string emits a quoted empty token (exercises write_json_str's
   * !len path -- the one that carried the double-comma typo) */
  h = begin(&s, 1);
  jsonwriter_start_array(h);
  jsonwriter_cstr(h, "");
  jsonwriter_end(h);
  done("empty-string", h, &s, "[\"\"]");

  /* a NULL string value is emitted as the JSON null literal */
  h = begin(&s, 1);
  jsonwriter_start_array(h);
  jsonwriter_str(h, NULL);
  jsonwriter_end(h);
  done("null-string", h, &s, "[null]");

  /* strn writes exactly len bytes */
  h = begin(&s, 1);
  jsonwriter_start_array(h);
  jsonwriter_cstrn(h, "hello", 3);
  jsonwriter_end(h);
  done("strn-len", h, &s, "[\"hel\"]");

  /* an embedded NUL is data, not a terminator: strn must escape it (\u0000)
   * and keep writing the rest, not truncate the length-delimited string */
  h = begin(&s, 1);
  jsonwriter_start_array(h);
  jsonwriter_cstrn(h, "a\0b", 3);
  jsonwriter_end(h);
  done("strn-embedded-nul", h, &s, "[\"a\\u0000b\"]");

  /* same for a length-delimited object key */
  h = begin(&s, 1);
  jsonwriter_start_object(h);
  jsonwriter_object_keyn(h, "k\0y", 3);
  jsonwriter_cstr(h, "v");
  jsonwriter_end(h);
  done("keyn-embedded-nul", h, &s, "{\"k\\u0000y\":\"v\"}");

  /* quote + backslash escaping */
  h = begin(&s, 1);
  jsonwriter_start_array(h);
  jsonwriter_cstr(h, "a\"b\\c");
  jsonwriter_end(h);
  done("escape-quote-backslash", h, &s, "[\"a\\\"b\\\\c\"]");

  /* short control escapes \t \n \r and \b \f */
  h = begin(&s, 1);
  jsonwriter_start_array(h);
  jsonwriter_cstr(h, "\t\n\r");
  jsonwriter_cstr(h, "\b\f");
  jsonwriter_end(h);
  done("escape-controls", h, &s, "[\"\\t\\n\\r\",\"\\b\\f\"]");

  /* other control bytes use \u00XX */
  h = begin(&s, 1);
  jsonwriter_start_array(h);
  jsonwriter_cstrn(h, "\x01\x1f", 2);
  jsonwriter_end(h);
  done("escape-ctrl-hex", h, &s, "[\"\\u0001\\u001f\"]");

  /* multibyte UTF-8 passes through verbatim (e-acute = C3 A9) */
  h = begin(&s, 1);
  jsonwriter_start_array(h);
  jsonwriter_cstr(h, "\xc3\xa9");
  jsonwriter_end(h);
  done("utf8-passthrough", h, &s, "[\"\xc3\xa9\"]");
}

/* ------------------------------------------------- UTF-8 / escaping safety */

/* Regression: a crafted invalid UTF-8 lead byte (0xF8-0xFF, or any lead whose
 * continuation bytes are wrong) once let the *next* byte pass through verbatim.
 * If that byte was '"', '\\' or a control char it broke out of / corrupted the
 * JSON string. Every such byte must now be escaped or the bad lead dropped --
 * the output must always be a well-formed JSON string token. */
static void run_utf8_safety_tests(void) {
  sink s;
  jsonwriter_handle h;

  /* 0xFB is an illegal lead; the following '"' must be escaped, not emitted */
  h = begin(&s, 1);
  jsonwriter_start_array(h);
  jsonwriter_cstrn(h, "\xfb\x22", 2);
  jsonwriter_end(h);
  done("invalid-lead-quote", h, &s, "[\"\\\"\"]");

  /* invalid lead dropped; quote, backslash and control byte each escaped */
  h = begin(&s, 1);
  jsonwriter_start_array(h);
  jsonwriter_cstrn(h, "\xfb\x22\x5c\x04", 4);
  jsonwriter_end(h);
  done("invalid-lead-specials", h, &s, "[\"\\\"\\\\\\u0004\"]");

  /* a valid lead with a non-continuation following byte: lead dropped, the
   * following ASCII byte emitted normally (no breakout) */
  h = begin(&s, 1);
  jsonwriter_start_array(h);
  jsonwriter_cstrn(h, "\xc3\x41", 2);
  jsonwriter_end(h);
  done("bad-continuation", h, &s, "[\"A\"]");
}

/* ----------------------------------------------------------- numbers */

static void run_number_tests(void) {
  sink s;
  jsonwriter_handle h;

  h = begin(&s, 1);
  jsonwriter_start_array(h);
  jsonwriter_int(h, 0);
  jsonwriter_int(h, -1);
  jsonwriter_int(h, JSW_INT64_MAX);
  jsonwriter_int(h, JSW_INT64_MIN);
  jsonwriter_end(h);
  done("ints", h, &s,
       "[0,-1,9223372036854775807,-9223372036854775808]");

  h = begin(&s, 1);
  jsonwriter_start_array(h);
  jsonwriter_size_t(h, 0);
  jsonwriter_size_t(h, 12345);
  jsonwriter_end(h);
  done("size_t", h, &s, "[0,12345]");

  /* jsonwriter_dbl trims trailing zeros; use exactly-representable values */
  h = begin(&s, 1);
  jsonwriter_start_array(h);
  jsonwriter_dbl(h, 1.5L);
  jsonwriter_dbl(h, 3.0L);
  jsonwriter_dbl(h, 0.0L);
  jsonwriter_dbl(h, -2.25L);
  jsonwriter_end(h);
  done("doubles", h, &s, "[1.5,3,0,-2.25]");

  /* explicit format string, no trimming */
  h = begin(&s, 1);
  jsonwriter_start_array(h);
  jsonwriter_dblf(h, 3.14159L, "%.2Lf", 0);
  jsonwriter_end(h);
  done("dblf-format", h, &s, "[3.14]");

  /* non-finite doubles become quoted tokens (not valid JSON numbers) */
  h = begin(&s, 1);
  jsonwriter_start_array(h);
  jsonwriter_dbl(h, (long double)NAN);
  jsonwriter_dbl(h, (long double)INFINITY);
  jsonwriter_end(h);
  done("dbl-nonfinite", h, &s, "[\"NaN\",\"Infinity\"]");
}

/* ----------------------------------------------- raw / unknown values */

static void run_raw_unknown_tests(void) {
  sink s;
  jsonwriter_handle h;

  /* jsonwriter_unknown: numbers/true/false pass through; anything else is a
   * string (including not-quite-numbers like a leading-zero "01") */
  h = begin(&s, 1);
  jsonwriter_start_array(h);
  jsonwriter_unknown(h, (const unsigned char *)"42", 2, 0);
  jsonwriter_unknown(h, (const unsigned char *)"-1.5e3", 6, 0);
  jsonwriter_unknown(h, (const unsigned char *)"true", 4, 0);
  jsonwriter_unknown(h, (const unsigned char *)"false", 5, 0);
  jsonwriter_unknown(h, (const unsigned char *)"hello", 5, 0);
  jsonwriter_unknown(h, (const unsigned char *)"01", 2, 0);
  jsonwriter_end(h);
  done("unknown", h, &s, "[42,-1.5e3,true,false,\"hello\",\"01\"]");

  /* write_raw emits caller-supplied bytes verbatim as one array element */
  h = begin(&s, 1);
  jsonwriter_start_array(h);
  jsonwriter_write_raw(h, (const unsigned char *)"{\"x\":1}", 7);
  jsonwriter_int(h, 2);
  jsonwriter_end(h);
  done("write-raw", h, &s, "[{\"x\":1},2]");
}

/* ----------------------------------------------------------- variants */

static void run_variant_tests(void) {
  sink s;
  jsonwriter_handle h;
  struct jsonwriter_variant v[6];

  v[0].type = jsonwriter_datatype_null;
  v[1].type = jsonwriter_datatype_string;
  v[1].value.str = (unsigned char *)"hi";
  v[2].type = jsonwriter_datatype_integer;
  v[2].value.i = 5;
  v[3].type = jsonwriter_datatype_float;
  v[3].value.dbl = 2.5L;
  v[4].type = jsonwriter_datatype_bool;
  v[4].value.i = 1;                 /* bool reads the integer slot */
  v[5].type = jsonwriter_datatype_raw;
  v[5].value.str = (unsigned char *)"[1,2]";

  h = begin(&s, 1);
  ok("variant-handler-set",
     jsonwriter_set_variant_handler(h, id_variant, count_cleanup) ==
         jsonwriter_status_ok);
  g_variant_cleanups = 0;
  jsonwriter_start_array(h);
  for (int i = 0; i < 6; i++)
    jsonwriter_variant(h, &v[i]);
  jsonwriter_end(h);
  done("variant-values", h, &s, "[null,\"hi\",5,2.5,true,[1,2]]");
  ok("variant-cleanup-called", g_variant_cleanups == 6);

  /* without a handler, jsonwriter_variant reports misconfiguration */
  h = begin(&s, 1);
  jsonwriter_start_array(h);
  ok("variant-misconfig",
     jsonwriter_variant(h, &v[0]) == jsonwriter_status_misconfiguration);
  jsonwriter_end(h);
  jsonwriter_delete(h);
  free(s.p);

  /* a NULL handler is rejected */
  h = begin(&s, 1);
  ok("variant-null-handler",
     jsonwriter_set_variant_handler(h, NULL, NULL) ==
         jsonwriter_status_invalid_value);
  jsonwriter_delete(h);
  free(s.p);
}

/* ----------------------------------------------------- pretty printing */

static void run_pretty_tests(void) {
  sink s;
  jsonwriter_handle h;

  h = begin(&s, 0);   /* pretty is the default */
  jsonwriter_start_object(h);
  jsonwriter_object_cstr(h, "a", "b");
  jsonwriter_end(h);
  done("pretty-object", h, &s, "{\n  \"a\": \"b\"\n}\n");

  h = begin(&s, 0);
  jsonwriter_start_array(h);
  jsonwriter_int(h, 1);
  jsonwriter_int(h, 2);
  jsonwriter_end(h);
  done("pretty-array", h, &s, "[\n  1,\n  2\n]\n");

  h = begin(&s, 0);
  jsonwriter_start_object(h);
  jsonwriter_object_key(h, "user");
  jsonwriter_start_object(h);
  jsonwriter_object_cstr(h, "name", "Ann");
  jsonwriter_end(h);
  jsonwriter_end(h);
  done("pretty-nested", h, &s, "{\n  \"user\": {\n    \"name\": \"Ann\"\n  }\n}\n");
}

/* ----------------------------------------------------- error returns */

static void run_error_tests(void) {
  sink s;
  jsonwriter_handle h;

  /* ending the wrong container type is rejected and leaves the frame open */
  h = begin(&s, 1);
  jsonwriter_start_object(h);
  ok("end-array-on-object",
     jsonwriter_end_array(h) == jsonwriter_status_invalid_end);
  ok("end-object-recovers",
     jsonwriter_end_object(h) == jsonwriter_status_ok);
  jsonwriter_delete(h);
  free(s.p);

  h = begin(&s, 1);
  jsonwriter_start_array(h);
  ok("end-object-on-array",
     jsonwriter_end_object(h) == jsonwriter_status_invalid_end);
  ok("end-array-recovers",
     jsonwriter_end_array(h) == jsonwriter_status_ok);
  jsonwriter_delete(h);
  free(s.p);

  /* nothing open -> invalid_end */
  h = begin(&s, 1);
  ok("end-nothing", jsonwriter_end(h) == jsonwriter_status_invalid_end);
  jsonwriter_delete(h);
  free(s.p);

  /* nesting is bounded: opens succeed up to the cap, then fail (no crash,
   * no overflow -- run under ASAN this also pins the stack bounds) */
  h = begin(&s, 1);
  {
    int i, opened = 0, refused = 0;
    for (i = 0; i < 300; i++) {
      if (jsonwriter_start_array(h) == 0) opened++;
      else { refused++; break; }
    }
    ok("nesting-cap-opens", opened == 255);
    ok("nesting-cap-refused", refused == 1);
  }
  jsonwriter_end_all(h);
  jsonwriter_delete(h);
  free(s.p);
}

/* ------------------------------------------- caller-overridable nesting cap */

/* Open arrays until refused; report how many opened. cap N allows N-1 opens
 * (same conservative off-by-one the default 256 -> 255 exhibits). */
static int open_until_refused(jsonwriter_handle h) {
  int opened = 0;
  while (jsonwriter_start_array(h) == 0)
    opened++;
  return opened;
}

static void run_max_nesting_tests(void) {
  sink s;
  jsonwriter_handle h;

  /* lower the cap: 4 -> 3 opens allowed */
  h = begin(&s, 1);
  ok("set-max-nesting-lower-ok",
     jsonwriter_set_max_nesting(h, 4) == jsonwriter_status_ok);
  ok("set-max-nesting-lower", open_until_refused(h) == 3);
  jsonwriter_end_all(h);
  jsonwriter_delete(h);
  free(s.p);

  /* raise the cap above the 256 default: 300 -> 299 opens allowed */
  h = begin(&s, 1);
  ok("set-max-nesting-higher-ok",
     jsonwriter_set_max_nesting(h, 300) == jsonwriter_status_ok);
  ok("set-max-nesting-higher", open_until_refused(h) == 299);
  jsonwriter_end_all(h);
  jsonwriter_delete(h);
  free(s.p);

  /* zero is rejected, cap unchanged (still the default) */
  h = begin(&s, 1);
  ok("set-max-nesting-zero",
     jsonwriter_set_max_nesting(h, 0) == jsonwriter_status_invalid_value);
  ok("set-max-nesting-zero-unchanged", open_until_refused(h) == 255);
  jsonwriter_end_all(h);
  jsonwriter_delete(h);
  free(s.p);

  /* refused once a container is open */
  h = begin(&s, 1);
  jsonwriter_start_array(h);
  ok("set-max-nesting-open",
     jsonwriter_set_max_nesting(h, 8) == jsonwriter_status_misconfiguration);
  jsonwriter_end_all(h);
  jsonwriter_delete(h);
  free(s.p);

  /* writing works normally after a resize (within the new cap) */
  h = begin(&s, 1);
  jsonwriter_set_max_nesting(h, 4);
  jsonwriter_start_object(h);
  jsonwriter_object_key(h, "a");
  jsonwriter_start_array(h);
  jsonwriter_int(h, 1);
  jsonwriter_int(h, 2);
  jsonwriter_end(h); /* array */
  jsonwriter_end(h); /* object */
  done("max-nesting-functional", h, &s, "{\"a\":[1,2]}");
}

/* ----------------------------------------------- FILE* convenience path */

static void run_file_tests(void) {
  FILE *f = tmpfile();
  jsonwriter_handle h;
  char buf[64];
  size_t n;

  if (!f) { ok("file-tmpfile", 0); return; }
  h = jsonwriter_new(f);
  if (!h) { ok("file-new", 0); fclose(f); return; }
  jsonwriter_set_option(h, jsonwriter_option_compact);
  jsonwriter_start_object(h);
  jsonwriter_object_cstr(h, "ok", "1");
  jsonwriter_end(h);
  jsonwriter_flush(h);
  rewind(f);
  n = fread(buf, 1, sizeof buf - 1, f);
  buf[n] = '\0';
  ok("file-output", strcmp(buf, "{\"ok\":\"1\"}") == 0);
  jsonwriter_delete(h);
  fclose(f);
}

int main(void) {
  run_structure_tests();
  run_string_tests();
  run_utf8_safety_tests();
  run_number_tests();
  run_raw_unknown_tests();
  run_variant_tests();
  run_pretty_tests();
  run_error_tests();
  run_max_nesting_tests();
  run_file_tests();

  printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
