/**
 * @file nova_ndp.c
 * @brief Nova Data Processor (NDP) - Unified Multi-Format Codec
 *
 * Single-file implementation of decode/encode for:
 *   JSON, CSV/TSV, INI, TOML, HTML
 *
 * Architecture:
 *   PART 1: String Builder (shared growable buffer)
 *   PART 2: Shared Helpers (whitespace, number parse, escapes)
 *   PART 3: Format Detection + Utilities
 *   PART 4: JSON Codec
 *   PART 5: CSV/TSV Codec
 *   PART 6: INI Codec
 *   PART 7: TOML Codec
 *   PART 8: HTML Codec
 *   PART 9: Public API (ndp_decode / ndp_encode dispatch)
 *
 * @author Anthony Taliento
 * @date 2026-02-10
 * @version 1.0.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 *
 * DEPENDENCIES:
 *   - nova_vm.h (table creation, value push/get)
 *   - nova_ndp.h (public types)
 *
 * THREAD SAFETY:
 *   Not thread-safe. Operates on a single VM instance.
 */

#include "nova/nova_ndp.h"
#include "nova/nova_vm.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ============================================================
 * PART 1: STRING BUILDER
 * ============================================================ */

#define NDP_BUF_INIT_CAP 256

void ndp_buf_init(NdpBuf *buf) {
    if (buf == NULL) {
        return;
    }
    buf->data = NULL;
    buf->len  = 0;
    buf->cap  = 0;
}

void ndp_buf_free(NdpBuf *buf) {
    if (buf == NULL) {
        return;
    }
    free(buf->data);
    buf->data = NULL;
    buf->len  = 0;
    buf->cap  = 0;
}

/**
 * @brief Ensure buf has room for at least `extra` more bytes.
 */
static int ndp_buf_grow(NdpBuf *buf, size_t extra) {
    if (buf == NULL) {
        return -1;
    }
    size_t needed = buf->len + extra;
    if (needed <= buf->cap) {
        return 0;
    }
    size_t newcap = (buf->cap == 0) ? NDP_BUF_INIT_CAP : buf->cap;
    while (newcap < needed) {
        newcap *= 2;
    }
    char *tmp = (char *)realloc(buf->data, newcap);
    if (tmp == NULL) {
        return -1;
    }
    buf->data = tmp;
    buf->cap  = newcap;
    return 0;
}

void ndp_buf_append(NdpBuf *buf, const char *data, size_t len) {
    if (buf == NULL || data == NULL || len == 0) {
        return;
    }
    if (ndp_buf_grow(buf, len) != 0) {
        return;
    }
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
}

void ndp_buf_puts(NdpBuf *buf, const char *s) {
    if (s == NULL) {
        return;
    }
    ndp_buf_append(buf, s, strlen(s));
}

void ndp_buf_putc(NdpBuf *buf, char c) {
    ndp_buf_append(buf, &c, 1);
}

void ndp_buf_printf(NdpBuf *buf, const char *fmt, ...) {
    if (buf == NULL || fmt == NULL) {
        return;
    }
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) {
        ndp_buf_append(buf, tmp, (size_t)n);
    }
}

/* ============================================================
 * PART 2: SHARED HELPERS
 * ============================================================ */

/** Parser state for all codecs */
typedef struct {
    const char *src;    /**< Input text         */
    size_t      len;    /**< Total length       */
    size_t      pos;    /**< Current position   */
    int         line;   /**< Current line (1+)  */
    int         col;    /**< Current column (1+)*/
    NovaVM     *vm;     /**< VM for allocation  */
    char       *errbuf; /**< Error output       */
} NdpParser;

static void ndp_parser_init(NdpParser *P, NovaVM *vm,
                             const char *src, size_t len,
                             char *errbuf) {
    if (P == NULL) {
        return;
    }
    P->src    = src;
    P->len    = len;
    P->pos    = 0;
    P->line   = 1;
    P->col    = 1;
    P->vm     = vm;
    P->errbuf = errbuf;
}

static void ndp_error(NdpParser *P, const char *fmt, ...) {
    if (P == NULL || P->errbuf == NULL) {
        return;
    }
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    (void)snprintf(P->errbuf, 256, "line %d col %d: %s",
                   P->line, P->col, msg);
}

static int ndp_eof(const NdpParser *P) {
    return P->pos >= P->len;
}

static char ndp_peek(const NdpParser *P) {
    if (P->pos >= P->len) {
        return '\0';
    }
    return P->src[P->pos];
}

static char ndp_advance(NdpParser *P) {
    if (P->pos >= P->len) {
        return '\0';
    }
    char c = P->src[P->pos];
    P->pos++;
    if (c == '\n') {
        P->line++;
        P->col = 1;
    } else {
        P->col++;
    }
    return c;
}

static void ndp_skip_whitespace(NdpParser *P) {
    while (!ndp_eof(P)) {
        char c = ndp_peek(P);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ndp_advance(P);
        } else {
            break;
        }
    }
}

static int ndp_match(NdpParser *P, char expected) {
    if (ndp_peek(P) == expected) {
        ndp_advance(P);
        return 1;
    }
    return 0;
}

/** Check if string at current position matches literal (does not advance). */
static int ndp_check_literal(const NdpParser *P, const char *lit) {
    size_t slen = strlen(lit);
    if (P->pos + slen > P->len) {
        return 0;
    }
    return memcmp(P->src + P->pos, lit, slen) == 0;
}

/** Consume literal and advance. Returns 1 on success. */
static int ndp_consume_literal(NdpParser *P, const char *lit) {
    size_t slen = strlen(lit);
    if (P->pos + slen > P->len) {
        return 0;
    }
    if (memcmp(P->src + P->pos, lit, slen) != 0) {
        return 0;
    }
    size_t i = 0;
    for (i = 0; i < slen; i++) {
        ndp_advance(P);
    }
    return 1;
}

/* ============================================================
 * PART 3: FORMAT DETECTION + UTILITIES
 * ============================================================ */

void ndp_options_init(NdpOptions *opts) {
    if (opts == NULL) {
        return;
    }
    memset(opts, 0, sizeof(*opts));
    opts->format        = NDP_FORMAT_UNKNOWN;
    opts->csv_delimiter = ',';
    opts->csv_header    = 1;
    opts->csv_quote     = '"';
    opts->json_strict   = 0;
    opts->pretty        = 1;
    opts->indent        = 2;
    opts->ini_typed     = 1;
    opts->html_text_only = 0;
    opts->nini_interpolate = 1;
    opts->nini_tasks_only  = 0;
}

const char *ndp_format_name(NdpFormat fmt) {
    switch (fmt) {
        case NDP_FORMAT_JSON: return "json";
        case NDP_FORMAT_CSV:  return "csv";
        case NDP_FORMAT_TSV:  return "tsv";
        case NDP_FORMAT_INI:  return "ini";
        case NDP_FORMAT_TOML: return "toml";
        case NDP_FORMAT_HTML: return "html";
        case NDP_FORMAT_YAML: return "yaml";
        case NDP_FORMAT_NINI: return "nini";
        default:              return "unknown";
    }
}

NdpFormat ndp_format_from_name(const char *name) {
    if (name == NULL) {
        return NDP_FORMAT_UNKNOWN;
    }
    /* Case-insensitive comparison for short strings */
    if (strcasecmp(name, "json") == 0) { return NDP_FORMAT_JSON; }
    if (strcasecmp(name, "csv") == 0)  { return NDP_FORMAT_CSV; }
    if (strcasecmp(name, "tsv") == 0)  { return NDP_FORMAT_TSV; }
    if (strcasecmp(name, "ini") == 0)  { return NDP_FORMAT_INI; }
    if (strcasecmp(name, "toml") == 0) { return NDP_FORMAT_TOML; }
    if (strcasecmp(name, "html") == 0) { return NDP_FORMAT_HTML; }
    if (strcasecmp(name, "htm") == 0)  { return NDP_FORMAT_HTML; }
    if (strcasecmp(name, "yaml") == 0) { return NDP_FORMAT_YAML; }
    if (strcasecmp(name, "yml") == 0)  { return NDP_FORMAT_YAML; }
    if (strcasecmp(name, "nini") == 0) { return NDP_FORMAT_NINI; }
    return NDP_FORMAT_UNKNOWN;
}

NdpFormat ndp_detect(const char *text, size_t len) {
    if (text == NULL || len == 0) {
        return NDP_FORMAT_UNKNOWN;
    }

    /* Skip leading whitespace/BOM */
    size_t i = 0;
    /* Skip UTF-8 BOM */
    if (len >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
        i = 3;
    }
    while (i < len && (text[i] == ' ' || text[i] == '\t' ||
                       text[i] == '\r' || text[i] == '\n')) {
        i++;
    }
    if (i >= len) {
        return NDP_FORMAT_UNKNOWN;
    }

    char first = text[i];

    /* HTML: starts with < and has tag-like structure */
    if (first == '<') {
        /* Check for common HTML markers */
        if (len - i >= 5) {
            if (strncasecmp(text + i, "<!doc", 5) == 0 ||
                strncasecmp(text + i, "<html", 5) == 0 ||
                strncasecmp(text + i, "<head", 5) == 0 ||
                strncasecmp(text + i, "<body", 5) == 0 ||
                strncasecmp(text + i, "<div ", 5) == 0 ||
                strncasecmp(text + i, "<p>",   3) == 0 ||
                strncasecmp(text + i, "<h1",   3) == 0 ||
                strncasecmp(text + i, "<ul",   3) == 0) {
                return NDP_FORMAT_HTML;
            }
        }
        /* Generic tag */
        if (i + 1 < len && (isalpha((unsigned char)text[i + 1]) ||
                             text[i + 1] == '!' || text[i + 1] == '?')) {
            return NDP_FORMAT_HTML;
        }
    }

    /* JSON: starts with { */
    if (first == '{') {
        return NDP_FORMAT_JSON;
    }

    /* [ could be JSON array or INI/TOML [section].
       INI/TOML: [letter...], JSON: [value...] */
    if (first == '[') {
        size_t ni = i + 1;
        /* Skip possible [[ (TOML array of tables) */
        if (ni < len && text[ni] == '[') {
            /* Could be TOML [[section]], scan ahead to see if it closes ]] */
            /* Fall through to line-scanning heuristic below */
        } else if (ni < len && isalpha((unsigned char)text[ni])) {
            /* [section] pattern — likely INI or TOML, not JSON */
            /* Fall through to line-scanning heuristic below */
        } else {
            return NDP_FORMAT_JSON;
        }
    }

    /* JSON: starts with " or digit or true/false/null */
    if (first == '"' || first == '-' || (first >= '0' && first <= '9')) {
        return NDP_FORMAT_JSON;
    }
    if (len - i >= 4 && (strncmp(text + i, "true", 4) == 0 ||
                         strncmp(text + i, "null", 4) == 0)) {
        return NDP_FORMAT_JSON;
    }
    if (len - i >= 5 && strncmp(text + i, "false", 5) == 0) {
        return NDP_FORMAT_JSON;
    }

    /* Scan first few lines to distinguish INI/TOML vs CSV vs YAML */
    int has_section = 0;
    int has_equals  = 0;
    int has_comma   = 0;
    int has_tab     = 0;
    int has_colon_space = 0;
    int has_dash_seq   = 0;
    int lines_seen  = 0;
    size_t j = i;

    while (j < len && lines_seen < 10) {
        size_t line_start = j;
        while (j < len && text[j] != '\n') {
            j++;
        }
        size_t line_end = j;
        if (j < len) {
            j++; /* skip newline */
        }
        lines_seen++;

        /* Skip empty/comment lines */
        size_t k = line_start;
        while (k < line_end && (text[k] == ' ' || text[k] == '\t')) {
            k++;
        }
        if (k >= line_end || text[k] == '#' || text[k] == ';') {
            continue;
        }

        /* Check for [section] */
        if (text[k] == '[') {
            has_section = 1;
        }

        /* Check for "- " sequence item at start of content */
        if (text[k] == '-' && k + 1 < line_end && text[k + 1] == ' ') {
            has_dash_seq = 1;
        }

        /* Check for key=value, key: value, or CSV/TSV patterns */
        for (size_t m = k; m < line_end; m++) {
            if (text[m] == '=' || (text[m] == ':' && m + 1 < line_end &&
                                    text[m + 1] == ' ')) {
                if (text[m] == '=') {
                    has_equals = 1;
                } else {
                    has_colon_space = 1;
                }
                break;
            }
            if (text[m] == ',') {
                has_comma = 1;
            }
            if (text[m] == '\t') {
                has_tab = 1;
            }
        }
    }

    /* INI: has [section] headers */
    if (has_section && has_equals) {
        /* Distinguish TOML from INI: TOML has typed values, arrays, etc.
           Simple heuristic: if we see [[ ]] (array of tables), it's TOML */
        for (j = i; j + 1 < len; j++) {
            if (text[j] == '[' && text[j + 1] == '[') {
                return NDP_FORMAT_TOML;
            }
        }
        /* Default to INI for [section]+key=value */
        return NDP_FORMAT_INI;
    }

    /* Key=value without sections could be INI or TOML */
    if (has_equals && !has_comma) {
        return NDP_FORMAT_TOML;
    }

    /* YAML: colon-space key:value (no = sign) or sequence items (- ) */
    if ((has_colon_space || has_dash_seq) && !has_equals && !has_comma) {
        return NDP_FORMAT_YAML;
    }

    /* TSV: tabs but no commas */
    if (has_tab && !has_comma) {
        return NDP_FORMAT_TSV;
    }

    /* CSV: commas */
    if (has_comma) {
        return NDP_FORMAT_CSV;
    }

    return NDP_FORMAT_UNKNOWN;
}

/* ============================================================
 * PART 4: JSON CODEC
 *
 * Recursive descent parser (RFC 8259 compliant).
 * Decode: text -> NovaTable/value on VM stack.
 * Encode: Nova value -> JSON text in NdpBuf.
 * ============================================================ */

/* --- JSON Decode --- */

static int ndp_json_decode_value(NdpParser *P);

/**
 * @brief Decode a JSON string literal.
 * Handles escape sequences: \", \\, \/, \b, \f, \n, \r, \t, \uXXXX
 * Pushes the decoded string onto the VM stack.
 */
