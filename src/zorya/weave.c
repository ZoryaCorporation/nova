/**
 * @file weave.c
 * @brief WEAVE - The Third Sister: Industrial-Strength String Library
 *
 * @author Anthony Taliento
 * @date 2025-12-18
 * @version 1.0.0
 *
 * @copyright Copyright (c) 2025 Zorya Corporation
 * @license MIT
 *
 * DESCRIPTION:
 *   Complete implementation of the Weave string library, including:
 *   - Weave: Mutable fat strings
 *   - Tablet: Interned string pool
 *   - Cord: Deferred concatenation
 *   - WTC: Combined high-level operations
 *
 * DEPENDENCIES:
 *   - nxh.h/nxh.c (hashing)
 *   - dagger.h/dagger.c (Tablet backing)
 *   - zorya_arena.h (Tablet memory pool)
 */

#include "zorya/weave.h"
#include "zorya/nxh.h"
#include "zorya/dagger.h"
#include "zorya/zorya_arena.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>

/* ============================================================
 * INTERNAL STRUCTURES
 * ============================================================ */

/**
 * @brief Weave internal structure
 *
 * Memory layout is a single allocation:
 *   struct Weave {
 *       size_t len;
 *       size_t cap;
 *       uint8_t flags;
 *       char data[cap + 1];  // +1 for NUL
 *   }
 */
struct Weave {
    size_t  len;        /* Current length (not including NUL) */
    size_t  cap;        /* Capacity (not including NUL) */
    uint8_t flags;      /* WEAVE_FLAG_* */
    char    data[];     /* Flexible array member */
};

/**
 * @brief Tablet internal structure
 */
struct Tablet {
    DaggerTable *pool;      /* hash -> Weave* mapping */
    Arena       *arena;     /* Memory for interned strings */
    size_t       count;     /* Number of interned strings */
    size_t       memory;    /* Total bytes in arena */
};

/**
 * @brief Cord chunk structure
 */
typedef struct CordChunk {
    const char *data;       /* Pointer to string data */
    size_t      len;        /* Length of this chunk */
    bool        owned;      /* true if we should free data */
} CordChunk;

/**
 * @brief Cord internal structure
 */
struct Cord {
    size_t      total_len;      /* Sum of all chunk lengths */
    size_t      chunk_count;    /* Number of chunks */
    size_t      chunk_cap;      /* Capacity of chunks array */
    CordChunk  *chunks;         /* Array of chunks */
};

/* ============================================================
 * HELPER FUNCTIONS (PCM-Optimized)
 * ============================================================ */

/**
 * @brief Calculate allocation size for Weave with given capacity
 * HOT PATH: Called on every Weave creation
 */
static WEAVE_INLINE size_t weave_alloc_size(size_t cap) {
    /* Align to 8 bytes for better cache performance */
    return WEAVE_ALIGN_UP(sizeof(Weave) + cap + 1, 8);
}

/**
 * @brief Calculate new capacity using growth factor
 * HOT PATH: Called on every resize
 */
static WEAVE_INLINE size_t weave_grow_cap(size_t current, size_t needed) {
    /* Fast path: use power-of-2 rounding for cache alignment */
    if (WEAVE_LIKELY(needed <= 64)) {
        return 64;  /* Common small string case */
    }
    
    /* For larger strings, round up to next power of 2 */
    size_t new_cap = WEAVE_NEXT_POW2(needed);
    
    /* Ensure minimum growth */
    if (WEAVE_UNLIKELY(new_cap < current * 2)) {
        new_cap = WEAVE_NEXT_POW2(current * 2);
    }
    
    return new_cap;
}

/**
 * @brief Safe strlen that handles NULL
 * HOT PATH: Called frequently
 */
static WEAVE_INLINE WEAVE_PURE size_t safe_strlen(const char *s) {
    if (WEAVE_UNLIKELY(s == NULL)) return 0;
    return strlen(s);
}

/**
 * @brief Standard whitespace characters
 */
static const char *WHITESPACE = " \t\n\r\f\v";

/**
 * @brief Check if character is in set
 * HOT PATH: Called in trim/scan operations
 */
static WEAVE_INLINE WEAVE_HOT bool char_in_set(char c, const char *set) {
    if (WEAVE_UNLIKELY(set == NULL)) return false;
    
    /* Prefetch the set string for sequential scan */
    WEAVE_PREFETCH(set);
    
    while (*set) {
        if (WEAVE_UNLIKELY(*set == c)) return true;
        set++;
    }
    return false;
}

/* ============================================================
 * WEAVE - Creation and Destruction
 * ============================================================ */

Weave* weave_new(const char *s) {
    return weave_new_len(s, safe_strlen(s));
}

Weave* weave_new_len(const char *s, size_t len) {
    /* Round capacity to power-of-2 for cache alignment */
    size_t cap = len < WEAVE_INITIAL_CAP ? WEAVE_INITIAL_CAP : WEAVE_NEXT_POW2(len);
    
    Weave *w = malloc(weave_alloc_size(cap));
    if (WEAVE_UNLIKELY(w == NULL)) return NULL;
    
    w->len = len;
    w->cap = cap;
    w->flags = WEAVE_FLAG_NONE;
    
    if (WEAVE_LIKELY(s != NULL && len > 0)) {
        memcpy(w->data, s, len);
    }
    w->data[len] = '\0';
    
    return w;
}

Weave* weave_with_cap(size_t cap) {
    if (cap < WEAVE_INITIAL_CAP) {
        cap = WEAVE_INITIAL_CAP;
    }
    
    Weave *w = malloc(weave_alloc_size(cap));
    if (w == NULL) return NULL;
    
    w->len = 0;
    w->cap = cap;
    w->flags = WEAVE_FLAG_NONE;
    w->data[0] = '\0';
    
    return w;
}

Weave* weave_dup(const Weave *w) {
    if (w == NULL) return NULL;
    return weave_new_len(w->data, w->len);
}

