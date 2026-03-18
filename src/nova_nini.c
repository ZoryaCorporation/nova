/**
 * @file nova_nini.c
 * @brief NINI — Nova's native INI format (lingua franca)
 *
 * Standalone NINI parser and encoder. Does NOT depend on NDP.
 * NDP integrates by including nova_nini.h and calling these functions.
 *
 * Design principles:
 *   - Direct table API (nova_table_set_str, nova_table_get_cstr)
 *   - Minimal stack usage (only root table on VM stack)
 *   - GC-safe: all tables reachable from stack-rooted root before
 *     any allocating call; strings are immortal (interned in weave)
 *
 * @author Anthony Taliento
 * @date 2026-02-18
 * @version 1.0.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#include "nova/nova_nini.h"
#include "nova/nova_vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strncasecmp */
#include <errno.h>
#include <stdarg.h>

/* ============================================================
 * INTERNAL STRING BUFFER
 * ============================================================ */

typedef struct {
    char   *data;
    size_t  len;
    size_t  cap;
} NiniBuf;

static void nini_buf_init(NiniBuf *b) {
    if (b == NULL) { return; }
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

static void nini_buf_free(NiniBuf *b) {
    if (b == NULL) { return; }
    free(b->data);
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

static int nini_buf_grow(NiniBuf *b, size_t need) {
    if (b == NULL) { return -1; }
    size_t required = b->len + need + 1;
    if (required <= b->cap) { return 0; }
    size_t new_cap = (b->cap == 0) ? 256 : b->cap;
    while (new_cap < required) { new_cap *= 2; }
    char *tmp = (char *)realloc(b->data, new_cap);
    if (tmp == NULL) { return -1; }
    b->data = tmp;
    b->cap  = new_cap;
    return 0;
}

static void nini_buf_append(NiniBuf *b, const char *s, size_t n) {
    if (b == NULL || n == 0) { return; }
    if (nini_buf_grow(b, n) != 0) { return; }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void nini_buf_putc(NiniBuf *b, char c) {
    nini_buf_append(b, &c, 1);
}

static void nini_buf_puts(NiniBuf *b, const char *s) {
    if (s == NULL) { return; }
    nini_buf_append(b, s, strlen(s));
}

static void nini_buf_printf(NiniBuf *b, const char *fmt, ...) {
    if (b == NULL || fmt == NULL) { return; }
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) {
        nini_buf_append(b, tmp, (size_t)n);
    }
}

/* ============================================================
 * VALUE CONSTRUCTION HELPERS
 *
 * These produce NovaValue without touching the VM stack.
 * ============================================================ */

/**
 * @brief Parse a string literal into a typed NovaValue.
 *
 * Recognises: true/false/yes/no/on/off, nil/null/none,
 * integers, floats, quoted strings, bare strings.
 */
static NovaValue nini_make_typed(NovaVM *vm, const char *s, size_t len) {
    if (len == 0) {
        return nova_value_string(nova_string_new(vm, "", 0));
    }

    /* Boolean true */
    if ((len == 4 && strncasecmp(s, "true", 4) == 0) ||
        (len == 3 && strncasecmp(s, "yes", 3) == 0) ||
        (len == 2 && strncasecmp(s, "on", 2) == 0)) {
        return nova_value_bool(1);
    }
    /* Boolean false */
    if ((len == 5 && strncasecmp(s, "false", 5) == 0) ||
        (len == 2 && strncasecmp(s, "no", 2) == 0) ||
        (len == 3 && strncasecmp(s, "off", 3) == 0)) {
        return nova_value_bool(0);
    }

    /* Nil */
    if ((len == 3 && strncasecmp(s, "nil", 3) == 0) ||
        (len == 4 && strncasecmp(s, "none", 4) == 0) ||
        (len == 4 && strncasecmp(s, "null", 4) == 0)) {
        return nova_value_nil();
    }

    /* Try integer */
    {
        char *end = NULL;
        errno = 0;
        long long ll = strtoll(s, &end, 10);
        if ((size_t)(end - s) == len && errno == 0) {
            return nova_value_integer((nova_int_t)ll);
        }
    }

    /* Try float */
    {
        char *end = NULL;
        errno = 0;
        double d = strtod(s, &end);
        if ((size_t)(end - s) == len && errno == 0) {
            return nova_value_number(d);
        }
    }

    /* Strip quotes */
    if (len >= 2) {
        if ((s[0] == '"' && s[len - 1] == '"') ||
            (s[0] == '\'' && s[len - 1] == '\'')) {
            return nova_value_string(
                nova_string_new(vm, s + 1, len - 2));
        }
    }

    /* Default: bare string */
    return nova_value_string(nova_string_new(vm, s, len));
}

/* ============================================================
 * TABLE HELPERS
 *
 * GC safety: all helpers assume the root table is on the VM stack.
 * New tables are attached to a reachable parent BEFORE any further
 * allocating call.
 * ============================================================ */

/**
 * @brief Get or create a named sub-table inside a parent table.
 *
 * If parent[name] is already a table, returns it.
 * Otherwise creates a new table, sets parent[name] to it, and returns it.
 *
 * GC safe: new table is reachable from parent before returning.
 */
static NovaTable *nini_ensure_subtable(NovaVM *vm, NovaTable *parent,
                                        const char *name, size_t nlen) {
    if (parent == NULL || name == NULL) { return NULL; }

    NovaValue existing = nova_table_get_cstr(parent, name, (uint32_t)nlen);
    if (nova_is_table(existing) && nova_as_table(existing) != NULL) {
        return nova_as_table(existing);
    }

    /* Create new table and attach immediately.
     * Push on stack temporarily for GC safety during nova_string_new. */
    nova_vm_push_table(vm);
    NovaValue tv = nova_vm_get(vm, nova_vm_get_top(vm) - 1);
    NovaTable *new_t = nova_as_table(tv);

    NovaString *key = nova_string_new(vm, name, nlen);
    nova_table_set_str(vm, parent, key, tv);

    nova_vm_pop(vm, 1);
    return new_t;
}

/**
 * @brief Set a named field in a table to a value.
 *
 * GC safe: the value must already be reachable or an immediate.
 */
static void nini_table_set(NovaVM *vm, NovaTable *t,
                            const char *name, size_t nlen,
                            NovaValue val) {
    if (t == NULL || name == NULL) { return; }
    NovaString *key = nova_string_new(vm, name, nlen);
    if (key == NULL) { return; }
    nova_table_set_str(vm, t, key, val);
}

/**
 * @brief Navigate a dot-separated path, creating tables as needed.
 *
 * "a.b.c" -> root["a"]["b"]["c"], returns pointer to the "c" table.
 */
static NovaTable *nini_ensure_path(NovaVM *vm, NovaTable *root,
                                    const char *path, size_t path_len) {
    NovaTable *current = root;
    size_t pos = 0;

    while (pos < path_len && current != NULL) {
        size_t seg_start = pos;
        while (pos < path_len && path[pos] != '.') { pos++; }
        size_t seg_len = pos - seg_start;
        if (pos < path_len) { pos++; }

        char seg_name[256];
        if (seg_len >= sizeof(seg_name)) { seg_len = sizeof(seg_name) - 1; }
        memcpy(seg_name, path + seg_start, seg_len);
        seg_name[seg_len] = '\0';

        current = nini_ensure_subtable(vm, current, seg_name, seg_len);
    }

    return current;
}

/**
 * @brief Parse an inline array [a, b, c] into a new table.
 *
 * Returns a NovaValue wrapping the array table.
 * The table is pushed/popped from the stack for GC safety.
 */
static NovaValue nini_parse_inline_array(NovaVM *vm,
                                          const char *s, size_t len) {
    nova_vm_push_table(vm);
    NovaValue tv = nova_vm_get(vm, nova_vm_get_top(vm) - 1);
    NovaTable *arr = nova_as_table(tv);
    int elem_count = 0;

    size_t pos = 0;
    if (pos < len && s[pos] == '[') { pos++; }

    while (pos < len) {
        /* Skip whitespace */
        while (pos < len && (s[pos] == ' ' || s[pos] == '\t')) { pos++; }
        if (pos >= len || s[pos] == ']') { break; }

        /* Find end of element */
        size_t elem_start = pos;
        int in_quote = 0;
        while (pos < len && s[pos] != ']') {
            if (s[pos] == '"' || s[pos] == '\'') {
                in_quote = !in_quote;
            }
            if (s[pos] == ',' && !in_quote) { break; }
            pos++;
        }
        size_t elem_end = pos;
        if (pos < len && s[pos] == ',') { pos++; }

        /* Trim */
        while (elem_start < elem_end &&
               (s[elem_start] == ' ' || s[elem_start] == '\t')) {
            elem_start++;
        }
        while (elem_end > elem_start &&
               (s[elem_end - 1] == ' ' || s[elem_end - 1] == '\t')) {
            elem_end--;
        }

        if (elem_end > elem_start) {
            NovaValue ev = nini_make_typed(vm, s + elem_start,
                                            elem_end - elem_start);
            nova_table_set_int(vm, arr, (nova_int_t)elem_count, ev);
            elem_count++;
        }
    }

    nova_vm_pop(vm, 1);   /* pop temp; table reachable via caller */
    return tv;
}

/* ============================================================
 * INTERPOLATION
 *
 * Resolve ${section.key} references in a string value.
 * Navigates the root table using dot-separated path.
 * ============================================================ */

static NovaValue nini_interpolate_value(NovaVM *vm, NovaTable *root,
                                         const char *s, size_t len) {
    NiniBuf buf;
    nini_buf_init(&buf);
    size_t pos = 0;

    while (pos < len) {
        if (pos + 1 < len && s[pos] == '$' && s[pos + 1] == '{') {
            size_t ref_start = pos + 2;
            size_t ref_end = ref_start;
            while (ref_end < len && s[ref_end] != '}') { ref_end++; }

            if (ref_end < len) {
                /* Extract reference path and navigate */
                char ref_path[512];
                size_t ref_len = ref_end - ref_start;
                if (ref_len >= sizeof(ref_path)) {
                    ref_len = sizeof(ref_path) - 1;
                }
                memcpy(ref_path, s + ref_start, ref_len);
                ref_path[ref_len] = '\0';

                /* Walk path: split on dots */
                NovaTable *current = root;
                int resolved = 1;
                char *part = ref_path;
                char *dot = NULL;

                while (part != NULL && *part != '\0') {
                    dot = strchr(part, '.');
                    if (dot != NULL) { *dot = '\0'; }

                    size_t plen = strlen(part);
                    NovaValue v = nova_table_get_cstr(
                        current, part, (uint32_t)plen);

                    if (dot != NULL && nova_is_table(v) &&
                        nova_as_table(v) != NULL) {
                        current = nova_as_table(v);
                        part = dot + 1;
                    } else if (dot == NULL) {
                        /* Terminal value — stringify */
                        if (nova_is_string(v)) {
                            nini_buf_append(&buf,
                                nova_str_data(nova_as_string(v)),
                                nova_str_len(nova_as_string(v)));
                        } else if (nova_is_integer(v)) {
                            nini_buf_printf(&buf, "%lld",
                                (long long)nova_as_integer(v));
                        } else if (nova_is_number(v)) {
                            nini_buf_printf(&buf, "%.17g",
                                nova_as_number(v));
                        } else if (nova_is_bool(v)) {
                            nini_buf_puts(&buf,
                                nova_as_bool(v) ? "true" : "false");
                        } else {
                            resolved = 0;
                        }
                        part = NULL;
                    } else {
                        resolved = 0;
                        break;
                    }
                }

                if (!resolved) {
                    /* Keep literal ${...} */
                    nini_buf_append(&buf, s + pos, ref_end - pos + 1);
                }
                pos = ref_end + 1;
            } else {
                nini_buf_putc(&buf, s[pos]);
                pos++;
            }
        } else {
            nini_buf_putc(&buf, s[pos]);
            pos++;
        }
    }

    NovaValue result = nova_value_string(
        nova_string_new(vm, buf.data ? buf.data : "", buf.len));
    nini_buf_free(&buf);
    return result;
}

/* ============================================================
 * PUBLIC API
 * ============================================================ */

void nova_nini_options_init(NiniOptions *opts) {
    if (opts == NULL) { return; }
    opts->interpolate = 1;
    opts->tasks_only  = 0;
}

/* ---- DECODER ---- */

int nova_nini_decode(struct NovaVM *vm, const char *text, size_t len,
                     const NiniOptions *opts,
                     char *errbuf, size_t errbuf_len) {
    if (vm == NULL || text == NULL || opts == NULL) {
        if (errbuf != NULL && errbuf_len > 0) {
            (void)snprintf(errbuf, errbuf_len,
                           "nova_nini_decode: NULL argument");
        }
        return -1;
    }

    /* Create root table, push onto stack (GC root) */
    nova_vm_push_table(vm);
    int root_stack = nova_vm_get_top(vm) - 1;
    NovaTable *root = nova_as_table(nova_vm_get(vm, root_stack));
    if (root == NULL) {
        if (errbuf != NULL && errbuf_len > 0) {
            (void)snprintf(errbuf, errbuf_len,
                           "nova_nini_decode: failed to create root table");
        }
        return -1;
    }

    NovaTable *section = root;      /* current section pointer */
    NovaTable *tasks   = NULL;      /* __tasks sub-table (lazy) */

    size_t pos = 0;
    int line_num = 1;

    while (pos < len) {
        /* Skip whitespace (not newline) */
        while (pos < len && (text[pos] == ' ' || text[pos] == '\t')) {
            pos++;
        }
        if (pos >= len) { break; }

        char c = text[pos];

        /* Empty line / newline */
        if (c == '\n') {
            pos++;
            line_num++;
            continue;
        }
        if (c == '\r') {
            pos++;
            if (pos < len && text[pos] == '\n') { pos++; }
            line_num++;
            continue;
        }

        /* Comments: # or ; */
        if (c == '#' || c == ';') {
            while (pos < len && text[pos] != '\n') { pos++; }
            continue;
        }

        /* @include directive (skip — preprocessor concern) */
        if (c == '@') {
            while (pos < len && text[pos] != '\n') { pos++; }
            continue;
        }

        /* ---- Section header: [name] or [task:name] ---- */
        if (c == '[') {
            pos++;   /* consume '[' */
            size_t name_start = pos;
            while (pos < len && text[pos] != ']' && text[pos] != '\n') {
                pos++;
            }
            size_t name_end = pos;
            if (pos < len && text[pos] == ']') {
                pos++;   /* consume ']' */
            } else {
                if (errbuf != NULL && errbuf_len > 0) {
                    (void)snprintf(errbuf, errbuf_len,
                        "line %d: unterminated section header", line_num);
                }
                return -1;
            }

            /* Trim section name */
            while (name_start < name_end &&
                   (text[name_start] == ' ' ||
                    text[name_start] == '\t')) {
                name_start++;
            }
            while (name_end > name_start &&
                   (text[name_end - 1] == ' ' ||
                    text[name_end - 1] == '\t')) {
                name_end--;
            }

            size_t nlen = name_end - name_start;
            char sec_name[256];
            if (nlen >= sizeof(sec_name)) { nlen = sizeof(sec_name) - 1; }
            memcpy(sec_name, text + name_start, nlen);
            sec_name[nlen] = '\0';

            /* Skip rest of line */
            while (pos < len && text[pos] != '\n') { pos++; }

            /* Task section: [task:name] */
            if (nlen > 5 && strncmp(sec_name, "task:", 5) == 0) {
                if (tasks == NULL) {
                    tasks = nini_ensure_subtable(vm, root,
                                                  "__tasks", 7);
                }
                if (tasks != NULL) {
                    const char *tname = sec_name + 5;
                    size_t tlen = nlen - 5;
                    section = nini_ensure_subtable(vm, tasks,
                                                    tname, tlen);
                }
            }
            /* Nested section: a.b.c */
            else if (memchr(sec_name, '.', nlen) != NULL) {
                section = nini_ensure_path(vm, root, sec_name, nlen);
            }
            /* Simple section */
            else {
                if (!opts->tasks_only) {
                    section = nini_ensure_subtable(vm, root,
                                                    sec_name, nlen);
                }
            }

            continue;
        }

        /* ---- Key = Value line ---- */
        size_t key_start = pos;
        while (pos < len && text[pos] != '=' && text[pos] != '\n') {
            pos++;
        }
        size_t key_end = pos;

        if (pos >= len || text[pos] == '\n') {
            /* Line without '='; skip */
            continue;
        }
        pos++;   /* consume '=' */

        /* Skip whitespace around value */
        while (pos < len && (text[pos] == ' ' || text[pos] == '\t')) {
            pos++;
        }

        /* Trim key */
        while (key_start < key_end &&
               (text[key_start] == ' ' || text[key_start] == '\t')) {
            key_start++;
        }
        while (key_end > key_start &&
               (text[key_end - 1] == ' ' || text[key_end - 1] == '\t')) {
            key_end--;
        }

        char key_name[256];
        size_t klen = key_end - key_start;
        if (klen >= sizeof(key_name)) { klen = sizeof(key_name) - 1; }
        memcpy(key_name, text + key_start, klen);
        key_name[klen] = '\0';

        /* Check for array-push key: key[] */
        int is_array_push = 0;
        if (klen >= 2 && key_name[klen - 2] == '[' &&
            key_name[klen - 1] == ']') {
            key_name[klen - 2] = '\0';
            klen -= 2;
            is_array_push = 1;
        }

        /* Check for env.KEY pattern */
        int is_env_key = 0;
        char env_subkey[256];
        env_subkey[0] = '\0';
        if (klen > 4 && strncmp(key_name, "env.", 4) == 0) {
            is_env_key = 1;
            size_t elen = klen - 4;
            if (elen >= sizeof(env_subkey)) {
                elen = sizeof(env_subkey) - 1;
            }
            memcpy(env_subkey, key_name + 4, elen);
            env_subkey[elen] = '\0';
        }

        /* ---- Parse value ---- */
        NovaValue val;

        /* Multi-line string: """ */
        if (pos + 2 < len &&
            text[pos] == '"' && text[pos + 1] == '"' &&
            text[pos + 2] == '"') {
            pos += 3;

            /* Skip to next line */
            while (pos < len && text[pos] != '\n') { pos++; }
            if (pos < len) { pos++; line_num++; }

            NiniBuf mlbuf;
            nini_buf_init(&mlbuf);
            size_t ml_start = pos;

            while (pos < len) {
                size_t line_start = pos;
                /* Skip leading whitespace */
                while (pos < len &&
                       (text[pos] == ' ' || text[pos] == '\t')) {
                    pos++;
                }
                /* Check for closing """ */
                if (pos + 2 < len &&
                    text[pos] == '"' && text[pos + 1] == '"' &&
                    text[pos + 2] == '"') {
                    /* Capture content up to this line */
                    size_t content_end = line_start;
                    if (content_end > ml_start &&
                        text[content_end - 1] == '\n') {
                        content_end--;
                    }
                    if (content_end > ml_start &&
                        text[content_end - 1] == '\r') {
                        content_end--;
                    }
                    if (content_end > ml_start) {
                        nini_buf_append(&mlbuf, text + ml_start,
                                        content_end - ml_start);
                    }
                    pos += 3;
                    break;
                }
                /* Regular line — advance to end */
                while (pos < len && text[pos] != '\n') { pos++; }
                if (pos < len) { pos++; line_num++; }
            }

            val = nova_value_string(
                nova_string_new(vm, mlbuf.data ? mlbuf.data : "",
                                mlbuf.len));
            nini_buf_free(&mlbuf);

        } else {
            /* Single-line value */
            size_t val_start = pos;
            while (pos < len && text[pos] != '\n' && text[pos] != '\r') {
                pos++;
            }
            size_t val_end = pos;

            /* Strip inline comments (# or ; preceded by space) */
            {
                int in_quote = 0;
                size_t cp = val_start;
                while (cp < val_end) {
                    char ch = text[cp];
                    if (ch == '"' || ch == '\'') { in_quote = !in_quote; }
                    if (!in_quote && (ch == '#' || ch == ';') &&
                        cp > val_start && text[cp - 1] == ' ') {
                        val_end = cp;
                        break;
                    }
                    cp++;
                }
            }

            /* Trim trailing whitespace */
            while (val_end > val_start &&
                   (text[val_end - 1] == ' ' ||
                    text[val_end - 1] == '\t')) {
                val_end--;
            }

            size_t vlen = val_end - val_start;

            /* Inline array: [a, b, c] */
            if (vlen >= 2 && text[val_start] == '[' &&
                text[val_end - 1] == ']') {
                val = nini_parse_inline_array(vm, text + val_start, vlen);
            } else {
                val = nini_make_typed(vm, text + val_start, vlen);
            }
        }

        /* ---- Store value into table ---- */
        if (section == NULL) { continue; }

        if (is_array_push) {
            /* key[] = value -> append to array table */
            NovaTable *arr = nini_ensure_subtable(
                vm, section, key_name, klen);
            if (arr != NULL) {
                nova_int_t idx =
                    (nova_int_t)nova_table_array_len(arr);
                nova_table_set_int(vm, arr, idx, val);
            }
        } else if (is_env_key) {
            /* env.KEY = value -> section["env"]["KEY"] = value */
            NovaTable *env = nini_ensure_subtable(
                vm, section, "env", 3);
            if (env != NULL) {
                size_t elen = strlen(env_subkey);
                nini_table_set(vm, env, env_subkey, elen, val);
            }
        } else {
            /* Normal key = value */
            nini_table_set(vm, section, key_name, klen, val);
        }
    }

    /* ---- Interpolation pass ---- */
    if (opts->interpolate) {
        /* Recursive helper: interpolate all string values in a table,
         * descending into sub-tables up to max_depth levels.           */
        #define NINI_INTERP_MAX_DEPTH 8
        struct { NovaTable *tbl; uint32_t iter; } stack[NINI_INTERP_MAX_DEPTH];
        int sp = 0;

        stack[0].tbl  = root;
        stack[0].iter = 0;
        sp = 1;

        while (sp > 0) {
            int cur = sp - 1;
            NovaValue hk, hv;
            if (!nova_table_next(stack[cur].tbl,
                                 &stack[cur].iter, &hk, &hv)) {
                sp--;
                continue;
            }

            if (nova_is_table(hv) && nova_as_table(hv) != NULL
                && sp < NINI_INTERP_MAX_DEPTH) {
                /* Descend into sub-table */
                stack[sp].tbl  = nova_as_table(hv);
                stack[sp].iter = 0;
                sp++;
            } else if (nova_is_string(hv) && nova_is_string(hk)) {
                const char *sdata = nova_str_data(nova_as_string(hv));
                size_t slen = nova_str_len(nova_as_string(hv));
                if (memchr(sdata, '$', slen) != NULL) {
                    NovaValue nv = nini_interpolate_value(
                        vm, root, sdata, slen);
                    nova_table_set_str(vm, stack[cur].tbl,
                                       nova_as_string(hk), nv);
                }
            }
        }
        #undef NINI_INTERP_MAX_DEPTH
    }

    /* Root table is already on the stack at root_stack — done */
    return 0;
}

/* ---- ENCODER ---- */

static void nini_encode_value(NovaVM *vm, NovaValue val, NiniBuf *out) {
    (void)vm;
    switch (nova_typeof(val)) {
        case NOVA_TYPE_STRING: {
            const char *s = nova_str_data(nova_as_string(val));
            size_t slen = nova_str_len(nova_as_string(val));
            int needs_quote = 0;

            if (slen > 0) {
                /* Check if it resembles a typed keyword */
                if (strcasecmp(s, "true") == 0 ||
                    strcasecmp(s, "false") == 0 ||
                    strcasecmp(s, "yes") == 0 ||
                    strcasecmp(s, "no") == 0 ||
                    strcasecmp(s, "on") == 0 ||
                    strcasecmp(s, "off") == 0 ||
                    strcasecmp(s, "nil") == 0 ||
                    strcasecmp(s, "null") == 0 ||
                    strcasecmp(s, "none") == 0) {
                    needs_quote = 1;
                }
                /* Check if it looks like a number */
                char *end = NULL;
                errno = 0;
                (void)strtod(s, &end);
                if ((size_t)(end - s) == slen && errno == 0) {
                    needs_quote = 1;
                }
                /* Multi-line: use triple quotes */
                if (memchr(s, '\n', slen) != NULL) {
                    nini_buf_puts(out, "\"\"\"\n");
                    nini_buf_append(out, s, slen);
                    nini_buf_puts(out, "\n\"\"\"");
                    return;
                }
            }
            if (needs_quote) {
                nini_buf_putc(out, '"');
                nini_buf_append(out, s, slen);
                nini_buf_putc(out, '"');
            } else {
                nini_buf_append(out, s, slen);
            }
            break;
        }
        case NOVA_TYPE_INTEGER:
            nini_buf_printf(out, "%lld",
                (long long)nova_as_integer(val));
            break;
        case NOVA_TYPE_NUMBER:
            nini_buf_printf(out, "%.17g", nova_as_number(val));
            break;
        case NOVA_TYPE_BOOL:
            nini_buf_puts(out, nova_as_bool(val) ? "true" : "false");
            break;
        default:
            nini_buf_puts(out, "nil");
            break;
    }
}

/**
 * @brief Check whether a table looks like an array (has int keys 0..n-1).
 */
static int nini_is_array_table(NovaTable *t) {
    if (t == NULL) { return 0; }
    return nova_table_array_len(t) > 0;
}

int nova_nini_encode(struct NovaVM *vm, int idx,
                     const NiniOptions *opts,
                     char **out, size_t *out_len,
                     char *errbuf, size_t errbuf_len) {
    (void)opts;

    if (vm == NULL || out == NULL || out_len == NULL) {
        if (errbuf != NULL && errbuf_len > 0) {
            (void)snprintf(errbuf, errbuf_len,
                           "nova_nini_encode: NULL argument");
        }
        return -1;
    }

    NovaValue val = nova_vm_get(vm, idx);
    if (!nova_is_table(val)) {
        if (errbuf != NULL && errbuf_len > 0) {
            (void)snprintf(errbuf, errbuf_len,
                "nini encode: expected table, got %s",
                nova_vm_typename(nova_typeof(val)));
        }
        return -1;
    }

    NovaTable *t = nova_as_table(val);
    if (t == NULL) {
        *out = NULL;
        *out_len = 0;
        return 0;
    }

    NiniBuf buf;
    nini_buf_init(&buf);

    nini_buf_puts(&buf, "# Generated by Nova NINI\n");

    /* First pass: bare key=value (non-table values at root) */
    {
        uint32_t iter = 0;
        NovaValue hk, hv;
        while (nova_table_next(t, &iter, &hk, &hv)) {
            if (!nova_is_string(hk)) { continue; }
            if (nova_is_table(hv)) { continue; }
            const char *kname = nova_str_data(nova_as_string(hk));
            if (strcmp(kname, "__tasks") == 0) { continue; }

            nini_buf_puts(&buf, kname);
            nini_buf_puts(&buf, " = ");
            nini_encode_value(vm, hv, &buf);
            nini_buf_putc(&buf, '\n');
        }
    }

    /* Second pass: sections (non-task tables) */
    {
        uint32_t iter = 0;
        NovaValue hk, hv;
        while (nova_table_next(t, &iter, &hk, &hv)) {
            if (!nova_is_string(hk) || !nova_is_table(hv)) { continue; }
            const char *kname = nova_str_data(nova_as_string(hk));
            if (strcmp(kname, "__tasks") == 0) { continue; }

            nini_buf_putc(&buf, '\n');
            nini_buf_printf(&buf, "[%s]\n", kname);

            NovaTable *sec = nova_as_table(hv);
            if (sec == NULL) { continue; }

            uint32_t si = 0;
            NovaValue sk, sv;
            while (nova_table_next(sec, &si, &sk, &sv)) {
                if (!nova_is_string(sk)) { continue; }
                const char *sname = nova_str_data(nova_as_string(sk));

                if (nova_is_table(sv)) {
                    NovaTable *sub = nova_as_table(sv);
                    if (sub == NULL) { continue; }

                    if (strcmp(sname, "env") == 0) {
                        /* env sub-table: env.KEY = VALUE */
                        uint32_t ei = 0;
                        NovaValue ek, ev;
                        while (nova_table_next(sub, &ei, &ek, &ev)) {
                            if (!nova_is_string(ek)) { continue; }
                            nini_buf_printf(&buf, "env.%s = ",
                                nova_str_data(nova_as_string(ek)));
                            nini_encode_value(vm, ev, &buf);
                            nini_buf_putc(&buf, '\n');
                        }
                    } else if (nini_is_array_table(sub)) {
                        /* Array: repeated key[] syntax */
                        uint32_t alen = nova_table_array_len(sub);
                        for (uint32_t ai = 0; ai < alen; ai++) {
                            NovaValue elem = sub->array[ai];
                            nini_buf_printf(&buf, "%s[] = ", sname);
                            nini_encode_value(vm, elem, &buf);
                            nini_buf_putc(&buf, '\n');
                        }
                    }
                } else {
                    nini_buf_printf(&buf, "%s = ", sname);
                    nini_encode_value(vm, sv, &buf);
                    nini_buf_putc(&buf, '\n');
                }
            }
        }
    }

    /* Third pass: task sections */
    {
        NovaValue tasks_v = nova_table_get_cstr(t, "__tasks", 7);
        if (nova_is_table(tasks_v) && nova_as_table(tasks_v) != NULL) {
            NovaTable *tasks_t = nova_as_table(tasks_v);
            uint32_t ti = 0;
            NovaValue tk, tv;
            while (nova_table_next(tasks_t, &ti, &tk, &tv)) {
                if (!nova_is_string(tk) || !nova_is_table(tv)) {
                    continue;
                }

                nini_buf_printf(&buf, "\n[task:%s]\n",
                    nova_str_data(nova_as_string(tk)));

                NovaTable *task = nova_as_table(tv);
                if (task == NULL) { continue; }

                uint32_t tai = 0;
                NovaValue tak, tav;
                while (nova_table_next(task, &tai, &tak, &tav)) {
                    if (!nova_is_string(tak)) { continue; }
                    const char *tname =
                        nova_str_data(nova_as_string(tak));

                    if (nova_is_table(tav) &&
                        strcmp(tname, "env") == 0) {
                        NovaTable *env = nova_as_table(tav);
                        if (env == NULL) { continue; }
                        uint32_t ei = 0;
                        NovaValue ek, ev;
                        while (nova_table_next(env, &ei, &ek, &ev)) {
                            if (!nova_is_string(ek)) { continue; }
                            nini_buf_printf(&buf, "env.%s = ",
                                nova_str_data(nova_as_string(ek)));
                            nini_encode_value(vm, ev, &buf);
                            nini_buf_putc(&buf, '\n');
                        }
                    } else if (nova_is_table(tav) &&
                               strcmp(tname, "depends") == 0) {
                        NovaTable *deps = nova_as_table(tav);
                        uint32_t dlen = nova_table_array_len(deps);
                        if (dlen > 0) {
                            nini_buf_puts(&buf, "depends = [");
                            for (uint32_t di = 0; di < dlen; di++) {
                                if (di > 0) {
                                    nini_buf_puts(&buf, ", ");
                                }
                                nini_encode_value(vm, deps->array[di],
                                                  &buf);
                            }
                            nini_buf_puts(&buf, "]\n");
                        }
                    } else {
                        nini_buf_printf(&buf, "%s = ", tname);
                        nini_encode_value(vm, tav, &buf);
                        nini_buf_putc(&buf, '\n');
                    }
                }
            }
        }
    }

    /* Transfer buffer to caller */
    *out = buf.data;
    *out_len = buf.len;
    /* Do NOT free buf — ownership transferred */
    return 0;
}
