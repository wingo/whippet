#include <limits.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#define GC_IMPL 1

#include "gc-options-internal.h"
#include "gc-platform.h"

// M(UPPER, lower, repr, type, parser, default, min, max)
#define FOR_EACH_INT_GC_OPTION(M)                                       \
  M(HEAP_SIZE_POLICY, heap_size_policy, "heap-size-policy",             \
    int, heap_size_policy, GC_HEAP_SIZE_FIXED, GC_HEAP_SIZE_FIXED,      \
    GC_HEAP_SIZE_ADAPTIVE)                                              \
  M(PARALLELISM, parallelism, "parallelism",                            \
    int, int, default_parallelism(), 1, 64)

#define FOR_EACH_SIZE_GC_OPTION(M)                                      \
  M(HEAP_SIZE, heap_size, "heap-size",                                  \
    size, size, 6 * 1024 * 1024, 0, -1)                                 \
  M(MAXIMUM_HEAP_SIZE, maximum_heap_size, "maximum-heap-size",          \
    size, size, 0, 0, -1)

#define FOR_EACH_DOUBLE_GC_OPTION(M)                                    \
  M(HEAP_SIZE_MULTIPLIER, heap_size_multiplier, "heap-size-multiplier", \
    double, double, 1.75, 1.0, 1e6)                                     \
  M(HEAP_EXPANSIVENESS, heap_expansiveness, "heap-expansiveness",       \
    double, double, 1.0, 0.0, 50.0)

typedef int gc_option_int;
typedef size_t gc_option_size;
typedef double gc_option_double;

#define FOR_EACH_COMMON_GC_OPTION(M)                                    \
  FOR_EACH_INT_GC_OPTION(M)                                             \
  FOR_EACH_SIZE_GC_OPTION(M)                                            \
  FOR_EACH_DOUBLE_GC_OPTION(M)

static int clamp_int(int n, int lo, int hi) {
  return n < lo ? lo : n > hi ? hi : n;
}
static size_t clamp_size(size_t n, size_t lo, size_t hi) {
  return n < lo ? lo : n > hi ? hi : n;
}
static double clamp_double(double n, double lo, double hi) {
  return n < lo ? lo : n > hi ? hi : n;
}

static int default_parallelism(void) {
  return clamp_int(gc_platform_processor_count(), 1, 8);
}

void gc_init_common_options(struct gc_common_options *options) {
#define INIT(UPPER, lower, repr, type, parser, default, min, max) \
  options->lower = default;
  FOR_EACH_COMMON_GC_OPTION(INIT)
#undef INIT
}

int gc_common_option_from_string(const char *str) {
#define GET_OPTION(UPPER, lower, repr, type, parser, default, min, max) \
  if (strcmp(str, repr) == 0) return GC_OPTION_##UPPER;
  FOR_EACH_COMMON_GC_OPTION(GET_OPTION)
#undef GET_OPTION
  return -1;
}

#define SET_OPTION(UPPER, lower, repr, type, parser, default, min, max)  \
  case GC_OPTION_##UPPER:                                               \
  if (value != clamp_##type(value, min, max)) return 0;                 \
    options->lower = value;                                             \
    return 1;
#define DEFINE_SETTER(STEM, stem, type)                                 \
  int gc_common_options_set_##stem(struct gc_common_options *options,    \
                                   int option, type value) {            \
    switch (option) {                                                   \
      FOR_EACH_##STEM##_GC_OPTION(SET_OPTION)                           \
      default: return 0;                                                \
    }                                                                   \
  }
DEFINE_SETTER(INT, int, int)
DEFINE_SETTER(SIZE, size, size_t)
DEFINE_SETTER(DOUBLE, double, double)
#undef SET_OPTION
#undef DEFINE_SETTER

static int parse_size(const char *arg, size_t *val) {
  char *end;
  long i = strtol(arg, &end, 0);
  if (i < 0 || i == LONG_MAX) return 0;
  if (end == arg) return 0;
  char delim = *end;
  if (delim == 'k' || delim == 'K')
    ++end, i *= 1024L;
  else if (delim == 'm' || delim == 'M')
    ++end, i *= 1024L * 1024L;
  else if (delim == 'g' || delim == 'G')
    ++end, i *= 1024L * 1024L * 1024L;
  else if (delim == 't' || delim == 'T')
    ++end, i *= 1024L * 1024L * 1024L * 1024L;

  if (*end != '\0') return 0;
  *val = i;
  return 1;
}

static int parse_int(const char *arg, int *val) {
  char *end;
  long i = strtol(arg, &end, 0);
  if (i == LONG_MIN || i == LONG_MAX || end == arg || *end)
    return 0;
  *val = i;
  return 1;
}

static int parse_heap_size_policy(const char *arg, int *val) {
  if (strcmp(arg, "fixed") == 0) {
    *val = GC_HEAP_SIZE_FIXED;
    return 1;
  }
  if (strcmp(arg, "growable") == 0) {
    *val = GC_HEAP_SIZE_GROWABLE;
    return 1;
  }
  if (strcmp(arg, "adaptive") == 0) {
    *val = GC_HEAP_SIZE_ADAPTIVE;
    return 1;
  }
  return parse_int(arg, val);
}

static int parse_double(const char *arg, double *val) {
  char *end;
  double d = strtod(arg, &end);
  if (end == arg || *end)
    return 0;
  *val = d;
  return 1;
}

int gc_common_options_parse_and_set(struct gc_common_options *options,
                                    int option, const char *value) {
  switch (option) {
#define SET_OPTION(UPPER, lower, repr, type, parser, default, min, max)  \
    case GC_OPTION_##UPPER: {                                            \
      gc_option_##type v;                                                \
      if (!parse_##parser(value, &v)) return 0;                          \
      return gc_common_options_set_##type(options, option, v);           \
    }
    FOR_EACH_COMMON_GC_OPTION(SET_OPTION)
    default: return 0;
  }
}

static int is_lower(char c) { return 'a' <= c && c <= 'z'; }
static int is_digit(char c) { return '0' <= c && c <= '9'; }
static int is_option(char c) { return is_lower(c) || c == '-'; }
static int is_option_end(char c) { return c == '='; }
static int is_value(char c) {
  return is_lower(c) || is_digit(c) || c == '-' || c == '+' || c == '.';
}
static int is_value_end(char c) { return c == '\0' || c == ','; }
static char* read_token(char *p, int (*is_tok)(char c), int (*is_end)(char c),
                        char *delim) {
  char c;
  for (c = *p; is_tok(c); c = *++p);
  if (!is_end(c)) return NULL;
  *delim = c;
  *p = '\0';
  return p + 1;
}
int gc_options_parse_and_set_many(struct gc_options *options,
                                  const char *str) {
  if (!*str) return 1;
  char *copy = strdup(str);
  char *cur = copy;
  int ret = 0;
  while (1) {
    char delim;
    char *next = read_token(cur, is_option, is_option_end, &delim);
    if (!next) break;
    int option = gc_option_from_string(cur);
    if (option < 0) break;

    cur = next;
    next = read_token(cur, is_value, is_value_end, &delim);
    if (!next) break;
    if (!gc_options_parse_and_set(options, option, cur)) break;
    cur = next;
    if (delim == '\0') {
      ret = 1;
      break;
    }
  }
  free(copy);
  return ret;
}