static int ndp_json_decode_string(NdpParser *P) {
    if (!ndp_match(P, '"')) {
        ndp_error(P, "expected '\"' at start of string");
        return -1;
    }

    NdpBuf tmp;
    ndp_buf_init(&tmp);

    while (!ndp_eof(P)) {
        char c = ndp_peek(P);
        if (c == '"') {
            ndp_advance(P);
            /* Push string to VM */
            const char *s = (tmp.data != NULL) ? tmp.data : "";
            nova_vm_push_string(P->vm, s, tmp.len);
            ndp_buf_free(&tmp);
            return 0;
        }
        if (c == '\\') {
            ndp_advance(P);
            char esc = ndp_advance(P);
            switch (esc) {
                case '"':  ndp_buf_putc(&tmp, '"');  break;
                case '\\': ndp_buf_putc(&tmp, '\\'); break;
                case '/':  ndp_buf_putc(&tmp, '/');  break;
                case 'b':  ndp_buf_putc(&tmp, '\b'); break;
                case 'f':  ndp_buf_putc(&tmp, '\f'); break;
                case 'n':  ndp_buf_putc(&tmp, '\n'); break;
                case 'r':  ndp_buf_putc(&tmp, '\r'); break;
                case 't':  ndp_buf_putc(&tmp, '\t'); break;
                case 'u': {
                    /* \uXXXX -> decode to UTF-8 */
                    unsigned int cp = 0;
                    int di = 0;
                    for (di = 0; di < 4; di++) {
                        char h = ndp_advance(P);
                        cp <<= 4;
                        if (h >= '0' && h <= '9') {
                            cp |= (unsigned int)(h - '0');
                        } else if (h >= 'a' && h <= 'f') {
                            cp |= (unsigned int)(h - 'a' + 10);
                        } else if (h >= 'A' && h <= 'F') {
                            cp |= (unsigned int)(h - 'A' + 10);
                        } else {
                            ndp_error(P, "invalid hex in \\u escape");
                            ndp_buf_free(&tmp);
                            return -1;
                        }
                    }
                    /* Handle surrogate pairs */
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (!ndp_match(P, '\\') || !ndp_match(P, 'u')) {
                            ndp_error(P, "expected \\u for surrogate pair");
                            ndp_buf_free(&tmp);
                            return -1;
                        }
                        unsigned int lo = 0;
                        for (di = 0; di < 4; di++) {
                            char h = ndp_advance(P);
                            lo <<= 4;
                            if (h >= '0' && h <= '9') {
                                lo |= (unsigned int)(h - '0');
                            } else if (h >= 'a' && h <= 'f') {
                                lo |= (unsigned int)(h - 'a' + 10);
                            } else if (h >= 'A' && h <= 'F') {
                                lo |= (unsigned int)(h - 'A' + 10);
                            } else {
                                ndp_error(P, "invalid hex in surrogate");
                                ndp_buf_free(&tmp);
                                return -1;
                            }
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    /* Encode as UTF-8 */
                    if (cp < 0x80) {
                        ndp_buf_putc(&tmp, (char)cp);
                    } else if (cp < 0x800) {
                        ndp_buf_putc(&tmp, (char)(0xC0 | (cp >> 6)));
                        ndp_buf_putc(&tmp, (char)(0x80 | (cp & 0x3F)));
                    } else if (cp < 0x10000) {
                        ndp_buf_putc(&tmp, (char)(0xE0 | (cp >> 12)));
                        ndp_buf_putc(&tmp, (char)(0x80 | ((cp >> 6) & 0x3F)));
                        ndp_buf_putc(&tmp, (char)(0x80 | (cp & 0x3F)));
                    } else {
                        ndp_buf_putc(&tmp, (char)(0xF0 | (cp >> 18)));
                        ndp_buf_putc(&tmp, (char)(0x80 | ((cp >> 12) & 0x3F)));
                        ndp_buf_putc(&tmp, (char)(0x80 | ((cp >> 6) & 0x3F)));
                        ndp_buf_putc(&tmp, (char)(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default:
                    ndp_error(P, "invalid escape '\\%c'", esc);
                    ndp_buf_free(&tmp);
                    return -1;
            }
        } else {
            ndp_buf_putc(&tmp, c);
            ndp_advance(P);
        }
    }

    ndp_error(P, "unterminated string");
    ndp_buf_free(&tmp);
    return -1;
}

/**
 * @brief Decode a JSON number.
 * Pushes integer or number onto VM stack.
 */
static int ndp_json_decode_number(NdpParser *P) {
    size_t start = P->pos;
    int is_float = 0;

    /* Optional minus */
    if (ndp_peek(P) == '-') {
        ndp_advance(P);
    }

    /* Integer part */
    if (ndp_peek(P) == '0') {
        ndp_advance(P);
    } else if (ndp_peek(P) >= '1' && ndp_peek(P) <= '9') {
        while (!ndp_eof(P) && ndp_peek(P) >= '0' && ndp_peek(P) <= '9') {
            ndp_advance(P);
        }
    } else {
        ndp_error(P, "invalid number");
        return -1;
    }

    /* Fractional part */
    if (!ndp_eof(P) && ndp_peek(P) == '.') {
        is_float = 1;
        ndp_advance(P);
        if (ndp_eof(P) || ndp_peek(P) < '0' || ndp_peek(P) > '9') {
            ndp_error(P, "expected digit after '.'");
            return -1;
        }
        while (!ndp_eof(P) && ndp_peek(P) >= '0' && ndp_peek(P) <= '9') {
            ndp_advance(P);
        }
    }

    /* Exponent */
    if (!ndp_eof(P) && (ndp_peek(P) == 'e' || ndp_peek(P) == 'E')) {
        is_float = 1;
        ndp_advance(P);
        if (!ndp_eof(P) && (ndp_peek(P) == '+' || ndp_peek(P) == '-')) {
            ndp_advance(P);
        }
        if (ndp_eof(P) || ndp_peek(P) < '0' || ndp_peek(P) > '9') {
            ndp_error(P, "expected digit in exponent");
            return -1;
        }
        while (!ndp_eof(P) && ndp_peek(P) >= '0' && ndp_peek(P) <= '9') {
            ndp_advance(P);
        }
    }

    /* Parse the collected number text */
    size_t nlen = P->pos - start;
    char numbuf[64];
    if (nlen >= sizeof(numbuf)) {
        ndp_error(P, "number too long");
        return -1;
    }
    memcpy(numbuf, P->src + start, nlen);
    numbuf[nlen] = '\0';

    if (is_float) {
        char *end = NULL;
        double d = strtod(numbuf, &end);
        nova_vm_push_number(P->vm, d);
    } else {
        char *end = NULL;
        long long ll = strtoll(numbuf, &end, 10);
        nova_vm_push_integer(P->vm, (nova_int_t)ll);
    }

    return 0;
}

/**
 * @brief Decode a JSON array: [ value, value, ... ]
 * Pushes a table with 0-based integer keys onto VM stack.
 */
static int ndp_json_decode_array(NdpParser *P) {
    ndp_advance(P);  /* consume '[' */
    ndp_skip_whitespace(P);

    nova_vm_push_table(P->vm);
    int table_idx = nova_vm_get_top(P->vm) - 1;
    NovaValue tval = nova_vm_get(P->vm, table_idx);
    NovaTable *t = nova_as_table(tval);
    nova_int_t index = 0;

    if (ndp_peek(P) == ']') {
        ndp_advance(P);
        return 0;  /* empty array */
    }

    for (;;) {
        ndp_skip_whitespace(P);

        /* Decode the element value (pushed on stack) */
        if (ndp_json_decode_value(P) != 0) {
            return -1;
        }

        /* Pop value and set in table's array part */
        NovaValue val = nova_vm_get(P->vm, -1);
        nova_vm_pop(P->vm, 1);
        nova_table_raw_set_int(P->vm, t, index, val);
        index++;

        ndp_skip_whitespace(P);
        if (ndp_match(P, ']')) {
            break;
        }
        if (!ndp_match(P, ',')) {
            ndp_error(P, "expected ',' or ']' in array");
            return -1;
        }
    }

    return 0;
}

/**
 * @brief Decode a JSON object: { "key": value, ... }
 * Pushes a table with string keys onto VM stack.
 */
static int ndp_json_decode_object(NdpParser *P) {
    ndp_advance(P);  /* consume '{' */
    ndp_skip_whitespace(P);

    nova_vm_push_table(P->vm);
    int table_idx = nova_vm_get_top(P->vm) - 1;

    if (ndp_peek(P) == '}') {
        ndp_advance(P);
        return 0;  /* empty object */
    }

    for (;;) {
        ndp_skip_whitespace(P);

        /* Key must be a string */
        if (ndp_peek(P) != '"') {
            ndp_error(P, "expected string key in object");
            return -1;
        }
        if (ndp_json_decode_string(P) != 0) {
            return -1;
        }
        /* Key string is on top of stack */
        NovaValue kval = nova_vm_get(P->vm, -1);
        nova_vm_pop(P->vm, 1);

        ndp_skip_whitespace(P);
        if (!ndp_match(P, ':')) {
            ndp_error(P, "expected ':' after key");
            return -1;
        }
        ndp_skip_whitespace(P);

        /* Decode value */
        if (ndp_json_decode_value(P) != 0) {
            return -1;
        }

        /* Set table[key] = value.  Use set_field for string keys. */
        /* Value is on top, push it as field on our table */
        nova_vm_set_field(P->vm, table_idx, nova_str_data(nova_as_string(kval)));

        ndp_skip_whitespace(P);
        if (ndp_match(P, '}')) {
            break;
        }
        if (!ndp_match(P, ',')) {
            ndp_error(P, "expected ',' or '}' in object");
            return -1;
        }
    }

    return 0;
}

/**
 * @brief Decode any JSON value.
 * Pushes result on VM stack. Returns 0 on success, -1 on error.
 */
static int ndp_json_decode_value(NdpParser *P) {
    ndp_skip_whitespace(P);

    if (ndp_eof(P)) {
        ndp_error(P, "unexpected end of input");
        return -1;
    }

    char c = ndp_peek(P);

    switch (c) {
        case '"':
            return ndp_json_decode_string(P);
        case '{':
            return ndp_json_decode_object(P);
        case '[':
            return ndp_json_decode_array(P);
        case 't':
            if (ndp_consume_literal(P, "true")) {
                nova_vm_push_bool(P->vm, 1);
                return 0;
            }
            ndp_error(P, "invalid literal (expected 'true')");
            return -1;
        case 'f':
            if (ndp_consume_literal(P, "false")) {
                nova_vm_push_bool(P->vm, 0);
                return 0;
            }
            ndp_error(P, "invalid literal (expected 'false')");
            return -1;
        case 'n':
            if (ndp_consume_literal(P, "null")) {
                nova_vm_push_nil(P->vm);
                return 0;
            }
            ndp_error(P, "invalid literal (expected 'null')");
            return -1;
        default:
            if (c == '-' || (c >= '0' && c <= '9')) {
                return ndp_json_decode_number(P);
            }
            ndp_error(P, "unexpected character '%c'", c);
            return -1;
    }
}

static int ndp_json_decode(NovaVM *vm, const char *text, size_t len,
                            const NdpOptions *opts, char *errbuf) {
    (void)opts;  /* json_strict not yet enforced */
    NdpParser P;
    ndp_parser_init(&P, vm, text, len, errbuf);

    if (ndp_json_decode_value(&P) != 0) {
        return -1;
    }

    /* Check for trailing junk */
    ndp_skip_whitespace(&P);
    if (!ndp_eof(&P)) {
        ndp_error(&P, "trailing content after JSON value");
        return -1;
    }

    return 0;
}

/* --- JSON Encode --- */

static int ndp_json_encode_value(NovaVM *vm, NovaValue val,
                                  NdpBuf *out, const NdpOptions *opts,
                                  int depth, char *errbuf);

static void ndp_json_indent(NdpBuf *out, const NdpOptions *opts, int depth) {
    if (!opts->pretty) {
        return;
    }
    ndp_buf_putc(out, '\n');
    int i = 0;
    int spaces = depth * opts->indent;
    for (i = 0; i < spaces; i++) {
        ndp_buf_putc(out, ' ');
    }
}

static void ndp_json_encode_string(NdpBuf *out, const char *s, size_t len) {
    ndp_buf_putc(out, '"');
    size_t i = 0;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  ndp_buf_puts(out, "\\\""); break;
            case '\\': ndp_buf_puts(out, "\\\\"); break;
            case '\b': ndp_buf_puts(out, "\\b");  break;
            case '\f': ndp_buf_puts(out, "\\f");  break;
            case '\n': ndp_buf_puts(out, "\\n");  break;
            case '\r': ndp_buf_puts(out, "\\r");  break;
            case '\t': ndp_buf_puts(out, "\\t");  break;
            default:
                if (c < 0x20) {
                    ndp_buf_printf(out, "\\u%04x", (unsigned int)c);
                } else {
                    ndp_buf_putc(out, (char)c);
                }
                break;
        }
    }
    ndp_buf_putc(out, '"');
}

/**
 * @brief Check if a table is array-like (only 0-based integer keys).
 */
static int ndp_table_is_array(NovaTable *t) {
    if (t == NULL) {
        return 0;
    }
    /* Has array elements and no hash entries -> array */
    if (nova_table_array_len(t) > 0 && nova_table_hash_count(t) == 0) {
        return 1;
    }
    /* Empty table -> array (encode as []) */
    if (nova_table_array_len(t) == 0 && nova_table_hash_count(t) == 0) {
        return 0;  /* empty -> object {} by default */
    }
    return 0;
}

static int ndp_json_encode_value(NovaVM *vm, NovaValue val,
                                  NdpBuf *out, const NdpOptions *opts,
                                  int depth, char *errbuf) {
    switch (nova_typeof(val)) {
        case NOVA_TYPE_NIL:
            ndp_buf_puts(out, "null");
            break;

        case NOVA_TYPE_BOOL:
            ndp_buf_puts(out, nova_as_bool(val) ? "true" : "false");
            break;

        case NOVA_TYPE_INTEGER:
            ndp_buf_printf(out, "%lld", (long long)nova_as_integer(val));
            break;

        case NOVA_TYPE_NUMBER: {
            if (isinf(nova_as_number(val)) || isnan(nova_as_number(val))) {
                ndp_buf_puts(out, "null");  /* JSON has no inf/nan */
            } else {
                ndp_buf_printf(out, "%.17g", nova_as_number(val));
            }
            break;
        }

        case NOVA_TYPE_STRING:
            ndp_json_encode_string(out, nova_str_data(nova_as_string(val)),
                                   nova_str_len(nova_as_string(val)));
            break;

        case NOVA_TYPE_TABLE: {
            NovaTable *t = nova_as_table(val);
            if (t == NULL) {
                ndp_buf_puts(out, "null");
                break;
            }
            if (depth > 100) {
                if (errbuf != NULL) {
                    (void)snprintf(errbuf, 256, "JSON encode: depth > 100 (circular?)");
                }
                return -1;
            }

            if (ndp_table_is_array(t)) {
                /* Encode as JSON array */
                ndp_buf_putc(out, '[');
                uint32_t i = 0;
                for (i = 0; i < nova_table_array_len(t); i++) {
                    if (i > 0) {
                        ndp_buf_putc(out, ',');
                    }
                    ndp_json_indent(out, opts, depth + 1);
                    if (ndp_json_encode_value(vm, nova_table_get_int(t, i), out,
                                              opts, depth + 1, errbuf) != 0) {
                        return -1;
                    }
                }
                if (nova_table_array_len(t) > 0) {
                    ndp_json_indent(out, opts, depth);
                }
                ndp_buf_putc(out, ']');
            } else {
                /* Encode as JSON object */
                ndp_buf_putc(out, '{');
                int first = 1;

                /* Hash entries (string keys) */
                uint32_t iter_h = nova_table_array_len(t);
                NovaValue hkey, hval;
                while (nova_table_next(t, &iter_h, &hkey, &hval)) {

                    if (!first) {
                        ndp_buf_putc(out, ',');
                    }
                    first = 0;
                    ndp_json_indent(out, opts, depth + 1);

                    /* Key: convert to string */
                    if (nova_is_string(hkey)) {
                        ndp_json_encode_string(out, nova_str_data(nova_as_string(hkey)),
                                               nova_str_len(nova_as_string(hkey)));
                    } else if (nova_is_integer(hkey)) {
                        char kb[32];
                        int kn = snprintf(kb, sizeof(kb), "%lld",
                                         (long long)nova_as_integer(hkey));
                        ndp_json_encode_string(out, kb, (size_t)kn);
                    } else {
                        ndp_json_encode_string(out, "?", 1);
                    }

                    ndp_buf_putc(out, ':');
                    if (opts->pretty) {
                        ndp_buf_putc(out, ' ');
                    }

                    if (ndp_json_encode_value(vm, hval, out,
                                              opts, depth + 1, errbuf) != 0) {
                        return -1;
                    }
                }

                /* Also encode array part elements with integer keys */
                uint32_t ai = 0;
                for (ai = 0; ai < nova_table_array_len(t); ai++) {
                    if (!first) {
                        ndp_buf_putc(out, ',');
                    }
                    first = 0;
                    ndp_json_indent(out, opts, depth + 1);
                    char kb[32];
                    int kn = snprintf(kb, sizeof(kb), "%u", ai);
                    ndp_json_encode_string(out, kb, (size_t)kn);
                    ndp_buf_putc(out, ':');
                    if (opts->pretty) {
                        ndp_buf_putc(out, ' ');
                    }
                    if (ndp_json_encode_value(vm, nova_table_get_int(t, ai), out,
                                              opts, depth + 1, errbuf) != 0) {
                        return -1;
                    }
                }

                if (!first) {
                    ndp_json_indent(out, opts, depth);
                }
                ndp_buf_putc(out, '}');
            }
            break;
        }

        default:
            /* Functions, userdata, coroutines -> null */
            ndp_buf_puts(out, "null");
            break;
    }

    return 0;
}

static int ndp_json_encode(NovaVM *vm, int idx, const NdpOptions *opts,
                            NdpBuf *out, char *errbuf) {
    NovaValue val = nova_vm_get(vm, idx);
    return ndp_json_encode_value(vm, val, out, opts, 0, errbuf);
}

/* ============================================================
 * PART 5: CSV/TSV CODEC
 *
 * RFC 4180 compliant.  Delimiter is configurable (comma for CSV,
 * tab for TSV).  Optional header row for named fields.
 *
 * Decode:
 *   With header:  array of {col1: val, col2: val, ...} tables
 *   Without header: array of arrays [val, val, ...]
 *
 * Encode:
 *   Expects array-of-tables (with header) or array-of-arrays.
 *   First row's keys become the header line.
 * ============================================================ */

/**
 * @brief Parse one CSV field, handling quoting.
 * Returns a heap-allocated string (caller must free), or NULL on error.
 */
static char *ndp_csv_parse_field(NdpParser *P, char delim, char quote,
                                  size_t *out_len) {
    NdpBuf field;
    ndp_buf_init(&field);

    if (!ndp_eof(P) && ndp_peek(P) == quote) {
        /* Quoted field */
        ndp_advance(P);  /* consume opening quote */
        for (;;) {
            if (ndp_eof(P)) {
                ndp_error(P, "unterminated quoted field");
                ndp_buf_free(&field);
                return NULL;
            }
            char c = ndp_peek(P);
            if (c == quote) {
                ndp_advance(P);
                /* Doubled quote = literal quote */
                if (!ndp_eof(P) && ndp_peek(P) == quote) {
                    ndp_buf_putc(&field, quote);
                    ndp_advance(P);
                } else {
                    break;  /* end of quoted field */
                }
            } else {
                ndp_buf_putc(&field, c);
                ndp_advance(P);
            }
        }
    } else {
        /* Unquoted field: read until delimiter, \r, \n, or EOF */
        while (!ndp_eof(P)) {
            char c = ndp_peek(P);
            if (c == delim || c == '\r' || c == '\n') {
                break;
            }
            ndp_buf_putc(&field, c);
            ndp_advance(P);
        }
    }

    /* NUL-terminate */
    ndp_buf_putc(&field, '\0');
    *out_len = field.len - 1;  /* exclude NUL from length */
    return field.data;  /* caller owns this memory */
}

/**
 * @brief Try to parse a CSV field value as typed (int, float, bool).
 * Pushes the appropriate Nova value onto VM stack.
 */
static void ndp_csv_push_typed_value(NovaVM *vm, const char *s, size_t len) {
    if (len == 0) {
        nova_vm_push_nil(vm);
        return;
    }

    /* Check for boolean */
    if (len == 4 && strncasecmp(s, "true", 4) == 0) {
        nova_vm_push_bool(vm, 1);
        return;
    }
    if (len == 5 && strncasecmp(s, "false", 5) == 0) {
        nova_vm_push_bool(vm, 0);
        return;
    }

    /* Try integer */
    char *end = NULL;
    errno = 0;
    long long ll = strtoll(s, &end, 10);
    if (end == s + len && errno == 0) {
        nova_vm_push_integer(vm, (nova_int_t)ll);
        return;
    }

    /* Try float */
    errno = 0;
    double d = strtod(s, &end);
    if (end == s + len && errno == 0) {
        nova_vm_push_number(vm, d);
        return;
    }

    /* Default: string */
    nova_vm_push_string(vm, s, len);
}

static int ndp_csv_decode(NovaVM *vm, const char *text, size_t len,
                           const NdpOptions *opts, char *errbuf) {
    NdpParser P;
    ndp_parser_init(&P, vm, text, len, errbuf);

    char delim = opts->csv_delimiter;
    char quote = opts->csv_quote;
    int use_header = opts->csv_header;

    /* For TSV, override delimiter */
    if (opts->format == NDP_FORMAT_TSV) {
        delim = '\t';
    }

    /* Parse header row if enabled */
    char **headers = NULL;
    size_t *header_lens = NULL;
    int ncols = 0;

    if (use_header && !ndp_eof(&P)) {
        /* Count and collect header fields */
        int cap = 16;
        headers = (char **)calloc((size_t)cap, sizeof(char *));
        header_lens = (size_t *)calloc((size_t)cap, sizeof(size_t));
        if (headers == NULL || header_lens == NULL) {
            free(headers);
            free(header_lens);
            if (errbuf != NULL) {
                (void)snprintf(errbuf, 256, "CSV: out of memory for headers");
            }
            return -1;
        }

        for (;;) {
            size_t flen = 0;
            char *field = ndp_csv_parse_field(&P, delim, quote, &flen);
            if (field == NULL) {
                /* Error already set */
                int fi = 0;
                for (fi = 0; fi < ncols; fi++) { free(headers[fi]); }
                free(headers);
                free(header_lens);
                return -1;
            }
            if (ncols >= cap) {
                cap *= 2;
                char **new_headers = (char **)realloc(
                    headers, (size_t)cap * sizeof(char *));
                if (new_headers == NULL) {
                    int fi = 0;
                    for (fi = 0; fi < ncols; fi++) { free(headers[fi]); }
                    free(headers);
                    free(header_lens);
                    free(field);
                    ndp_error(&P, "out of memory expanding CSV headers");
                    return -1;
                }
                headers = new_headers;
                size_t *new_lens = (size_t *)realloc(
                    header_lens, (size_t)cap * sizeof(size_t));
                if (new_lens == NULL) {
                    int fi = 0;
                    for (fi = 0; fi < ncols; fi++) { free(headers[fi]); }
                    free(headers);
                    free(header_lens);
                    free(field);
                    ndp_error(&P, "out of memory expanding CSV headers");
                    return -1;
                }
                header_lens = new_lens;
            }
            headers[ncols] = field;
            header_lens[ncols] = flen;
            ncols++;

            if (ndp_eof(&P) || ndp_peek(&P) == '\r' || ndp_peek(&P) == '\n') {
                break;
            }
            if (!ndp_match(&P, delim)) {
                break;
            }
        }
        /* Skip line ending */
        if (!ndp_eof(&P) && ndp_peek(&P) == '\r') { ndp_advance(&P); }
        if (!ndp_eof(&P) && ndp_peek(&P) == '\n') { ndp_advance(&P); }
    }

    /* Create outer array table */
    nova_vm_push_table(vm);
    int result_idx = nova_vm_get_top(vm) - 1;
    NovaValue result_val = nova_vm_get(vm, result_idx);
    NovaTable *result_tbl = nova_as_table(result_val);
    nova_int_t row_index = 0;

    /* Parse data rows */
    while (!ndp_eof(&P)) {
        /* Skip blank lines */
        if (ndp_peek(&P) == '\r' || ndp_peek(&P) == '\n') {
            ndp_advance(&P);
            continue;
        }

        /* Create row table */
        nova_vm_push_table(vm);
        int row_tbl_idx = nova_vm_get_top(vm) - 1;
        NovaValue row_val = nova_vm_get(vm, row_tbl_idx);
        NovaTable *row_tbl = nova_as_table(row_val);

        int col = 0;
        for (;;) {
            size_t flen = 0;
            char *field = ndp_csv_parse_field(&P, delim, quote, &flen);
            if (field == NULL) {
                int fi = 0;
                for (fi = 0; fi < ncols; fi++) { free(headers[fi]); }
                free(headers);
                free(header_lens);
                return -1;
            }

            /* Push typed value */
            ndp_csv_push_typed_value(vm, field, flen);
            NovaValue fval = nova_vm_get(vm, -1);
            nova_vm_pop(vm, 1);

            if (use_header && col < ncols) {
                /* Set as named field: row[header] = value */
                NovaString *hstr = nova_vm_intern_string(vm,
                    headers[col], header_lens[col]);
                if (hstr != NULL) {
                    nova_table_raw_set_str(vm, row_tbl, hstr, fval);
                }
            } else {
                /* Set as indexed: row[col] = value */
                nova_table_raw_set_int(vm, row_tbl, (nova_int_t)col, fval);
            }

            free(field);
            col++;

            if (ndp_eof(&P) || ndp_peek(&P) == '\r' || ndp_peek(&P) == '\n') {
                break;
            }
            if (!ndp_match(&P, delim)) {
                break;
            }
        }

        /* Skip line ending */
        if (!ndp_eof(&P) && ndp_peek(&P) == '\r') { ndp_advance(&P); }
        if (!ndp_eof(&P) && ndp_peek(&P) == '\n') { ndp_advance(&P); }

        /* Pop row table from stack and insert into result array */
        nova_vm_pop(vm, 1);  /* pop row table (we have row_val) */
        nova_table_raw_set_int(vm, result_tbl, row_index, row_val);
        row_index++;
    }

    /* Clean up headers */
    if (headers != NULL) {
        int fi = 0;
        for (fi = 0; fi < ncols; fi++) {
            free(headers[fi]);
        }
        free(headers);
        free(header_lens);
    }

    return 0;
}

static int ndp_csv_encode(NovaVM *vm, int idx, const NdpOptions *opts,
                           NdpBuf *out, char *errbuf) {
    NovaValue val = nova_vm_get(vm, idx);
    if (!nova_is_table(val)) {
        if (errbuf != NULL) {
            (void)snprintf(errbuf, 256, "CSV encode: expected table, got %s",
                           nova_vm_typename(nova_typeof(val)));
        }
        return -1;
    }

    NovaTable *t = nova_as_table(val);
    if (t == NULL || nova_table_array_len(t) == 0) {
        return 0;  /* empty -> empty output */
    }

    char delim = opts->csv_delimiter;
    if (opts->format == NDP_FORMAT_TSV) {
        delim = '\t';
    }

    /* Check first row to determine structure */
    NovaValue first_row = nova_table_get_int(t, 0);
    if (!nova_is_table(first_row)) {
        if (errbuf != NULL) {
            (void)snprintf(errbuf, 256, "CSV encode: rows must be tables");
        }
        return -1;
    }

    NovaTable *fr = nova_as_table(first_row);
    int is_named = (nova_table_hash_count(fr) > 0);

    /* Collect header names from first row hash keys */
    char **headers = NULL;
    int ncols = 0;

    if (is_named && opts->csv_header) {
        ncols = (int)nova_table_hash_count(fr);
        headers = (char **)calloc((size_t)ncols, sizeof(char *));
        if (headers == NULL) {
            if (errbuf != NULL) {
                (void)snprintf(errbuf, 256, "CSV encode: out of memory");
            }
            return -1;
        }
        int hi = 0;
        uint32_t hj_iter = nova_table_array_len(fr);
        NovaValue hk_it, hv_it;
        while (nova_table_next(fr, &hj_iter, &hk_it, &hv_it)) {
            if (nova_is_string(hk_it) && hi < ncols) {
                headers[hi] = nova_str_data(nova_as_string(hk_it));
                hi++;
            }
        }
        ncols = hi;

        /* Write header row */
        int ci = 0;
        for (ci = 0; ci < ncols; ci++) {
            if (ci > 0) {
                ndp_buf_putc(out, delim);
            }
            ndp_buf_puts(out, headers[ci]);
        }
        ndp_buf_putc(out, '\n');
    }

    /* Write data rows */
    uint32_t ri = 0;
    for (ri = 0; ri < nova_table_array_len(t); ri++) {
        NovaValue row = nova_table_get_int(t, ri);
        if (!nova_is_table(row)) {
            continue;
        }
        NovaTable *rt = nova_as_table(row);

        if (is_named && headers != NULL) {
            /* Named columns: output in header order */
            int ci = 0;
            for (ci = 0; ci < ncols; ci++) {
                if (ci > 0) {
                    ndp_buf_putc(out, delim);
                }
                /* Find value for this column name */
                NovaString *key = nova_vm_intern_string(vm, headers[ci],
                    strlen(headers[ci]));
                NovaValue cv = nova_value_nil();
                if (key != NULL) {
                    cv = nova_table_get_str(rt, key);
                }
                /* Format value */
                if (nova_is_string(cv)) {
                    /* Quote if contains delimiter, quote, or newline */
                    const char *s = nova_str_data(nova_as_string(cv));
                    int need_quote = 0;
                    size_t si = 0;
                    for (si = 0; si < nova_str_len(nova_as_string(cv)); si++) {
                        if (s[si] == delim || s[si] == '"' ||
                            s[si] == '\n' || s[si] == '\r') {
                            need_quote = 1;
                            break;
                        }
                    }
                    if (need_quote) {
                        ndp_buf_putc(out, '"');
                        for (si = 0; si < nova_str_len(nova_as_string(cv)); si++) {
                            if (s[si] == '"') {
                                ndp_buf_puts(out, "\"\"");
                            } else {
                                ndp_buf_putc(out, s[si]);
                            }
                        }
                        ndp_buf_putc(out, '"');
                    } else {
                        ndp_buf_append(out, s, nova_str_len(nova_as_string(cv)));
                    }
                } else if (nova_is_integer(cv)) {
                    ndp_buf_printf(out, "%lld", (long long)nova_as_integer(cv));
                } else if (nova_is_number(cv)) {
                    ndp_buf_printf(out, "%.17g", nova_as_number(cv));
                } else if (nova_is_bool(cv)) {
                    ndp_buf_puts(out, nova_as_bool(cv) ? "true" : "false");
                }
                /* nil -> empty field */
            }
        } else {
            /* Array-indexed columns */
            uint32_t ci = 0;
            for (ci = 0; ci < nova_table_array_len(rt); ci++) {
                if (ci > 0) {
                    ndp_buf_putc(out, delim);
                }
                NovaValue cv = nova_table_get_int(rt, ci);
                if (nova_is_string(cv)) {
                    ndp_buf_append(out, nova_str_data(nova_as_string(cv)),
                                   nova_str_len(nova_as_string(cv)));
                } else if (nova_is_integer(cv)) {
                    ndp_buf_printf(out, "%lld", (long long)nova_as_integer(cv));
                } else if (nova_is_number(cv)) {
                    ndp_buf_printf(out, "%.17g", nova_as_number(cv));
                } else if (nova_is_bool(cv)) {
                    ndp_buf_puts(out, nova_as_bool(cv) ? "true" : "false");
                }
            }
        }
        ndp_buf_putc(out, '\n');
    }

    free(headers);
    return 0;
}

/* ============================================================
 * PART 6: INI CODEC
 *
 * Decode: INI text -> nested table {section: {key: value, ...}}
 *   - Lines starting with ; or # are comments
 *   - [section] creates a sub-table
 *   - key = value pairs stored as typed or string values
 *   - Bare keys (no section) go into root table
 *
 * Encode: nested table -> INI text
 *   - Top-level string/number/bool keys -> bare keys
 *   - Top-level table values -> [section] blocks
 * ============================================================ */

/**
 * @brief Try to parse a value string as typed Nova value
 *        (int, float, bool, nil). Falls back to string.
 */
static void ndp_ini_push_typed(NovaVM *vm, const char *s, size_t len,
                                int typed) {
    if (!typed || len == 0) {
        nova_vm_push_string(vm, s, len);
        return;
    }

    /* Boolean */
    if ((len == 4 && strncasecmp(s, "true", 4) == 0) ||
        (len == 3 && strncasecmp(s, "yes", 3) == 0) ||
        (len == 2 && strncasecmp(s, "on", 2) == 0)) {
        nova_vm_push_bool(vm, 1);
        return;
    }
    if ((len == 5 && strncasecmp(s, "false", 5) == 0) ||
        (len == 2 && strncasecmp(s, "no", 2) == 0) ||
        (len == 3 && strncasecmp(s, "off", 3) == 0)) {
        nova_vm_push_bool(vm, 0);
        return;
    }

    /* Nil / none */
    if ((len == 3 && strncasecmp(s, "nil", 3) == 0) ||
        (len == 4 && strncasecmp(s, "none", 4) == 0) ||
        (len == 4 && strncasecmp(s, "null", 4) == 0)) {
        nova_vm_push_nil(vm);
        return;
    }

    /* Try integer */
    char *end = NULL;
    errno = 0;
    long long ll = strtoll(s, &end, 10);
    if ((size_t)(end - s) == len && errno == 0) {
        nova_vm_push_integer(vm, (nova_int_t)ll);
        return;
    }

    /* Try float */
    errno = 0;
    double d = strtod(s, &end);
    if ((size_t)(end - s) == len && errno == 0) {
        nova_vm_push_number(vm, d);
        return;
    }

    /* Default: string */
    nova_vm_push_string(vm, s, len);
}

static int ndp_ini_decode(NovaVM *vm, const char *text, size_t len,
                           const NdpOptions *opts, char *errbuf) {
    NdpParser P;
    ndp_parser_init(&P, vm, text, len, errbuf);

    /* Root table */
    nova_vm_push_table(vm);
    int root_idx = nova_vm_get_top(vm) - 1;

    /* Current section table (NULL = root) */
    int section_idx = root_idx;

    while (!ndp_eof(&P)) {
        /* Skip whitespace (but not newlines for line structure) */
        while (!ndp_eof(&P) && (ndp_peek(&P) == ' ' || ndp_peek(&P) == '\t')) {
            ndp_advance(&P);
        }

        if (ndp_eof(&P)) {
            break;
        }

        char c = ndp_peek(&P);

        /* Skip empty lines */
        if (c == '\r' || c == '\n') {
            ndp_advance(&P);
            continue;
        }

        /* Comment line */
        if (c == ';' || c == '#') {
            while (!ndp_eof(&P) && ndp_peek(&P) != '\n') {
                ndp_advance(&P);
            }
            continue;
        }

        /* Section header: [name] */
        if (c == '[') {
            ndp_advance(&P);  /* consume '[' */
            size_t name_start = P.pos;
            while (!ndp_eof(&P) && ndp_peek(&P) != ']' && ndp_peek(&P) != '\n') {
                ndp_advance(&P);
            }
            size_t name_end = P.pos;
            if (!ndp_match(&P, ']')) {
                ndp_error(&P, "unterminated section header");
                return -1;
            }

            /* Trim whitespace from section name */
            while (name_start < name_end &&
                   (P.src[name_start] == ' ' || P.src[name_start] == '\t')) {
                name_start++;
            }
            while (name_end > name_start &&
                   (P.src[name_end - 1] == ' ' || P.src[name_end - 1] == '\t')) {
                name_end--;
            }

            /* Create section table and set on root */
            nova_vm_push_table(vm);
            section_idx = nova_vm_get_top(vm) - 1;

            /* Save the section table value before set_field pops it.
             * nova_vm_set_field pops TOS, but we need the table reference
             * to remain accessible on the stack for subsequent key=value
             * assignments.  The old recovery code used pointer comparison
             * on strings, which always failed because novai_string_new
             * does not deduplicate / intern -- every call returns a fresh
             * object.  Saving + re-pushing is safe because push_value
             * does not allocate. */
            NovaValue section_val = nova_vm_get(vm, section_idx);

            /* Need NUL-terminated name for set_field */
            char sec_name[256];
            size_t nlen = name_end - name_start;
            if (nlen >= sizeof(sec_name)) {
                nlen = sizeof(sec_name) - 1;
            }
            memcpy(sec_name, P.src + name_start, nlen);
            sec_name[nlen] = '\0';

            nova_vm_set_field(vm, root_idx, sec_name);  /* pops TOS */

            /* Re-push the saved section table so key=value pairs
             * that follow are set on the section, not on root. */
            nova_vm_push_value(vm, section_val);
            section_idx = nova_vm_get_top(vm) - 1;

            /* Skip rest of line */
            while (!ndp_eof(&P) && ndp_peek(&P) != '\n') {
                ndp_advance(&P);
            }
            continue;
        }

        /* Key = Value line */
        size_t key_start = P.pos;
        while (!ndp_eof(&P) && ndp_peek(&P) != '=' && ndp_peek(&P) != ':' &&
               ndp_peek(&P) != '\n') {
            ndp_advance(&P);
        }
        size_t key_end = P.pos;

        if (ndp_eof(&P) || ndp_peek(&P) == '\n') {
            /* Line with no = sign; skip it */
            continue;
        }

        ndp_advance(&P);  /* consume '=' or ':' */

        /* Skip whitespace around value */
        while (!ndp_eof(&P) && (ndp_peek(&P) == ' ' || ndp_peek(&P) == '\t')) {
            ndp_advance(&P);
        }

        size_t val_start = P.pos;
        while (!ndp_eof(&P) && ndp_peek(&P) != '\n' && ndp_peek(&P) != '\r') {
            ndp_advance(&P);
        }
        size_t val_end = P.pos;

        /* Trim key */
        while (key_start < key_end &&
               (P.src[key_start] == ' ' || P.src[key_start] == '\t')) {
            key_start++;
        }
        while (key_end > key_start &&
               (P.src[key_end - 1] == ' ' || P.src[key_end - 1] == '\t')) {
            key_end--;
        }

        /* Trim value */
        while (val_end > val_start &&
               (P.src[val_end - 1] == ' ' || P.src[val_end - 1] == '\t')) {
            val_end--;
        }

        /* Strip quotes from value if present */
        size_t vlen = val_end - val_start;
        if (vlen >= 2) {
            char first_ch = P.src[val_start];
            char last_ch = P.src[val_end - 1];
            if ((first_ch == '"' && last_ch == '"') ||
                (first_ch == '\'' && last_ch == '\'')) {
                val_start++;
                val_end--;
                vlen -= 2;
            }
        }

        /* Push typed value */
        ndp_ini_push_typed(vm, P.src + val_start, val_end - val_start,
                            opts->ini_typed);

        /* NUL-terminated key for set_field */
        char key_name[256];
        size_t klen = key_end - key_start;
        if (klen >= sizeof(key_name)) {
            klen = sizeof(key_name) - 1;
        }
        memcpy(key_name, P.src + key_start, klen);
        key_name[klen] = '\0';

        nova_vm_set_field(vm, section_idx, key_name);
    }

    /* Clean up stack: only root table should remain */
    /* The section tables were set into root, so we can trim stack */
    nova_vm_set_top(vm, root_idx + 1);

    return 0;
}

static int ndp_ini_encode(NovaVM *vm, int idx, const NdpOptions *opts,
                           NdpBuf *out, char *errbuf) {
    (void)opts;
    NovaValue val = nova_vm_get(vm, idx);
    if (!nova_is_table(val)) {
        if (errbuf != NULL) {
            (void)snprintf(errbuf, 256, "INI encode: expected table, got %s",
                           nova_vm_typename(nova_typeof(val)));
        }
        return -1;
    }

    NovaTable *t = nova_as_table(val);
    if (t == NULL) {
        return 0;
    }

    /* First pass: write bare key=value pairs (non-table values) */
    uint32_t iter_ini = nova_table_array_len(t);
    NovaValue hkey, hval;
    while (nova_table_next(t, &iter_ini, &hkey, &hval)) {

        if (!nova_is_string(hkey)) {
            continue;
        }
        if (nova_is_table(hval)) {
            continue;  /* sections handled in second pass */
        }

        ndp_buf_puts(out, nova_str_data(nova_as_string(hkey)));
        ndp_buf_puts(out, " = ");

        switch (nova_typeof(hval)) {
            case NOVA_TYPE_STRING:
                ndp_buf_append(out, nova_str_data(nova_as_string(hval)),
                               nova_str_len(nova_as_string(hval)));
                break;
            case NOVA_TYPE_INTEGER:
                ndp_buf_printf(out, "%lld", (long long)nova_as_integer(hval));
                break;
            case NOVA_TYPE_NUMBER:
                ndp_buf_printf(out, "%.17g", nova_as_number(hval));
                break;
            case NOVA_TYPE_BOOL:
                ndp_buf_puts(out, nova_as_bool(hval) ? "true" : "false");
                break;
            default:
                break;
        }
        ndp_buf_putc(out, '\n');
    }

    /* Second pass: write [section] blocks */
    uint32_t iter_sec = nova_table_array_len(t);
    NovaValue hkey2, hval2;
    while (nova_table_next(t, &iter_sec, &hkey2, &hval2)) {

        if (!nova_is_string(hkey2) || !nova_is_table(hval2)) {
            continue;
        }

        ndp_buf_putc(out, '\n');
        ndp_buf_putc(out, '[');
        ndp_buf_puts(out, nova_str_data(nova_as_string(hkey2)));
        ndp_buf_puts(out, "]\n");

        NovaTable *sec = nova_as_table(hval2);
        if (sec == NULL) {
            continue;
        }

        uint32_t iter_sk = nova_table_array_len(sec);
        NovaValue skey, sval;
        while (nova_table_next(sec, &iter_sk, &skey, &sval)) {

            if (!nova_is_string(skey)) {
                continue;
            }

            ndp_buf_puts(out, nova_str_data(nova_as_string(skey)));
            ndp_buf_puts(out, " = ");

            switch (nova_typeof(sval)) {
                case NOVA_TYPE_STRING:
                    ndp_buf_append(out, nova_str_data(nova_as_string(sval)),
                                   nova_str_len(nova_as_string(sval)));
                    break;
                case NOVA_TYPE_INTEGER:
                    ndp_buf_printf(out, "%lld", (long long)nova_as_integer(sval));
                    break;
                case NOVA_TYPE_NUMBER:
                    ndp_buf_printf(out, "%.17g", nova_as_number(sval));
                    break;
                case NOVA_TYPE_BOOL:
                    ndp_buf_puts(out, nova_as_bool(sval) ? "true" : "false");
                    break;
                default:
                    break;
            }
            ndp_buf_putc(out, '\n');
        }
    }

    return 0;
}

/* ============================================================
 * PART 7: TOML CODEC
 *
 * "Basic TOML" subset (TOML v1.0.0 core features):
 *   - Key = value pairs (string, integer, float, bool, datetime)
 *   - [table] sections
 *   - [[array_of_tables]] sections
 *   - Inline arrays: [1, 2, 3]
 *   - Basic strings ("...") and literal strings ('...')
 *   - # single-line comments
 *
 * NOT supported in v1 (deferred):
 *   - Multi-line basic/literal strings ("""/''')
 *   - Dotted keys (e.g. fruit.apple.color = "red")
 *   - Inline tables ({key = val, ...})
 *   - Datetime parsing (stored as strings)
 * ============================================================ */

/* Forward declaration for recursive TOML array parsing */
static int ndp_toml_decode_array(NdpParser *P);

/**
 * @brief Parse a TOML value (string, int, float, bool, array).
 * Pushes result on VM stack.
 */
static int ndp_toml_decode_value(NdpParser *P) {
    ndp_skip_whitespace(P);

    if (ndp_eof(P)) {
        ndp_error(P, "unexpected end of input in TOML value");
        return -1;
    }

    char c = ndp_peek(P);

    /* String: "..." or '...' */
    if (c == '"') {
        ndp_advance(P);
        NdpBuf tmp;
        ndp_buf_init(&tmp);
        while (!ndp_eof(P) && ndp_peek(P) != '"') {
            char ch = ndp_advance(P);
            if (ch == '\\') {
                char esc = ndp_advance(P);
                switch (esc) {
                    case '"': ndp_buf_putc(&tmp, '"'); break;
                    case '\\': ndp_buf_putc(&tmp, '\\'); break;
                    case 'n': ndp_buf_putc(&tmp, '\n'); break;
                    case 't': ndp_buf_putc(&tmp, '\t'); break;
                    case 'r': ndp_buf_putc(&tmp, '\r'); break;
                    default: ndp_buf_putc(&tmp, esc); break;
                }
            } else {
                ndp_buf_putc(&tmp, ch);
            }
        }
        if (!ndp_match(P, '"')) {
            ndp_error(P, "unterminated TOML string");
            ndp_buf_free(&tmp);
            return -1;
        }
        nova_vm_push_string(P->vm, tmp.data ? tmp.data : "", tmp.len);
        ndp_buf_free(&tmp);
        return 0;
    }

    /* Literal string: '...' */
    if (c == '\'') {
        ndp_advance(P);
        size_t start = P->pos;
        while (!ndp_eof(P) && ndp_peek(P) != '\'') {
            ndp_advance(P);
        }
        size_t end = P->pos;
        if (!ndp_match(P, '\'')) {
            ndp_error(P, "unterminated TOML literal string");
            return -1;
        }
        nova_vm_push_string(P->vm, P->src + start, end - start);
        return 0;
    }

    /* Array: [...] */
    if (c == '[') {
        return ndp_toml_decode_array(P);
    }

    /* Boolean */
    if (ndp_check_literal(P, "true")) {
        ndp_consume_literal(P, "true");
        nova_vm_push_bool(P->vm, 1);
        return 0;
    }
    if (ndp_check_literal(P, "false")) {
        ndp_consume_literal(P, "false");
        nova_vm_push_bool(P->vm, 0);
        return 0;
    }

    /* Number (int or float) -- read until whitespace/comment/comma/] */
    size_t start = P->pos;
    int is_float = 0;
    int has_sign = 0;

    if (c == '+' || c == '-') {
        ndp_advance(P);
        has_sign = 1;
        (void)has_sign;  /* suppress unused warning */
    }

    /* Check for special float values */
    if (ndp_check_literal(P, "inf") || ndp_check_literal(P, "nan")) {
        while (!ndp_eof(P) && isalpha((unsigned char)ndp_peek(P))) {
            ndp_advance(P);
        }
        size_t nlen = P->pos - start;
        char numbuf[32];
        if (nlen >= sizeof(numbuf)) { nlen = sizeof(numbuf) - 1; }
        memcpy(numbuf, P->src + start, nlen);
        numbuf[nlen] = '\0';
        char *end = NULL;
        double d = strtod(numbuf, &end);
        nova_vm_push_number(P->vm, d);
        return 0;
    }

    while (!ndp_eof(P)) {
        char nc = ndp_peek(P);
        if (nc == '.' || nc == 'e' || nc == 'E') {
            is_float = 1;
        }
        if (nc == ' ' || nc == '\t' || nc == '\n' || nc == '\r' ||
            nc == '#' || nc == ',' || nc == ']' || nc == '}') {
            break;
        }
        /* TOML allows _ as digit separator */
        ndp_advance(P);
    }

    size_t nlen = P->pos - start;
    if (nlen == 0) {
        ndp_error(P, "empty TOML value");
        return -1;
    }

    /* Copy, stripping underscores */
    char numbuf[64];
    size_t ni = 0;
    size_t si = 0;
    for (si = 0; si < nlen && ni < sizeof(numbuf) - 1; si++) {
        if (P->src[start + si] != '_') {
            numbuf[ni++] = P->src[start + si];
        }
    }
    numbuf[ni] = '\0';

    if (is_float) {
        char *end = NULL;
        double d = strtod(numbuf, &end);
        nova_vm_push_number(P->vm, d);
    } else {
        /* Try integer (supports 0x, 0o, 0b prefixes) */
        char *end = NULL;
        errno = 0;
        long long ll = strtoll(numbuf, &end, 0);
        if (errno != 0 || *end != '\0') {
            /* Fall back to float */
            double d = strtod(numbuf, &end);
            nova_vm_push_number(P->vm, d);
        } else {
            nova_vm_push_integer(P->vm, (nova_int_t)ll);
        }
    }
    return 0;
}

static int ndp_toml_decode_array(NdpParser *P) {
    ndp_advance(P);  /* consume '[' */
    ndp_skip_whitespace(P);

    nova_vm_push_table(P->vm);
    int arr_idx = nova_vm_get_top(P->vm) - 1;
    NovaValue arr_val = nova_vm_get(P->vm, arr_idx);
    NovaTable *arr = nova_as_table(arr_val);
    nova_int_t index = 0;

    while (!ndp_eof(P) && ndp_peek(P) != ']') {
        /* Skip whitespace and comments */
        ndp_skip_whitespace(P);
        if (!ndp_eof(P) && ndp_peek(P) == '#') {
            while (!ndp_eof(P) && ndp_peek(P) != '\n') { ndp_advance(P); }
            continue;
        }
        if (ndp_eof(P) || ndp_peek(P) == ']') { break; }

        if (ndp_toml_decode_value(P) != 0) {
            return -1;
        }
        NovaValue elem = nova_vm_get(P->vm, -1);
        nova_vm_pop(P->vm, 1);
        nova_table_raw_set_int(P->vm, arr, index, elem);
        index++;

        ndp_skip_whitespace(P);
        if (!ndp_eof(P) && ndp_peek(P) == ',') {
            ndp_advance(P);
        }
    }

    if (!ndp_match(P, ']')) {
        ndp_error(P, "unterminated TOML array");
        return -1;
    }
    return 0;
}

static int ndp_toml_decode(NovaVM *vm, const char *text, size_t len,
                            const NdpOptions *opts, char *errbuf) {
    (void)opts;
    NdpParser P;
    ndp_parser_init(&P, vm, text, len, errbuf);

    /* Root table */
    nova_vm_push_table(vm);
    int root_idx = nova_vm_get_top(vm) - 1;

    /* Current target table index */
    int target_idx = root_idx;

    while (!ndp_eof(&P)) {
        /* Skip whitespace */
        while (!ndp_eof(&P) && (ndp_peek(&P) == ' ' || ndp_peek(&P) == '\t')) {
            ndp_advance(&P);
        }
        if (ndp_eof(&P)) { break; }

        char c = ndp_peek(&P);

        /* Empty line */
        if (c == '\r' || c == '\n') {
            ndp_advance(&P);
            continue;
        }

        /* Comment */
        if (c == '#') {
            while (!ndp_eof(&P) && ndp_peek(&P) != '\n') { ndp_advance(&P); }
            continue;
        }

        /* [[array_of_tables]] */
        if (c == '[' && P.pos + 1 < P.len && P.src[P.pos + 1] == '[') {
            ndp_advance(&P);  /* first '[' */
            ndp_advance(&P);  /* second '[' */

            /* Read section name */
            while (!ndp_eof(&P) && (ndp_peek(&P) == ' ' || ndp_peek(&P) == '\t')) {
                ndp_advance(&P);
            }
            size_t name_start = P.pos;
            while (!ndp_eof(&P) && ndp_peek(&P) != ']') {
                ndp_advance(&P);
            }
            size_t name_end = P.pos;

            if (!ndp_match(&P, ']') || !ndp_match(&P, ']')) {
                ndp_error(&P, "unterminated [[array_of_tables]]");
                return -1;
            }

            /* Trim name */
            while (name_end > name_start &&
                   (P.src[name_end - 1] == ' ' || P.src[name_end - 1] == '\t')) {
                name_end--;
            }

            char sec_name[256];
            size_t nlen = name_end - name_start;
            if (nlen >= sizeof(sec_name)) { nlen = sizeof(sec_name) - 1; }
            memcpy(sec_name, P.src + name_start, nlen);
            sec_name[nlen] = '\0';

            /* Get or create the array at root[sec_name] */
            NovaValue root_val = nova_vm_get(vm, root_idx);
            NovaTable *rt = nova_as_table(root_val);
            NovaTable *arr = NULL;

            /* Search for existing array using content comparison.
             * nova_table_get_cstr handles non-interned string lookup. */
            if (rt != NULL) {
                NovaValue existing = nova_table_get_cstr(rt, sec_name, (uint32_t)nlen);
                if (nova_is_table(existing)) {
                    arr = nova_as_table(existing);
                }
            }

            if (arr == NULL) {
                /* Create new array table, save reference, set on root */
                nova_vm_push_table(vm);
                int arr_slot = nova_vm_get_top(vm) - 1;
                NovaValue arr_val = nova_vm_get(vm, arr_slot);
                nova_vm_set_field(vm, root_idx, sec_name);  /* pops TOS */

                /* The array table is now stored in root[sec_name] */
                arr = nova_as_table(arr_val);
            }

            if (arr == NULL) {
                ndp_error(&P, "failed to create array of tables");
                return -1;
            }

            /* Create new element table and add to array */
            nova_vm_push_table(vm);
            target_idx = nova_vm_get_top(vm) - 1;
            NovaValue elem = nova_vm_get(vm, target_idx);
            nova_table_raw_set_int(vm, arr, (nova_int_t)nova_table_array_len(arr), elem);

            /* Skip rest of line */
            while (!ndp_eof(&P) && ndp_peek(&P) != '\n') { ndp_advance(&P); }
            continue;
        }

        /* [table] section */
        if (c == '[') {
            ndp_advance(&P);
            while (!ndp_eof(&P) && (ndp_peek(&P) == ' ' || ndp_peek(&P) == '\t')) {
                ndp_advance(&P);
            }
            size_t name_start = P.pos;
            while (!ndp_eof(&P) && ndp_peek(&P) != ']' && ndp_peek(&P) != '\n') {
                ndp_advance(&P);
            }
            size_t name_end = P.pos;

            if (!ndp_match(&P, ']')) {
                ndp_error(&P, "unterminated [table]");
                return -1;
            }

            while (name_end > name_start &&
                   (P.src[name_end - 1] == ' ' || P.src[name_end - 1] == '\t')) {
                name_end--;
            }

            char sec_name[256];
            size_t nlen = name_end - name_start;
            if (nlen >= sizeof(sec_name)) { nlen = sizeof(sec_name) - 1; }
            memcpy(sec_name, P.src + name_start, nlen);
            sec_name[nlen] = '\0';

            /* Create section table */
            nova_vm_push_table(vm);
            target_idx = nova_vm_get_top(vm) - 1;
            NovaValue toml_sec_val = nova_vm_get(vm, target_idx);

            nova_vm_set_field(vm, root_idx, sec_name);  /* pops TOS */

            /* Re-push saved section table for subsequent key=value fields.
             * Same fix as INI: nova_vm_intern_string does not deduplicate,
             * so the old pointer-comparison recovery always failed. */
            nova_vm_push_value(vm, toml_sec_val);
            target_idx = nova_vm_get_top(vm) - 1;

            while (!ndp_eof(&P) && ndp_peek(&P) != '\n') { ndp_advance(&P); }
            continue;
        }

        /* Key = value */
        size_t key_start = P.pos;
        while (!ndp_eof(&P) && ndp_peek(&P) != '=' && ndp_peek(&P) != '\n') {
            ndp_advance(&P);
        }
        size_t key_end = P.pos;

        if (ndp_eof(&P) || ndp_peek(&P) == '\n') {
            continue;  /* no = found, skip line */
        }

        ndp_advance(&P);  /* consume '=' */

        /* Trim key */
        while (key_start < key_end &&
               (P.src[key_start] == ' ' || P.src[key_start] == '\t')) {
            key_start++;
        }
        while (key_end > key_start &&
               (P.src[key_end - 1] == ' ' || P.src[key_end - 1] == '\t')) {
            key_end--;
        }

        /* Parse value */
        if (ndp_toml_decode_value(&P) != 0) {
            return -1;
        }

        /* Set field */
        char key_name[256];
        size_t klen = key_end - key_start;
        if (klen >= sizeof(key_name)) { klen = sizeof(key_name) - 1; }
        memcpy(key_name, P.src + key_start, klen);
        key_name[klen] = '\0';

        nova_vm_set_field(vm, target_idx, key_name);

        /* Skip rest of line (comments) */
        while (!ndp_eof(&P) && ndp_peek(&P) != '\n') {
            if (ndp_peek(&P) == '#') { break; }
            ndp_advance(&P);
        }
        while (!ndp_eof(&P) && ndp_peek(&P) != '\n') { ndp_advance(&P); }
    }

    /* Trim stack to just root */
    nova_vm_set_top(vm, root_idx + 1);
    return 0;
}

static int ndp_toml_encode(NovaVM *vm, int idx, const NdpOptions *opts,
                            NdpBuf *out, char *errbuf) {
    /* TOML encode reuses INI encode format for now.
       The main difference: TOML uses typed values natively,
       and supports inline arrays which we render as [...] */
    NovaValue val = nova_vm_get(vm, idx);
    if (!nova_is_table(val)) {
        if (errbuf != NULL) {
            (void)snprintf(errbuf, 256, "TOML encode: expected table, got %s",
                           nova_vm_typename(nova_typeof(val)));
        }
        return -1;
    }

    NovaTable *t = nova_as_table(val);
    if (t == NULL) {
        return 0;
    }

    /* Forward declaration needed for recursive array encoding */
    /* We inline a small helper here */

    /* First pass: write bare key = value (non-table values) */
    uint32_t iter_tp1 = nova_table_array_len(t);
    NovaValue hkey, hval;
    while (nova_table_next(t, &iter_tp1, &hkey, &hval)) {
        if (!nova_is_string(hkey)) { continue; }
        if (nova_is_table(hval)) { continue; }

        ndp_buf_puts(out, nova_str_data(nova_as_string(hkey)));
        ndp_buf_puts(out, " = ");

        switch (nova_typeof(hval)) {
            case NOVA_TYPE_STRING:
                ndp_buf_putc(out, '"');
                /* Escape special chars */
                {
                    size_t si = 0;
                    for (si = 0; si < nova_str_len(nova_as_string(hval)); si++) {
                        char sc = nova_str_data(nova_as_string(hval))[si];
                        if (sc == '"') { ndp_buf_puts(out, "\\\""); }
                        else if (sc == '\\') { ndp_buf_puts(out, "\\\\"); }
                        else if (sc == '\n') { ndp_buf_puts(out, "\\n"); }
                        else if (sc == '\t') { ndp_buf_puts(out, "\\t"); }
                        else { ndp_buf_putc(out, sc); }
                    }
                }
                ndp_buf_putc(out, '"');
                break;
            case NOVA_TYPE_INTEGER:
                ndp_buf_printf(out, "%lld", (long long)nova_as_integer(hval));
                break;
            case NOVA_TYPE_NUMBER:
                if (isinf(nova_as_number(hval))) {
                    ndp_buf_puts(out, nova_as_number(hval) > 0 ? "inf" : "-inf");
                } else if (isnan(nova_as_number(hval))) {
                    ndp_buf_puts(out, "nan");
                } else {
                    ndp_buf_printf(out, "%.17g", nova_as_number(hval));
                }
                break;
            case NOVA_TYPE_BOOL:
                ndp_buf_puts(out, nova_as_bool(hval) ? "true" : "false");
                break;
            default:
                break;
        }
        ndp_buf_putc(out, '\n');
    }

    /* Second pass: [section] blocks for table values */
    uint32_t iter_tp2 = nova_table_array_len(t);
    NovaValue hkey2, hval2;
    while (nova_table_next(t, &iter_tp2, &hkey2, &hval2)) {
        if (!nova_is_string(hkey2) || !nova_is_table(hval2)) {
            continue;
        }

        NovaTable *sec = nova_as_table(hval2);
        if (sec == NULL) { continue; }

        /* Check if it's an array of tables (array part with table elements) */
        if (nova_table_array_len(sec) > 0 && nova_table_hash_count(sec) == 0 &&
            nova_is_table(nova_table_get_int(sec, 0))) {
            /* [[array_of_tables]] */
            uint32_t ai = 0;
            for (ai = 0; ai < nova_table_array_len(sec); ai++) {
                ndp_buf_puts(out, "\n[[");
                ndp_buf_puts(out, nova_str_data(nova_as_string(hkey2)));
                ndp_buf_puts(out, "]]\n");

                NovaValue sec_elem = nova_table_get_int(sec, ai);
                if (nova_is_table(sec_elem)) {
                    NovaTable *elem = nova_as_table(sec_elem);
                    if (elem != NULL) {
                        uint32_t iter_ek = nova_table_array_len(elem);
                        NovaValue ek_key, ev;
                        while (nova_table_next(elem, &iter_ek, &ek_key, &ev)) {
                            if (!nova_is_string(ek_key)) { continue; }
                            ndp_buf_puts(out, nova_str_data(nova_as_string(ek_key)));
                            ndp_buf_puts(out, " = ");
                            if (nova_is_string(ev)) {
                                ndp_buf_putc(out, '"');
                                ndp_buf_append(out, nova_str_data(nova_as_string(ev)), nova_str_len(nova_as_string(ev)));
                                ndp_buf_putc(out, '"');
                            } else if (nova_is_integer(ev)) {
                                ndp_buf_printf(out, "%lld", (long long)nova_as_integer(ev));
                            } else if (nova_is_number(ev)) {
                                ndp_buf_printf(out, "%.17g", nova_as_number(ev));
                            } else if (nova_is_bool(ev)) {
                                ndp_buf_puts(out, nova_as_bool(ev) ? "true" : "false");
                            }
                            ndp_buf_putc(out, '\n');
                        }
                    }
                }
            }
        } else {
            /* Regular [table] */
            ndp_buf_puts(out, "\n[");
            ndp_buf_puts(out, nova_str_data(nova_as_string(hkey2)));
            ndp_buf_puts(out, "]\n");

            uint32_t iter_sk = nova_table_array_len(sec);
            NovaValue sk_key, sv;
            while (nova_table_next(sec, &iter_sk, &sk_key, &sv)) {
                if (!nova_is_string(sk_key)) { continue; }
                ndp_buf_puts(out, nova_str_data(nova_as_string(sk_key)));
                ndp_buf_puts(out, " = ");
                if (nova_is_string(sv)) {
                    ndp_buf_putc(out, '"');
                    ndp_buf_append(out, nova_str_data(nova_as_string(sv)), nova_str_len(nova_as_string(sv)));
                    ndp_buf_putc(out, '"');
                } else if (nova_is_integer(sv)) {
                    ndp_buf_printf(out, "%lld", (long long)nova_as_integer(sv));
                } else if (nova_is_number(sv)) {
                    ndp_buf_printf(out, "%.17g", nova_as_number(sv));
                } else if (nova_is_bool(sv)) {
                    ndp_buf_puts(out, nova_as_bool(sv) ? "true" : "false");
                } else if (nova_is_table(sv)) {
                    /* Inline array */
                    NovaTable *at = nova_as_table(sv);
                    if (at != NULL && nova_table_array_len(at) > 0) {
                        ndp_buf_putc(out, '[');
                        uint32_t ai2 = 0;
                        for (ai2 = 0; ai2 < nova_table_array_len(at); ai2++) {
                            if (ai2 > 0) { ndp_buf_puts(out, ", "); }
                            NovaValue av = nova_table_get_int(at, ai2);
                            if (nova_is_string(av)) {
                                ndp_buf_putc(out, '"');
                                ndp_buf_append(out, nova_str_data(nova_as_string(av)), nova_str_len(nova_as_string(av)));
                                ndp_buf_putc(out, '"');
                            } else if (nova_is_integer(av)) {
                                ndp_buf_printf(out, "%lld", (long long)nova_as_integer(av));
                            } else if (nova_is_number(av)) {
                                ndp_buf_printf(out, "%.17g", nova_as_number(av));
                            } else if (nova_is_bool(av)) {
                                ndp_buf_puts(out, nova_as_bool(av) ? "true" : "false");
                            }
                        }
                        ndp_buf_putc(out, ']');
                    }
                }
                ndp_buf_putc(out, '\n');
            }
        }
    }

    (void)opts;
    return 0;
}

/* ============================================================
 * PART 8a: YAML CODEC
 *
 * Practical YAML subset parser supporting:
 *   - Mappings (key: value)
 *   - Block sequences (- item)
 *   - Nested structures via indentation
 *   - Scalars: strings, integers, floats, booleans, null
 *   - Quoted strings (single and double)
 *   - Comments (# ...)
 *   - Document markers (--- / ...) — takes first document
 *
 * Does NOT support: anchors, aliases, flow collections,
 * multi-line scalars, tags, complex keys.
 * ============================================================ */

/**
 * @brief Determine the indentation level of a line.
 */
static size_t ndp_yaml_indent(const char *line, size_t len) {
    size_t n = 0;
    while (n < len && line[n] == ' ') {
        n++;
    }
    return n;
}

/**
 * @brief Parse a YAML scalar value and push onto the VM stack.
 * Handles null, booleans, integers, floats, and strings.
 */
static void ndp_yaml_push_scalar(NovaVM *vm, const char *s, size_t len) {
    if (len == 0) {
        nova_vm_push_nil(vm);
        return;
    }

    /* Strip inline comment */
    size_t orig_len = len;
    size_t ci = 0;
    int in_sq = 0;
    int in_dq = 0;
    for (ci = 0; ci < orig_len; ci++) {
        if (s[ci] == '\'' && !in_dq) { in_sq = !in_sq; }
        else if (s[ci] == '"' && !in_sq) { in_dq = !in_dq; }
        else if (s[ci] == '#' && !in_sq && !in_dq && ci > 0 && s[ci - 1] == ' ') {
            len = ci;
            /* Trim trailing spaces before the comment */
            while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) {
                len--;
            }
            break;
        }
    }

    /* Trim trailing whitespace */
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                       s[len - 1] == '\r')) {
        len--;
    }

    if (len == 0) {
        nova_vm_push_nil(vm);
        return;
    }

    /* Quoted string */
    if (len >= 2) {
        if ((s[0] == '"' && s[len - 1] == '"') ||
            (s[0] == '\'' && s[len - 1] == '\'')) {
            nova_vm_push_string(vm, s + 1, len - 2);
            return;
        }
    }

    /* Null variants */
    if ((len == 1 && s[0] == '~') ||
        (len == 4 && strncasecmp(s, "null", 4) == 0) ||
        (len == 3 && strncasecmp(s, "nil", 3) == 0)) {
        nova_vm_push_nil(vm);
        return;
    }

    /* Boolean */
    if ((len == 4 && strncasecmp(s, "true", 4) == 0) ||
        (len == 3 && strncasecmp(s, "yes", 3) == 0) ||
        (len == 2 && strncasecmp(s, "on", 2) == 0)) {
        nova_vm_push_bool(vm, 1);
        return;
    }
    if ((len == 5 && strncasecmp(s, "false", 5) == 0) ||
        (len == 2 && strncasecmp(s, "no", 2) == 0) ||
        (len == 3 && strncasecmp(s, "off", 3) == 0)) {
        nova_vm_push_bool(vm, 0);
        return;
    }

    /* Try integer */
    char *end = NULL;
    errno = 0;
    long long ll = strtoll(s, &end, 10);
    if ((size_t)(end - s) == len && errno == 0) {
        nova_vm_push_integer(vm, (nova_int_t)ll);
        return;
    }

    /* Try float */
    errno = 0;
    double d = strtod(s, &end);
    if ((size_t)(end - s) == len && errno == 0) {
        nova_vm_push_number(vm, d);
        return;
    }

    /* Default: bare string */
    nova_vm_push_string(vm, s, len);
}