void weave_free(Weave *w) {
    if (w == NULL) return;
    
    /* Don't free interned strings - owned by Tablet */
    if (w->flags & WEAVE_FLAG_INTERNED) return;
    
    /* Don't free static strings */
    if (w->flags & WEAVE_FLAG_STATIC) return;
    
    free(w);
}

/* ============================================================
 * WEAVE - Access
 * ============================================================ */

static const char EMPTY_STRING[] = "";

/* HOT PATH: These accessors are called constantly */
const char* weave_cstr(const Weave *w) {
    return WEAVE_LIKELY(w != NULL) ? w->data : EMPTY_STRING;
}

size_t weave_len(const Weave *w) {
    return WEAVE_LIKELY(w != NULL) ? w->len : 0;
}

size_t weave_cap(const Weave *w) {
    return WEAVE_LIKELY(w != NULL) ? w->cap : 0;
}

bool weave_empty(const Weave *w) {
    return WEAVE_UNLIKELY(w == NULL) || w->len == 0;
}

char weave_at(const Weave *w, size_t i) {
    if (w == NULL || i >= w->len) return '\0';
    return w->data[i];
}

uint8_t weave_flags(const Weave *w) {
    return w ? w->flags : 0;
}

bool weave_is_interned(const Weave *w) {
    return w && (w->flags & WEAVE_FLAG_INTERNED);
}

/* ============================================================
 * WEAVE - Mutation
 * ============================================================ */

/**
 * @brief Ensure Weave has capacity for additional bytes
 */
static Weave* weave_ensure_cap(Weave *w, size_t additional) {
    if (w == NULL) return NULL;
    
    /* Check if mutation is allowed */
    if (w->flags & (WEAVE_FLAG_INTERNED | WEAVE_FLAG_READONLY)) {
        return NULL;  /* Cannot mutate */
    }
    
    size_t needed = w->len + additional;
    if (needed <= w->cap) {
        return w;  /* Already have capacity */
    }
    
    /* Need to grow */
    size_t new_cap = weave_grow_cap(w->cap, needed);
    Weave *new_w = realloc(w, weave_alloc_size(new_cap));
    if (new_w == NULL) return NULL;
    
    new_w->cap = new_cap;
    return new_w;
}

Weave* weave_append(Weave *w, const char *s) {
    return weave_append_len(w, s, safe_strlen(s));
}

Weave* weave_append_len(Weave *w, const char *s, size_t len) {
    if (w == NULL) return NULL;
    if (s == NULL || len == 0) return w;
    
    w = weave_ensure_cap(w, len);
    if (w == NULL) return NULL;
    
    memcpy(w->data + w->len, s, len);
    w->len += len;
    w->data[w->len] = '\0';
    
    return w;
}

Weave* weave_append_char(Weave *w, char c) {
    return weave_append_len(w, &c, 1);
}

Weave* weave_append_weave(Weave *w, const Weave *other) {
    if (other == NULL) return w;
    return weave_append_len(w, other->data, other->len);
}

Weave* weave_prepend(Weave *w, const char *s) {
    return weave_prepend_len(w, s, safe_strlen(s));
}

Weave* weave_prepend_len(Weave *w, const char *s, size_t len) {
    if (w == NULL) return NULL;
    if (s == NULL || len == 0) return w;
    
    w = weave_ensure_cap(w, len);
    if (w == NULL) return NULL;
    
    /* Shift existing data right */
    memmove(w->data + len, w->data, w->len + 1);  /* +1 for NUL */
    memcpy(w->data, s, len);
    w->len += len;
    
    return w;
}

void weave_clear(Weave *w) {
    if (w == NULL) return;
    if (w->flags & (WEAVE_FLAG_INTERNED | WEAVE_FLAG_READONLY)) return;
    
    w->len = 0;
    w->data[0] = '\0';
}

void weave_truncate(Weave *w, size_t len) {
    if (w == NULL) return;
    if (w->flags & (WEAVE_FLAG_INTERNED | WEAVE_FLAG_READONLY)) return;
    if (len >= w->len) return;
    
    w->len = len;
    w->data[len] = '\0';
}

Weave* weave_reserve(Weave *w, size_t min_cap) {
    if (w == NULL) return NULL;
    if (min_cap <= w->cap) return w;
    
    return weave_ensure_cap(w, min_cap - w->len);
}

Weave* weave_shrink(Weave *w) {
    if (w == NULL) return NULL;
    if (w->flags & WEAVE_FLAG_INTERNED) return w;
    if (w->len == w->cap) return w;
    
    Weave *new_w = realloc(w, weave_alloc_size(w->len));
    if (new_w == NULL) return w;  /* Keep old on failure */
    
    new_w->cap = new_w->len;
    return new_w;
}

/* ============================================================
 * WEAVE - Operations (return new Weave)
 * ============================================================ */

Weave* weave_substr(const Weave *w, size_t start, size_t len) {
    if (w == NULL || start >= w->len) {
        return weave_new("");
    }
    
    size_t avail = w->len - start;
    if (len > avail) len = avail;
    
    return weave_new_len(w->data + start, len);
}

Weave* weave_slice(const Weave *w, size_t start, int64_t end) {
    if (w == NULL) return weave_new("");
    
    /* Handle negative end (from end of string) */
    size_t actual_end;
    if (end < 0) {
        actual_end = w->len;
    } else {
        actual_end = (size_t)end;
        if (actual_end > w->len) actual_end = w->len;
    }
    
    if (start >= actual_end) {
        return weave_new("");
    }
    
    return weave_new_len(w->data + start, actual_end - start);
}

Weave* weave_replace(const Weave *w, const char *old, const char *new_s) {
    if (w == NULL) return NULL;
    if (old == NULL || *old == '\0') return weave_dup(w);
    if (new_s == NULL) new_s = "";
    
    /* Find first occurrence */
    const char *pos = strstr(w->data, old);
    if (pos == NULL) {
        return weave_dup(w);
    }
    
    size_t old_len = strlen(old);
    size_t new_len = strlen(new_s);
    size_t prefix_len = (size_t)(pos - w->data);
    size_t suffix_len = w->len - prefix_len - old_len;
    
    /* Build result */
    Weave *result = weave_with_cap(prefix_len + new_len + suffix_len);
    if (result == NULL) return NULL;
    
    memcpy(result->data, w->data, prefix_len);
    memcpy(result->data + prefix_len, new_s, new_len);
    memcpy(result->data + prefix_len + new_len, pos + old_len, suffix_len);
    
    result->len = prefix_len + new_len + suffix_len;
    result->data[result->len] = '\0';
    
    return result;
}

