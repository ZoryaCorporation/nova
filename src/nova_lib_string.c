/**
 * @file nova_lib_string.c
 * @brief Nova Language - String Standard Library
 *
 * Provides string manipulation functions as the "string" module.
 *
 * Functions:
 *   string.len(s)            String byte length
 *   string.sub(s, i [,j])    Substring extraction
 *   string.upper(s)          Uppercase conversion
 *   string.lower(s)          Lowercase conversion
 *   string.rep(s, n [,sep])  Repeat string n times
 *   string.reverse(s)        Reverse string
 *   string.byte(s [,i [,j]]) Get byte values
 *   string.char(...)          Build string from byte values
 *   string.find(s, pat [,init [,plain]])  Pattern search
 *   string.format(fmt, ...)   Printf-style formatting
 *   string.gsub(s, pat, rep [,n])  Global substitute (basic)
 *   string.match(s, pat)      Pattern match (basic)
 *   string.gmatch(s, pat)     Global match iterator
 *
 * @author Anthony Taliento
 * @date 2026-02-08
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * DEPENDENCIES:
 *   - nova_lib.h
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#include "nova/nova_lib.h"
#include "nova/nova_vm.h"
#include "nova/nova_meta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ============================================================
 * INTERNAL: POSITION NORMALIZATION
 *
 * Nova 0-based convention: positive indices start at 0,
 * negative indices count from end (-1 = last byte).
 * ============================================================ */

static nova_int_t novai_str_posrelat(nova_int_t pos, size_t len) {
    if (pos >= 0) {
        return pos;
    }
    if (-(size_t)pos > len) {
        return 0;
    }
    return (nova_int_t)len + pos;
}

/* ============================================================
 * LEN
 * ============================================================ */

static int nova_string_len(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    NovaValue v = nova_vm_get(vm, 0);
    if (!nova_is_string(v)) {
        nova_vm_raise_error(vm, "bad argument #1 to 'len' (string expected, got %s)",
                           nova_vm_typename(nova_typeof(v)));
        return -1;
    }

    nova_vm_push_integer(vm, (nova_int_t)nova_str_len(nova_as_string(v)));
    return 1;
}

/* ============================================================
 * SUB
 * ============================================================ */

static int nova_string_sub(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) {
        return -1;
    }

    NovaValue sv = nova_vm_get(vm, 0);
    if (!nova_is_string(sv)) {
        nova_vm_raise_error(vm, "bad argument #1 to 'sub' (string expected)");
        return -1;
    }

    const char *s = nova_str_data(nova_as_string(sv));
    size_t slen = nova_str_len(nova_as_string(sv));

    nova_int_t start = 0;
    if (!nova_lib_check_integer(vm, 1, &start)) {
        return -1;
    }

    int nargs = nova_vm_get_top(vm);
    nova_int_t end = -1;
    if (nargs >= 3) {
        if (!nova_lib_check_integer(vm, 2, &end)) {
            return -1;
        }
    }

    /* Normalize positions (0-based, negative from end) */
    start = novai_str_posrelat(start, slen);
    end = novai_str_posrelat(end, slen);

    if (start < 0) start = 0;
    if (end >= (nova_int_t)slen) end = (nova_int_t)slen - 1;

    if (start > end) {
        nova_vm_push_string(vm, "", 0);
    } else {
        size_t len = (size_t)(end - start + 1);
        nova_vm_push_string(vm, s + start, len);
    }
    return 1;
}

/* ============================================================
 * UPPER / LOWER
 * ============================================================ */

static int nova_string_upper(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    NovaValue sv = nova_vm_get(vm, 0);
    if (!nova_is_string(sv)) {
        nova_vm_raise_error(vm, "bad argument #1 to 'upper' (string expected)");
        return -1;
    }

    size_t len = nova_str_len(nova_as_string(sv));
    char *buf = (char *)malloc(len + 1);
    if (buf == NULL) {
        nova_vm_raise_error(vm, "memory allocation failed in 'upper'");
        return -1;
    }

    for (size_t i = 0; i < len; i++) {
        buf[i] = (char)toupper((unsigned char)nova_str_data(nova_as_string(sv))[i]);
    }
    buf[len] = '\0';

    nova_vm_push_string(vm, buf, len);
    free(buf);
    return 1;
}

static int nova_string_lower(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    NovaValue sv = nova_vm_get(vm, 0);
    if (!nova_is_string(sv)) {
        nova_vm_raise_error(vm, "bad argument #1 to 'lower' (string expected)");
        return -1;
    }

    size_t len = nova_str_len(nova_as_string(sv));
    char *buf = (char *)malloc(len + 1);
    if (buf == NULL) {
        nova_vm_raise_error(vm, "memory allocation failed in 'lower'");
        return -1;
    }

    for (size_t i = 0; i < len; i++) {
        buf[i] = (char)tolower((unsigned char)nova_str_data(nova_as_string(sv))[i]);
    }
    buf[len] = '\0';

    nova_vm_push_string(vm, buf, len);
    free(buf);
    return 1;
}

/* ============================================================
 * REP
 * ============================================================ */