/**
 * @brief YAML line descriptor.
 */
typedef struct {
    const char *start;     /**< Pointer to line start (after leading spaces) */
    size_t      len;       /**< Length of content (excluding newline) */
    size_t      indent;    /**< Leading space count */
    size_t      raw_start; /**< Offset of line start from text */
    size_t      raw_len;   /**< Total line length including indent */
} YamlLine;

/**
 * @brief Split YAML text into lines, skipping comments and blanks.
 */
static int ndp_yaml_split_lines(const char *text, size_t len,
                                 YamlLine **out_lines, size_t *out_count) {
    /* Count lines first */
    size_t capacity = 64;
    YamlLine *lines = (YamlLine *)malloc(capacity * sizeof(YamlLine));
    if (lines == NULL) {
        return -1;
    }
    size_t count = 0;
    size_t pos = 0;

    while (pos < len) {
        size_t line_start = pos;
        while (pos < len && text[pos] != '\n') {
            pos++;
        }
        size_t line_end = pos;
        if (pos < len) { pos++; }  /* skip newline */

        /* Remove trailing \r */
        if (line_end > line_start && text[line_end - 1] == '\r') {
            line_end--;
        }

        size_t indent = ndp_yaml_indent(text + line_start,
                                         line_end - line_start);
        const char *content = text + line_start + indent;
        size_t clen = line_end - line_start - indent;

        /* Skip blank lines */
        if (clen == 0) { continue; }

        /* Skip comment-only lines */
        if (content[0] == '#') { continue; }

        /* Skip document markers */
        if (clen >= 3 && memcmp(content, "---", 3) == 0) { continue; }
        if (clen >= 3 && memcmp(content, "...", 3) == 0) { continue; }

        /* Store line */
        if (count >= capacity) {
            capacity *= 2;
            YamlLine *tmp = (YamlLine *)realloc(lines,
                                                 capacity * sizeof(YamlLine));
            if (tmp == NULL) {
                free(lines);
                return -1;
            }
            lines = tmp;
        }

        lines[count].start     = content;
        lines[count].len       = clen;
        lines[count].indent    = indent;
        lines[count].raw_start = line_start;
        lines[count].raw_len   = line_end - line_start;
        count++;
    }

    *out_lines = lines;
    *out_count = count;
    return 0;
}