/**
 * @brief Replace all occurrences of old with new_s
 * CRITICAL HOT PATH: PCM optimized for performance
 */
Weave* weave_replace_all(const Weave *w, const char *old, const char *new_s) {
    if (WEAVE_UNLIKELY(w == NULL)) return NULL;
    if (WEAVE_UNLIKELY(old == NULL || *old == '\0')) return weave_dup(w);
    if (new_s == NULL) new_s = "";
    
    size_t old_len = strlen(old);
    size_t new_len = strlen(new_s);
    
    /* Prefetch source data for scanning */
    WEAVE_PREFETCH(w->data);
    
    /* Count occurrences (single pass) */
    size_t count = 0;
    const char *p = w->data;
    while (WEAVE_LIKELY((p = strstr(p, old)) != NULL)) {
        count++;
        p += old_len;
        /* Prefetch ahead for next iteration */
        if (p < w->data + w->len) {
            WEAVE_PREFETCH(p + 64);
        }
    }
    
    /* Fast path: no matches (most common case) */
    if (WEAVE_LIKELY(count == 0)) {
        return weave_dup(w);
    }
    
    /* Calculate new size with overflow check */
    size_t new_size;
    if (new_len >= old_len) {
        new_size = w->len + count * (new_len - old_len);
    } else {
        new_size = w->len - count * (old_len - new_len);
    }
    
    /* Allocate result with power-of-2 capacity */
    Weave *result = weave_with_cap(new_size);
    if (WEAVE_UNLIKELY(result == NULL)) return NULL;
    
    /* Build result (single allocation, no intermediate copies) */
    char *dst = result->data;
    const char *src = w->data;
    
    while (WEAVE_LIKELY((p = strstr(src, old)) != NULL)) {
        /* Copy prefix */
        size_t prefix_len = (size_t)(p - src);
        if (WEAVE_LIKELY(prefix_len > 0)) {
            memcpy(dst, src, prefix_len);
            dst += prefix_len;
        }
        
        /* Copy replacement */
        if (WEAVE_LIKELY(new_len > 0)) {
            memcpy(dst, new_s, new_len);
            dst += new_len;
        }
        
        src = p + old_len;
    }
    
    /* Copy remainder */
    size_t remain = w->len - (size_t)(src - w->data);
    memcpy(dst, src, remain);
    dst += remain;
    
    *dst = '\0';
    result->len = (size_t)(dst - result->data);
    
    return result;
}

Weave* weave_trim(const Weave *w) {
    return weave_trim_chars(w, WHITESPACE);
}

Weave* weave_trim_chars(const Weave *w, const char *chars) {
    if (w == NULL) return weave_new("");
    if (chars == NULL) chars = WHITESPACE;
    
    /* Find start (skip leading chars) */
    size_t start = 0;
    while (start < w->len && char_in_set(w->data[start], chars)) {
        start++;
    }
    
    /* Find end (skip trailing chars) */
    size_t end = w->len;
    while (end > start && char_in_set(w->data[end - 1], chars)) {
        end--;
    }
    
    return weave_new_len(w->data + start, end - start);
}

Weave* weave_trim_left(const Weave *w, const char *chars) {
    if (w == NULL) return weave_new("");
    if (chars == NULL) chars = WHITESPACE;
    
    size_t start = 0;
    while (start < w->len && char_in_set(w->data[start], chars)) {
        start++;
    }
    
    return weave_new_len(w->data + start, w->len - start);
}

Weave* weave_trim_right(const Weave *w, const char *chars) {
    if (w == NULL) return weave_new("");
    if (chars == NULL) chars = WHITESPACE;
    
    size_t end = w->len;
    while (end > 0 && char_in_set(w->data[end - 1], chars)) {
        end--;
    }
    
    return weave_new_len(w->data, end);
}

Weave* weave_to_upper(const Weave *w) {
    if (w == NULL) return weave_new("");
    
    Weave *result = weave_dup(w);
    if (result == NULL) return NULL;
    
    for (size_t i = 0; i < result->len; i++) {
        result->data[i] = (char)toupper((unsigned char)result->data[i]);
    }
    
    return result;
}

Weave* weave_to_lower(const Weave *w) {
    if (w == NULL) return weave_new("");
    
    Weave *result = weave_dup(w);
    if (result == NULL) return NULL;
    
    for (size_t i = 0; i < result->len; i++) {
        result->data[i] = (char)tolower((unsigned char)result->data[i]);
    }
    
    return result;
}

Weave* weave_repeat(const Weave *w, size_t n) {
    if (w == NULL || n == 0) return weave_new("");
    if (n == 1) return weave_dup(w);
    
    Weave *result = weave_with_cap(w->len * n);
    if (result == NULL) return NULL;
    
    for (size_t i = 0; i < n; i++) {
        memcpy(result->data + (i * w->len), w->data, w->len);
    }
    
    result->len = w->len * n;
    result->data[result->len] = '\0';
    
    return result;
}

Weave* weave_reverse(const Weave *w) {
    if (w == NULL) return weave_new("");
    
    Weave *result = weave_with_cap(w->len);
    if (result == NULL) return NULL;
    
    for (size_t i = 0; i < w->len; i++) {
        result->data[i] = w->data[w->len - 1 - i];
    }
    
    result->len = w->len;
    result->data[result->len] = '\0';
    
    return result;
}

/* ============================================================
 * WEAVE - Search
 * ============================================================ */

int64_t weave_find(const Weave *w, const char *needle) {
    return weave_find_from(w, needle, 0);
}