static int nova_string_rep(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) {
        return -1;
    }

    NovaValue sv = nova_vm_get(vm, 0);
    if (!nova_is_string(sv)) {
        nova_vm_raise_error(vm, "bad argument #1 to 'rep' (string expected)");
        return -1;
    }

    nova_int_t n = 0;
    if (!nova_lib_check_integer(vm, 1, &n)) {
        return -1;
    }

    if (n <= 0) {
        nova_vm_push_string(vm, "", 0);
        return 1;
    }

    const char *s = nova_str_data(nova_as_string(sv));
    size_t slen = nova_str_len(nova_as_string(sv));

    /* Get optional separator */
    const char *sep = "";
    size_t sep_len = 0;
    int nargs = nova_vm_get_top(vm);
    if (nargs >= 3) {
        NovaValue sepv = nova_vm_get(vm, 2);
        if (nova_is_string(sepv)) {
            sep = nova_str_data(nova_as_string(sepv));
            sep_len = nova_str_len(nova_as_string(sepv));
        }
    }

    /* Calculate total length */
    size_t total = (size_t)n * slen + ((size_t)n - 1) * sep_len;
    char *buf = (char *)malloc(total + 1);
    if (buf == NULL) {
        nova_vm_raise_error(vm, "memory allocation failed in 'rep'");
        return -1;
    }

    char *p = buf;
    for (nova_int_t i = 0; i < n; i++) {
        if (i > 0 && sep_len > 0) {
            memcpy(p, sep, sep_len);
            p += sep_len;
        }
        memcpy(p, s, slen);
        p += slen;
    }
    *p = '\0';

    nova_vm_push_string(vm, buf, total);
    free(buf);
    return 1;
}

/* ============================================================
 * REVERSE
 * ============================================================ */

static int nova_string_reverse(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    NovaValue sv = nova_vm_get(vm, 0);
    if (!nova_is_string(sv)) {
        nova_vm_raise_error(vm, "bad argument #1 to 'reverse' (string expected)");
        return -1;
    }

    size_t len = nova_str_len(nova_as_string(sv));
    char *buf = (char *)malloc(len + 1);
    if (buf == NULL) {
        nova_vm_raise_error(vm, "memory allocation failed in 'reverse'");
        return -1;
    }

    for (size_t i = 0; i < len; i++) {
        buf[i] = nova_str_data(nova_as_string(sv))[len - 1 - i];
    }
    buf[len] = '\0';

    nova_vm_push_string(vm, buf, len);
    free(buf);
    return 1;
}

/* ============================================================
 * BYTE / CHAR
 * ============================================================ */

static int nova_string_byte(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    NovaValue sv = nova_vm_get(vm, 0);
    if (!nova_is_string(sv)) {
        nova_vm_raise_error(vm, "bad argument #1 to 'byte' (string expected)");
        return -1;
    }

    size_t slen = nova_str_len(nova_as_string(sv));
    int nargs = nova_vm_get_top(vm);

    nova_int_t i = 0;
    nova_int_t j = 0;

    if (nargs >= 2) {
        if (!nova_lib_check_integer(vm, 1, &i)) {
            return -1;
        }
    }

    j = i;  /* default: single byte */
    if (nargs >= 3) {
        if (!nova_lib_check_integer(vm, 2, &j)) {
            return -1;
        }
    }

    i = novai_str_posrelat(i, slen);
    j = novai_str_posrelat(j, slen);

    if (i < 0) i = 0;
    if (j >= (nova_int_t)slen) j = (nova_int_t)slen - 1;

    int count = 0;
    for (nova_int_t k = i; k <= j; k++) {
        nova_vm_push_integer(vm, (nova_int_t)(unsigned char)nova_str_data(nova_as_string(sv))[k]);
        count++;
    }
    return count;
}

static int nova_string_char(NovaVM *vm) {
    int nargs = nova_vm_get_top(vm);

    char *buf = (char *)malloc((size_t)nargs + 1);
    if (buf == NULL) {
        nova_vm_raise_error(vm, "memory allocation failed in 'char'");
        return -1;
    }

    for (int i = 0; i < nargs; i++) {
        nova_int_t c = 0;
        if (!nova_lib_check_integer(vm, i, &c)) {
            free(buf);
            return -1;
        }
        if (c < 0 || c > 255) {
            free(buf);
            nova_vm_raise_error(vm, "bad argument #%d to 'char' (value out of range)", i + 1);
            return -1;
        }
        buf[i] = (char)(unsigned char)c;
    }
    buf[nargs] = '\0';

    nova_vm_push_string(vm, buf, (size_t)nargs);
    free(buf);
    return 1;
}

/* ============================================================
 * PATTERN MATCHING ENGINE
 *
 * Lua-compatible pattern matching with character classes,
 * quantifiers, anchors, captures, and back-references.
 *
 * Classes:  .  %a %c %d %l %p %s %u %w %x  (+ uppercase complements)
 * Sets:     [set]  [^set]  (with ranges: [a-z])
 * Quants:   *  +  -  ?
 * Anchors:  ^  $
 * Captures: (...)   %1-%9
 * Balance:  %bxy
 * ============================================================ */

#define NOVAI_MAXCAPTURES  32
#define NOVAI_MATCH_DEPTH  200

typedef struct {
    const char *init;       /* Capture start pointer              */
    ptrdiff_t   len;        /* -1 = position capture, -2 = open   */
} NovaMatchCapture;

typedef struct {
    const char         *src_init;   /* Source string start         */
    const char         *src_end;    /* Source string end           */
    const char         *p_end;      /* Pattern end                */
    NovaVM             *vm;         /* VM for pushing results     */
    int                 level;      /* Number of active captures  */
    int                 depth;      /* Recursion depth guard      */
    NovaMatchCapture    cap[NOVAI_MAXCAPTURES];
} NovaMatchState;

/* ---- Character class matching ---- */

/**
 * @brief Test if character c matches class cls (%a, %d, etc.).
 *
 * @param c    Character to test
 * @param cls  Class character (a,c,d,l,p,s,u,w,x or uppercase)
 *
 * @return Non-zero if c matches cls.
 */