/* Forward declaration for recursive parsing */
static size_t ndp_yaml_parse_value(NovaVM *vm, YamlLine *lines,
                                    size_t count, size_t idx,
                                    size_t base_indent);

/**
 * @brief Parse a YAML mapping (indented key: value block).
 * Pushes a table onto the VM stack.
 * @return Index of next line to process after the mapping.
 */
static size_t ndp_yaml_parse_mapping(NovaVM *vm, YamlLine *lines,
                                      size_t count, size_t idx,
                                      size_t map_indent) {
    nova_vm_push_table(vm);
    int table_idx = nova_vm_get_top(vm) - 1;

    while (idx < count && lines[idx].indent == map_indent) {
        const char *line = lines[idx].start;
        size_t llen = lines[idx].len;

        /* Check for sequence item at this indent */
        if (llen >= 2 && line[0] == '-' && line[1] == ' ') {
            /* This is a sequence, not a mapping — backtrack.
             * Caller should have called parse_sequence. */
            break;
        }

        /* Find colon separator (key: value) */
        size_t colon = 0;
        int found_colon = 0;
        int in_quotes = 0;
        for (colon = 0; colon < llen; colon++) {
            if (line[colon] == '"' || line[colon] == '\'') {
                in_quotes = !in_quotes;
            }
            if (!in_quotes && line[colon] == ':' &&
                (colon + 1 >= llen || line[colon + 1] == ' ')) {
                found_colon = 1;
                break;
            }
        }

        if (!found_colon) {
            /* Not a key:value line, skip */
            idx++;
            continue;
        }

        /* Extract key */
        size_t klen = colon;
        while (klen > 0 && (line[klen - 1] == ' ' || line[klen - 1] == '\t')) {
            klen--;
        }

        /* NUL-terminated key for set_field */
        char key_buf[256];
        if (klen >= sizeof(key_buf)) { klen = sizeof(key_buf) - 1; }
        memcpy(key_buf, line, klen);
        key_buf[klen] = '\0';

        /* Value starts after ": " */
        size_t vstart = colon + 1;
        while (vstart < llen && (line[vstart] == ' ' || line[vstart] == '\t')) {
            vstart++;
        }

        if (vstart < llen) {
            /* Inline value */
            ndp_yaml_push_scalar(vm, line + vstart, llen - vstart);
            nova_vm_set_field(vm, table_idx, key_buf);
            idx++;
        } else {
            /* Empty value — check next line for nested content */
            idx++;
            if (idx < count && lines[idx].indent > map_indent) {
                size_t child_indent = lines[idx].indent;
                idx = ndp_yaml_parse_value(vm, lines, count, idx,
                                            child_indent);
                nova_vm_set_field(vm, table_idx, key_buf);
            } else {
                /* Empty scalar */
                nova_vm_push_nil(vm);
                nova_vm_set_field(vm, table_idx, key_buf);
            }
        }
    }

    return idx;
}