int64_t weave_find_from(const Weave *w, const char *needle, size_t start) {
    if (w == NULL || needle == NULL || start >= w->len) {
        return -1;
    }
    
    const char *pos = strstr(w->data + start, needle);
    if (pos == NULL) return -1;
    
    return (int64_t)(pos - w->data);
}

int64_t weave_rfind(const Weave *w, const char *needle) {
    if (w == NULL || needle == NULL) return -1;
    
    size_t needle_len = strlen(needle);
    if (needle_len == 0) return (int64_t)w->len;
    if (needle_len > w->len) return -1;
    
    /* Search backwards */
    for (size_t i = w->len - needle_len + 1; i > 0; i--) {
        if (memcmp(w->data + i - 1, needle, needle_len) == 0) {
            return (int64_t)(i - 1);
        }
    }
    
    return -1;
}

int64_t weave_find_char(const Weave *w, char c) {
    if (w == NULL) return -1;
    
    const char *pos = memchr(w->data, c, w->len);
    if (pos == NULL) return -1;
    
    return (int64_t)(pos - w->data);
}

int64_t weave_rfind_char(const Weave *w, char c) {
    if (w == NULL) return -1;
    
    for (size_t i = w->len; i > 0; i--) {
        if (w->data[i - 1] == c) {
            return (int64_t)(i - 1);
        }
    }
    
    return -1;
}

int64_t weave_find_any(const Weave *w, const char *chars) {
    if (w == NULL || chars == NULL) return -1;
    
    for (size_t i = 0; i < w->len; i++) {
        if (char_in_set(w->data[i], chars)) {
            return (int64_t)i;
        }
    }
    
    return -1;
}

int64_t weave_find_not(const Weave *w, const char *chars) {
    if (w == NULL || chars == NULL) return -1;
    
    for (size_t i = 0; i < w->len; i++) {
        if (!char_in_set(w->data[i], chars)) {
            return (int64_t)i;
        }
    }
    
    return -1;
}

bool weave_contains(const Weave *w, const char *needle) {
    return weave_find(w, needle) >= 0;
}

bool weave_starts_with(const Weave *w, const char *prefix) {
    if (w == NULL || prefix == NULL) return false;
    
    size_t prefix_len = strlen(prefix);
    if (prefix_len > w->len) return false;
    
    return memcmp(w->data, prefix, prefix_len) == 0;
}

bool weave_ends_with(const Weave *w, const char *suffix) {
    if (w == NULL || suffix == NULL) return false;
    
    size_t suffix_len = strlen(suffix);
    if (suffix_len > w->len) return false;
    
    return memcmp(w->data + w->len - suffix_len, suffix, suffix_len) == 0;
}

size_t weave_count(const Weave *w, const char *needle) {
    if (w == NULL || needle == NULL || *needle == '\0') return 0;
    
    size_t count = 0;
    size_t needle_len = strlen(needle);
    const char *p = w->data;
    
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += needle_len;
    }
    
    return count;
}

/* ============================================================
 * WEAVE - Comparison
 * ============================================================ */

int weave_cmp(const Weave *a, const Weave *b) {
    if (a == b) return 0;
    if (a == NULL) return -1;
    if (b == NULL) return 1;
    
    size_t min_len = a->len < b->len ? a->len : b->len;
    int result = memcmp(a->data, b->data, min_len);
    
    if (result != 0) return result;
    if (a->len < b->len) return -1;
    if (a->len > b->len) return 1;
    return 0;
}

int weave_cmp_cstr(const Weave *w, const char *s) {
    if (w == NULL && s == NULL) return 0;
    if (w == NULL) return -1;
    if (s == NULL) return 1;
    
    return strcmp(w->data, s);
}

int weave_casecmp(const Weave *a, const Weave *b) {
    if (a == b) return 0;
    if (a == NULL) return -1;
    if (b == NULL) return 1;
    
    size_t min_len = a->len < b->len ? a->len : b->len;
    
    for (size_t i = 0; i < min_len; i++) {
        int ca = tolower((unsigned char)a->data[i]);
        int cb = tolower((unsigned char)b->data[i]);
        if (ca != cb) return ca - cb;
    }
    
    if (a->len < b->len) return -1;
    if (a->len > b->len) return 1;
    return 0;
}

bool weave_eq(const Weave *a, const Weave *b) {
    if (a == b) return true;
    if (a == NULL || b == NULL) return false;
    if (a->len != b->len) return false;
    
    return memcmp(a->data, b->data, a->len) == 0;
}

bool weave_eq_cstr(const Weave *w, const char *s) {
    if (w == NULL && s == NULL) return true;
    if (w == NULL || s == NULL) return false;
    
    size_t s_len = strlen(s);
    if (w->len != s_len) return false;
    
    return memcmp(w->data, s, s_len) == 0;
}

bool weave_caseeq(const Weave *a, const Weave *b) {
    return weave_casecmp(a, b) == 0;
}

/* ============================================================
 * WEAVE - Hashing
 * ============================================================ */

uint64_t weave_hash(const Weave *w) {
    if (w == NULL) return 0;
    return nxh64(w->data, w->len, 0);
}

/* ============================================================
 * WEAVE - Split and Join
 * ============================================================ */

Weave** weave_split(const Weave *w, const char *delim, size_t *count) {
    if (count) *count = 0;
    if (w == NULL || delim == NULL) return NULL;
    
    size_t delim_len = strlen(delim);
    if (delim_len == 0) {
        /* Empty delimiter: return single element */
        Weave **result = malloc(sizeof(Weave*));
        if (result == NULL) return NULL;
        result[0] = weave_dup(w);
        if (count) *count = 1;
        return result;
    }
    
    /* Count parts */
    size_t part_count = 1;
    const char *p = w->data;
    while ((p = strstr(p, delim)) != NULL) {
        part_count++;
        p += delim_len;
    }
    
    /* Allocate result array */
    Weave **result = malloc(part_count * sizeof(Weave*));
    if (result == NULL) return NULL;
    
    /* Split */
    size_t idx = 0;
    const char *start = w->data;
    p = w->data;
    
    while ((p = strstr(p, delim)) != NULL) {
        result[idx++] = weave_new_len(start, (size_t)(p - start));
        p += delim_len;
        start = p;
    }
    
    /* Last part */
    result[idx++] = weave_new_len(start, w->len - (size_t)(start - w->data));
    
    if (count) *count = part_count;
    return result;
}