static int novai_match_class(int c, int cls) {
    int res = 0;
    int lc = tolower(cls);
    switch (lc) {
        case 'a': res = isalpha(c);  break;
        case 'c': res = iscntrl(c);  break;
        case 'd': res = isdigit(c);  break;
        case 'l': res = islower(c);  break;
        case 'p': res = ispunct(c);  break;
        case 's': res = isspace(c);  break;
        case 'u': res = isupper(c);  break;
        case 'w': res = isalnum(c);  break;
        case 'x': res = isxdigit(c); break;
        default:
            return (cls == c);
    }
    /* Uppercase class letter = complement */
    if (isupper(cls)) {
        res = !res;
    }
    return res;
}

/**
 * @brief Find the end of a pattern class descriptor.
 *
 * Advances past a single class element: literal char, %x escape,
 * or [set] bracket expression.
 *
 * @param p  Current position in pattern
 *
 * @return Pointer past the class descriptor.
 */
static const char *novai_classend(const char *p) {
    switch (*p) {
        case '\0':
            return p;
        case '%':
            p++;
            if (*p == '\0') {
                return p;   /* error: trailing % */
            }
            return p + 1;
        case '[':
            p++;
            if (*p == '^') {
                p++;
            }
            /* Scan until closing ] */
            do {
                if (*p == '\0') {
                    return p;   /* error: unclosed [ */
                }
                if (*p == '%') {
                    p++;
                    if (*p == '\0') {
                        return p;
                    }
                }
                p++;
            } while (*p != ']');
            return p + 1;
        default:
            return p + 1;
    }
}

/**
 * @brief Test if character c matches the pattern element [p..ep).
 *
 * @param c   Character (unsigned)
 * @param p   Start of class in pattern
 * @param ep  End of class (from novai_classend)
 *
 * @return Non-zero if c matches.
 */
static int novai_singlematch(int c, const char *p, const char *ep) {
    if (c == '\0') {
        return 0;
    }
    switch (*p) {
        case '.':
            return 1;
        case '%':
            return novai_match_class(c, *(p + 1));
        case '[': {
            const char *endclass = ep - 1;  /* points to ']' */
            int sig = 1;
            p++;
            if (*p == '^') {
                sig = 0;
                p++;
            }
            while (p < endclass) {
                if (*p == '%') {
                    p++;
                    if (novai_match_class(c, *p)) {
                        return sig;
                    }
                } else if (p + 2 < endclass && *(p + 1) == '-') {
                    if ((unsigned char)*p <= (unsigned char)c &&
                        (unsigned char)c <= (unsigned char)*(p + 2)) {
                        return sig;
                    }
                    p += 2;
                } else {
                    if ((unsigned char)*p == (unsigned char)c) {
                        return sig;
                    }
                }
                p++;
            }
            return !sig;
        }
        default:
            return ((unsigned char)*p == (unsigned char)c);
    }
}

/* Forward declaration */
static const char *novai_match(NovaMatchState *ms,
                               const char *s, const char *p);

/** @brief Greedy quantifier expansion (* and +). */
static const char *novai_max_expand(NovaMatchState *ms, const char *s,
                                    const char *p, const char *ep) {
    ptrdiff_t i = 0;
    while (s + i < ms->src_end &&
           novai_singlematch((unsigned char)*(s + i), p, ep)) {
        i++;
    }
    while (i >= 0) {
        const char *res = novai_match(ms, s + i, ep + 1);
        if (res != NULL) {
            return res;
        }
        i--;
    }
    return NULL;
}

/** @brief Lazy quantifier expansion (-). */
static const char *novai_min_expand(NovaMatchState *ms, const char *s,
                                    const char *p, const char *ep) {
    for (;;) {
        const char *res = novai_match(ms, s, ep + 1);
        if (res != NULL) {
            return res;
        }
        if (s < ms->src_end &&
            novai_singlematch((unsigned char)*s, p, ep)) {
            s++;
        } else {
            return NULL;
        }
    }
}

/** @brief Start a capture group: '(' in pattern. */
static const char *novai_start_capture(NovaMatchState *ms, const char *s,
                                       const char *p, ptrdiff_t what) {
    int level = ms->level;
    if (level >= NOVAI_MAXCAPTURES) {
        return NULL;
    }
    ms->cap[level].init = s;
    ms->cap[level].len = what;
    ms->level = level + 1;
    const char *res = novai_match(ms, s, p);
    if (res == NULL) {
        ms->level--;
    }
    return res;
}

/** @brief End a capture group: ')' in pattern. */
static const char *novai_end_capture(NovaMatchState *ms, const char *s,
                                     const char *p) {
    int l = ms->level;
    for (l--; l >= 0; l--) {
        if (ms->cap[l].len == -2) {
            ms->cap[l].len = s - ms->cap[l].init;
            const char *res = novai_match(ms, s, p);
            if (res == NULL) {
                ms->cap[l].len = -2;
            }
            return res;
        }
    }
    return NULL;
}

/** @brief Match a back-reference %1-%9. */
static const char *novai_match_capture(NovaMatchState *ms, const char *s,
                                       int l) {
    l -= '1';
    if (l < 0 || l >= ms->level || ms->cap[l].len < 0) {
        return NULL;
    }
    size_t len = (size_t)ms->cap[l].len;
    if ((size_t)(ms->src_end - s) >= len &&
        memcmp(ms->cap[l].init, s, len) == 0) {
        return s + len;
    }
    return NULL;
}

/** @brief Match %bxy balanced pair. */
static const char *novai_matchbalance(NovaMatchState *ms, const char *s,
                                      const char *p) {
    if (p >= ms->p_end - 1) {
        return NULL;
    }
    if (*s != *p) {
        return NULL;
    }
    int b = (unsigned char)*p;
    int e = (unsigned char)*(p + 1);
    int cont = 1;
    while (++s < ms->src_end) {
        if ((unsigned char)*s == e) {
            if (--cont == 0) {
                return s + 1;
            }
        } else if ((unsigned char)*s == b) {
            cont++;
        }
    }
    return NULL;
}