/**
 * @brief Parse a YAML sequence (indented - item block).
 * Pushes a table (array) onto the VM stack.
 * @return Index of next line to process after the sequence.
 */
static size_t ndp_yaml_parse_sequence(NovaVM *vm, YamlLine *lines,
                                       size_t count, size_t idx,
                                       size_t seq_indent) {
    nova_vm_push_table(vm);
    int arr_idx = nova_vm_get_top(vm) - 1;
    NovaValue arr_val = nova_vm_get(vm, arr_idx);
    nova_int_t arr_n = 0;

    while (idx < count && lines[idx].indent == seq_indent) {
        const char *line = lines[idx].start;
        size_t llen = lines[idx].len;

        if (llen < 2 || line[0] != '-' || line[1] != ' ') {
            break;  /* Not a sequence item anymore */
        }

        /* Content after "- " */
        const char *content = line + 2;
        size_t clen = llen - 2;

        /* Trim leading spaces */
        while (clen > 0 && content[0] == ' ') {
            content++;
            clen--;
        }

        if (clen > 0) {
            /* Check if item has nested key: value */
            size_t ck = 0;
            int has_colon = 0;
            for (ck = 0; ck < clen; ck++) {
                if (content[ck] == ':' &&
                    (ck + 1 >= clen || content[ck + 1] == ' ')) {
                    has_colon = 1;
                    break;
                }
            }

            if (has_colon) {
                /* This list item is a mapping.
                 * Create a sub-table for this item.
                 * The first key:value is on this line, rest follow at
                 * deeper indent. We need to push this as a mapping entry. */
                nova_vm_push_table(vm);
                int item_idx = nova_vm_get_top(vm) - 1;

                /* Parse first key: value inline */
                size_t klen2 = ck;
                while (klen2 > 0 && (content[klen2 - 1] == ' ')) { klen2--; }
                char kbuf[256];
                if (klen2 >= sizeof(kbuf)) { klen2 = sizeof(kbuf) - 1; }
                memcpy(kbuf, content, klen2);
                kbuf[klen2] = '\0';

                size_t vs = ck + 1;
                while (vs < clen && content[vs] == ' ') { vs++; }

                if (vs < clen) {
                    ndp_yaml_push_scalar(vm, content + vs, clen - vs);
                } else {
                    nova_vm_push_nil(vm);
                }
                nova_vm_set_field(vm, item_idx, kbuf);

                /* Parse any continuation lines at deeper indent */
                size_t next = idx + 1;
                size_t item_indent = seq_indent + 2;
                while (next < count && lines[next].indent >= item_indent) {
                    const char *nl = lines[next].start;
                    size_t nll = lines[next].len;

                    /* Find colon in continuation */
                    size_t nc = 0;
                    int nfc = 0;
                    for (nc = 0; nc < nll; nc++) {
                        if (nl[nc] == ':' &&
                            (nc + 1 >= nll || nl[nc + 1] == ' ')) {
                            nfc = 1;
                            break;
                        }
                    }

                    if (nfc) {
                        /* Another key:value in this item */
                        size_t nkl = nc;
                        while (nkl > 0 && nl[nkl - 1] == ' ') { nkl--; }
                        char nkb[256];
                        if (nkl >= sizeof(nkb)) { nkl = sizeof(nkb) - 1; }
                        memcpy(nkb, nl, nkl);
                        nkb[nkl] = '\0';

                        size_t nvs = nc + 1;
                        while (nvs < nll && nl[nvs] == ' ') { nvs++; }

                        if (nvs < nll) {
                            ndp_yaml_push_scalar(vm, nl + nvs, nll - nvs);
                            nova_vm_set_field(vm, item_idx, nkb);
                            next++;
                        } else {
                            /* Check for nested block */
                            if (next + 1 < count &&
                                lines[next + 1].indent > lines[next].indent) {
                                size_t ci2 = lines[next + 1].indent;
                                next++;
                                next = ndp_yaml_parse_value(vm, lines, count,
                                                            next, ci2);
                                nova_vm_set_field(vm, item_idx, nkb);
                                /* Don't next++ — parse_value already
                                 * advanced past consumed lines. */
                            } else {
                                nova_vm_push_nil(vm);
                                nova_vm_set_field(vm, item_idx, nkb);
                                next++;
                            }
                        }
                    } else {
                        next++;
                    }
                }
                idx = next;

                /* Add item to array */
                NovaValue item_val = nova_vm_get(vm, item_idx);
                nova_table_raw_set_int(vm, nova_as_table(arr_val), arr_n, item_val);
                arr_n++;

                /* Pop the item table */
                nova_vm_set_top(vm, item_idx);
            } else {
                /* Simple scalar item */
                ndp_yaml_push_scalar(vm, content, clen);
                NovaValue sv = nova_vm_get(vm, nova_vm_get_top(vm) - 1);
                nova_table_raw_set_int(vm, nova_as_table(arr_val), arr_n, sv);
                nova_vm_set_top(vm, arr_idx + 1);  /* pop scalar */
                arr_n++;
                idx++;
            }
        } else {
            /* Check for nested block under "- " */
            idx++;
            if (idx < count && lines[idx].indent > seq_indent) {
                size_t child_indent = lines[idx].indent;
                idx = ndp_yaml_parse_value(vm, lines, count, idx,
                                            child_indent);
                NovaValue cv = nova_vm_get(vm, nova_vm_get_top(vm) - 1);
                nova_table_raw_set_int(vm, nova_as_table(arr_val), arr_n, cv);
                nova_vm_set_top(vm, arr_idx + 1);  /* pop child */
                arr_n++;
            } else {
                /* Empty item = null */
                nova_table_raw_set_int(vm, nova_as_table(arr_val), arr_n,
                                       nova_value_nil());
                arr_n++;
            }
        }
    }

    return idx;
}