Weave** weave_split_any(const Weave *w, const char *chars, size_t *count) {
    if (count) *count = 0;
    if (w == NULL || chars == NULL) return NULL;
    
    /* Count parts */
    size_t part_count = 0;
    bool in_part = false;
    
    for (size_t i = 0; i < w->len; i++) {
        if (char_in_set(w->data[i], chars)) {
            in_part = false;
        } else if (!in_part) {
            part_count++;
            in_part = true;
        }
    }
    
    if (part_count == 0) {
        return NULL;
    }
    
    /* Allocate result */
    Weave **result = malloc(part_count * sizeof(Weave*));
    if (result == NULL) return NULL;
    
    /* Split */
    size_t idx = 0;
    size_t start = 0;
    in_part = false;
    
    for (size_t i = 0; i <= w->len; i++) {
        bool is_delim = (i == w->len) || char_in_set(w->data[i], chars);
        
        if (is_delim && in_part) {
            result[idx++] = weave_new_len(w->data + start, i - start);
            in_part = false;
        } else if (!is_delim && !in_part) {
            start = i;
            in_part = true;
        }
    }
    
    if (count) *count = part_count;
    return result;
}

Weave** weave_lines(const Weave *w, size_t *count) {
    if (count) *count = 0;
    if (w == NULL) return NULL;
    
    /* Count lines */
    size_t line_count = 1;
    for (size_t i = 0; i < w->len; i++) {
        if (w->data[i] == '\n') line_count++;
        else if (w->data[i] == '\r') {
            line_count++;
            /* Skip \r\n */
            if (i + 1 < w->len && w->data[i + 1] == '\n') i++;
        }
    }
    
    /* Allocate result */
    Weave **result = malloc(line_count * sizeof(Weave*));
    if (result == NULL) return NULL;
    
    /* Split */
    size_t idx = 0;
    size_t start = 0;
    
    for (size_t i = 0; i <= w->len; i++) {
        bool is_eol = (i == w->len) || 
                      (w->data[i] == '\n') || 
                      (w->data[i] == '\r');
        
        if (is_eol) {
            result[idx++] = weave_new_len(w->data + start, i - start);
            
            /* Handle \r\n */
            if (i < w->len && w->data[i] == '\r' && 
                i + 1 < w->len && w->data[i + 1] == '\n') {
                i++;
            }
            start = i + 1;
        }
    }
    
    if (count) *count = line_count;
    return result;
}

Weave* weave_join(const char **parts, size_t count, const char *sep) {
    if (parts == NULL || count == 0) return weave_new("");
    if (sep == NULL) sep = "";
    
    size_t sep_len = strlen(sep);
    
    /* Calculate total length */
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        if (parts[i]) total += strlen(parts[i]);
        if (i + 1 < count) total += sep_len;
    }
    
    /* Build result */
    Weave *result = weave_with_cap(total);
    if (result == NULL) return NULL;
    
    for (size_t i = 0; i < count; i++) {
        if (parts[i]) {
            result = weave_append(result, parts[i]);
        }
        if (i + 1 < count && sep_len > 0) {
            result = weave_append(result, sep);
        }
    }
    
    return result;
}

Weave* weave_join_weave(Weave **parts, size_t count, const char *sep) {
    if (parts == NULL || count == 0) return weave_new("");
    if (sep == NULL) sep = "";
    
    size_t sep_len = strlen(sep);
    
    /* Calculate total length */
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        if (parts[i]) total += parts[i]->len;
        if (i + 1 < count) total += sep_len;
    }
    
    /* Build result */
    Weave *result = weave_with_cap(total);
    if (result == NULL) return NULL;
    
    for (size_t i = 0; i < count; i++) {
        if (parts[i]) {
            result = weave_append_weave(result, parts[i]);
        }
        if (i + 1 < count && sep_len > 0) {
            result = weave_append(result, sep);
        }
    }
    
    return result;
}

void weave_free_array(Weave **arr, size_t count) {
    if (arr == NULL) return;
    
    for (size_t i = 0; i < count; i++) {
        weave_free(arr[i]);
    }
    free(arr);
}

/* ============================================================
 * WEAVE - Formatting
 * ============================================================ */

Weave* weave_fmt(const char *fmt, ...) {
    if (fmt == NULL) return weave_new("");
    
    va_list args, args_copy;
    va_start(args, fmt);
    va_copy(args_copy, args);
    
    /* Get required size */
    int size = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    
    if (size < 0) {
        va_end(args_copy);
        return NULL;
    }
    
    /* Allocate and format */
    Weave *result = weave_with_cap((size_t)size);
    if (result == NULL) {
        va_end(args_copy);
        return NULL;
    }
    
    vsnprintf(result->data, (size_t)size + 1, fmt, args_copy);
    va_end(args_copy);
    
    result->len = (size_t)size;
    return result;
}

Weave* weave_append_fmt(Weave *w, const char *fmt, ...) {
    if (w == NULL || fmt == NULL) return w;
    
    va_list args, args_copy;
    va_start(args, fmt);
    va_copy(args_copy, args);
    
    /* Get required size */
    int size = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    
    if (size < 0) {
        va_end(args_copy);
        return w;
    }
    
    /* Ensure capacity */
    w = weave_ensure_cap(w, (size_t)size);
    if (w == NULL) {
        va_end(args_copy);
        return NULL;
    }
    
    /* Append formatted */
    vsnprintf(w->data + w->len, (size_t)size + 1, fmt, args_copy);
    va_end(args_copy);
    
    w->len += (size_t)size;
    return w;
}

/* ============================================================
 * TABLET - Interned String Pool
 * ============================================================ */