/**
 * @brief Core recursive pattern matcher.
 *
 * Matches source string s against pattern p within the context
 * of match state ms.  Returns pointer past end of match on
 * success, NULL on failure.
 *
 * @param ms  Match state (captures, bounds, depth)
 * @param s   Current position in source
 * @param p   Current position in pattern
 *
 * @return End of match, or NULL.
 */
static const char *novai_match(NovaMatchState *ms,
                               const char *s, const char *p) {
    if (ms->depth++ > NOVAI_MATCH_DEPTH) {
        ms->depth--;
        return NULL;
    }

    while (1) {
        if (p >= ms->p_end) {
            ms->depth--;
            return s;
        }

        switch (*p) {
            case '(': {
                const char *res = NULL;
                if (*(p + 1) == ')') {
                    res = novai_start_capture(ms, s, p + 2, -1);
                } else {
                    res = novai_start_capture(ms, s, p + 1, -2);
                }
                ms->depth--;
                return res;
            }

            case ')': {
                const char *res = novai_end_capture(ms, s, p + 1);
                ms->depth--;
                return res;
            }

            case '$': {
                if (p + 1 >= ms->p_end) {
                    ms->depth--;
                    return (s == ms->src_end) ? s : NULL;
                }
                goto dflt;
            }

            case '%': {
                if (*(p + 1) == 'b') {
                    s = novai_matchbalance(ms, s, p + 2);
                    if (s == NULL) {
                        ms->depth--;
                        return NULL;
                    }
                    p += 4;
                    continue;
                }
                if (*(p + 1) >= '1' && *(p + 1) <= '9') {
                    s = novai_match_capture(ms, s, *(p + 1));
                    if (s == NULL) {
                        ms->depth--;
                        return NULL;
                    }
                    p += 2;
                    continue;
                }
                goto dflt;
            }

            default:
            dflt: {
                const char *ep = novai_classend(p);
                int sm = (s < ms->src_end &&
                          novai_singlematch((unsigned char)*s, p, ep));

                if (ep < ms->p_end) {
                    switch (*ep) {
                        case '?': {
                            if (sm) {
                                const char *res =
                                    novai_match(ms, s + 1, ep + 1);
                                if (res != NULL) {
                                    ms->depth--;
                                    return res;
                                }
                            }
                            p = ep + 1;
                            continue;
                        }
                        case '+': {
                            if (sm) {
                                const char *res =
                                    novai_max_expand(ms, s + 1, p, ep);
                                ms->depth--;
                                return res;
                            }
                            ms->depth--;
                            return NULL;
                        }
                        case '*': {
                            const char *res =
                                novai_max_expand(ms, s, p, ep);
                            ms->depth--;
                            return res;
                        }
                        case '-': {
                            const char *res =
                                novai_min_expand(ms, s, p, ep);
                            ms->depth--;
                            return res;
                        }
                        default:
                            break;
                    }
                }

                if (sm) {
                    s++;
                    p = ep;
                    continue;
                }
                ms->depth--;
                return NULL;
            }
        }
    }
}

/** @brief Initialize match state. */
static void novai_match_init(NovaMatchState *ms, NovaVM *vm,
                             const char *src, size_t src_len,
                             const char *pat, size_t pat_len) {
    ms->src_init = src;
    ms->src_end  = src + src_len;
    ms->p_end    = pat + pat_len;
    ms->vm       = vm;
    ms->level    = 0;
    ms->depth    = 0;
}

/**
 * @brief Push captures (or whole match) onto VM stack.
 *
 * @param ms           Match state with filled captures
 * @param match_start  Start of overall match
 * @param match_end    End of overall match
 *
 * @return Number of values pushed.
 */
static int novai_push_captures(NovaMatchState *ms,
                               const char *match_start,
                               const char *match_end) {
    int nlevels = ms->level;
    if (nlevels == 0) {
        size_t len = (size_t)(match_end - match_start);
        nova_vm_push_string(ms->vm, match_start, len);
        return 1;
    }
    for (int i = 0; i < nlevels; i++) {
        if (ms->cap[i].len == -1) {
            /* Position capture */
            ptrdiff_t pos = ms->cap[i].init - ms->src_init;
            nova_vm_push_integer(ms->vm, (nova_int_t)pos);
        } else if (ms->cap[i].len >= 0) {
            nova_vm_push_string(ms->vm,
                                ms->cap[i].init,
                                (size_t)ms->cap[i].len);
        } else {
            nova_vm_push_nil(ms->vm);
        }
    }
    return nlevels;
}

/**
 * @brief Append replacement string with capture substitution.
 *
 * In the replacement string:
 *   %0 = entire match,  %1-%9 = captures,  %% = literal %
 *
 * @param ms           Match state
 * @param buf          Output buffer (may be reallocated)
 * @param pos          Current write position
 * @param cap          Buffer capacity
 * @param rep          Replacement string
 * @param rlen         Replacement length
 * @param match_start  Start of match in source
 * @param match_end    End of match in source
 */