/**
 * @brief Parse a YAML value (mapping or sequence) at the given indent.
 * Pushes exactly one value onto the VM stack.
 * @return Index of next line to process.
 */
static size_t ndp_yaml_parse_value(NovaVM *vm, YamlLine *lines,
                                    size_t count, size_t idx,
                                    size_t base_indent) {
    if (idx >= count) {
        nova_vm_push_nil(vm);
        return idx;
    }

    const char *line = lines[idx].start;
    size_t llen = lines[idx].len;

    /* Sequence? */
    if (llen >= 2 && line[0] == '-' && line[1] == ' ') {
        return ndp_yaml_parse_sequence(vm, lines, count, idx, base_indent);
    }

    /* Mapping? (has colon) */
    return ndp_yaml_parse_mapping(vm, lines, count, idx, base_indent);
}

/**
 * @brief Decode YAML text into a Nova table.
 */
static int ndp_yaml_decode(NovaVM *vm, const char *text, size_t len,
                            const NdpOptions *opts, char *errbuf) {
    (void)opts;

    YamlLine *lines = NULL;
    size_t line_count = 0;

    if (ndp_yaml_split_lines(text, len, &lines, &line_count) != 0) {
        if (errbuf != NULL) {
            (void)snprintf(errbuf, 256, "YAML decode: memory allocation failed");
        }
        return -1;
    }

    if (line_count == 0) {
        free(lines);
        nova_vm_push_table(vm);
        return 0;
    }

    /* Determine if root is sequence or mapping */
    size_t root_indent = lines[0].indent;
    (void)ndp_yaml_parse_value(vm, lines, line_count, 0, root_indent);

    free(lines);
    return 0;
}

/**
 * @brief Emit YAML indent.
 */
static void ndp_yaml_emit_indent(NdpBuf *out, int depth) {
    int i = 0;
    for (i = 0; i < depth * 2; i++) {
        ndp_buf_putc(out, ' ');
    }
}

/**
 * @brief Emit a YAML scalar value.
 */