Tablet* tablet_create(void) {
    return tablet_create_sized(TABLET_INITIAL_BUCKETS);
}

Tablet* tablet_create_sized(size_t initial_buckets) {
    Tablet *T = malloc(sizeof(Tablet));
    if (T == NULL) return NULL;
    
    T->pool = dagger_create(initial_buckets, NULL);  /* NULL = default memcmp */
    if (T->pool == NULL) {
        free(T);
        return NULL;
    }
    
    T->arena = arena_create(4096);  /* 4KB chunks */
    if (T->arena == NULL) {
        dagger_destroy(T->pool);
        free(T);
        return NULL;
    }
    
    T->count = 0;
    T->memory = 0;
    
    return T;
}

void tablet_destroy(Tablet *T) {
    if (T == NULL) return;
    
    dagger_destroy(T->pool);
    arena_destroy(T->arena);
    free(T);
}

const Weave* tablet_intern(Tablet *T, const char *s) {
    if (T == NULL || s == NULL) return NULL;
    return tablet_intern_len(T, s, strlen(s));
}

/**
 * @brief Intern a string into the Tablet
 * CRITICAL HOT PATH: O(1) lookup with DAGGER, optimized with PCM
 */
const Weave* tablet_intern_len(Tablet *T, const char *s, size_t len) {
    if (WEAVE_UNLIKELY(T == NULL)) return NULL;
    if (WEAVE_UNLIKELY(s == NULL && len > 0)) return NULL;
    
    /* Fast path: check if already interned (common case) */
    void *existing = NULL;
    dagger_result_t result = dagger_get(T->pool, s, (uint32_t)len, &existing);
    if (WEAVE_LIKELY(result == DAGGER_OK && existing != NULL)) {
        return (const Weave*)existing;  /* Cache hit! */
    }
    
    /* Slow path: create new interned string in arena */
    size_t alloc_size = weave_alloc_size(len);
    Weave *w = arena_alloc(T->arena, alloc_size);
    if (WEAVE_UNLIKELY(w == NULL)) return NULL;
    
    w->len = len;
    w->cap = len;
    w->flags = WEAVE_FLAG_INTERNED | WEAVE_FLAG_READONLY;
    
    if (WEAVE_LIKELY(s != NULL && len > 0)) {
        memcpy(w->data, s, len);
    }
    w->data[len] = '\0';
    
    /* Store in pool (use string data as key for O(1) lookup) */
    result = dagger_set(T->pool, w->data, (uint32_t)len, w, 0);
    if (WEAVE_UNLIKELY(result != DAGGER_OK)) {
        /* Arena allocation can't be freed individually, but that's OK */
        return NULL;
    }
    
    T->count++;
    T->memory += alloc_size;
    
    return w;
}

const Weave* tablet_intern_weave(Tablet *T, const Weave *w) {
    if (T == NULL || w == NULL) return NULL;
    
    /* If already interned in this tablet, just return it */
    if (w->flags & WEAVE_FLAG_INTERNED) {
        /* Verify it's actually from this tablet by lookup */
        const Weave *existing = tablet_lookup_len(T, w->data, w->len);
        if (existing == w) return w;
    }
    
    return tablet_intern_len(T, w->data, w->len);
}

const Weave* tablet_lookup(const Tablet *T, const char *s) {
    if (T == NULL || s == NULL) return NULL;
    return tablet_lookup_len(T, s, strlen(s));
}

const Weave* tablet_lookup_len(const Tablet *T, const char *s, size_t len) {
    if (T == NULL) return NULL;
    
    void *existing = NULL;
    dagger_result_t result = dagger_get(T->pool, s, (uint32_t)len, &existing);
    
    if (result == DAGGER_OK && existing != NULL) {
        return (const Weave*)existing;
    }
    
    return NULL;
}

bool tablet_contains(const Tablet *T, const char *s) {
    return tablet_lookup(T, s) != NULL;
}

size_t tablet_count(const Tablet *T) {
    return T ? T->count : 0;
}

size_t tablet_memory(const Tablet *T) {
    return T ? T->memory : 0;
}

/* ============================================================
 * CORD - Deferred Concatenation
 * ============================================================ */

Cord* cord_new(void) {
    return cord_with_cap(CORD_INITIAL_CHUNKS);
}

Cord* cord_with_cap(size_t chunk_cap) {
    Cord *c = malloc(sizeof(Cord));
    if (c == NULL) return NULL;
    
    c->chunks = malloc(chunk_cap * sizeof(CordChunk));
    if (c->chunks == NULL) {
        free(c);
        return NULL;
    }
    
    c->total_len = 0;
    c->chunk_count = 0;
    c->chunk_cap = chunk_cap;
    
    return c;
}

void cord_free(Cord *c) {
    if (c == NULL) return;
    
    /* Free owned chunks */
    for (size_t i = 0; i < c->chunk_count; i++) {
        if (c->chunks[i].owned) {
            free((void*)c->chunks[i].data);
        }
    }
    
    free(c->chunks);
    free(c);
}

void cord_clear(Cord *c) {
    if (c == NULL) return;
    
    /* Free owned chunks */
    for (size_t i = 0; i < c->chunk_count; i++) {
        if (c->chunks[i].owned) {
            free((void*)c->chunks[i].data);
        }
    }
    
    c->total_len = 0;
    c->chunk_count = 0;
}

/**
 * @brief Ensure Cord has capacity for more chunks
 */
static Cord* cord_ensure_cap(Cord *c) {
    if (c == NULL) return NULL;
    if (c->chunk_count < c->chunk_cap) return c;
    
    size_t new_cap = (c->chunk_cap * 3) / 2;
    CordChunk *new_chunks = realloc(c->chunks, new_cap * sizeof(CordChunk));
    if (new_chunks == NULL) return NULL;
    
    c->chunks = new_chunks;
    c->chunk_cap = new_cap;
    return c;
}

Cord* cord_append(Cord *c, const char *s) {
    if (c == NULL || s == NULL) return c;
    return cord_append_len(c, s, strlen(s));
}