static void novai_add_replacement(NovaMatchState *ms,
                                  char **buf, size_t *pos, size_t *cap,
                                  const char *rep, size_t rlen,
                                  const char *match_start,
                                  const char *match_end) {
    for (size_t i = 0; i < rlen; i++) {
        if (rep[i] == '%' && i + 1 < rlen) {
            int d = rep[i + 1];
            if (d >= '0' && d <= '9') {
                const char *cs = NULL;
                size_t cl = 0;
                if (d == '0') {
                    cs = match_start;
                    cl = (size_t)(match_end - match_start);
                } else {
                    int idx = d - '1';
                    if (idx < ms->level && ms->cap[idx].len >= 0) {
                        cs = ms->cap[idx].init;
                        cl = (size_t)ms->cap[idx].len;
                    }
                }
                if (cs != NULL) {
                    while (*pos + cl >= *cap) {
                        *cap *= 2;
                        char *nb = (char *)realloc(*buf, *cap);
                        if (nb == NULL) {
                            return;
                        }
                        *buf = nb;
                    }
                    memcpy(*buf + *pos, cs, cl);
                    *pos += cl;
                }
                i++;
                continue;
            }
            if (d == '%') {
                i++;   /* skip second %, fall through to append it */
            }
        }
        if (*pos + 1 >= *cap) {
            *cap *= 2;
            char *nb = (char *)realloc(*buf, *cap);
            if (nb == NULL) {
                return;
            }
            *buf = nb;
        }
        (*buf)[(*pos)++] = rep[i];
    }
}

/* ============================================================
 * FIND (pattern or plain text search)
 * ============================================================ */

static int nova_string_find(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) {
        return -1;
    }

    NovaValue sv = nova_vm_get(vm, 0);
    NovaValue pv = nova_vm_get(vm, 1);

    if (!nova_is_string(sv) || !nova_is_string(pv)) {
        nova_vm_raise_error(vm, "string expected in 'find'");
        return -1;
    }

    const char *s = nova_str_data(nova_as_string(sv));
    size_t slen = nova_str_len(nova_as_string(sv));
    const char *pat = nova_str_data(nova_as_string(pv));
    size_t plen = nova_str_len(nova_as_string(pv));

    int nargs = nova_vm_get_top(vm);
    nova_int_t init = 0;
    if (nargs >= 3) {
        NovaValue iv = nova_vm_get(vm, 2);
        if (nova_is_integer(iv)) {
            init = nova_as_integer(iv);
        }
    }

    /* Optional plain flag (4th argument) */
    int plain = 0;
    if (nargs >= 4) {
        NovaValue fv = nova_vm_get(vm, 3);
        if (nova_is_bool(fv)) {
            plain = nova_as_bool(fv);
        } else {
            plain = (!nova_is_nil(fv));
        }
    }

    init = novai_str_posrelat(init, slen);
    if (init < 0) init = 0;

    /* Plain text search */
    if (plain || plen == 0) {
        if (plen == 0) {
            nova_vm_push_integer(vm, init);
            nova_vm_push_integer(vm, init - 1);
            return 2;
        }
        for (size_t i = (size_t)init; i + plen <= slen; i++) {
            if (memcmp(s + i, pat, plen) == 0) {
                nova_vm_push_integer(vm, (nova_int_t)i);
                nova_vm_push_integer(vm, (nova_int_t)(i + plen - 1));
                return 2;
            }
        }
        nova_vm_push_nil(vm);
        return 1;
    }

    /* Pattern search */
    const char *p = pat;

    int anchor = 0;
    if (plen > 0 && pat[0] == '^') {
        anchor = 1;
        p++;
        plen--;
    }

    NovaMatchState ms;
    novai_match_init(&ms, vm, s, slen, p, plen);

    for (size_t i = (size_t)init; i <= slen; i++) {
        ms.level = 0;
        ms.depth = 0;
        const char *res = novai_match(&ms, s + i, p);
        if (res != NULL) {
            nova_vm_push_integer(vm, (nova_int_t)i);
            nova_vm_push_integer(vm, (nova_int_t)(res - s - 1));
            int ncap = novai_push_captures(&ms, s + i, res);
            return 2 + ncap;
        }
        if (anchor) {
            break;
        }
    }

    nova_vm_push_nil(vm);
    return 1;
}

/* ============================================================
 * FORMAT (simplified printf-style)
 * ============================================================ */

/*
** PCM: format-nonliteral suppression
** Purpose: string.format() builds format specifiers at runtime by design
** Rationale: This IS a printf-implementation; dynamic format strings are
**   validated and constructed character-by-character from user input.
** Audit Date: 2026-02-08
*/
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"