static void ndp_yaml_emit_scalar(NdpBuf *out, NovaValue val) {
    switch (nova_typeof(val)) {
        case NOVA_TYPE_NIL:
            ndp_buf_puts(out, "null");
            break;
        case NOVA_TYPE_BOOL:
            ndp_buf_puts(out, nova_as_bool(val) ? "true" : "false");
            break;
        case NOVA_TYPE_INTEGER:
            ndp_buf_printf(out, "%lld", (long long)nova_as_integer(val));
            break;
        case NOVA_TYPE_NUMBER:
            ndp_buf_printf(out, "%.17g", nova_as_number(val));
            break;
        case NOVA_TYPE_STRING: {
            /* Check if quoting needed */
            const char *s = nova_str_data(nova_as_string(val));
            size_t slen = nova_str_len(nova_as_string(val));
            int needs_quote = 0;
            size_t qi = 0;
            if (slen == 0) {
                needs_quote = 1;
            }
            for (qi = 0; qi < slen && !needs_quote; qi++) {
                if (s[qi] == ':' || s[qi] == '#' || s[qi] == '"' ||
                    s[qi] == '\'' || s[qi] == '[' || s[qi] == ']' ||
                    s[qi] == '{' || s[qi] == '}' || s[qi] == ',' ||
                    s[qi] == '\n' || s[qi] == '\r') {
                    needs_quote = 1;
                }
            }
            /* Also quote if it looks like a boolean/null/number */
            if (!needs_quote && slen <= 5) {
                if (strncasecmp(s, "true", 4) == 0 ||
                    strncasecmp(s, "false", 5) == 0 ||
                    strncasecmp(s, "null", 4) == 0 ||
                    strncasecmp(s, "nil", 3) == 0 ||
                    strncasecmp(s, "yes", 3) == 0 ||
                    strncasecmp(s, "no", 2) == 0 ||
                    s[0] == '~') {
                    needs_quote = 1;
                }
            }
            if (needs_quote) {
                ndp_buf_putc(out, '"');
                ndp_buf_append(out, s, slen);
                ndp_buf_putc(out, '"');
            } else {
                ndp_buf_append(out, s, slen);
            }
            break;
        }
        default:
            ndp_buf_puts(out, "null");
            break;
    }
}

/* Forward declaration */
static void ndp_yaml_emit_value(NdpBuf *out, NovaValue val, int depth);

/**
 * @brief Emit a YAML table (mapping or array) recursively.
 */
static void ndp_yaml_emit_table(NdpBuf *out, NovaTable *t, int depth) {
    if (t == NULL) {
        ndp_buf_puts(out, "{}");
        return;
    }

    /* Check if it's an array (has integer-indexed elements) */
    if (nova_table_array_len(t) > 0) {
        /* Emit as sequence */
        uint32_t ai = 0;
        for (ai = 0; ai < nova_table_array_len(t); ai++) {
            ndp_yaml_emit_indent(out, depth);
            ndp_buf_puts(out, "- ");
            NovaValue av = nova_table_get_int(t, ai);
            if (nova_is_table(av)) {
                ndp_buf_putc(out, '\n');
                ndp_yaml_emit_table(out, nova_as_table(av), depth + 1);
            } else {
                ndp_yaml_emit_scalar(out, av);
                ndp_buf_putc(out, '\n');
            }
        }
        return;
    }

    /* Emit as mapping */
    uint32_t iter_ym = nova_table_array_len(t);
    NovaValue hkey, hval;
    while (nova_table_next(t, &iter_ym, &hkey, &hval)) {

        if (!nova_is_string(hkey)) { continue; }

        ndp_yaml_emit_indent(out, depth);
        ndp_buf_append(out, nova_str_data(nova_as_string(hkey)), nova_str_len(nova_as_string(hkey)));
        ndp_buf_puts(out, ": ");

        if (nova_is_table(hval)) {
            ndp_buf_putc(out, '\n');
            ndp_yaml_emit_table(out, nova_as_table(hval), depth + 1);
        } else {
            ndp_yaml_emit_scalar(out, hval);
            ndp_buf_putc(out, '\n');
        }
    }
}

/**
 * @brief Emit any YAML value recursively.
 */
__attribute__((unused))
static void ndp_yaml_emit_value(NdpBuf *out, NovaValue val, int depth) {
    if (nova_is_table(val)) {
        ndp_yaml_emit_table(out, nova_as_table(val), depth);
    } else {
        ndp_yaml_emit_scalar(out, val);
        ndp_buf_putc(out, '\n');
    }
}

/**
 * @brief Encode a Nova value as YAML text.
 */
static int ndp_yaml_encode(NovaVM *vm, int idx, const NdpOptions *opts,
                            NdpBuf *out, char *errbuf) {
    (void)opts;
    NovaValue val = nova_vm_get(vm, idx);

    if (!nova_is_table(val)) {
        /* Scalar at root level */
        ndp_yaml_emit_scalar(out, val);
        ndp_buf_putc(out, '\n');
        return 0;
    }

    ndp_yaml_emit_table(out, nova_as_table(val), 0);

    if (errbuf != NULL) {
        errbuf[0] = '\0';
    }
    return 0;
}

/* ============================================================
 * PART 8: HTML CODEC
 *
 * Tag-soup tolerant HTML parser that produces a tree of tables:
 *   {tag="div", attrs={class="foo"}, children={...}}
 *
 * Handles:
 *   - Self-closing tags (<br/>, <img .../>)
 *   - Void elements (br, hr, img, input, meta, link, etc.)
 *   - Attributes (quoted and unquoted values)
 *   - Text nodes (as plain string children)
 *   - Comments (<!-- ... --> skipped)
 *   - DOCTYPE (skipped)
 *   - Case-insensitive tag matching
 *   - Unmatched closing tags (ignored gracefully)
 *
 * Encode (reverse): table tree -> HTML text
 *
 * text_only mode: extracts concatenated text content only
 * ============================================================ */