Cord* cord_append_len(Cord *c, const char *s, size_t len) {
    if (c == NULL) return NULL;
    if (s == NULL || len == 0) return c;
    
    c = cord_ensure_cap(c);
    if (c == NULL) return NULL;
    
    /* Copy the data (we own it) */
    char *copy = malloc(len);
    if (copy == NULL) return NULL;
    memcpy(copy, s, len);
    
    c->chunks[c->chunk_count].data = copy;
    c->chunks[c->chunk_count].len = len;
    c->chunks[c->chunk_count].owned = true;
    c->chunk_count++;
    c->total_len += len;
    
    return c;
}

Cord* cord_append_weave(Cord *c, const Weave *w) {
    if (c == NULL || w == NULL || w->len == 0) return c;
    
    c = cord_ensure_cap(c);
    if (c == NULL) return NULL;
    
    /* Reference the Weave data (don't copy) */
    c->chunks[c->chunk_count].data = w->data;
    c->chunks[c->chunk_count].len = w->len;
    c->chunks[c->chunk_count].owned = false;  /* Don't free - owned by Weave */
    c->chunk_count++;
    c->total_len += w->len;
    
    return c;
}

Cord* cord_append_cord(Cord *c, const Cord *other) {
    if (c == NULL || other == NULL) return c;
    
    for (size_t i = 0; i < other->chunk_count; i++) {
        /* Copy each chunk (we need ownership) */
        c = cord_append_len(c, other->chunks[i].data, other->chunks[i].len);
        if (c == NULL) return NULL;
    }
    
    return c;
}

Cord* cord_append_char(Cord *c, char ch) {
    return cord_append_len(c, &ch, 1);
}

Cord* cord_append_fmt(Cord *c, const char *fmt, ...) {
    if (c == NULL || fmt == NULL) return c;
    
    va_list args, args_copy;
    va_start(args, fmt);
    va_copy(args_copy, args);
    
    int size = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    
    if (size <= 0) {
        va_end(args_copy);
        return c;
    }
    
    char *buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        va_end(args_copy);
        return NULL;
    }
    
    vsnprintf(buf, (size_t)size + 1, fmt, args_copy);
    va_end(args_copy);
    
    c = cord_ensure_cap(c);
    if (c == NULL) {
        free(buf);
        return NULL;
    }
    
    c->chunks[c->chunk_count].data = buf;
    c->chunks[c->chunk_count].len = (size_t)size;
    c->chunks[c->chunk_count].owned = true;
    c->chunk_count++;
    c->total_len += (size_t)size;
    
    return c;
}

size_t cord_len(const Cord *c) {
    return c ? c->total_len : 0;
}

size_t cord_chunk_count(const Cord *c) {
    return c ? c->chunk_count : 0;
}

bool cord_empty(const Cord *c) {
    return c == NULL || c->total_len == 0;
}

Weave* cord_to_weave(const Cord *c) {
    if (c == NULL) return weave_new("");
    
    Weave *result = weave_with_cap(c->total_len);
    if (result == NULL) return NULL;
    
    for (size_t i = 0; i < c->chunk_count; i++) {
        memcpy(result->data + result->len, c->chunks[i].data, c->chunks[i].len);
        result->len += c->chunks[i].len;
    }
    
    result->data[result->len] = '\0';
    return result;
}

char* cord_to_cstr(const Cord *c) {
    if (c == NULL) {
        char *empty = malloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }
    
    char *result = malloc(c->total_len + 1);
    if (result == NULL) return NULL;
    
    char *p = result;
    for (size_t i = 0; i < c->chunk_count; i++) {
        memcpy(p, c->chunks[i].data, c->chunks[i].len);
        p += c->chunks[i].len;
    }
    *p = '\0';
    
    return result;
}

size_t cord_to_buf(const Cord *c, char *buf, size_t buflen) {
    if (c == NULL) {
        if (buf && buflen > 0) buf[0] = '\0';
        return 0;
    }
    
    /* If buf is NULL, just return required size */
    if (buf == NULL) {
        return c->total_len;
    }
    
    size_t written = 0;
    for (size_t i = 0; i < c->chunk_count && written < buflen - 1; i++) {
        size_t to_copy = c->chunks[i].len;
        if (written + to_copy > buflen - 1) {
            to_copy = buflen - 1 - written;
        }
        memcpy(buf + written, c->chunks[i].data, to_copy);
        written += to_copy;
    }
    
    buf[written] = '\0';
    return written;
}

void cord_foreach(const Cord *c, cord_iter_fn fn, void *ctx) {
    if (c == NULL || fn == NULL) return;
    
    for (size_t i = 0; i < c->chunk_count; i++) {
        if (!fn(c->chunks[i].data, c->chunks[i].len, ctx)) {
            break;
        }
    }
}

/* ============================================================
 * WTC - Combined Operations
 * ============================================================ */