int nova_string_format(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    NovaValue fmtv = nova_vm_get(vm, 0);
    if (!nova_is_string(fmtv)) {
        nova_vm_raise_error(vm, "bad argument #1 to 'format' (string expected)");
        return -1;
    }

    const char *fmt = nova_str_data(nova_as_string(fmtv));
    size_t fmtlen = nova_str_len(nova_as_string(fmtv));
    int nargs = nova_vm_get_top(vm);
    int arg_idx = 1;  /* Current argument index (0 = format string) */

    /* Build result into a dynamic buffer */
    size_t cap = fmtlen * 2 + 64;
    char *buf = (char *)malloc(cap);
    if (buf == NULL) {
        nova_vm_raise_error(vm, "memory allocation failed in 'format'");
        return -1;
    }
    size_t pos = 0;

    for (size_t i = 0; i < fmtlen; i++) {
        if (fmt[i] != '%') {
            /* Ensure space */
            if (pos + 1 >= cap) {
                cap *= 2;
                char *nb = (char *)realloc(buf, cap);
                if (nb == NULL) { free(buf); return -1; }
                buf = nb;
            }
            buf[pos++] = fmt[i];
            continue;
        }

        i++;  /* Skip '%' */
        if (i >= fmtlen) {
            break;
        }

        /* Handle %% */
        if (fmt[i] == '%') {
            if (pos + 1 >= cap) {
                cap *= 2;
                char *nb = (char *)realloc(buf, cap);
                if (nb == NULL) { free(buf); return -1; }
                buf = nb;
            }
            buf[pos++] = '%';
            continue;
        }

        /* Build sub-format string: capture flags, width, precision.
         * subfmt stores the full specifier (e.g. "%03d", "%-10.2f")
         * so we can delegate to snprintf with the correct formatting. */
        char subfmt[32];
        size_t sf = 0;
        subfmt[sf++] = '%';

        /* Capture flags */
        while (i < fmtlen && (fmt[i] == '-' || fmt[i] == '+' ||
               fmt[i] == ' ' || fmt[i] == '0' || fmt[i] == '#')) {
            if (sf < sizeof(subfmt) - 4) subfmt[sf++] = fmt[i];
            i++;
        }
        /* Capture width */
        while (i < fmtlen && fmt[i] >= '0' && fmt[i] <= '9') {
            if (sf < sizeof(subfmt) - 4) subfmt[sf++] = fmt[i];
            i++;
        }
        /* Capture precision */
        if (i < fmtlen && fmt[i] == '.') {
            if (sf < sizeof(subfmt) - 4) subfmt[sf++] = '.';
            i++;
            while (i < fmtlen && fmt[i] >= '0' && fmt[i] <= '9') {
                if (sf < sizeof(subfmt) - 4) subfmt[sf++] = fmt[i];
                i++;
            }
        }

        if (i >= fmtlen) {
            break;
        }

        char spec = fmt[i];

        if (arg_idx >= nargs) {
            nova_vm_raise_error(vm, "bad argument #%d to 'format' (no value)",
                               arg_idx + 1);
            free(buf);
            return -1;
        }

        NovaValue arg = nova_vm_get(vm, arg_idx++);

        /* Ensure enough output space */
        if (pos + 64 >= cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (nb == NULL) { free(buf); return -1; }
            buf = nb;
        }

        switch (spec) {
            case 'd':
            case 'i': {
                nova_int_t val = 0;
                if (nova_is_integer(arg)) {
                    val = nova_as_integer(arg);
                } else if (nova_is_number(arg)) {
                    val = (nova_int_t)nova_as_number(arg);
                }
                /* Append "lld" to captured subfmt for long long */
                subfmt[sf++] = 'l';
                subfmt[sf++] = 'l';
                subfmt[sf++] = spec;
                subfmt[sf] = '\0';
                int n = snprintf(buf + pos, cap - pos, subfmt,
                                 (long long)val);
                if (n > 0) pos += (size_t)n;
                break;
            }
            case 'f':
            case 'e':
            case 'g': {
                nova_number_t val = 0.0;
                if (nova_is_number(arg)) {
                    val = nova_as_number(arg);
                } else if (nova_is_integer(arg)) {
                    val = (nova_number_t)nova_as_integer(arg);
                }
                subfmt[sf++] = spec;
                subfmt[sf] = '\0';
                int n = snprintf(buf + pos, cap - pos, subfmt, val);
                if (n > 0) pos += (size_t)n;
                break;
            }
            case 's': {
                const char *sv2 = "nil";
                size_t sv2len = 3;
                if (nova_is_string(arg)) {
                    sv2 = nova_str_data(nova_as_string(arg));
                    sv2len = nova_str_len(nova_as_string(arg));
                }
                /* Use subfmt for width/precision on strings too */
                subfmt[sf++] = 's';
                subfmt[sf] = '\0';
                /* Ensure space */
                if (pos + sv2len + 64 >= cap) {
                    cap = pos + sv2len + 128;
                    char *nb = (char *)realloc(buf, cap);
                    if (nb == NULL) { free(buf); return -1; }
                    buf = nb;
                }
                int n = snprintf(buf + pos, cap - pos, subfmt, sv2);
                if (n > 0) pos += (size_t)n;
                break;
            }
            case 'x':
            case 'X':
            case 'o': {
                nova_int_t val = 0;
                if (nova_is_integer(arg)) {
                    val = nova_as_integer(arg);
                } else if (nova_is_number(arg)) {
                    val = (nova_int_t)nova_as_number(arg);
                }
                subfmt[sf++] = 'l';
                subfmt[sf++] = 'l';
                subfmt[sf++] = spec;
                subfmt[sf] = '\0';
                int n = snprintf(buf + pos, cap - pos, subfmt,
                                 (long long)val);
                if (n > 0) pos += (size_t)n;
                break;
            }
            case 'c': {
                nova_int_t val = 0;
                if (nova_is_integer(arg)) {
                    val = nova_as_integer(arg);
                }
                if (val >= 0 && val <= 127) {
                    buf[pos++] = (char)val;
                }
                break;
            }
            case 'q': {
                /* Quoted string */
                const char *qs = "nil";
                size_t qslen = 3;
                if (nova_is_string(arg)) {
                    qs = nova_str_data(nova_as_string(arg));
                    qslen = nova_str_len(nova_as_string(arg));
                }
                /* Ensure space for worst case: every char escaped + quotes */
                if (pos + qslen * 2 + 4 >= cap) {
                    cap = pos + qslen * 2 + 64;
                    char *nb = (char *)realloc(buf, cap);
                    if (nb == NULL) { free(buf); return -1; }
                    buf = nb;
                }
                buf[pos++] = '"';
                for (size_t k = 0; k < qslen; k++) {
                    char c = qs[k];
                    if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\0') {
                        buf[pos++] = '\\';
                        if (c == '\n') { buf[pos++] = 'n'; }
                        else if (c == '\r') { buf[pos++] = 'r'; }
                        else if (c == '\0') { buf[pos++] = '0'; }
                        else { buf[pos++] = c; }
                    } else {
                        buf[pos++] = c;
                    }
                }
                buf[pos++] = '"';
                break;
            }
            default:
                break;
        }
    }

    buf[pos] = '\0';
    nova_vm_push_string(vm, buf, pos);
    free(buf);
    return 1;
}