/** List of HTML void elements (no closing tag) */
static int ndp_html_is_void(const char *tag) {
    static const char *voids[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr", NULL
    };
    int i = 0;
    for (i = 0; voids[i] != NULL; i++) {
        if (strcasecmp(tag, voids[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/**
 * @brief Decode HTML entity references.
 * Only common ones: &amp; &lt; &gt; &quot; &apos; &#NNN; &#xHHH;
 */
static void ndp_html_decode_entity(NdpParser *P, NdpBuf *out) {
    ndp_advance(P);  /* consume '&' */

    if (!ndp_eof(P) && ndp_peek(P) == '#') {
        ndp_advance(P);
        unsigned int cp = 0;
        if (!ndp_eof(P) && (ndp_peek(P) == 'x' || ndp_peek(P) == 'X')) {
            ndp_advance(P);
            while (!ndp_eof(P) && ndp_peek(P) != ';') {
                char h = ndp_advance(P);
                cp <<= 4;
                if (h >= '0' && h <= '9') { cp |= (unsigned int)(h - '0'); }
                else if (h >= 'a' && h <= 'f') { cp |= (unsigned int)(h - 'a' + 10); }
                else if (h >= 'A' && h <= 'F') { cp |= (unsigned int)(h - 'A' + 10); }
            }
        } else {
            while (!ndp_eof(P) && ndp_peek(P) != ';') {
                char d = ndp_advance(P);
                if (d >= '0' && d <= '9') {
                    cp = cp * 10 + (unsigned int)(d - '0');
                }
            }
        }
        ndp_match(P, ';');
        /* Encode codepoint as UTF-8 */
        if (cp < 0x80) {
            ndp_buf_putc(out, (char)cp);
        } else if (cp < 0x800) {
            ndp_buf_putc(out, (char)(0xC0 | (cp >> 6)));
            ndp_buf_putc(out, (char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            ndp_buf_putc(out, (char)(0xE0 | (cp >> 12)));
            ndp_buf_putc(out, (char)(0x80 | ((cp >> 6) & 0x3F)));
            ndp_buf_putc(out, (char)(0x80 | (cp & 0x3F)));
        } else {
            ndp_buf_putc(out, (char)(0xF0 | (cp >> 18)));
            ndp_buf_putc(out, (char)(0x80 | ((cp >> 12) & 0x3F)));
            ndp_buf_putc(out, (char)(0x80 | ((cp >> 6) & 0x3F)));
            ndp_buf_putc(out, (char)(0x80 | (cp & 0x3F)));
        }
        return;
    }

    /* Named entities */
    char ent[16];
    int ei = 0;
    while (!ndp_eof(P) && ndp_peek(P) != ';' && ei < 15) {
        ent[ei++] = ndp_advance(P);
    }
    ent[ei] = '\0';
    ndp_match(P, ';');

    if (strcmp(ent, "amp") == 0) { ndp_buf_putc(out, '&'); }
    else if (strcmp(ent, "lt") == 0) { ndp_buf_putc(out, '<'); }
    else if (strcmp(ent, "gt") == 0) { ndp_buf_putc(out, '>'); }
    else if (strcmp(ent, "quot") == 0) { ndp_buf_putc(out, '"'); }
    else if (strcmp(ent, "apos") == 0) { ndp_buf_putc(out, '\''); }
    else if (strcmp(ent, "nbsp") == 0) { ndp_buf_putc(out, ' '); }
    else {
        /* Unknown entity: pass through */
        ndp_buf_putc(out, '&');
        ndp_buf_puts(out, ent);
        ndp_buf_putc(out, ';');
    }
}

/**
 * @brief Parse HTML text content into a string.
 * Handles entity decoding. Pushes string onto VM stack.
 */
static void ndp_html_parse_text(NdpParser *P) {
    NdpBuf text;
    ndp_buf_init(&text);

    while (!ndp_eof(P) && ndp_peek(P) != '<') {
        if (ndp_peek(P) == '&') {
            ndp_html_decode_entity(P, &text);
        } else {
            ndp_buf_putc(&text, ndp_advance(P));
        }
    }

    /* Trim leading/trailing whitespace */
    const char *s = text.data ? text.data : "";
    size_t slen = text.len;
    size_t start = 0;
    size_t end = slen;
    while (start < end && (s[start] == ' ' || s[start] == '\t' ||
                            s[start] == '\r' || s[start] == '\n')) {
        start++;
    }
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' ||
                            s[end - 1] == '\r' || s[end - 1] == '\n')) {
        end--;
    }

    if (end > start) {
        nova_vm_push_string(P->vm, s + start, end - start);
    } else {
        nova_vm_push_nil(P->vm);  /* flag: ignore empty text */
    }

    ndp_buf_free(&text);
}

/**
 * @brief Parse an HTML element (tag + attributes + children).
 * Pushes a table {tag="name", attrs={...}, children={...}} on stack,
 * or pushes nil if nothing meaningful was parsed.
 */
static int ndp_html_parse_element(NdpParser *P, int depth);

static int ndp_html_parse_element(NdpParser *P, int depth) {
    if (depth > 100) {
        ndp_error(P, "HTML nesting too deep (>100)");
        return -1;
    }

    ndp_advance(P);  /* consume '<' */

    /* Skip DOCTYPE, comments, processing instructions */
    if (ndp_peek(P) == '!') {
        ndp_advance(P);
        if (P->pos + 1 < P->len && P->src[P->pos] == '-' && P->src[P->pos + 1] == '-') {
            /* Comment: <!-- ... --> */
            ndp_advance(P); ndp_advance(P);
            while (!ndp_eof(P)) {
                if (P->pos + 2 < P->len && P->src[P->pos] == '-' &&
                    P->src[P->pos + 1] == '-' && P->src[P->pos + 2] == '>') {
                    ndp_advance(P); ndp_advance(P); ndp_advance(P);
                    break;
                }
                ndp_advance(P);
            }
        } else {
            /* DOCTYPE or other: skip to > */
            while (!ndp_eof(P) && ndp_peek(P) != '>') { ndp_advance(P); }
            ndp_match(P, '>');
        }
        nova_vm_push_nil(P->vm);
        return 0;
    }

    /* Processing instruction <?...?> */
    if (ndp_peek(P) == '?') {
        while (!ndp_eof(P) && ndp_peek(P) != '>') { ndp_advance(P); }
        ndp_match(P, '>');
        nova_vm_push_nil(P->vm);
        return 0;
    }

    /* Closing tag </name> - should not be called here normally */
    if (ndp_peek(P) == '/') {
        ndp_advance(P);
        while (!ndp_eof(P) && ndp_peek(P) != '>') { ndp_advance(P); }
        ndp_match(P, '>');
        nova_vm_push_nil(P->vm);
        return 0;
    }

    /* Parse tag name */
    char tag[64];
    int ti = 0;
    while (!ndp_eof(P) && ti < 63 &&
           ndp_peek(P) != ' ' && ndp_peek(P) != '\t' &&
           ndp_peek(P) != '/' && ndp_peek(P) != '>' &&
           ndp_peek(P) != '\n' && ndp_peek(P) != '\r') {
        tag[ti++] = (char)tolower((unsigned char)ndp_advance(P));
    }
    tag[ti] = '\0';

    /* Create element table */
    nova_vm_push_table(P->vm);
    int elem_idx = nova_vm_get_top(P->vm) - 1;

    /* Set tag name */
    nova_vm_push_string(P->vm, tag, (size_t)ti);
    nova_vm_set_field(P->vm, elem_idx, "tag");

    /* Parse attributes */
    nova_vm_push_table(P->vm);
    int attrs_idx = nova_vm_get_top(P->vm) - 1;

    int self_closing = 0;
    while (!ndp_eof(P) && ndp_peek(P) != '>' && ndp_peek(P) != '/') {
        /* Skip whitespace */
        while (!ndp_eof(P) && (ndp_peek(P) == ' ' || ndp_peek(P) == '\t' ||
                                ndp_peek(P) == '\n' || ndp_peek(P) == '\r')) {
            ndp_advance(P);
        }
        if (ndp_eof(P) || ndp_peek(P) == '>' || ndp_peek(P) == '/') { break; }

        /* Attribute name */
        char attr[64];
        int ai = 0;
        while (!ndp_eof(P) && ai < 63 &&
               ndp_peek(P) != '=' && ndp_peek(P) != ' ' &&
               ndp_peek(P) != '>' && ndp_peek(P) != '/') {
            attr[ai++] = (char)tolower((unsigned char)ndp_advance(P));
        }
        attr[ai] = '\0';

        if (ai == 0) { break; }

        /* Skip whitespace around = */
        while (!ndp_eof(P) && ndp_peek(P) == ' ') { ndp_advance(P); }

        if (!ndp_eof(P) && ndp_peek(P) == '=') {
            ndp_advance(P);
            while (!ndp_eof(P) && ndp_peek(P) == ' ') { ndp_advance(P); }

            /* Attribute value */
            NdpBuf val;
            ndp_buf_init(&val);

            if (!ndp_eof(P) && (ndp_peek(P) == '"' || ndp_peek(P) == '\'')) {
                char q = ndp_advance(P);
                while (!ndp_eof(P) && ndp_peek(P) != q) {
                    if (ndp_peek(P) == '&') {
                        ndp_html_decode_entity(P, &val);
                    } else {
                        ndp_buf_putc(&val, ndp_advance(P));
                    }
                }
                ndp_match(P, q);
            } else {
                /* Unquoted value */
                while (!ndp_eof(P) && ndp_peek(P) != ' ' &&
                       ndp_peek(P) != '>' && ndp_peek(P) != '/') {
                    ndp_buf_putc(&val, ndp_advance(P));
                }
            }

            nova_vm_push_string(P->vm, val.data ? val.data : "", val.len);
            nova_vm_set_field(P->vm, attrs_idx, attr);
            ndp_buf_free(&val);
        } else {
            /* Boolean attribute (no value) */
            nova_vm_push_bool(P->vm, 1);
            nova_vm_set_field(P->vm, attrs_idx, attr);
        }
    }

    /* Set attrs on element */
    /* attrs table is at attrs_idx; we need to move it under elem */
    nova_vm_set_field(P->vm, elem_idx, "attrs");

    /* Check for self-closing />  */
    if (!ndp_eof(P) && ndp_peek(P) == '/') {
        ndp_advance(P);
        self_closing = 1;
    }
    ndp_match(P, '>');

    /* Void elements don't need closing tags */
    if (self_closing || ndp_html_is_void(tag)) {
        return 0;
    }

    /* Parse children until closing tag </tag> */
    nova_vm_push_table(P->vm);
    int children_idx = nova_vm_get_top(P->vm) - 1;
    NovaValue children_val = nova_vm_get(P->vm, children_idx);
    NovaTable *children = nova_as_table(children_val);
    nova_int_t child_idx = 0;

    while (!ndp_eof(P)) {
        if (ndp_peek(P) == '<') {
            /* Check closing tag */
            if (P->pos + 1 < P->len && P->src[P->pos + 1] == '/') {
                /* Closing tag: check if it matches our tag */
                size_t save = P->pos;
                int save_line = P->line;
                int save_col = P->col;

                ndp_advance(P);  /* < */
                ndp_advance(P);  /* / */
                char ctag[64];
                int ci = 0;
                while (!ndp_eof(P) && ci < 63 && ndp_peek(P) != '>') {
                    ctag[ci++] = (char)tolower((unsigned char)ndp_advance(P));
                }
                ctag[ci] = '\0';
                /* Trim */
                while (ci > 0 && (ctag[ci-1] == ' ' || ctag[ci-1] == '\t')) {
                    ctag[--ci] = '\0';
                }
                ndp_match(P, '>');

                if (strcasecmp(ctag, tag) == 0) {
                    break;  /* matched our closing tag */
                }

                /* Unmatched closing tag: ignore it (tag-soup tolerance) */
                (void)save;
                (void)save_line;
                (void)save_col;
                continue;
            }

            /* Child element */
            if (ndp_html_parse_element(P, depth + 1) != 0) {
                return -1;
            }

            NovaValue child = nova_vm_get(P->vm, -1);
            nova_vm_pop(P->vm, 1);
            if (!nova_is_nil(child)) {
                nova_table_raw_set_int(P->vm, children, child_idx, child);
                child_idx++;
            }
        } else {
            /* Text content */
            ndp_html_parse_text(P);
            NovaValue text_val = nova_vm_get(P->vm, -1);
            nova_vm_pop(P->vm, 1);
            if (!nova_is_nil(text_val)) {
                nova_table_raw_set_int(P->vm, children, child_idx, text_val);
                child_idx++;
            }
        }
    }

    /* Set children on element */
    nova_vm_set_field(P->vm, elem_idx, "children");

    /* Clean stack: only element table should remain at elem_idx */
    nova_vm_set_top(P->vm, elem_idx + 1);

    return 0;
}

/**
 * @brief Text-only extraction: recursively extract text from table tree.
 */
static void ndp_html_extract_text(NovaVM *vm, NovaValue val, NdpBuf *out) {
    if (nova_is_string(val)) {
        ndp_buf_append(out, nova_str_data(nova_as_string(val)), nova_str_len(nova_as_string(val)));
        return;
    }
    if (!nova_is_table(val)) {
        return;
    }
    NovaTable *t = nova_as_table(val);
    if (t == NULL) {
        return;
    }

    /* Look for "children" field */
    NovaString *ckey = nova_vm_intern_string(vm, "children", 8);
    if (ckey != NULL) {
        NovaValue children = nova_table_get_str(t, ckey);
        if (nova_is_table(children) && nova_as_table(children) != NULL) {
            NovaTable *ct = nova_as_table(children);
            uint32_t ci = 0;
            for (ci = 0; ci < nova_table_array_len(ct); ci++) {
                ndp_html_extract_text(vm, nova_table_get_int(ct, ci), out);
            }
        }
    }
}

static int ndp_html_decode(NovaVM *vm, const char *text, size_t len,
                            const NdpOptions *opts, char *errbuf) {
    NdpParser P;
    ndp_parser_init(&P, vm, text, len, errbuf);

    /* Create root document table with children array */
    nova_vm_push_table(vm);
    int doc_idx = nova_vm_get_top(vm) - 1;

    nova_vm_push_string(vm, "document", 8);
    nova_vm_set_field(vm, doc_idx, "tag");

    nova_vm_push_table(vm);
    int children_idx = nova_vm_get_top(vm) - 1;
    NovaValue children_val = nova_vm_get(vm, children_idx);
    NovaTable *children = nova_as_table(children_val);
    nova_int_t child_count = 0;

    while (!ndp_eof(&P)) {
        /* Skip whitespace between elements */
        while (!ndp_eof(&P) && ndp_peek(&P) != '<') {
            char c = ndp_peek(&P);
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                /* Text node at top level */
                ndp_html_parse_text(&P);
                NovaValue tv = nova_vm_get(vm, -1);
                nova_vm_pop(vm, 1);
                if (!nova_is_nil(tv)) {
                    nova_table_raw_set_int(vm, children, child_count, tv);
                    child_count++;
                }
                continue;
            }
            ndp_advance(&P);
        }

        if (ndp_eof(&P)) { break; }

        if (ndp_html_parse_element(&P, 0) != 0) {
            return -1;
        }
        NovaValue elem = nova_vm_get(vm, -1);
        nova_vm_pop(vm, 1);
        if (!nova_is_nil(elem)) {
            nova_table_raw_set_int(vm, children, child_count, elem);
            child_count++;
        }
    }

    /* Set children on document */
    nova_vm_set_field(vm, doc_idx, "children");

    /* If text_only mode, extract text and push string instead */
    if (opts->html_text_only) {
        NovaValue doc = nova_vm_get(vm, doc_idx);
        NdpBuf text_buf;
        ndp_buf_init(&text_buf);
        ndp_html_extract_text(vm, doc, &text_buf);
        nova_vm_pop(vm, 1);  /* pop doc table */
        nova_vm_push_string(vm, text_buf.data ? text_buf.data : "", text_buf.len);
        ndp_buf_free(&text_buf);
    } else {
        /* Trim stack: only doc table should remain */
        nova_vm_set_top(vm, doc_idx + 1);
    }

    return 0;
}

/* --- HTML Encode --- */

static void ndp_html_encode_node(NovaVM *vm, NovaValue val,
                                  NdpBuf *out, int indent_level,
                                  const NdpOptions *opts);

static void ndp_html_encode_indent(NdpBuf *out, const NdpOptions *opts,
                                    int level) {
    if (!opts->pretty) { return; }
    int i = 0;
    int spaces = level * opts->indent;
    for (i = 0; i < spaces; i++) {
        ndp_buf_putc(out, ' ');
    }
}

static void ndp_html_encode_node(NovaVM *vm, NovaValue val,
                                  NdpBuf *out, int indent_level,
                                  const NdpOptions *opts) {
    if (nova_is_string(val)) {
        /* Text node */
        ndp_html_encode_indent(out, opts, indent_level);
        /* HTML-escape the text */
        const char *s = nova_str_data(nova_as_string(val));
        size_t slen = nova_str_len(nova_as_string(val));
        size_t i = 0;
        for (i = 0; i < slen; i++) {
            switch (s[i]) {
                case '<': ndp_buf_puts(out, "&lt;"); break;
                case '>': ndp_buf_puts(out, "&gt;"); break;
                case '&': ndp_buf_puts(out, "&amp;"); break;
                default:  ndp_buf_putc(out, s[i]); break;
            }
        }
        if (opts->pretty) { ndp_buf_putc(out, '\n'); }
        return;
    }

    if (!nova_is_table(val)) { return; }
    NovaTable *t = nova_as_table(val);
    if (t == NULL) { return; }

    /* Find tag, attrs, children */
    const char *tag = NULL;
    NovaTable *attrs = NULL;
    NovaTable *children_tbl = NULL;

    uint32_t iter_hn = nova_table_array_len(t);
    NovaValue hk_n, hv_n;
    while (nova_table_next(t, &iter_hn, &hk_n, &hv_n)) {
        if (!nova_is_string(hk_n)) {
            continue;
        }
        const char *k = nova_str_data(nova_as_string(hk_n));
        NovaValue v = hv_n;
        if (strcmp(k, "tag") == 0 && nova_is_string(v)) {
            tag = nova_str_data(nova_as_string(v));
        } else if (strcmp(k, "attrs") == 0 && nova_is_table(v)) {
            attrs = nova_as_table(v);
        } else if (strcmp(k, "children") == 0 && nova_is_table(v)) {
            children_tbl = nova_as_table(v);
        }
    }

    if (tag == NULL) { return; }

    /* Don't output the synthetic "document" wrapper tag */
    int is_doc = (strcmp(tag, "document") == 0);

    if (!is_doc) {
        ndp_html_encode_indent(out, opts, indent_level);
        ndp_buf_putc(out, '<');
        ndp_buf_puts(out, tag);

        /* Attributes */
        if (attrs != NULL) {
            uint32_t iter_at = nova_table_array_len(attrs);
            NovaValue ak_it, av_it;
            while (nova_table_next(attrs, &iter_at, &ak_it, &av_it)) {
                if (!nova_is_string(ak_it)) {
                    continue;
                }
                ndp_buf_putc(out, ' ');
                ndp_buf_puts(out, nova_str_data(nova_as_string(ak_it)));
                NovaValue av = av_it;
                if (nova_is_string(av)) {
                    ndp_buf_puts(out, "=\"");
                    /* Escape attribute value */
                    size_t ai2 = 0;
                    for (ai2 = 0; ai2 < nova_str_len(nova_as_string(av)); ai2++) {
                        char ac = nova_str_data(nova_as_string(av))[ai2];
                        if (ac == '"') { ndp_buf_puts(out, "&quot;"); }
                        else if (ac == '&') { ndp_buf_puts(out, "&amp;"); }
                        else { ndp_buf_putc(out, ac); }
                    }
                    ndp_buf_putc(out, '"');
                } else if (nova_is_bool(av) && nova_as_bool(av)) {
                    /* Boolean attribute: just the name */
                }
            }
        }

        if (ndp_html_is_void(tag)) {
            ndp_buf_puts(out, " />");
            if (opts->pretty) { ndp_buf_putc(out, '\n'); }
            return;
        }

        ndp_buf_putc(out, '>');
        if (opts->pretty) { ndp_buf_putc(out, '\n'); }
    }

    /* Children */
    if (children_tbl != NULL) {
        uint32_t ci = 0;
        int child_indent = is_doc ? indent_level : indent_level + 1;
        for (ci = 0; ci < nova_table_array_len(children_tbl); ci++) {
            ndp_html_encode_node(vm, nova_table_get_int(children_tbl, ci), out,
                                 child_indent, opts);
        }
    }

    if (!is_doc) {
        ndp_html_encode_indent(out, opts, indent_level);
        ndp_buf_puts(out, "</");
        ndp_buf_puts(out, tag);
        ndp_buf_putc(out, '>');
        if (opts->pretty) { ndp_buf_putc(out, '\n'); }
    }

    (void)vm;
}

static int ndp_html_encode(NovaVM *vm, int idx, const NdpOptions *opts,
                            NdpBuf *out, char *errbuf) {
    NovaValue val = nova_vm_get(vm, idx);
    if (nova_is_string(val)) {
        /* Plain text -> HTML-escape it */
        const char *s = nova_str_data(nova_as_string(val));
        size_t slen = nova_str_len(nova_as_string(val));
        size_t i = 0;
        for (i = 0; i < slen; i++) {
            switch (s[i]) {
                case '<': ndp_buf_puts(out, "&lt;"); break;
                case '>': ndp_buf_puts(out, "&gt;"); break;
                case '&': ndp_buf_puts(out, "&amp;"); break;
                case '"': ndp_buf_puts(out, "&quot;"); break;
                default:  ndp_buf_putc(out, s[i]); break;
            }
        }
        return 0;
    }
    if (!nova_is_table(val)) {
        if (errbuf != NULL) {
            (void)snprintf(errbuf, 256, "HTML encode: expected table or string");
        }
        return -1;
    }

    ndp_html_encode_node(vm, val, out, 0, opts);
    return 0;
}

/* ============================================================
 * PART 9A: NINI CODEC (delegated to nova_nini.c)
 *
 * The full NINI parser/encoder lives in its own module:
 *   include/nova/nova_nini.h  -- public API
 *   src/nova_nini.c           -- implementation
 *
 * NDP provides thin wrappers that bridge NdpOptions <-> NiniOptions
 * and delegate to the standalone codec.
 * ============================================================ */

#include "nova/nova_nini.h"

/* ---- NINI NDP decode wrapper ---- */
static int ndp_nini_decode(NovaVM *vm, const char *text, size_t len,
                            const NdpOptions *opts, char *errbuf) {
    NiniOptions nini_opts;
    nova_nini_options_init(&nini_opts);
    nini_opts.interpolate = opts->nini_interpolate;
    nini_opts.tasks_only  = opts->nini_tasks_only;
    return nova_nini_decode(vm, text, len, &nini_opts, errbuf, 256);
}

/* ---- NINI NDP encode wrapper ---- */
static int ndp_nini_encode(NovaVM *vm, int idx, const NdpOptions *opts,
                            NdpBuf *out, char *errbuf) {
    NiniOptions nini_opts;
    nova_nini_options_init(&nini_opts);
    nini_opts.interpolate = opts->nini_interpolate;
    nini_opts.tasks_only  = opts->nini_tasks_only;
    char *result = NULL;
    size_t result_len = 0;
    int rc = nova_nini_encode(vm, idx, &nini_opts,
                              &result, &result_len, errbuf, 256);
    if (rc == 0 && result != NULL) {
        ndp_buf_append(out, result, result_len);
        free(result);
    }
    return rc;
}

/* ============================================================
 * PART 10: PUBLIC API DISPATCH
 * ============================================================ */

int ndp_decode(NovaVM *vm, const char *text, size_t len,
               const NdpOptions *opts, char *errbuf) {
    if (vm == NULL || text == NULL || opts == NULL) {
        if (errbuf != NULL) {
            (void)snprintf(errbuf, 256, "ndp_decode: NULL argument");
        }
        return -1;
    }

    switch (opts->format) {
        case NDP_FORMAT_JSON:
            return ndp_json_decode(vm, text, len, opts, errbuf);
        case NDP_FORMAT_CSV:
        case NDP_FORMAT_TSV:
            return ndp_csv_decode(vm, text, len, opts, errbuf);
        case NDP_FORMAT_INI:
            return ndp_ini_decode(vm, text, len, opts, errbuf);
        case NDP_FORMAT_TOML:
            return ndp_toml_decode(vm, text, len, opts, errbuf);
        case NDP_FORMAT_HTML:
            return ndp_html_decode(vm, text, len, opts, errbuf);
        case NDP_FORMAT_YAML:
            return ndp_yaml_decode(vm, text, len, opts, errbuf);
        case NDP_FORMAT_NINI:
            return ndp_nini_decode(vm, text, len, opts, errbuf);
        default:
            if (errbuf != NULL) {
                (void)snprintf(errbuf, 256, "unknown format: %d",
                               (int)opts->format);
            }
            return -1;
    }
}

int ndp_encode(NovaVM *vm, int idx, const NdpOptions *opts,
               NdpBuf *out, char *errbuf) {
    if (vm == NULL || opts == NULL || out == NULL) {
        if (errbuf != NULL) {
            (void)snprintf(errbuf, 256, "ndp_encode: NULL argument");
        }
        return -1;
    }

    switch (opts->format) {
        case NDP_FORMAT_JSON:
            return ndp_json_encode(vm, idx, opts, out, errbuf);
        case NDP_FORMAT_CSV:
        case NDP_FORMAT_TSV:
            return ndp_csv_encode(vm, idx, opts, out, errbuf);
        case NDP_FORMAT_INI:
            return ndp_ini_encode(vm, idx, opts, out, errbuf);
        case NDP_FORMAT_TOML:
            return ndp_toml_encode(vm, idx, opts, out, errbuf);
        case NDP_FORMAT_HTML:
            return ndp_html_encode(vm, idx, opts, out, errbuf);
        case NDP_FORMAT_YAML:
            return ndp_yaml_encode(vm, idx, opts, out, errbuf);
        case NDP_FORMAT_NINI:
            return ndp_nini_encode(vm, idx, opts, out, errbuf);
        default:
            if (errbuf != NULL) {
                (void)snprintf(errbuf, 256, "unknown format: %d",
                               (int)opts->format);
            }
            return -1;
    }
}