Weave* wtc_interpolate(const Weave *template_str, wtc_lookup_fn lookup, void *ctx) {
    if (template_str == NULL) return weave_new("");
    if (lookup == NULL) return weave_dup(template_str);
    
    Cord *c = cord_new();
    if (c == NULL) return NULL;
    
    const char *p = template_str->data;
    const char *end = p + template_str->len;
    const char *chunk_start = p;
    
    while (p < end) {
        /* Look for $ */
        if (*p == '$') {
            /* Flush pending chunk */
            if (p > chunk_start) {
                c = cord_append_len(c, chunk_start, (size_t)(p - chunk_start));
                if (c == NULL) return NULL;
            }
            
            p++;
            if (p >= end) {
                /* Trailing $ */
                c = cord_append_char(c, '$');
                break;
            }
            
            if (*p == '$') {
                /* Escaped $$ -> $ */
                c = cord_append_char(c, '$');
                p++;
                chunk_start = p;
                continue;
            }
            
            if (*p == '{') {
                /* Variable reference ${...} */
                p++;
                const char *var_start = p;
                int brace_depth = 1;
                
                while (p < end && brace_depth > 0) {
                    if (*p == '{') brace_depth++;
                    else if (*p == '}') brace_depth--;
                    if (brace_depth > 0) p++;
                }
                
                if (brace_depth != 0) {
                    /* Unclosed brace - treat as literal */
                    c = cord_append(c, "${");
                    chunk_start = var_start;
                    continue;
                }
                
                /* Extract variable name and optional default */
                size_t var_len = (size_t)(p - var_start);
                char *var_name = malloc(var_len + 1);
                if (var_name == NULL) {
                    cord_free(c);
                    return NULL;
                }
                memcpy(var_name, var_start, var_len);
                var_name[var_len] = '\0';
                
                /* Check for default value: ${var:-default} */
                const char *default_val = NULL;
                char *colon_dash = strstr(var_name, ":-");
                if (colon_dash != NULL) {
                    *colon_dash = '\0';
                    default_val = colon_dash + 2;
                }
                
                /* Lookup variable */
                const char *value = lookup(var_name, ctx);
                
                if (value != NULL) {
                    c = cord_append(c, value);
                } else if (default_val != NULL) {
                    c = cord_append(c, default_val);
                }
                /* If not found and no default, variable is removed */
                
                free(var_name);
                p++;  /* Skip closing } */
                chunk_start = p;
                continue;
            }
            
            /* Bare $ - treat as literal */
            c = cord_append_char(c, '$');
            chunk_start = p;
            continue;
        }
        
        p++;
    }
    
    /* Flush remaining chunk */
    if (p > chunk_start) {
        c = cord_append_len(c, chunk_start, (size_t)(p - chunk_start));
    }
    
    Weave *result = cord_to_weave(c);
    cord_free(c);
    
    return result;
}

const Weave* wtc_interpolate_intern(
    Tablet *T,
    const Weave *template_str,
    wtc_lookup_fn lookup,
    void *ctx
) {
    Weave *result = wtc_interpolate(template_str, lookup, ctx);
    if (result == NULL) return NULL;
    
    const Weave *interned = tablet_intern_weave(T, result);
    weave_free(result);
    
    return interned;
}

const Weave* wtc_format(Tablet *T, const char *fmt, ...) {
    if (T == NULL || fmt == NULL) return NULL;
    
    va_list args, args_copy;
    va_start(args, fmt);
    va_copy(args_copy, args);
    
    int size = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    
    if (size < 0) {
        va_end(args_copy);
        return NULL;
    }
    
    char *buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        va_end(args_copy);
        return NULL;
    }
    
    vsnprintf(buf, (size_t)size + 1, fmt, args_copy);
    va_end(args_copy);
    
    const Weave *result = tablet_intern_len(T, buf, (size_t)size);
    free(buf);
    
    return result;
}

const Weave* wtc_join(Tablet *T, const char **parts, size_t count, const char *sep) {
    Weave *joined = weave_join(parts, count, sep);
    if (joined == NULL) return NULL;
    
    const Weave *result = tablet_intern_weave(T, joined);
    weave_free(joined);
    
    return result;
}

/* ============================================================
 * OPTIONAL: UTF-8 UTILITIES
 * ============================================================ */

#ifdef WEAVE_UTF8

size_t weave_utf8_len(const Weave *w) {
    if (w == NULL) return 0;
    
    size_t count = 0;
    const unsigned char *p = (const unsigned char*)w->data;
    const unsigned char *end = p + w->len;
    
    while (p < end) {
        /* Count leading bytes (not continuation bytes 10xxxxxx) */
        if ((*p & 0xC0) != 0x80) {
            count++;
        }
        p++;
    }
    
    return count;
}

bool weave_utf8_valid(const Weave *w) {
    if (w == NULL) return true;  /* Empty is valid */
    
    const unsigned char *p = (const unsigned char*)w->data;
    const unsigned char *end = p + w->len;
    
    while (p < end) {
        if (*p < 0x80) {
            /* ASCII */
            p++;
        } else if ((*p & 0xE0) == 0xC0) {
            /* 2-byte sequence */
            if (p + 1 >= end) return false;
            if ((p[1] & 0xC0) != 0x80) return false;
            if ((*p & 0x1E) == 0) return false;  /* Overlong */
            p += 2;
        } else if ((*p & 0xF0) == 0xE0) {
            /* 3-byte sequence */
            if (p + 2 >= end) return false;
            if ((p[1] & 0xC0) != 0x80) return false;
            if ((p[2] & 0xC0) != 0x80) return false;
            if (*p == 0xE0 && (p[1] & 0x20) == 0) return false;  /* Overlong */
            p += 3;
        } else if ((*p & 0xF8) == 0xF0) {
            /* 4-byte sequence */
            if (p + 3 >= end) return false;
            if ((p[1] & 0xC0) != 0x80) return false;
            if ((p[2] & 0xC0) != 0x80) return false;
            if ((p[3] & 0xC0) != 0x80) return false;
            if (*p == 0xF0 && (p[1] & 0x30) == 0) return false;  /* Overlong */
            if (*p > 0xF4) return false;  /* Beyond Unicode range */
            p += 4;
        } else {
            /* Invalid leading byte */
            return false;
        }
    }
    
    return true;
}

int64_t weave_utf8_offset(const Weave *w, size_t n) {
    if (w == NULL) return -1;
    
    size_t count = 0;
    const unsigned char *p = (const unsigned char*)w->data;
    const unsigned char *end = p + w->len;
    
    while (p < end) {
        if (count == n) {
            return (int64_t)(p - (const unsigned char*)w->data);
        }
        
        /* Skip to next codepoint */
        if ((*p & 0xC0) != 0x80) {
            count++;
        }
        
        /* Skip continuation bytes */
        if (*p < 0x80) p++;
        else if ((*p & 0xE0) == 0xC0) p += 2;
        else if ((*p & 0xF0) == 0xE0) p += 3;
        else if ((*p & 0xF8) == 0xF0) p += 4;
        else p++;  /* Invalid, skip one byte */
    }
    
    return -1;  /* n exceeds string length */
}

#endif /* WEAVE_UTF8 */