#pragma GCC diagnostic pop

/* ============================================================
 * GSUB (plain text replace)
 * ============================================================ */

static int nova_string_gsub(NovaVM *vm) {
    if (nova_lib_check_args(vm, 3) != 0) {
        return -1;
    }

    NovaValue sv = nova_vm_get(vm, 0);
    NovaValue pv = nova_vm_get(vm, 1);
    NovaValue rv = nova_vm_get(vm, 2);

    if (!nova_is_string(sv) || !nova_is_string(pv) ||
        !nova_is_string(rv)) {
        nova_vm_raise_error(vm, "string expected in 'gsub'");
        return -1;
    }

    const char *s = nova_str_data(nova_as_string(sv));
    size_t slen = nova_str_len(nova_as_string(sv));
    const char *pat = nova_str_data(nova_as_string(pv));
    size_t plen = nova_str_len(nova_as_string(pv));
    const char *rep = nova_str_data(nova_as_string(rv));
    size_t rlen = nova_str_len(nova_as_string(rv));

    int nargs = nova_vm_get_top(vm);
    nova_int_t max_sub = -1;  /* -1 = unlimited */
    if (nargs >= 4) {
        NovaValue nv = nova_vm_get(vm, 3);
        if (nova_is_integer(nv)) {
            max_sub = nova_as_integer(nv);
        }
    }

    if (plen == 0) {
        nova_vm_push_string(vm, s, slen);
        nova_vm_push_integer(vm, 0);
        return 2;
    }

    /* Handle anchor */
    const char *p = pat;
    int anchor = 0;
    if (pat[0] == '^') {
        anchor = 1;
        p++;
        plen--;
    }

    /* Build result */
    size_t cap = slen * 2 + 64;
    char *buf = (char *)malloc(cap);
    if (buf == NULL) {
        nova_vm_raise_error(vm, "memory allocation failed");
        return -1;
    }

    size_t pos = 0;
    nova_int_t count = 0;
    size_t src_pos = 0;

    NovaMatchState ms;
    novai_match_init(&ms, vm, s, slen, p, plen);

    while (src_pos <= slen) {
        if (max_sub >= 0 && count >= max_sub) {
            break;
        }

        ms.level = 0;
        ms.depth = 0;
        const char *match_end = novai_match(&ms, s + src_pos, p);

        if (match_end != NULL) {
            /* Copy pre-match text */
            size_t pre = src_pos;
            if (pos + pre >= cap) {
                cap = (pos + pre) * 2 + 64;
                char *nb = (char *)realloc(buf, cap);
                if (nb == NULL) { free(buf); return -1; }
                buf = nb;
            }

            /* Append replacement with capture substitution */
            novai_add_replacement(&ms, &buf, &pos, &cap,
                                  rep, rlen,
                                  s + src_pos, match_end);
            count++;

            /* Advance past match (avoid infinite loop on empty match) */
            if (match_end == s + src_pos) {
                if (src_pos < slen) {
                    if (pos + 1 >= cap) {
                        cap *= 2;
                        char *nb = (char *)realloc(buf, cap);
                        if (nb == NULL) { free(buf); return -1; }
                        buf = nb;
                    }
                    buf[pos++] = s[src_pos];
                }
                src_pos++;
            } else {
                src_pos = (size_t)(match_end - s);
            }
        } else {
            /* No match: copy one character */
            if (src_pos < slen) {
                if (pos + 1 >= cap) {
                    cap *= 2;
                    char *nb = (char *)realloc(buf, cap);
                    if (nb == NULL) { free(buf); return -1; }
                    buf = nb;
                }
                buf[pos++] = s[src_pos];
            }
            src_pos++;
        }

        if (anchor) {
            break;
        }
    }

    /* Copy remainder */
    if (src_pos <= slen) {
        size_t remain = slen - src_pos;
        if (pos + remain >= cap) {
            cap = pos + remain + 1;
            char *nb = (char *)realloc(buf, cap);
            if (nb == NULL) { free(buf); return -1; }
            buf = nb;
        }
        memcpy(buf + pos, s + src_pos, remain);
        pos += remain;
    }

    nova_vm_push_string(vm, buf, pos);
    nova_vm_push_integer(vm, count);
    free(buf);
    return 2;
}

/* ============================================================
 * MATCH (pattern-aware, returns captures or whole match)
 * ============================================================ */

static int nova_string_match(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) {
        return -1;
    }

    NovaValue sv = nova_vm_get(vm, 0);
    NovaValue pv = nova_vm_get(vm, 1);

    if (!nova_is_string(sv) || !nova_is_string(pv)) {
        nova_vm_raise_error(vm, "string expected in 'match'");
        return -1;
    }

    const char *s = nova_str_data(nova_as_string(sv));
    size_t slen = nova_str_len(nova_as_string(sv));
    const char *pat = nova_str_data(nova_as_string(pv));
    size_t plen = nova_str_len(nova_as_string(pv));

    int nargs = nova_vm_get_top(vm);
    nova_int_t init = 0;
    if (nargs >= 3) {
        NovaValue iv = nova_vm_get(vm, 2);
        if (nova_is_integer(iv)) {
            init = nova_as_integer(iv);
        }
    }
    init = novai_str_posrelat(init, slen);
    if (init < 0) init = 0;

    const char *p = pat;
    int anchor = 0;
    if (plen > 0 && pat[0] == '^') {
        anchor = 1;
        p++;
        plen--;
    }

    NovaMatchState ms;
    novai_match_init(&ms, vm, s, slen, p, plen);

    for (size_t i = (size_t)init; i <= slen; i++) {
        ms.level = 0;
        ms.depth = 0;
        const char *res = novai_match(&ms, s + i, p);
        if (res != NULL) {
            return novai_push_captures(&ms, s + i, res);
        }
        if (anchor) {
            break;
        }
    }

    nova_vm_push_nil(vm);
    return 1;
}

/* ============================================================
 * GMATCH (returns table of all matches)
 *
 * string.gmatch(s, pattern) -> table
 *
 * Returns a table containing all successive matches.
 * Each entry is the whole match (no captures) or first
 * capture (if captures exist).  For multi-capture patterns,
 * each entry is a sub-table of captures.
 * ============================================================ */

static int nova_string_gmatch(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) {
        return -1;
    }

    NovaValue sv = nova_vm_get(vm, 0);
    NovaValue pv = nova_vm_get(vm, 1);

    if (!nova_is_string(sv) || !nova_is_string(pv)) {
        nova_vm_raise_error(vm, "string expected in 'gmatch'");
        return -1;
    }

    const char *s = nova_str_data(nova_as_string(sv));
    size_t slen = nova_str_len(nova_as_string(sv));
    const char *pat = nova_str_data(nova_as_string(pv));
    size_t plen = nova_str_len(nova_as_string(pv));

    const char *p = pat;
    if (plen > 0 && pat[0] == '^') {
        p++;
        plen--;
    }

    /* Create result table */
    nova_vm_push_table(vm);
    NovaValue tval = nova_vm_get(vm, -1);
    if (!nova_is_table(tval)) {
        return 1;
    }
    NovaTable *t = nova_as_table(tval);

    NovaMatchState ms;
    novai_match_init(&ms, vm, s, slen, p, plen);

    size_t src_pos = 0;
    nova_int_t idx = 0;

    while (src_pos <= slen) {
        ms.level = 0;
        ms.depth = 0;
        const char *res = novai_match(&ms, s + src_pos, p);
        if (res != NULL) {
            NovaValue entry = nova_value_nil();

            if (ms.level == 0) {
                /* No captures: store whole match */
                size_t mlen = (size_t)(res - (s + src_pos));
                NovaString *ns = nova_vm_intern_string(vm,
                    s + src_pos, mlen);
                if (ns != NULL) {
                    entry = nova_value_string(ns);
                }
            } else if (ms.level == 1 && ms.cap[0].len >= 0) {
                /* Single capture */
                NovaString *ns = nova_vm_intern_string(vm,
                    ms.cap[0].init, (size_t)ms.cap[0].len);
                if (ns != NULL) {
                    entry = nova_value_string(ns);
                }
            } else {
                /* Multiple captures: store as sub-table */
                nova_vm_push_table(vm);
                NovaValue stval = nova_vm_get(vm, -1);
                if (nova_is_table(stval)) {
                    NovaTable *sub = nova_as_table(stval);
                    for (int ci = 0; ci < ms.level; ci++) {
                        if (ms.cap[ci].len >= 0) {
                            NovaString *cs = nova_vm_intern_string(vm,
                                ms.cap[ci].init,
                                (size_t)ms.cap[ci].len);
                            if (cs != NULL) {
                                nova_table_raw_set_int(vm, sub,
                                    (nova_int_t)ci,
                                    nova_value_string(cs));
                            }
                        }
                    }
                    entry = stval;
                }
                nova_vm_pop(vm, 1);  /* pop sub-table */
            }

            nova_table_raw_set_int(vm, t, idx, entry);
            idx++;

            /* Advance past match */
            if (res == s + src_pos) {
                src_pos++;
            } else {
                src_pos = (size_t)(res - s);
            }
        } else {
            src_pos++;
        }
    }

    return 1;  /* result table on stack */
}

/* ============================================================
 * REGISTRATION TABLE
 * ============================================================ */

static const NovaLibReg nova_string_lib[] = {
    {"len",     nova_string_len},
    {"sub",     nova_string_sub},
    {"upper",   nova_string_upper},
    {"lower",   nova_string_lower},
    {"rep",     nova_string_rep},
    {"reverse", nova_string_reverse},
    {"byte",    nova_string_byte},
    {"char",    nova_string_char},
    {"find",    nova_string_find},
    {"format",  nova_string_format},
    {"gsub",    nova_string_gsub},
    {"match",   nova_string_match},
    {"gmatch",  nova_string_gmatch},
    {NULL,      NULL}
};

/* ============================================================
 * MODULE OPENER
 * ============================================================ */

int nova_open_string(NovaVM *vm) {
    if (vm == NULL) {
        return -1;
    }

    nova_lib_register_module(vm, "string", nova_string_lib);

    /* === String metatable ===
     * Create a metatable with __index = string module table.
     * This allows method-call syntax on strings:
     *   s:find(pat)    -->  string.find(s, pat)
     *   s:upper()      -->  string.upper(s)
     *   s:sub(0, 3)    -->  string.sub(s, 0, 3)
     */
    NovaValue str_mod = nova_vm_get_global(vm, "string");
    if (nova_is_table(str_mod)) {
        /* Create metatable */
        nova_vm_push_table(vm);
        NovaValue mt_val = nova_vm_get(vm, -1);
        vm->stack_top--;

        if (nova_is_table(mt_val)) {
            NovaTable *mt = nova_as_table(mt_val);

            /* Set __index = string module table */
            NovaString *idx_key = nova_string_new(vm, "__index", 7);
            if (idx_key != NULL) {
                nova_table_set_str(vm, mt, idx_key, str_mod);
            }

            /* Mark metatable as immortal so GC never collects it */
            ((NovaGCHeader *)mt)->gc_color = NOVA_GC_BLACK;

            /* Register with the metamethod pipeline (GC-rooted via VM) */
            nova_meta_set_string_mt(vm, mt);
        }
    }

    return 0;
}
