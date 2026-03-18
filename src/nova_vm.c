/**
 * @file nova_vm.c
 * @brief Nova Language - Virtual Machine Implementation
 *
 * Register-based bytecode execution engine. Executes NovaProto
 * trees produced by the compiler pipeline.
 *
 * @author Anthony Taliento
 * @date 2026-02-07
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * DEPENDENCIES:
 *   - nova_vm.h (types, API)
 *   - nova_opcode.h (opcode definitions)
 *   - nova_proto.h (NovaProto)
 *   - zorya/nxh.h (string hashing)
 *
 * THREAD SAFETY:
 *   Not thread-safe. Each thread should own a separate NovaVM.
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#include "nova/nova_vm.h"
#include "nova/nova_opcode.h"
#include "nova/nova_meta.h"
#include "nova/nova_proto.h"
#include "nova/nova_conf.h"
#include "nova/nova_trace.h"
#include "nova/nova_error.h"
#include "zorya/nxh.h"
#include "zorya/pcm.h"
#include "zorya/dagger.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <setjmp.h>

/* ============================================================
 * PART 1: INTERNAL CONSTANTS
 * ============================================================ */

/** Initial hash table size for globals */
#define NOVAI_GLOBALS_INIT_SIZE 64

/* NXH64 seed: NOVA_STRING_SEED from nova_vm.h (single source of truth) */

/* ============================================================
 * INTERNAL COMPATIBILITY ALIASES (Phase 10.5)
 *
 * All public API functions have nova_* names (declared in nova_vm.h).
 * These #defines let existing novai_* call sites work during
 * migration. Once all consumers switch to nova_*, remove these.
 * ============================================================ */
#define novai_string_new    nova_string_new
#define novai_string_free   nova_string_free
#define novai_string_equal  nova_string_equal
#define novai_table_new     nova_table_new
#define novai_table_free    nova_table_free
#define novai_table_get_str nova_table_get_str
#define novai_table_set_str nova_table_set_str
#define novai_table_get_int nova_table_get_int
#define novai_table_set_int nova_table_set_int
#define novai_table_grow_array nova_table_grow_array

/** Number of list items per SETLIST flush */
#ifndef NOVA_LFIELDS_PER_FLUSH
#define NOVA_LFIELDS_PER_FLUSH 50
#endif

/*
** PCM: NOVA_GC_BARRIER_INLINE
** Purpose: Inline write barrier that eliminates function-call overhead
** Rationale: nova_gc_barrier() is called 7 times in hot table paths;
**            inlining saves ~5ns per call (function prologue/epilogue)
** Performance Impact: 7 call sites x millions of table ops = measurable
** Audit Date: 2026-02-08
**
** Nova's two-white GC (WHITE0=0, WHITE1=1, GRAY=2, BLACK=3) only
** needs the barrier during MARK phase when parent is BLACK.
** This uses ZORYA_UNLIKELY from PCM since the barrier rarely fires.
*/
#define NOVA_GC_BARRIER_INLINE(vm_ptr, parent_hdr) \
    do { \
        if (ZORYA_UNLIKELY((vm_ptr)->gc_phase == NOVA_GC_PHASE_MARK \
                           && NOVA_GC_IS_BLACK(parent_hdr))) { \
            (parent_hdr)->gc_color = NOVA_GC_GRAY; \
            (parent_hdr)->gc_gray = (vm_ptr)->gray_list; \
            (vm_ptr)->gray_list = (parent_hdr); \
        } \
    } while (0)

/**
 * Extract signed C field (for immediate comparisons).
 * C is 8-bit field, interpret as signed [-128, 127].
 */
#define NOVAI_GET_SC(i) ((int8_t)NOVA_GET_C(i))

/**
 * Extract signed jump offset from sJ format.
 * For JMP: the entire B+C field (17 bits) encodes the offset.
 * We bias by half the range.
 */
#define NOVA_SJ_BIAS 131071
#define NOVAI_GET_SJ(i) \
    ((int32_t)(((NOVA_GET_B(i) << 8) | NOVA_GET_C(i)) - (NOVA_SJ_BIAS >> 1)))

/* ============================================================
 * PART 2: STRING HELPERS
 * ============================================================ */

/**
 * @brief Create a new NovaString from data.
 *
 * Allocates memory for the string object including the
 * flexible array member for inline data storage.
 *
 * @param vm   VM (for tracking allocation)
 * @param s    String data
 * @param len  Length in bytes
 * @return New string, or NULL on allocation failure.
 */
NovaString *nova_string_new(NovaVM *vm, const char *s, size_t len) {
    if (vm == NULL) {
        return NULL;
    }

    /* NULL source with non-zero length is invalid */
    if (s == NULL && len > 0) {
        return NULL;
    }

    /* Treat NULL as empty string */
    if (s == NULL) {
        s = "";
    }

    /* ---- Check intern table first (O(1) DAGGER lookup) ---- */
    DaggerTable *pool = (DaggerTable *)vm->string_pool;
    if (pool != NULL) {
        void *existing = NULL;
        if (dagger_get(pool, s, (uint32_t)len, &existing) == DAGGER_OK) {
            /* String already interned — return canonical pointer */
            return (NovaString *)existing;
        }
    }

    /* ---- Not found: create new interned string ---- */
    size_t alloc_size = sizeof(NovaString) + len + 1;

    NovaString *str = (NovaString *)malloc(alloc_size);
    if (str == NULL) {
        return NULL;
    }

    /* Zero the GC header — interned strings are NOT on all_objects.
     * They are immortal for the VM's lifetime, owned by string_pool. */
    memset(&str->gc, 0, sizeof(NovaGCHeader));
    str->gc.gc_type = NOVA_TYPE_STRING;
    str->gc.gc_color = NOVA_GC_BLACK;  /* Always black — never swept */
    str->gc.gc_size = (uint32_t)alloc_size;

    str->length = len;
    str->hash = nxh64(s, len, NOVA_STRING_SEED);
    if (len > 0) {
        memcpy(str->data, s, len);
    }
    str->data[len] = '\0';

    /* Insert into intern table.  Key = str->data (points into the
     * NovaString's own flexible array), value = the NovaString*.
     * When dagger_destroy runs, the value_destroy callback frees it. */
    if (pool != NULL) {
        dagger_result_t r = dagger_set(pool, str->data, (uint32_t)len,
                                       str, 0);
        if (r != DAGGER_OK && r != DAGGER_EXISTS) {
            /* Insertion failed (memory?) — still return the string,
             * it just won't be interned for dedup. Track as GC object
             * so it can be freed by the collector instead. */
            NOVA_GC_INIT(&str->gc, NOVA_TYPE_STRING,
                         vm->gc_current_white, (uint32_t)alloc_size);
            nova_gc_link(vm, NOVA_GC_HDR(str));
            vm->bytes_allocated += alloc_size;
            return str;
        }
    }

    /* Track interned string memory separately from GC budget */
    vm->string_bytes += alloc_size;
    return str;
}

/**
 * @brief Free a NovaString.
 * @note With string interning active, strings are owned by the
 *       DAGGER string_pool and freed at VM destruction.
 *       DO NOT call this on interned strings — it would corrupt
 *       the intern table. Kept only for API completeness.
 */
__attribute__((unused))
void nova_string_free(NovaVM *vm, NovaString *str) {
    (void)vm;
    (void)str;
    /* With interning, individual string freeing is not supported.
     * All strings are freed when the VM's string_pool is destroyed. */
}

/**
 * @brief Compare two strings for equality.
 *
 * With string interning (Phase 12.3), all strings with identical
 * content share the same canonical pointer.  Equality reduces to
 * a single pointer comparison — O(1) regardless of string length.
 *
 * The fallback path (length/hash/memcmp) handles the rare case
 * where a string couldn't be interned (allocation failure in
 * dagger_set).
 */
int nova_string_equal(const NovaString *a, const NovaString *b) {
    /* Pointer equality — covers all interned strings (fast path) */
    if (a == b) {
        return 1;
    }
    if (a == NULL || b == NULL) {
        return 0;
    }
    /* Fallback for non-interned strings (dagger_set failed) */
    if (a->length != b->length) {
        return 0;
    }
    if (a->hash != b->hash) {
        return 0;
    }
    return memcmp(a->data, b->data, a->length) == 0;
}

/* ============================================================
 * PART 3: TABLE HELPERS
 * ============================================================ */

/**
 * @brief Create a new empty table.
 */
NovaTable *nova_table_new(NovaVM *vm) {
    if (vm == NULL) {
        return NULL;
    }

    /* Trigger GC BEFORE allocation so any white-flip happens
     * before we color the new object with gc_current_white. */
    nova_gc_check(vm, sizeof(NovaTable));

    NovaTable *t = (NovaTable *)malloc(sizeof(NovaTable));
    if (t == NULL) {
        return NULL;
    }

    /* Initialize GC header — gc_current_white is now post-flip */
    NOVA_GC_INIT(&t->gc, NOVA_TYPE_TABLE, vm->gc_current_white,
                 sizeof(NovaTable));
    nova_gc_link(vm, NOVA_GC_HDR(t));

    t->array = NULL;
    t->array_size = 0;
    t->array_used = 0;
    t->hash = NULL;
    t->hash_size = 0;
    t->hash_used = 0;
    t->metatable = NULL;

    vm->bytes_allocated += sizeof(NovaTable);
    return t;
}

/**
 * @brief Free a table and its contents.
 * @note With GC active, sweep handles freeing. Kept for debug/emergency use.
 */
__attribute__((unused))
void nova_table_free(NovaVM *vm, NovaTable *t) {
    if (t == NULL) {
        return;
    }
    if (vm != NULL) {
        vm->bytes_allocated -= sizeof(NovaTable);
        if (t->array != NULL) {
            vm->bytes_allocated -= t->array_size * sizeof(NovaValue);
        }
        if (t->hash != NULL) {
            vm->bytes_allocated -= t->hash_size * sizeof(NovaTableEntry);
        }
    }
    free(t->array);
    free(t->hash);
    free(t);
}

/**
 * @brief Grow the array part of a table.
 */
int nova_table_grow_array(NovaVM *vm, NovaTable *t, uint32_t needed) {
    if (t->array_size >= needed) {
        return 0;
    }

    uint32_t new_size = t->array_size == 0 ? 4 : t->array_size;
    while (new_size < needed) {
        new_size *= 2;
    }

    NovaValue *new_arr = (NovaValue *)realloc(
        t->array, new_size * sizeof(NovaValue));
    if (new_arr == NULL) {
        return -1;
    }

    /* Initialize new slots to nil */
    REG_WINDOW_NIL_FILL(&new_arr[t->array_size],
                        (size_t)(new_size - t->array_size),
                        nova_value_nil());

    if (vm != NULL) {
        vm->bytes_allocated -= t->array_size * sizeof(NovaValue);
        vm->bytes_allocated += new_size * sizeof(NovaValue);
    }

    t->array = new_arr;
    t->array_size = new_size;
    return 0;
}

/**
 * @brief Grow the hash part of a table.
 */
static int novai_table_grow_hash(NovaVM *vm, NovaTable *t) {
    uint32_t old_size = t->hash_size;
    uint32_t new_size = old_size == 0 ? 8 : old_size * 2;

    NovaTableEntry *new_hash = (NovaTableEntry *)malloc(
        new_size * sizeof(NovaTableEntry));
    if (new_hash == NULL) {
        return -1;
    }

    /* Initialize all slots as empty */
    for (uint32_t i = 0; i < new_size; i++) {
        new_hash[i].key = nova_value_nil();
        new_hash[i].value = nova_value_nil();
        new_hash[i].occupied = 0;
    }

    /* Rehash existing entries */
    if (t->hash != NULL) {
        for (uint32_t i = 0; i < old_size; i++) {
            if (t->hash[i].occupied == 0) {
                continue;
            }

            /* Find slot in new table */
            uint64_t h = 0;
            if (nova_is_string(t->hash[i].key)) {
                h = nova_str_hash(nova_as_string(t->hash[i].key));
            } else if (nova_is_integer(t->hash[i].key)) {
                h = (uint64_t)nova_as_integer(t->hash[i].key);
            } else {
                h = (uint64_t)(uintptr_t)&t->hash[i].key;
            }

            uint32_t new_mask = new_size - 1;
            uint32_t idx = HASH_SLOT((uint32_t)h, new_mask);
            while (new_hash[idx].occupied) {
                HASH_PROBE_LINEAR(idx, new_mask);
            }

            new_hash[idx] = t->hash[i];
        }
    }

    if (vm != NULL) {
        vm->bytes_allocated -= old_size * sizeof(NovaTableEntry);
        vm->bytes_allocated += new_size * sizeof(NovaTableEntry);
    }

    free(t->hash);
    t->hash = new_hash;
    t->hash_size = new_size;
    return 0;
}

/**
 * @brief Get a value from a table by string key.
 */
NovaValue nova_table_get_str(NovaTable *t, const NovaString *key) {
    if (t == NULL || key == NULL || t->hash_size == 0) {
        return nova_value_nil();
    }

    uint32_t mask = t->hash_size - 1;
    uint32_t idx = HASH_SLOT((uint32_t)key->hash, mask);
    uint32_t start = idx;

    do {
        if (t->hash[idx].occupied == 0) {
            return nova_value_nil();
        }
        if (nova_is_string(t->hash[idx].key) &&
            novai_string_equal(nova_as_string(t->hash[idx].key), key)) {
            return t->hash[idx].value;
        }
        HASH_PROBE_LINEAR(idx, mask);
        PREFETCH_TABLE_SLOT(t->hash, idx);
    } while (idx != start);

    return nova_value_nil();
}

/**
 * @brief Set a value in a table by string key.
 */
int nova_table_set_str(NovaVM *vm, NovaTable *t,
                       NovaString *key, NovaValue val) {
    if (t == NULL || key == NULL) {
        return -1;
    }

    /* Grow if needed (load factor > 0.75) */
    if (t->hash_size == 0 || t->hash_used * 4 >= t->hash_size * 3) {
        if (novai_table_grow_hash(vm, t) != 0) {
            return -1;
        }
    }

    uint32_t mask = t->hash_size - 1;
    uint32_t idx = HASH_SLOT((uint32_t)key->hash, mask);

    while (t->hash[idx].occupied) {
        if (nova_is_string(t->hash[idx].key) &&
            novai_string_equal(nova_as_string(t->hash[idx].key), key)) {
            /* Update existing */
            t->hash[idx].value = val;
            NOVA_GC_BARRIER_INLINE(vm, NOVA_GC_HDR(t));
            return 0;
        }
        HASH_PROBE_LINEAR(idx, mask);
        PREFETCH_TABLE_SLOT(t->hash, idx);
    }

    /* Insert new */
    t->hash[idx].key = nova_value_string(key);
    t->hash[idx].value = val;
    t->hash[idx].occupied = 1;
    t->hash_used++;
    NOVA_GC_BARRIER_INLINE(vm, NOVA_GC_HDR(t));
    return 0;
}

/**
 * @brief Get a value from a table by integer key.
 */
NovaValue nova_table_get_int(NovaTable *t, nova_int_t key) {
    if (t == NULL) {
        return nova_value_nil();
    }

    /* Array part: 0-based keys (t[0]..t[n-1]) map to array[0..n-1] */
    if (key >= 0 && (uint64_t)key < (uint64_t)t->array_used) {
        return t->array[key];
    }

    /* Fall back to hash part */
    if (t->hash_size == 0) {
        return nova_value_nil();
    }

    uint64_t h = (uint64_t)key;
    uint32_t mask = t->hash_size - 1;
    uint32_t idx = HASH_SLOT((uint32_t)h, mask);
    uint32_t start = idx;

    do {
        if (t->hash[idx].occupied == 0) {
            return nova_value_nil();
        }
        if (nova_is_integer(t->hash[idx].key) &&
            nova_as_integer(t->hash[idx].key) == key) {
            return t->hash[idx].value;
        }
        HASH_PROBE_LINEAR(idx, mask);
        PREFETCH_TABLE_SLOT(t->hash, idx);
    } while (idx != start);

    return nova_value_nil();
}

/**
 * @brief Set a value in a table by integer key.
 */
int nova_table_set_int(NovaVM *vm, NovaTable *t,
                       nova_int_t key, NovaValue val) {
    if (t == NULL) {
        return -1;
    }

    /* Array part: 0-based keys (t[0]..t[n-1]) map to array[0..n-1] */
    if (key >= 0 && key < 1000000) {
        uint32_t idx = (uint32_t)key;           /* direct 0-based index */
        uint32_t needed = (uint32_t)(key + 1);  /* need at least key+1 slots */
        if (needed > t->array_size) {
            if (novai_table_grow_array(vm, t, needed) != 0) {
                return -1;
            }
        }
        t->array[idx] = val;
        if (needed > t->array_used) {
            t->array_used = needed;
        }
        NOVA_GC_BARRIER_INLINE(vm, NOVA_GC_HDR(t));
        return 0;
    }

    /* Use hash part for other keys */
    if (t->hash_size == 0 || t->hash_used * 4 >= t->hash_size * 3) {
        if (novai_table_grow_hash(vm, t) != 0) {
            return -1;
        }
    }

    uint64_t h = (uint64_t)key;
    uint32_t mask = t->hash_size - 1;
    uint32_t idx = HASH_SLOT((uint32_t)h, mask);

    while (t->hash[idx].occupied) {
        if (nova_is_integer(t->hash[idx].key) &&
            nova_as_integer(t->hash[idx].key) == key) {
            t->hash[idx].value = val;
            NOVA_GC_BARRIER_INLINE(vm, NOVA_GC_HDR(t));
            return 0;
        }
        HASH_PROBE_LINEAR(idx, mask);
        PREFETCH_TABLE_SLOT(t->hash, idx);
    }

    t->hash[idx].key = nova_value_integer(key);
    t->hash[idx].value = val;
    t->hash[idx].occupied = 1;
    t->hash_used++;
    NOVA_GC_BARRIER_INLINE(vm, NOVA_GC_HDR(t));
    return 0;
}

/**
 * @brief Iterate to the next key-value pair in a table.
 *
 * Visits array part first (keys 0..array_used-1), then hash part.
 * Start with *iter_idx = 0. Returns 1 if a pair was found,
 * 0 when iteration is complete.
 */
int nova_table_next(NovaTable *t, uint32_t *iter_idx,
                    NovaValue *out_key, NovaValue *out_val) {
    if (t == NULL || iter_idx == NULL) {
        return 0;
    }

    uint32_t idx = *iter_idx;

    /* Phase 1: Array part */
    while (idx < t->array_used) {
        NovaValue v = t->array[idx];
        if (!nova_is_nil(v)) {
            if (out_key != NULL) {
                *out_key = nova_value_integer((nova_int_t)idx);
            }
            if (out_val != NULL) {
                *out_val = v;
            }
            *iter_idx = idx + 1;
            return 1;
        }
        idx++;
    }

    /* Phase 2: Hash part */
    uint32_t hash_offset = t->array_used;
    uint32_t hash_idx = idx - hash_offset;

    while (hash_idx < t->hash_size) {
        if (t->hash[hash_idx].occupied) {
            if (out_key != NULL) {
                *out_key = t->hash[hash_idx].key;
            }
            if (out_val != NULL) {
                *out_val = t->hash[hash_idx].value;
            }
            *iter_idx = hash_offset + hash_idx + 1;
            return 1;
        }
        hash_idx++;
    }

    return 0;
}

/**
 * @brief Lookup a table value by C string key (non-interned).
 *
 * Hashes the raw C string and probes the hash table directly.
 * Does NOT require the key to be an interned NovaString.
 *
 * @param t        Table to search
 * @param key      C string key (NUL-terminated)
 * @param key_len  Byte length of key (excluding NUL)
 * @return Value associated with key, or nil if not found
 */
NovaValue nova_table_get_cstr(NovaTable *t, const char *key,
                              uint32_t key_len) {
    if (t == NULL || key == NULL || t->hash_size == 0) {
        return nova_value_nil();
    }

    uint64_t h = nxh64(key, (size_t)key_len, NOVA_STRING_SEED);
    uint32_t mask = t->hash_size - 1;
    uint32_t idx = HASH_SLOT((uint32_t)h, mask);
    uint32_t start = idx;

    do {
        if (t->hash[idx].occupied == 0) {
            return nova_value_nil();
        }
        if (nova_is_string(t->hash[idx].key)) {
            NovaString *ks = nova_as_string(t->hash[idx].key);
            if (nova_str_hash(ks) == h &&
                nova_str_len(ks) == key_len &&
                memcmp(nova_str_data(ks), key, (size_t)key_len) == 0) {
                return t->hash[idx].value;
            }
        }
        HASH_PROBE_LINEAR(idx, mask);
    } while (idx != start);

    return nova_value_nil();
}

/**
 * @brief Find the iterator cursor position for a given key.
 *
 * Searches both array and hash parts. Returns the iter_idx
 * (compatible with nova_table_next) pointing to the slot AFTER
 * the found key. Returns (uint32_t)-1 if not found.
 */
uint32_t nova_table_find_iter_idx(NovaTable *t, NovaValue key) {
    if (t == NULL) {
        return (uint32_t)-1;
    }

    /* Nil key means "start from beginning" */
    if (nova_is_nil(key)) {
        return 0;
    }

    /* Integer key in array range */
    if (nova_is_integer(key)) {
        nova_int_t idx = nova_as_integer(key);
        if (idx >= 0 && (uint32_t)idx < t->array_used) {
            return (uint32_t)idx + 1;
        }
    }

    /* Search hash part */
    if (t->hash_size > 0) {
        /* String keys: use hash-directed probe */
        if (nova_is_string(key)) {
            NovaString *ks = nova_as_string(key);
            uint64_t h = nova_str_hash(ks);
            uint32_t mask = t->hash_size - 1;
            uint32_t slot = HASH_SLOT((uint32_t)h, mask);
            uint32_t start = slot;
            do {
                if (t->hash[slot].occupied == 0) {
                    break;
                }
                if (nova_is_string(t->hash[slot].key)) {
                    NovaString *sk = nova_as_string(t->hash[slot].key);
                    if (nova_str_hash(sk) == h &&
                        nova_str_len(sk) == nova_str_len(ks) &&
                        memcmp(nova_str_data(sk), nova_str_data(ks),
                               nova_str_len(ks)) == 0) {
                        return t->array_used + slot + 1;
                    }
                }
                HASH_PROBE_LINEAR(slot, mask);
            } while (slot != start);
        }

        /* Non-string keys: linear scan of hash part */
        for (uint32_t j = 0; j < t->hash_size; j++) {
            if (!t->hash[j].occupied) {
                continue;
            }
            if (nova_typeof(t->hash[j].key) == nova_typeof(key)) {
                int match = 0;
                switch (nova_typeof(key)) {
                    case NOVA_TYPE_INTEGER:
                        match = (nova_as_integer(t->hash[j].key) ==
                                 nova_as_integer(key));
                        break;
                    case NOVA_TYPE_NUMBER:
                        match = (nova_as_number(t->hash[j].key) ==
                                 nova_as_number(key));
                        break;
                    case NOVA_TYPE_BOOL:
                        match = (nova_as_bool(t->hash[j].key) ==
                                 nova_as_bool(key));
                        break;
                    default:
                        break;
                }
                if (match) {
                    return t->array_used + j + 1;
                }
            }
        }
    }

    return (uint32_t)-1;
}

/* ============================================================
 * PART 4: UPVALUE HELPERS
 * ============================================================ */

/**
 * @brief Create a new open upvalue pointing to a stack slot.
 */
static NovaUpvalue *novai_upvalue_new(NovaVM *vm, NovaValue *location) {
    /* Trigger GC BEFORE allocation so any white-flip happens
     * before we color the new object with gc_current_white. */
    if (vm != NULL) {
        nova_gc_check(vm, sizeof(NovaUpvalue));
    }

    NovaUpvalue *uv = (NovaUpvalue *)malloc(sizeof(NovaUpvalue));
    if (uv == NULL) {
        return NULL;
    }

    /* Initialize GC header — use internal type tag for upvalues */
    NOVA_GC_INIT(&uv->gc, 10, /* NOVAI_GC_TYPE_UPVALUE */
                 vm != NULL ? vm->gc_current_white : NOVA_GC_WHITE0,
                 sizeof(NovaUpvalue));
    if (vm != NULL) {
        nova_gc_link(vm, NOVA_GC_HDR(uv));
    }

    uv->location = location;
    uv->closed = nova_value_nil();
    uv->next = NULL;

    if (vm != NULL) {
        vm->bytes_allocated += sizeof(NovaUpvalue);
    }
    return uv;
}

/**
 * @brief Free an upvalue.
 * @note With GC active, sweep handles freeing. Kept for debug/emergency use.
 */
__attribute__((unused))
static void novai_upvalue_free(NovaVM *vm, NovaUpvalue *uv) {
    if (uv == NULL) {
        return;
    }
    if (vm != NULL && vm->bytes_allocated >= sizeof(NovaUpvalue)) {
        vm->bytes_allocated -= sizeof(NovaUpvalue);
    }
    free(uv);
}

/**
 * @brief Find or create an upvalue for a stack slot.
 *
 * Maintains the open upvalue chain in sorted order (by stack position).
 */
static NovaUpvalue *novai_find_upvalue(NovaVM *vm, NovaValue *location) {
    NovaUpvalue **pp = &vm->open_upvalues;

    /* Walk the chain looking for existing upvalue */
    while (*pp != NULL && (*pp)->location > location) {
        pp = &(*pp)->next;
    }

    if (*pp != NULL && (*pp)->location == location) {
        return *pp;
    }

    /* Create new upvalue and insert in sorted order */
    NovaUpvalue *uv = novai_upvalue_new(vm, location);
    if (uv == NULL) {
        return NULL;
    }
    uv->next = *pp;
    *pp = uv;
    return uv;
}

/**
 * @brief Close all upvalues at or above the given stack position.
 *
 * Copies the value from the stack to upvalue->closed,
 * then points location at closed.
 */
static void novai_close_upvalues(NovaVM *vm, NovaValue *level) {
    while (vm->open_upvalues != NULL &&
           vm->open_upvalues->location >= level) {
        NovaUpvalue *uv = vm->open_upvalues;
        uv->closed = *uv->location;
        uv->location = &uv->closed;
        vm->open_upvalues = uv->next;
        /* Write barrier: upvalue now owns the closed value */
        NOVA_GC_BARRIER_INLINE(vm, NOVA_GC_HDR(uv));
    }
}

/* ============================================================
 * PART 5: CLOSURE HELPERS
 * ============================================================ */

/**
 * @brief Create a new closure from a prototype.
 */
static NovaClosure *novai_closure_new(NovaVM *vm, const NovaProto *proto) {
    if (proto == NULL) {
        return NULL;
    }

    uint8_t nupvals = proto->upvalue_count;
    size_t size = sizeof(NovaClosure) + (size_t)nupvals * sizeof(NovaUpvalue *);

    /* Trigger GC BEFORE allocation so any white-flip happens
     * before we color the new object with gc_current_white. */
    if (vm != NULL) {
        nova_gc_check(vm, size);
    }

    NovaClosure *cl = (NovaClosure *)malloc(size);
    if (cl == NULL) {
        return NULL;
    }

    /* Initialize GC header — gc_current_white is now post-flip */
    NOVA_GC_INIT(&cl->gc, NOVA_TYPE_FUNCTION,
                 vm != NULL ? vm->gc_current_white : NOVA_GC_WHITE0,
                 (uint32_t)size);
    if (vm != NULL) {
        nova_gc_link(vm, NOVA_GC_HDR(cl));
    }

    cl->proto = proto;
    cl->upvalue_count = nupvals;

    if (nupvals > 0) {
        cl->upvalues = (NovaUpvalue **)(cl + 1);
        for (uint8_t i = 0; i < nupvals; i++) {
            cl->upvalues[i] = NULL;
        }
    } else {
        cl->upvalues = NULL;
    }

    if (vm != NULL) {
        vm->bytes_allocated += size;
    }
    return cl;
}

/**
 * @brief Public wrapper for closure creation.
 *
 * Used by the module loader (nova_lib_package.c) to wrap a
 * compiled proto as a callable closure for nova_vm_call().
 */
NovaClosure *nova_closure_new(NovaVM *vm, const NovaProto *proto) {
    return novai_closure_new(vm, proto);
}

/**
 * @brief Free a closure (does not free upvalues -- they may be shared).
 * @note Currently unused - will be used by GC (Phase 8)
 */
__attribute__((unused))
static void novai_closure_free(NovaVM *vm, NovaClosure *cl) {
    if (cl == NULL) {
        return;
    }
    size_t size = sizeof(NovaClosure) +
                  (size_t)cl->upvalue_count * sizeof(NovaUpvalue *);
    if (vm != NULL && vm->bytes_allocated >= size) {
        vm->bytes_allocated -= size;
    }
    free(cl);
}

/* ============================================================
 * PART 6: STACK HELPERS
 * ============================================================ */

/**
 * @brief Ensure the stack has room for `needed` more slots.
 */
static int novai_stack_ensure(NovaVM *vm, size_t needed) {
    size_t used = (size_t)(vm->stack_top - vm->stack);
    size_t required = used + needed;

    if (required <= vm->stack_size) {
        return 0;
    }

    if (required > NOVA_MAX_STACK_SIZE) {
        vm->status = NOVA_VM_ERR_STACKOVERFLOW;
        return -1;
    }

    size_t new_size = vm->stack_size;
    while (new_size < required) {
        new_size *= 2;
    }
    if (new_size > NOVA_MAX_STACK_SIZE) {
        new_size = NOVA_MAX_STACK_SIZE;
    }

    NovaValue *new_stack = (NovaValue *)realloc(
        vm->stack, new_size * sizeof(NovaValue));
    if (new_stack == NULL) {
        vm->status = NOVA_VM_ERR_MEMORY;
        return -1;
    }

    /* Update pointers if stack moved */
    ptrdiff_t delta = new_stack - vm->stack;
    size_t old_size = vm->stack_size;
    vm->stack = new_stack;
    vm->stack_top = new_stack + used;
    vm->stack_size = new_size;

    /* Update frame base pointers */
    for (int i = 0; i < vm->frame_count; i++) {
        vm->frames[i].base += delta;
    }

    /* Update cfunc_base if it points into the current stack */
    if (vm->cfunc_base != NULL) {
        vm->cfunc_base += delta;
    }

    /* Update open upvalue locations */
    for (NovaUpvalue *uv = vm->open_upvalues; uv != NULL; uv = uv->next) {
        if (uv->location >= new_stack &&
            uv->location < new_stack + used) {
            uv->location += delta;
        }
    }

    vm->bytes_allocated += (new_size - old_size) * sizeof(NovaValue);
    return 0;
}

/* ============================================================
 * PART 7: CONSTANT TO VALUE CONVERSION
 * ============================================================ */

/**
 * @brief Convert a compile-time constant to a runtime value.
 */
static NovaValue novai_const_to_value(NovaVM *vm, const NovaConstant *k) {
    switch (k->tag) {
        case NOVA_CONST_NIL:
            return nova_value_nil();

        case NOVA_CONST_BOOL:
            return nova_value_bool(k->as.boolean);

        case NOVA_CONST_INTEGER:
            return nova_value_integer(k->as.integer);

        case NOVA_CONST_NUMBER:
            return nova_value_number(k->as.number);

        case NOVA_CONST_STRING: {
            /* Create runtime string from constant */
            NovaString *str = novai_string_new(
                vm, k->as.string.data, (size_t)k->as.string.length);
            if (str == NULL) {
                return nova_value_nil();
            }
            return nova_value_string(str);
        }

        default:
            return nova_value_nil();
    }
}

/**
 * @brief Get an RK value (register or constant).
 */
static inline NovaValue novai_rk(NovaVM *vm, NovaValue *base,
                                 const NovaConstant *k, uint8_t rk) {
    if (NOVA_IS_RK_CONST(rk)) {
        return novai_const_to_value(vm, &k[NOVA_RK_TO_CONST(rk)]);
    }
    return base[rk];
}

/* ============================================================
 * PART 8: ERROR HELPERS
 * ============================================================ */

/**
 * @brief Set a runtime error message.
 * Non-static: used by nova_meta.c metamethod pipeline.
 */
void novai_error(NovaVM *vm, int code, const char *msg) {
    vm->status = code;
    /* diag_code is set by the caller (via nova_vm_raise_error_ex or
     * directly) BEFORE calling novai_error.  If not set (0), the CLI
     * falls back to mapping vm->status to a NovaErrorCode.           */
    free(vm->error_msg);
    if (msg != NULL) {
        /* For runtime errors, prepend source:line if we can determine
         * the current position from the topmost frame.  Skip this for
         * memory errors to avoid allocation in low-memory state.      */
        if (code != NOVA_VM_ERR_MEMORY && vm->frame_count > 0) {
            const NovaCallFrame *f = &vm->frames[vm->frame_count - 1];
            const NovaProto *proto = f->proto;
            if (proto != NULL && f->ip != NULL &&
                proto->lines.line_numbers != NULL) {
                uint32_t pc = (uint32_t)(f->ip - proto->code);
                if (pc < proto->lines.count) {
                    uint32_t line = proto->lines.line_numbers[pc];
                    const char *src = proto->source ? proto->source : "?";
                    /* Build "source:line: msg" */
                    size_t slen = strlen(src);
                    size_t mlen = strlen(msg);
                    /* 10 digits for line + 2 colons + space + NUL */
                    size_t total = slen + 12 + mlen + 1;
                    char *annotated = (char *)malloc(total);
                    if (annotated != NULL) {
                        int n = snprintf(annotated, total, "%s:%u: %s",
                                         src, line, msg);
                        (void)n;
                        vm->error_msg = annotated;
                        goto finish;
                    }
                }
            }
        }

        /* Fallback: store plain message */
        {
            size_t len = strlen(msg);
            vm->error_msg = (char *)malloc(len + 1);
            if (vm->error_msg != NULL) {
                memcpy(vm->error_msg, msg, len + 1);
            }
        }
    } else {
        vm->error_msg = NULL;
    }

finish:
    /* If we are inside a protected call (pcall), longjmp back */
    if (vm->error_jmp != NULL) {
        vm->error_jmp->status = code;
        longjmp(vm->error_jmp->buf, 1);
    }
}

/* ============================================================
 * PART 9: ARITHMETIC HELPERS
 * ============================================================ */

/**
 * @brief Convert a value to a number if possible.
 * @return 1 if successful, 0 if not a number
 */
static int novai_to_number(NovaValue v, nova_number_t *out) {
    if (nova_is_number(v)) {
        *out = nova_as_number(v);
        return 1;
    }
    if (nova_is_integer(v)) {
        *out = (nova_number_t)nova_as_integer(v);
        return 1;
    }
    return 0;
}

/* ============================================================
 * PART 11: STRING CONCATENATION
 * ============================================================ */

/**
 * @brief Concatenate values in a register range to a string.
 */
static NovaValue novai_concat(NovaVM *vm, NovaValue *base, int count) {
    if (count <= 0) {
        return nova_value_string(novai_string_new(vm, "", 0));
    }
    if (count == 1) {
        if (nova_is_string(base[0])) {
            return base[0];
        }
    }

    /* Calculate total length */
    size_t total = 0;
    for (int i = 0; i < count; i++) {
        NovaValue v = base[i];
        if (nova_is_string(v)) {
            total += nova_str_len(nova_as_string(v));
        } else if (nova_is_integer(v)) {
            total += 21;  /* max digits for int64 + sign */
        } else if (nova_is_number(v)) {
            total += 32;  /* generous for double */
        } else {
            char errbuf[256];
            snprintf(errbuf, sizeof(errbuf),
                     "attempt to concatenate a %s value",
                     nova_vm_typename(nova_typeof(v)));
            vm->diag_code = NOVA_E2015;
            novai_error(vm, NOVA_VM_ERR_TYPE, errbuf);
            return nova_value_nil();
        }
    }

    char *buf = (char *)malloc(total + 1);
    if (buf == NULL) {
        novai_error(vm, NOVA_VM_ERR_MEMORY, "concat allocation failed");
        return nova_value_nil();
    }

    char *p = buf;
    for (int i = 0; i < count; i++) {
        NovaValue v = base[i];
        if (nova_is_string(v)) {
            memcpy(p, nova_str_data(nova_as_string(v)), nova_str_len(nova_as_string(v)));
            p += nova_str_len(nova_as_string(v));
        } else if (nova_is_integer(v)) {
            int n = sprintf(p, "%lld", (long long)nova_as_integer(v));
            p += n;
        } else if (nova_is_number(v)) {
            int n = sprintf(p, "%.14g", nova_as_number(v));
            p += n;
        }
    }
    *p = '\0';

    size_t len = (size_t)(p - buf);
    NovaString *str = novai_string_new(vm, buf, len);
    free(buf);

    if (str == NULL) {
        novai_error(vm, NOVA_VM_ERR_MEMORY, "concat string creation failed");
        return nova_value_nil();
    }

    return nova_value_string(str);
}

/* ============================================================
 * PART 12: MAIN DISPATCH LOOP
 * ============================================================ */

/*
** The novai_execute function uses GCC's computed goto extension
** for fast opcode dispatch. Suppress pedantic warnings for this
** performance-critical section.
*/
#if NOVA_COMPUTED_GOTO
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

/**
 * @brief Execute a prototype (internal, also called by coroutine runtime).
 */
int novai_execute(NovaVM *vm) {
    if (vm->frame_count == 0) {
        return NOVA_VM_ERR_RUNTIME;
    }

    NovaCallFrame *frame = &vm->frames[vm->frame_count - 1];
    const NovaProto *proto = frame->proto;
    NovaValue *base = frame->base;
    const NovaInstruction *code = proto->code;
    const NovaConstant *K = proto->constants;
    (void)code;  /* Used indirectly via ip */

/*
** Refresh base pointer after any operation that may reallocate the stack
** (metamethod invocations can trigger stack growth via realloc).
*/
#define REFRESH_BASE() (base = frame->base)

    /* Dispatch labels for computed goto */
#if NOVA_COMPUTED_GOTO
/*
** PCM: Computed goto dispatch table + PREFETCH_NEXT_OP
** Purpose: GCC/Clang extension for O(1) opcode dispatch with prefetch
** Rationale: ~30% faster than switch in tight loops (branch predictor)
**            + prefetch hides L1 miss latency on next instruction
** Audit Date: 2026-02-07
*/
#define DISPATCH() do { frame->ip = ip; PREFETCH_NEXT_OP(ip); goto *dispatch_table[NOVA_GET_OPCODE(*ip)]; } while(0)
#define CASE(op) L_##op:
    static const void *dispatch_table[] = {
        [NOVA_OP_MOVE] = &&L_NOVA_OP_MOVE,
        [NOVA_OP_LOADK] = &&L_NOVA_OP_LOADK,
        [NOVA_OP_LOADNIL] = &&L_NOVA_OP_LOADNIL,
        [NOVA_OP_LOADBOOL] = &&L_NOVA_OP_LOADBOOL,
        [NOVA_OP_LOADINT] = &&L_NOVA_OP_LOADINT,
        [NOVA_OP_LOADKX] = &&L_NOVA_OP_LOADKX,
        [NOVA_OP_NEWTABLE] = &&L_NOVA_OP_NEWTABLE,
        [NOVA_OP_GETTABLE] = &&L_NOVA_OP_GETTABLE,
        [NOVA_OP_SETTABLE] = &&L_NOVA_OP_SETTABLE,
        [NOVA_OP_GETFIELD] = &&L_NOVA_OP_GETFIELD,
        [NOVA_OP_SETFIELD] = &&L_NOVA_OP_SETFIELD,
        [NOVA_OP_GETI] = &&L_NOVA_OP_GETI,
        [NOVA_OP_SETI] = &&L_NOVA_OP_SETI,
        [NOVA_OP_SETLIST] = &&L_NOVA_OP_SETLIST,
        [NOVA_OP_SELF] = &&L_NOVA_OP_SELF,
        [NOVA_OP_GETUPVAL] = &&L_NOVA_OP_GETUPVAL,
        [NOVA_OP_SETUPVAL] = &&L_NOVA_OP_SETUPVAL,
        [NOVA_OP_GETGLOBAL] = &&L_NOVA_OP_GETGLOBAL,
        [NOVA_OP_SETGLOBAL] = &&L_NOVA_OP_SETGLOBAL,
        [NOVA_OP_GETTABUP] = &&L_NOVA_OP_GETTABUP,
        [NOVA_OP_SETTABUP] = &&L_NOVA_OP_SETTABUP,
        [NOVA_OP_ADD] = &&L_NOVA_OP_ADD,
        [NOVA_OP_SUB] = &&L_NOVA_OP_SUB,
        [NOVA_OP_MUL] = &&L_NOVA_OP_MUL,
        [NOVA_OP_DIV] = &&L_NOVA_OP_DIV,
        [NOVA_OP_IDIV] = &&L_NOVA_OP_IDIV,
        [NOVA_OP_MOD] = &&L_NOVA_OP_MOD,
        [NOVA_OP_POW] = &&L_NOVA_OP_POW,
        [NOVA_OP_UNM] = &&L_NOVA_OP_UNM,
        [NOVA_OP_ADDI] = &&L_NOVA_OP_ADDI,
        [NOVA_OP_ADDK] = &&L_NOVA_OP_ADDK,
        [NOVA_OP_SUBK] = &&L_NOVA_OP_SUBK,
        [NOVA_OP_MULK] = &&L_NOVA_OP_MULK,
        [NOVA_OP_DIVK] = &&L_NOVA_OP_DIVK,
        [NOVA_OP_MODK] = &&L_NOVA_OP_MODK,
        [NOVA_OP_BAND] = &&L_NOVA_OP_BAND,
        [NOVA_OP_BOR] = &&L_NOVA_OP_BOR,
        [NOVA_OP_BXOR] = &&L_NOVA_OP_BXOR,
        [NOVA_OP_BNOT] = &&L_NOVA_OP_BNOT,
        [NOVA_OP_SHL] = &&L_NOVA_OP_SHL,
        [NOVA_OP_SHR] = &&L_NOVA_OP_SHR,
        [NOVA_OP_CONCAT] = &&L_NOVA_OP_CONCAT,
        [NOVA_OP_STRLEN] = &&L_NOVA_OP_STRLEN,
        [NOVA_OP_EQ] = &&L_NOVA_OP_EQ,
        [NOVA_OP_LT] = &&L_NOVA_OP_LT,
        [NOVA_OP_LE] = &&L_NOVA_OP_LE,
        [NOVA_OP_EQK] = &&L_NOVA_OP_EQK,
        [NOVA_OP_EQI] = &&L_NOVA_OP_EQI,
        [NOVA_OP_LTI] = &&L_NOVA_OP_LTI,
        [NOVA_OP_LEI] = &&L_NOVA_OP_LEI,
        [NOVA_OP_GTI] = &&L_NOVA_OP_GTI,
        [NOVA_OP_GEI] = &&L_NOVA_OP_GEI,
        [NOVA_OP_NOT] = &&L_NOVA_OP_NOT,
        [NOVA_OP_TEST] = &&L_NOVA_OP_TEST,
        [NOVA_OP_TESTSET] = &&L_NOVA_OP_TESTSET,
        [NOVA_OP_JMP] = &&L_NOVA_OP_JMP,
        [NOVA_OP_CALL] = &&L_NOVA_OP_CALL,
        [NOVA_OP_TAILCALL] = &&L_NOVA_OP_TAILCALL,
        [NOVA_OP_RETURN] = &&L_NOVA_OP_RETURN,
        [NOVA_OP_RETURN0] = &&L_NOVA_OP_RETURN0,
        [NOVA_OP_RETURN1] = &&L_NOVA_OP_RETURN1,
        [NOVA_OP_FORPREP] = &&L_NOVA_OP_FORPREP,
        [NOVA_OP_FORLOOP] = &&L_NOVA_OP_FORLOOP,
        [NOVA_OP_TFORCALL] = &&L_NOVA_OP_TFORCALL,
        [NOVA_OP_TFORLOOP] = &&L_NOVA_OP_TFORLOOP,
        [NOVA_OP_CLOSURE] = &&L_NOVA_OP_CLOSURE,
        [NOVA_OP_VARARG] = &&L_NOVA_OP_VARARG,
        [NOVA_OP_VARARGPREP] = &&L_NOVA_OP_VARARGPREP,
        [NOVA_OP_CLOSE] = &&L_NOVA_OP_CLOSE,
        [NOVA_OP_AWAIT] = &&L_NOVA_OP_AWAIT,
        [NOVA_OP_SPAWN] = &&L_NOVA_OP_SPAWN,
        [NOVA_OP_YIELD] = &&L_NOVA_OP_YIELD,
        [NOVA_OP_IMPORT] = &&L_NOVA_OP_IMPORT,
        [NOVA_OP_EXPORT] = &&L_NOVA_OP_EXPORT,
        [NOVA_OP_NOP] = &&L_NOVA_OP_NOP,
        [NOVA_OP_DEBUG] = &&L_NOVA_OP_DEBUG,
        [NOVA_OP_EXTRAARG] = &&L_NOVA_OP_EXTRAARG,
    };
#else
#define DISPATCH() break
#define CASE(op) case op:
#endif

    const NovaInstruction *ip = frame->ip;

#if NOVA_COMPUTED_GOTO
    DISPATCH();
#else
    for (;;) {
        switch (NOVA_GET_OPCODE(*ip)) {
#endif

    /* -------------------------------------------------------
     * LOAD INSTRUCTIONS
     * ------------------------------------------------------- */

    CASE(NOVA_OP_MOVE) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        base[A] = base[B];
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_LOADK) {
        uint8_t A = NOVA_GET_A(*ip);
        uint32_t Bx = NOVA_GET_BX(*ip);
        base[A] = novai_const_to_value(vm, &K[Bx]);
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_LOADNIL) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        REG_WINDOW_NIL_FILL(&base[A], (size_t)(B + 1), nova_value_nil());
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_LOADBOOL) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        base[A] = nova_value_bool(B != 0);
        if (C != 0) {
            ip++;  /* Skip next instruction */
        }
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_LOADINT) {
        uint8_t A = NOVA_GET_A(*ip);
        int32_t sBx = NOVA_GET_SBX(*ip);
        base[A] = nova_value_integer((nova_int_t)sBx);
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_LOADKX) {
        uint8_t A = NOVA_GET_A(*ip);
        ip++;
        uint32_t Ax = NOVA_GET_AX(*ip);
        base[A] = novai_const_to_value(vm, &K[Ax]);
        ip++;
        DISPATCH();
    }

    /* -------------------------------------------------------
     * TABLE INSTRUCTIONS
     * ------------------------------------------------------- */

    CASE(NOVA_OP_NEWTABLE) {
        uint8_t A = NOVA_GET_A(*ip);
        NovaTable *t = novai_table_new(vm);
        if (t == NULL) {
            novai_error(vm, NOVA_VM_ERR_MEMORY, "table allocation failed");
            return vm->status;
        }
        base[A] = nova_value_table(t);
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_GETTABLE) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        NovaValue t = base[B];
        NovaValue key = novai_rk(vm, base, K, C);

        NovaValue meta_result;
        if (nova_meta_index(vm, t, key, &meta_result) != 0) {
            return vm->status;
        }
        REFRESH_BASE();
        base[A] = meta_result;
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_SETTABLE) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        NovaValue t = base[A];
        NovaValue key = novai_rk(vm, base, K, B);
        NovaValue val = novai_rk(vm, base, K, C);

        if (nova_meta_newindex(vm, t, key, val) != 0) {
            return vm->status;
        }
        REFRESH_BASE();
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_GETFIELD) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        NovaValue t = base[B];
        NovaValue kval = novai_const_to_value(vm, &K[C]);

        NovaValue meta_result;
        if (nova_meta_index(vm, t, kval, &meta_result) != 0) {
            return vm->status;
        }
        REFRESH_BASE();
        base[A] = meta_result;
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_SETFIELD) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        NovaValue t = base[A];
        NovaValue kval = novai_const_to_value(vm, &K[B]);
        NovaValue val = novai_rk(vm, base, K, C);

        if (nova_meta_newindex(vm, t, kval, val) != 0) {
            return vm->status;
        }
        REFRESH_BASE();
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_GETI) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        NovaValue t = base[B];

        NovaValue meta_result;
        if (nova_meta_index(vm, t, nova_value_integer((nova_int_t)C),
                            &meta_result) != 0) {
            return vm->status;
        }
        REFRESH_BASE();
        base[A] = meta_result;
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_SETI) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        NovaValue t = base[A];
        NovaValue val = novai_rk(vm, base, K, C);

        if (nova_meta_newindex(vm, t, nova_value_integer((nova_int_t)B),
                               val) != 0) {
            return vm->status;
        }
        REFRESH_BASE();
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_SETLIST) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        NovaValue t = base[A];

        if (!nova_is_table(t)) {
            novai_error(vm, NOVA_VM_ERR_TYPE, "SETLIST on non-table");
            return vm->status;
        }

        /* B=0 means use top, otherwise B values */
        int n = (B == 0) ? (int)(vm->stack_top - &base[A + 1]) : B;
        int offset = (C - 1) * NOVA_LFIELDS_PER_FLUSH;

        for (int i = 0; i < n; i++) {
            novai_table_set_int(vm, nova_as_table(t), (nova_int_t)(offset + i), base[A + 1 + i]);
        }
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_SELF) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        NovaValue t = base[B];
        NovaValue key = novai_rk(vm, base, K, C);

        base[A + 1] = t;  /* Save object */
        NovaValue meta_result;
        if (nova_meta_index(vm, t, key, &meta_result) != 0) {
            return vm->status;
        }
        REFRESH_BASE();
        base[A] = meta_result;
        ip++;
        DISPATCH();
    }

    /* -------------------------------------------------------
     * UPVALUE INSTRUCTIONS
     * ------------------------------------------------------- */

    CASE(NOVA_OP_GETUPVAL) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        NovaClosure *cl = frame->closure;
        if (cl != NULL && B < cl->upvalue_count) {
            base[A] = *cl->upvalues[B]->location;
        } else {
            base[A] = nova_value_nil();
        }
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_SETUPVAL) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        NovaClosure *cl = frame->closure;
        if (cl != NULL && B < cl->upvalue_count) {
            *cl->upvalues[B]->location = base[A];
            /* Write barrier: if upvalue is closed, the upvalue object
             * holds the value directly — barrier needed for GC */
            if (cl->upvalues[B]->location == &cl->upvalues[B]->closed) {
                NOVA_GC_BARRIER_INLINE(vm, NOVA_GC_HDR(cl->upvalues[B]));
            }
        }
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_GETGLOBAL) {
        uint8_t A = NOVA_GET_A(*ip);
        uint32_t Bx = NOVA_GET_BX(*ip);
        NovaValue kval = novai_const_to_value(vm, &K[Bx]);
        if (!nova_is_string(kval)) {
            novai_error(vm, NOVA_VM_ERR_TYPE, "GETGLOBAL expects string");
            return vm->status;
        }
        base[A] = novai_table_get_str(vm->globals, nova_as_string(kval));
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_SETGLOBAL) {
        uint8_t A = NOVA_GET_A(*ip);
        uint32_t Bx = NOVA_GET_BX(*ip);
        NovaValue kval = novai_const_to_value(vm, &K[Bx]);
        if (!nova_is_string(kval)) {
            novai_error(vm, NOVA_VM_ERR_TYPE, "SETGLOBAL expects string");
            return vm->status;
        }
        novai_table_set_str(vm, vm->globals, nova_as_string(kval), base[A]);
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_GETTABUP) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        NovaClosure *cl = frame->closure;
        NovaValue t = nova_value_nil();

        if (cl != NULL && B < cl->upvalue_count) {
            t = *cl->upvalues[B]->location;
        }
        if (!nova_is_table(t)) {
            novai_error(vm, NOVA_VM_ERR_TYPE, "GETTABUP on non-table upvalue");
            return vm->status;
        }

        NovaValue key = novai_rk(vm, base, K, C);
        if (nova_is_string(key)) {
            base[A] = novai_table_get_str(nova_as_table(t), nova_as_string(key));
        } else if (nova_is_integer(key)) {
            base[A] = novai_table_get_int(nova_as_table(t), nova_as_integer(key));
        } else {
            base[A] = nova_value_nil();
        }
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_SETTABUP) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        NovaClosure *cl = frame->closure;
        NovaValue t = nova_value_nil();

        if (cl != NULL && A < cl->upvalue_count) {
            t = *cl->upvalues[A]->location;
        }
        if (!nova_is_table(t)) {
            novai_error(vm, NOVA_VM_ERR_TYPE, "SETTABUP on non-table upvalue");
            return vm->status;
        }

        NovaValue key = novai_rk(vm, base, K, B);
        NovaValue val = novai_rk(vm, base, K, C);
        if (nova_is_string(key)) {
            novai_table_set_str(vm, nova_as_table(t), nova_as_string(key), val);
        } else if (nova_is_integer(key)) {
            novai_table_set_int(vm, nova_as_table(t), nova_as_integer(key), val);
        }
        ip++;
        DISPATCH();
    }

    /* -------------------------------------------------------
     * ARITHMETIC INSTRUCTIONS
     * ------------------------------------------------------- */

    CASE(NOVA_OP_ADD) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        NovaValue meta_result;
        if (nova_meta_arith(vm, NOVA_OP_ADD,
                            novai_rk(vm, base, K, B),
                            novai_rk(vm, base, K, C),
                            &meta_result) != 0) return vm->status;
        REFRESH_BASE();
        base[A] = meta_result;
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_SUB) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        NovaValue meta_result;
        if (nova_meta_arith(vm, NOVA_OP_SUB,
                            novai_rk(vm, base, K, B),
                            novai_rk(vm, base, K, C),
                            &meta_result) != 0) return vm->status;
        REFRESH_BASE();
        base[A] = meta_result;
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_MUL) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        NovaValue meta_result;
        if (nova_meta_arith(vm, NOVA_OP_MUL,
                            novai_rk(vm, base, K, B),
                            novai_rk(vm, base, K, C),
                            &meta_result) != 0) return vm->status;
        REFRESH_BASE();
        base[A] = meta_result;
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_DIV) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        NovaValue meta_result;
        if (nova_meta_arith(vm, NOVA_OP_DIV,
                            novai_rk(vm, base, K, B),
                            novai_rk(vm, base, K, C),
                            &meta_result) != 0) return vm->status;
        REFRESH_BASE();
        base[A] = meta_result;
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_IDIV) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        NovaValue meta_result;
        if (nova_meta_arith(vm, NOVA_OP_IDIV,
                            novai_rk(vm, base, K, B),
                            novai_rk(vm, base, K, C),
                            &meta_result) != 0) return vm->status;
        REFRESH_BASE();
        base[A] = meta_result;
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_MOD) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        NovaValue meta_result;
        if (nova_meta_arith(vm, NOVA_OP_MOD,
                            novai_rk(vm, base, K, B),
                            novai_rk(vm, base, K, C),
                            &meta_result) != 0) return vm->status;
        REFRESH_BASE();
        base[A] = meta_result;
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_POW) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        NovaValue meta_result;
        if (nova_meta_arith(vm, NOVA_OP_POW,
                            novai_rk(vm, base, K, B),
                            novai_rk(vm, base, K, C),
                            &meta_result) != 0) return vm->status;
        REFRESH_BASE();
        base[A] = meta_result;
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_UNM) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        NovaValue meta_result;
        if (nova_meta_unary(vm, NOVA_TM_UNM, base[B], &meta_result) != 0) {
            return vm->status;
        }
        REFRESH_BASE();
        base[A] = meta_result;
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_ADDI) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        int8_t sC = (int8_t)NOVAI_GET_SC(*ip);
        NovaValue meta_result;
        if (nova_meta_arith(vm, NOVA_OP_ADD, base[B],
                            nova_value_integer((nova_int_t)sC),
                            &meta_result) != 0) return vm->status;
        REFRESH_BASE();
        base[A] = meta_result;
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_ADDK) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        NovaValue meta_result;
        if (nova_meta_arith(vm, NOVA_OP_ADD, base[B],
                            novai_const_to_value(vm, &K[C]),
                            &meta_result) != 0) return vm->status;
        REFRESH_BASE();
        base[A] = meta_result;
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_SUBK) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        NovaValue meta_result;
        if (nova_meta_arith(vm, NOVA_OP_SUB, base[B],
                            novai_const_to_value(vm, &K[C]),
                            &meta_result) != 0) return vm->status;
        REFRESH_BASE();
        base[A] = meta_result;
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_MULK) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        NovaValue meta_result;
        if (nova_meta_arith(vm, NOVA_OP_MUL, base[B],
                            novai_const_to_value(vm, &K[C]),
                            &meta_result) != 0) return vm->status;
        REFRESH_BASE();
        base[A] = meta_result;
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_DIVK) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        NovaValue meta_result;
        if (nova_meta_arith(vm, NOVA_OP_DIV, base[B],
                            novai_const_to_value(vm, &K[C]),
                            &meta_result) != 0) return vm->status;
        REFRESH_BASE();
        base[A] = meta_result;
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_MODK) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        NovaValue meta_result;
        if (nova_meta_arith(vm, NOVA_OP_MOD, base[B],
                            novai_const_to_value(vm, &K[C]),
                            &meta_result) != 0) return vm->status;
        REFRESH_BASE();
        base[A] = meta_result;
        ip++;
        DISPATCH();
    }

    /* -------------------------------------------------------
     * BITWISE INSTRUCTIONS
     * ------------------------------------------------------- */

    CASE(NOVA_OP_BAND) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        NovaValue meta_result;
        if (nova_meta_arith(vm, NOVA_OP_BAND,
                            novai_rk(vm, base, K, B),
                            novai_rk(vm, base, K, C),
                            &meta_result) != 0) return vm->status;
        REFRESH_BASE();
        base[A] = meta_result;
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_BOR) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        NovaValue meta_result;
        if (nova_meta_arith(vm, NOVA_OP_BOR,
                            novai_rk(vm, base, K, B),
                            novai_rk(vm, base, K, C),
                            &meta_result) != 0) return vm->status;
        REFRESH_BASE();
        base[A] = meta_result;
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_BXOR) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        NovaValue meta_result;
        if (nova_meta_arith(vm, NOVA_OP_BXOR,
                            novai_rk(vm, base, K, B),
                            novai_rk(vm, base, K, C),
                            &meta_result) != 0) return vm->status;
        REFRESH_BASE();
        base[A] = meta_result;
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_BNOT) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        NovaValue meta_result;
        if (nova_meta_unary(vm, NOVA_TM_BNOT, base[B], &meta_result) != 0) {
            return vm->status;
        }
        REFRESH_BASE();
        base[A] = meta_result;
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_SHL) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        NovaValue meta_result;
        if (nova_meta_arith(vm, NOVA_OP_SHL,
                            novai_rk(vm, base, K, B),
                            novai_rk(vm, base, K, C),
                            &meta_result) != 0) return vm->status;
        REFRESH_BASE();
        base[A] = meta_result;
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_SHR) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        NovaValue meta_result;
        if (nova_meta_arith(vm, NOVA_OP_SHR,
                            novai_rk(vm, base, K, B),
                            novai_rk(vm, base, K, C),
                            &meta_result) != 0) return vm->status;
        REFRESH_BASE();
        base[A] = meta_result;
        ip++;
        DISPATCH();
    }

    /* -------------------------------------------------------
     * STRING INSTRUCTIONS
     * ------------------------------------------------------- */

    CASE(NOVA_OP_CONCAT) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        /* Binary concat: base[A] = base[B] .. base[C] */
        NovaValue vals[2];
        vals[0] = base[B];
        vals[1] = base[C];
        base[A] = novai_concat(vm, vals, 2);
        if (vm->status != NOVA_VM_OK) return vm->status;
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_STRLEN) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        NovaValue meta_result;
        if (nova_meta_unary(vm, NOVA_TM_LEN, base[B], &meta_result) != 0) {
            return vm->status;
        }
        REFRESH_BASE();
        base[A] = meta_result;
        ip++;
        DISPATCH();
    }

    /* -------------------------------------------------------
     * COMPARISON INSTRUCTIONS
     * ------------------------------------------------------- */

    CASE(NOVA_OP_EQ) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        int eq = 0;
        if (nova_meta_compare(vm, NOVA_TM_EQ,
                              novai_rk(vm, base, K, B),
                              novai_rk(vm, base, K, C), &eq) != 0) {
            return vm->status;
        }
        REFRESH_BASE();
        if (eq != A) {
            ip++;  /* Skip next JMP */
        }
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_LT) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        int lt = 0;
        if (nova_meta_compare(vm, NOVA_TM_LT,
                              novai_rk(vm, base, K, B),
                              novai_rk(vm, base, K, C), &lt) != 0) {
            return vm->status;
        }
        REFRESH_BASE();
        if (lt != A) {
            ip++;
        }
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_LE) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        int le = 0;
        if (nova_meta_compare(vm, NOVA_TM_LE,
                              novai_rk(vm, base, K, B),
                              novai_rk(vm, base, K, C), &le) != 0) {
            return vm->status;
        }
        REFRESH_BASE();
        if (le != A) {
            ip++;
        }
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_EQK) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        int eq = 0;
        if (nova_meta_compare(vm, NOVA_TM_EQ, base[B],
                              novai_const_to_value(vm, &K[C]), &eq) != 0) {
            return vm->status;
        }
        REFRESH_BASE();
        if (eq != A) {
            ip++;
        }
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_EQI) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        int8_t sC = (int8_t)NOVAI_GET_SC(*ip);
        int eq = 0;
        if (nova_meta_compare(vm, NOVA_TM_EQ, base[B],
                              nova_value_integer((nova_int_t)sC), &eq) != 0) {
            return vm->status;
        }
        REFRESH_BASE();
        if (eq != A) {
            ip++;
        }
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_LTI) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        int8_t sC = (int8_t)NOVAI_GET_SC(*ip);
        int lt = 0;
        if (nova_meta_compare(vm, NOVA_TM_LT, base[B],
                              nova_value_integer((nova_int_t)sC), &lt) != 0) {
            return vm->status;
        }
        REFRESH_BASE();
        if (lt != A) {
            ip++;
        }
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_LEI) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        int8_t sC = (int8_t)NOVAI_GET_SC(*ip);
        int le = 0;
        if (nova_meta_compare(vm, NOVA_TM_LE, base[B],
                              nova_value_integer((nova_int_t)sC), &le) != 0) {
            return vm->status;
        }
        REFRESH_BASE();
        if (le != A) {
            ip++;
        }
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_GTI) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        int8_t sC = (int8_t)NOVAI_GET_SC(*ip);
        /* R(B) > sC  means  sC < R(B) */
        int gt = 0;
        if (nova_meta_compare(vm, NOVA_TM_LT,
                              nova_value_integer((nova_int_t)sC),
                              base[B], &gt) != 0) {
            return vm->status;
        }
        REFRESH_BASE();
        if (gt != A) {
            ip++;
        }
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_GEI) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        int8_t sC = (int8_t)NOVAI_GET_SC(*ip);
        /* R(B) >= sC  means  sC <= R(B) */
        int ge = 0;
        if (nova_meta_compare(vm, NOVA_TM_LE,
                              nova_value_integer((nova_int_t)sC),
                              base[B], &ge) != 0) {
            return vm->status;
        }
        REFRESH_BASE();
        if (ge != A) {
            ip++;
        }
        ip++;
        DISPATCH();
    }

    /* -------------------------------------------------------
     * LOGICAL INSTRUCTIONS
     * ------------------------------------------------------- */

    CASE(NOVA_OP_NOT) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        base[A] = nova_value_bool(!nova_value_is_truthy(base[B]));
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_TEST) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        int truthy = nova_value_is_truthy(base[A]);
        if (truthy != C) {
            ip++;  /* Skip next JMP */
        }
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_TESTSET) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);
        int truthy = nova_value_is_truthy(base[B]);
        if (truthy == C) {
            base[A] = base[B];
        } else {
            ip++;  /* Skip next JMP */
        }
        ip++;
        DISPATCH();
    }

    /* -------------------------------------------------------
     * CONTROL FLOW INSTRUCTIONS
     * ------------------------------------------------------- */

    CASE(NOVA_OP_JMP) {
        int32_t sJ = (int32_t)NOVA_GET_SBX(*ip);
        ip += 1 + sJ;
        DISPATCH();
    }

    CASE(NOVA_OP_CALL) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        uint8_t C = NOVA_GET_C(*ip);

        NovaValue func = base[A];
        int nargs = (B == 0) ? (int)(vm->stack_top - &base[A + 1]) : B - 1;
        int nresults = C - 1;

        NTRACE(CALL, "CALL A=%d nargs=%d nresults=%d type=%d",
               A, nargs, nresults, nova_typeof(func));
        NTRACE_STACK(vm, "before CALL");

        /* __call metamethod: if not directly callable, resolve __call.
         * This shifts the stack: __call becomes the callee and the
         * original object becomes the first argument.  */
        if (!nova_is_cfunction(func) &&
            !nova_is_function(func)) {
            NovaValue callable;
            if (nova_meta_call(vm, func, &callable) != 0) {
                return vm->status;
            }
            /* Shift args right to insert original obj as arg 1 */
            if (novai_stack_ensure(vm, 1) != 0) {
                return vm->status;
            }
            REFRESH_BASE();
            for (int i = nargs; i >= 0; i--) {
                base[A + 1 + i + 1] = base[A + 1 + i];
            }
            base[A] = callable;    /* __call handler becomes callee */
            base[A + 1] = func;   /* original object is first arg  */
            nargs++;
            func = callable;
            if (B != 0) {
                vm->stack_top = &base[A + 1 + nargs];
            } else {
                vm->stack_top = vm->stack_top + 1;
            }
        }

        if (nova_is_cfunction(func)) {
            /* C function call - args already on stack */
            NovaValue *save_cfunc_base = vm->cfunc_base;
            vm->cfunc_base = &base[A + 1];
            vm->stack_top = &base[A + 1 + nargs];

            /* Save current IP so debug.traceback can read the calling
             * frame's line.  Minimal overhead: one pointer store.     */
            frame->ip = ip;

            /* Save base offset before cfunc (stack may realloc) */
            ptrdiff_t base_off = base - vm->stack;

            int got = nova_as_cfunction(func)(vm);
            if (got < 0 || vm->status != NOVA_VM_OK) {
                vm->cfunc_base = save_cfunc_base;
                if (vm->status == NOVA_VM_YIELD &&
                    vm->running_coroutine != NULL) {
                    /* C function yielded (e.g. coroutine.yield()).
                     * Advance IP past CALL for resume continuation.
                     * Move yield values from cfunc area (base[A+1..])
                     * down to base[A..] so resume can find them via
                     * stack_top - nresults. */
                    int ny = vm->running_coroutine->nresults;
                    base = vm->stack + base_off;
                    frame->base = base;
                    for (int yi = 0; yi < ny; yi++) {
                        base[A + yi] = base[A + 1 + yi];
                    }
                    vm->stack_top = &base[A + ny];
                    frame->ip = ip + 1;
                }
                return vm->status;
            }

            /* Re-derive base after cfunc (stack may have moved) */
            base = vm->stack + base_off;
            frame->base = base;

            /* Copy results from push position to base[A] */
            NovaValue *results_start = &base[A + 1 + nargs];
            int copy_count = got;
            if (nresults >= 0 && copy_count > nresults) {
                copy_count = nresults;
            }
            for (int i = 0; i < copy_count; i++) {
                base[A + i] = results_start[i];
            }
            /* Adjust results and restore register window.
             * For fixed results, nil-pad and ensure stack_top
             * covers the full caller register window for GC.
             * For variable results (nresults < 0, from C=0),
             * set stack_top to exactly result_end so the next
             * instruction with B=0 can compute nargs correctly. */
            if (nresults >= 0) {
                for (int i = got; i < nresults; i++) {
                    base[A + i] = nova_value_nil();
                }
                NovaValue *frame_end = base + proto->max_stack;
                NovaValue *result_end = &base[A + nresults];
                vm->stack_top = (result_end > frame_end)
                                ? result_end : frame_end;
            } else {
                vm->stack_top = &base[A + got];
            }
            vm->cfunc_base = save_cfunc_base;
            ip++;
            DISPATCH();
        }

        if (!nova_is_function(func)) {
            /* __call already tried above; this shouldn't happen */
            char errbuf[256];
            snprintf(errbuf, sizeof(errbuf),
                     "attempt to call a %s value",
                     nova_vm_typename(nova_typeof(func)));
            vm->diag_code = NOVA_E2008;
            novai_error(vm, NOVA_VM_ERR_TYPE, errbuf);
            return vm->status;
        }

        NovaClosure *cl = nova_as_closure(func);
        const NovaProto *callee = cl->proto;

        /* ---- ASYNC FUNCTION INTERCEPTION ----
         * If the callee is declared 'async', do NOT execute it.
         * Instead, create a coroutine wrapping the closure and
         * stash the arguments for later use when the task is
         * awaited or spawned. */
        if (callee->is_async) {
            NovaCoroutine *co = nova_coroutine_new(vm, cl);
            if (co == NULL) {
                novai_error(vm, NOVA_VM_ERR_MEMORY,
                            "failed to create async task");
                return vm->status;
            }

            /* Stash arguments for when the task is first resumed */
            if (nargs > 0) {
                co->pending_args = (NovaValue *)malloc(
                    (size_t)nargs * sizeof(NovaValue));
                if (co->pending_args == NULL) {
                    novai_error(vm, NOVA_VM_ERR_MEMORY,
                                "async argument allocation failed");
                    return vm->status;
                }
                for (int ai = 0; ai < nargs; ai++) {
                    co->pending_args[ai] = base[A + 1 + ai];
                }
                co->pending_nargs = nargs;
            }

            /* Write barrier: co references values that may be GC'd */
            nova_gc_barrier(vm, &co->gc);

            /* Store the coroutine (task) as the call result */
            base[A] = nova_value_coroutine(co);
            if (nresults >= 0) {
                for (int ri = 1; ri < nresults; ri++) {
                    base[A + ri] = nova_value_nil();
                }
            }
            vm->stack_top = base + proto->max_stack;
            ip++;
            DISPATCH();
        }

        /* Stack overflow check */
        if (vm->frame_count >= NOVA_MAX_CALL_DEPTH) {
            novai_error(vm, NOVA_VM_ERR_STACKOVERFLOW, "call stack overflow");
            return vm->status;
        }

        /* Ensure stack space */
        if (novai_stack_ensure(vm, callee->max_stack + 10) != 0) {
            return vm->status;
        }
        REFRESH_BASE();  /* stack may have been reallocated */

        /* Save current frame state */
        frame->ip = ip + 1;

        /* Set up new frame */
        NovaCallFrame *new_frame = &vm->frames[vm->frame_count++];
        new_frame->proto = callee;
        new_frame->closure = cl;
        new_frame->base = &base[A + 1];
        new_frame->ip = callee->code;
        new_frame->num_results = nresults;
        new_frame->num_args = nargs;
        new_frame->varargs = NULL;
        new_frame->num_varargs = 0;

        /* Update locals */
        frame = new_frame;
        proto = callee;
        base = new_frame->base;
        code = callee->code;
        K = callee->constants;
        ip = new_frame->ip;

        /* Nil-fill uninitialized registers so GC sees no stale ptrs */
        if (callee->max_stack > nargs) {
            REG_WINDOW_NIL_FILL(&base[nargs],
                                (size_t)(callee->max_stack - nargs),
                                nova_value_nil());
        }
        /* Extend stack_top so GC scans callee's full register window */
        vm->stack_top = base + callee->max_stack;

        NTRACE(CALL, "ENTER <%s> nargs=%d max_stack=%d frame=%d",
               callee->source ? callee->source : "?",
               nargs, callee->max_stack, vm->frame_count);
        NTRACE_ENTER(callee->source ? callee->source : "(anonymous)");

        DISPATCH();
    }

    CASE(NOVA_OP_TAILCALL) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);

        NovaValue func = base[A];
        int nargs = (B == 0) ? (int)(vm->stack_top - &base[A + 1]) : B - 1;

        /* __call metamethod for tailcall */
        if (!nova_is_cfunction(func) &&
            !nova_is_function(func)) {
            NovaValue callable;
            if (nova_meta_call(vm, func, &callable) != 0) {
                return vm->status;
            }
            if (novai_stack_ensure(vm, 1) != 0) {
                return vm->status;
            }
            REFRESH_BASE();
            for (int i = nargs; i >= 0; i--) {
                base[A + 1 + i + 1] = base[A + 1 + i];
            }
            base[A] = callable;
            base[A + 1] = func;
            nargs++;
            func = callable;
            if (B != 0) {
                vm->stack_top = &base[A + 1 + nargs];
            } else {
                vm->stack_top = vm->stack_top + 1;
            }
        }

        if (!nova_is_function(func)) {
            char errbuf[256];
            snprintf(errbuf, sizeof(errbuf),
                     "attempt to tailcall a %s value",
                     nova_vm_typename(nova_typeof(func)));
            vm->diag_code = NOVA_E2008;
            novai_error(vm, NOVA_VM_ERR_TYPE, errbuf);
            return vm->status;
        }

        NovaClosure *cl = nova_as_closure(func);
        const NovaProto *callee = cl->proto;

        /* Close upvalues */
        novai_close_upvalues(vm, frame->base);

        /* Move args to current base */
        NovaValue *old_base = frame->base;
        for (int i = 0; i <= nargs; i++) {
            old_base[i] = base[A + i];
        }

        /* Reuse current frame */
        frame->proto = callee;
        frame->closure = cl;
        frame->ip = callee->code;

        proto = callee;
        base = old_base;
        code = callee->code;
        K = callee->constants;
        ip = callee->code;

        /* Nil-fill uninitialized registers so GC sees no stale ptrs.
         * Slots 0..nargs hold func + args from the move above. */
        if (callee->max_stack > nargs + 1) {
            REG_WINDOW_NIL_FILL(&base[nargs + 1],
                                (size_t)(callee->max_stack - nargs - 1),
                                nova_value_nil());
        }
        /* Extend stack_top so GC scans callee's register window */
        vm->stack_top = base + callee->max_stack;

        DISPATCH();
    }

    CASE(NOVA_OP_RETURN) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        int nresults = (B == 0) ? (int)(vm->stack_top - &base[A]) : B - 1;

        NTRACE(CALL, "RETURN nresults=%d frame=%d", nresults,
               vm->frame_count);
        NTRACE_LEAVE();

        /* Close upvalues */
        novai_close_upvalues(vm, frame->base);

        /* Free vararg buffer */
        if (frame->varargs != NULL) {
            free(frame->varargs);
            frame->varargs = NULL;
            frame->num_varargs = 0;
        }

        /* Copy results */
        NovaValue *dst = frame->base - 1;
        for (int i = 0; i < nresults; i++) {
            dst[i] = base[A + i];
        }

        vm->frame_count--;
        if (vm->frame_count == 0 ||
            (vm->meta_stop_frame >= 0 &&
             vm->frame_count <= vm->meta_stop_frame)) {
            /* Top-level return or metamethod frame boundary */
            vm->stack_top = dst + nresults;
            return NOVA_VM_OK;
        }

        /* Pop frame */
        frame = &vm->frames[vm->frame_count - 1];
        proto = frame->proto;
        base = frame->base;
        code = proto->code;
        K = proto->constants;
        ip = frame->ip;

        /* Adjust based on expected results */
        int expected = frame->num_results;
        if (expected >= 0) {
            for (int i = nresults; i < expected; i++) {
                dst[i] = nova_value_nil();
            }
        }

        /* Restore stack_top to caller's register window.
         * For fixed results, ensure GC sees the full window.
         * For variable results (expected < 0, from C=0),
         * set stack_top precisely so the next B=0 instruction
         * reads the correct argument count. */
        if (expected >= 0) {
            NovaValue *frame_end = base + proto->max_stack;
            NovaValue *result_end = dst + expected;
            vm->stack_top = (result_end > frame_end)
                            ? result_end : frame_end;
        } else {
            vm->stack_top = dst + nresults;
        }

        DISPATCH();
    }

    CASE(NOVA_OP_RETURN0) {
        NTRACE(CALL, "RETURN0 frame=%d", vm->frame_count);
        NTRACE_LEAVE();
        novai_close_upvalues(vm, frame->base);
        /* Free vararg buffer */
        if (frame->varargs != NULL) {
            free(frame->varargs);
            frame->varargs = NULL;
            frame->num_varargs = 0;
        }
        vm->frame_count--;
        if (vm->frame_count == 0 ||
            (vm->meta_stop_frame >= 0 &&
             vm->frame_count <= vm->meta_stop_frame)) {
            vm->stack_top = frame->base - 1;  /* 0 results */
            return NOVA_VM_OK;
        }
        frame = &vm->frames[vm->frame_count - 1];
        proto = frame->proto;
        base = frame->base;
        code = proto->code;
        K = proto->constants;
        ip = frame->ip;
        vm->stack_top = base + proto->max_stack;
        DISPATCH();
    }

    CASE(NOVA_OP_RETURN1) {
        uint8_t A = NOVA_GET_A(*ip);
        NovaValue result = base[A];

        NTRACE(CALL, "RETURN1 A=%d frame=%d", A, vm->frame_count);
        NTRACE_VALUE("return-val", result);
        NTRACE_LEAVE();

        novai_close_upvalues(vm, frame->base);

        /* Free vararg buffer */
        if (frame->varargs != NULL) {
            free(frame->varargs);
            frame->varargs = NULL;
            frame->num_varargs = 0;
        }

        NovaValue *dst = frame->base - 1;
        dst[0] = result;

        vm->frame_count--;
        if (vm->frame_count == 0 ||
            (vm->meta_stop_frame >= 0 &&
             vm->frame_count <= vm->meta_stop_frame)) {
            vm->stack_top = dst + 1;
            return NOVA_VM_OK;
        }

        frame = &vm->frames[vm->frame_count - 1];
        proto = frame->proto;
        base = frame->base;
        code = proto->code;
        K = proto->constants;
        ip = frame->ip;

        /* Restore caller's register window.
         * For variable results (expected < 0), set stack_top
         * precisely so the next B=0 instruction reads the
         * correct argument count. */
        {
            int expected = frame->num_results;
            if (expected >= 0) {
                NovaValue *frame_end = base + proto->max_stack;
                NovaValue *result_end = dst + 1;
                vm->stack_top = (result_end > frame_end)
                                ? result_end : frame_end;
            } else {
                vm->stack_top = dst + 1;
            }
        }
        DISPATCH();
    }

    /* -------------------------------------------------------
     * FOR LOOP INSTRUCTIONS
     * ------------------------------------------------------- */

    CASE(NOVA_OP_FORPREP) {
        uint8_t A = NOVA_GET_A(*ip);
        int32_t sBx = NOVA_GET_SBX(*ip);

        /* R(A) = init, R(A+1) = limit, R(A+2) = step */
        /* Try integer path first for integer-valued loops */
        if (nova_is_integer(base[A]) &&
            nova_is_integer(base[A + 1]) &&
            nova_is_integer(base[A + 2])) {
            nova_int_t init = nova_as_integer(base[A]);
            nova_int_t step = nova_as_integer(base[A + 2]);
            base[A] = nova_value_integer(init - step);
            ip += 1 + sBx;
            DISPATCH();
        }

        nova_number_t init = 0, step = 0;
        if (!novai_to_number(base[A], &init) ||
            !novai_to_number(base[A + 2], &step)) {
            char errbuf[256];
            const char *bad_type = !novai_to_number(base[A], &init)
                ? nova_vm_typename(nova_typeof(base[A]))
                : nova_vm_typename(nova_typeof(base[A + 2]));
            snprintf(errbuf, sizeof(errbuf),
                     "'for' loop requires numeric values, got %s",
                     bad_type);
            vm->diag_code = NOVA_E2020;
            novai_error(vm, NOVA_VM_ERR_TYPE, errbuf);
            return vm->status;
        }

        /* Pre-subtract step */
        base[A] = nova_value_number(init - step);
        ip += 1 + sBx;
        DISPATCH();
    }

    CASE(NOVA_OP_FORLOOP) {
        uint8_t A = NOVA_GET_A(*ip);
        int32_t sBx = NOVA_GET_SBX(*ip);

        /* Integer fast path */
        if (nova_is_integer(base[A]) &&
            nova_is_integer(base[A + 1]) &&
            nova_is_integer(base[A + 2])) {
            nova_int_t idx = nova_as_integer(base[A]) + nova_as_integer(base[A + 2]);
            nova_int_t limit = nova_as_integer(base[A + 1]);
            nova_int_t step = nova_as_integer(base[A + 2]);
            base[A] = nova_value_integer(idx);

            int loop = (step >= 0) ? (idx <= limit) : (idx >= limit);
            if (loop) {
                base[A + 3] = nova_value_integer(idx);
                ip += sBx;
            }
            ip++;
            DISPATCH();
        }

        nova_number_t idx = 0, limit = 0, step = 0;
        if (!novai_to_number(base[A], &idx) ||
            !novai_to_number(base[A + 1], &limit) ||
            !novai_to_number(base[A + 2], &step)) {
            novai_error(vm, NOVA_VM_ERR_TYPE, "for loop internal error");
            return vm->status;
        }

        idx += step;
        base[A] = nova_value_number(idx);

        int loop = (step >= 0) ? (idx <= limit) : (idx >= limit);
        if (loop) {
            base[A + 3] = nova_value_number(idx);
            ip += sBx;
        }
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_TFORCALL) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t C = NOVA_GET_C(*ip);

        /* R(A) = iterator, R(A+1) = state, R(A+2) = control */
        NovaValue iter = base[A];
        NovaValue orig_iter = iter;  /* Saved for __call self arg */
        int has_metacall = 0;

        /* Resolve __call metamethod for non-function iterators */
        if (!nova_is_function(iter) &&
            !nova_is_cfunction(iter)) {
            NovaValue callable;
            if (nova_meta_call(vm, iter, &callable) != 0) {
                novai_error(vm, NOVA_VM_ERR_TYPE,
                            "iterator is not a function");
                return vm->status;
            }
            iter = callable;
            has_metacall = 1;

            /* __call needs 1 extra stack slot beyond what the
             * compiler allocated (self + state + control vs
             * just state + control).  Ensure we have room. */
            if (novai_stack_ensure(vm, 1) != 0) {
                return vm->status;
            }
            REFRESH_BASE();
        }

        /* Place args after result slots: R(A+3+C), R(A+3+C+1)...
         * For __call: need self, state, control (3 args)
         * Normal: need state, control (2 args) */
        int arg_base = A + 3 + (int)C;
        int iter_nargs = 2;
        if (has_metacall) {
            base[arg_base]     = orig_iter;    /* self    */
            base[arg_base + 1] = base[A + 1];  /* state   */
            base[arg_base + 2] = base[A + 2];  /* control */
            iter_nargs = 3;
        } else {
            base[arg_base]     = base[A + 1];  /* state   */
            base[arg_base + 1] = base[A + 2];  /* control */
        }

        /* Save frame state */
        frame->ip = ip + 1;

        if (nova_is_cfunction(iter)) {
            NovaValue *save_cfunc_base = vm->cfunc_base;
            vm->cfunc_base = &base[arg_base];
            vm->stack_top = &base[arg_base + iter_nargs];

            int got = nova_as_cfunction(iter)(vm);
            if (got < 0 || vm->status != NOVA_VM_OK) {
                vm->cfunc_base = save_cfunc_base;
                return vm->status;
            }

            /* Copy results from pushed positions to R(A+3)..R(A+2+C) */
            NovaValue *results = &base[arg_base + iter_nargs];
            for (int i = 0; i < C; i++) {
                base[A + 3 + i] = (i < got) ? results[i] : nova_value_nil();
            }

            vm->cfunc_base = save_cfunc_base;
        } else if (nova_is_function(iter)) {
            /* Nova function iterator */
            NovaClosure *icl = nova_as_closure(iter);
            const NovaProto *iproto = icl->proto;

            if (vm->frame_count >= NOVA_MAX_CALL_DEPTH) {
                novai_error(vm, NOVA_VM_ERR_STACKOVERFLOW,
                            "call stack overflow in for iterator");
                return vm->status;
            }
            if (novai_stack_ensure(vm, iproto->max_stack + 10) != 0) {
                return vm->status;
            }
            REFRESH_BASE();

            /* Set up call frame for the iterator function */
            int saved_stop = vm->meta_stop_frame;
            vm->meta_stop_frame = vm->frame_count;

            NovaCallFrame *iter_frame = &vm->frames[vm->frame_count++];
            iter_frame->proto       = iproto;
            iter_frame->closure     = icl;
            iter_frame->base        = &base[arg_base];
            iter_frame->ip          = iproto->code;
            iter_frame->num_results = (int)C;
            iter_frame->num_args    = iter_nargs;
            iter_frame->varargs     = NULL;
            iter_frame->num_varargs = 0;

            /* Nil-fill registers */
            if (iproto->max_stack > iter_nargs) {
                REG_WINDOW_NIL_FILL(&base[arg_base + iter_nargs],
                                    (size_t)(iproto->max_stack - iter_nargs),
                                    nova_value_nil());
            }
            vm->stack_top = &base[arg_base] + iproto->max_stack;

            int ires = novai_execute(vm);
            vm->meta_stop_frame = saved_stop;

            if (ires != NOVA_VM_OK) {
                return ires;
            }

            /* Results already placed by RETURN handler at
             * iter_frame->base - 1 = &base[arg_base - 1].
             * Actually, RETURN copies to dst = frame->base - 1
             * which is &base[arg_base - 1], but we need results
             * at R(A+3)..R(A+2+C). Re-derive base. */
            REFRESH_BASE();

            /* After return, results are at base[arg_base-1] onward,
             * but we need them at A+3. For direct iter calls, the
             * results are at the return dst. Copy them. */
            NovaValue *rdst = &base[arg_base - 1];
            for (int i = 0; i < (int)C; i++) {
                base[A + 3 + i] = rdst[i];
            }
        } else {
            novai_error(vm, NOVA_VM_ERR_TYPE,
                        "iterator is not a function");
            return vm->status;
        }

        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_TFORLOOP) {
        uint8_t A = NOVA_GET_A(*ip);
        int32_t sBx = NOVA_GET_SBX(*ip);

        /* Check first loop variable (A+3) from TFORCALL results */
        if (!nova_is_nil(base[A + 3])) {
            base[A + 2] = base[A + 3];  /* Update control variable */
            ip += sBx;
        }
        ip++;
        DISPATCH();
    }

    /* -------------------------------------------------------
     * CLOSURE INSTRUCTION
     * ------------------------------------------------------- */

    CASE(NOVA_OP_CLOSURE) {
        uint8_t A = NOVA_GET_A(*ip);
        uint32_t Bx = NOVA_GET_BX(*ip);

        const NovaProto *sub = proto->protos[Bx];
        NovaClosure *cl = novai_closure_new(vm, sub);
        if (cl == NULL) {
            novai_error(vm, NOVA_VM_ERR_MEMORY, "closure allocation failed");
            return vm->status;
        }

        /* Capture upvalues */
        for (uint8_t i = 0; i < sub->upvalue_count; i++) {
            ip++;
            uint8_t instack = NOVA_GET_A(*ip);
            uint8_t idx = NOVA_GET_B(*ip);

            if (instack) {
                cl->upvalues[i] = novai_find_upvalue(vm, &base[idx]);
            } else {
                cl->upvalues[i] = frame->closure->upvalues[idx];
            }
        }

        base[A] = nova_value_closure(cl);
        ip++;
        DISPATCH();
    }

    /* -------------------------------------------------------
     * VARARG INSTRUCTIONS
     * ------------------------------------------------------- */

    CASE(NOVA_OP_VARARG) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t C = NOVA_GET_C(*ip);

        int n_va = frame->num_varargs;
        int wanted = (C == 0) ? n_va : C - 1;

        /* Ensure stack space for varargs */
        if (novai_stack_ensure(vm, (size_t)(wanted + 2)) != 0) {
            return vm->status;
        }
        REFRESH_BASE();  /* stack may have been reallocated */

        for (int i = 0; i < wanted; i++) {
            if (i < n_va && frame->varargs != NULL) {
                base[A + i] = frame->varargs[i];
            } else {
                base[A + i] = nova_value_nil();
            }
        }

        if (C == 0) {
            /* Variable results — adjust stack top */
            vm->stack_top = &base[A + wanted];
        }

        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_VARARGPREP) {
        /* Save extra arguments beyond num_params as varargs */
        int num_params = (int)proto->num_params;
        int num_actual = frame->num_args;
        int n_va = num_actual > num_params ? num_actual - num_params : 0;

        if (n_va > 0) {
            frame->varargs = (NovaValue *)malloc(
                (size_t)n_va * sizeof(NovaValue));
            if (frame->varargs == NULL) {
                novai_error(vm, NOVA_VM_ERR_MEMORY,
                            "out of memory allocating varargs");
                DISPATCH();
            }
            for (int i = 0; i < n_va; i++) {
                frame->varargs[i] = base[num_params + i];
            }
            frame->num_varargs = n_va;
        }

        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_CLOSE) {
        uint8_t A = NOVA_GET_A(*ip);
        novai_close_upvalues(vm, &base[A]);
        ip++;
        DISPATCH();
    }

    /* -------------------------------------------------------
     * ASYNC/CONCURRENCY
     * ------------------------------------------------------- */

    CASE(NOVA_OP_AWAIT) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        NovaValue task_val = base[B];

        /* Pass-through: await on a non-coroutine value resolves
         * immediately (like awaiting an already-resolved promise). */
        if (!nova_is_thread(task_val)) {
            base[A] = task_val;
            ip++;
            DISPATCH();
        }

        NovaCoroutine *task = nova_as_coroutine(task_val);

        /* Dead task: already completed, return nil */
        if (task->status == NOVA_CO_DEAD) {
            base[A] = nova_value_nil();
            ip++;
            DISPATCH();
        }

        if (task->status != NOVA_CO_SUSPENDED) {
            novai_error(vm, NOVA_VM_ERR_RUNTIME,
                        "cannot await a running coroutine");
            return vm->status;
        }

        /* Save frame IP past await for when we finish */
        frame->ip = ip + 1;

        /* Flush pending args (stashed at async function call site) */
        int resume_nargs = 0;
        if (task->pending_args != NULL) {
            if (novai_stack_ensure(vm, (size_t)task->pending_nargs) != 0) {
                return vm->status;
            }
            REFRESH_BASE();
            for (int pa = 0; pa < task->pending_nargs; pa++) {
                *vm->stack_top = task->pending_args[pa];
                vm->stack_top++;
            }
            resume_nargs = task->pending_nargs;
            free(task->pending_args);
            task->pending_args = NULL;
            task->pending_nargs = 0;
        }

        /* Resume the task (blocking: run until completion) */
        {
            int result = nova_coroutine_resume(vm, task, resume_nargs);

            /* If the task yields, keep resuming until done.
             * Each yield point gives spawned tasks a turn. */
            while (result == NOVA_VM_YIELD &&
                   task->status == NOVA_CO_SUSPENDED) {
                /* Pop yield values from our stack */
                vm->stack_top -= task->nresults;

                /* Give spawned tasks a turn */
                nova_async_tick(vm);

                /* Resume the awaited task */
                result = nova_coroutine_resume(vm, task, 0);
            }

            /* Refresh base after resume chain (stack may have moved) */
            REFRESH_BASE();

            if (result == NOVA_VM_OK) {
                /* Task completed: harvest first return value */
                int nret = task->nresults;
                if (nret > 0) {
                    base[A] = *(vm->stack_top - nret);
                } else {
                    base[A] = nova_value_nil();
                }
                vm->stack_top -= nret;
            } else {
                /* Error in awaited task - propagate */
                return vm->status;
            }
        }

        ip = frame->ip;
        DISPATCH();
    }

    CASE(NOVA_OP_SPAWN) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        NovaValue task_val = base[B];

        if (!nova_is_thread(task_val)) {
            novai_error(vm, NOVA_VM_ERR_TYPE,
                        "spawn requires an async task (coroutine)");
            return vm->status;
        }

        NovaCoroutine *task = nova_as_coroutine(task_val);

        if (task->status != NOVA_CO_SUSPENDED) {
            novai_error(vm, NOVA_VM_ERR_RUNTIME,
                        "cannot spawn a non-suspended task");
            return vm->status;
        }

        /* Register the task with the scheduler */
        if (nova_async_enqueue(vm, task) != 0) {
            novai_error(vm, NOVA_VM_ERR_MEMORY,
                        "failed to enqueue spawned task");
            return vm->status;
        }

        /* Return the task handle (allows later await) */
        base[A] = task_val;
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_YIELD) {
        uint8_t A = NOVA_GET_A(*ip);
        uint8_t B = NOVA_GET_B(*ip);
        int nresults = (B == 0) ? (int)(vm->stack_top - &base[A]) : B - 1;

        /* Check that we are inside a coroutine */
        if (vm->running_coroutine == NULL) {
            novai_error(vm, NOVA_VM_ERR_RUNTIME,
                        "cannot yield from main thread");
            return vm->status;
        }

        /* Save the yield values: move them to the top of the stack */
        /* They are already at base[A]..base[A+nresults-1] */
        /* Save IP so we resume after the yield instruction */
        ip++;  /* Advance past YIELD so we resume at NEXT instruction */
        frame->ip = ip;

        /* Set stack_top to point after the yield values at base[A] */
        vm->stack_top = &base[A] + nresults;

        /* Signal yield */
        nova_coroutine_yield(vm, nresults);
        return NOVA_VM_YIELD;
    }

    /* -------------------------------------------------------
     * MODULE STUBS
     * ------------------------------------------------------- */

    CASE(NOVA_OP_IMPORT) {
        novai_error(vm, NOVA_VM_ERR_RUNTIME, "IMPORT not yet implemented");
        return vm->status;
    }

    CASE(NOVA_OP_EXPORT) {
        novai_error(vm, NOVA_VM_ERR_RUNTIME, "EXPORT not yet implemented");
        return vm->status;
    }

    /* -------------------------------------------------------
     * MISCELLANEOUS
     * ------------------------------------------------------- */

    CASE(NOVA_OP_NOP) {
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_DEBUG) {
        /* Debug hook placeholder */
        ip++;
        DISPATCH();
    }

    CASE(NOVA_OP_EXTRAARG) {
        /* Should not be executed directly */
        ip++;
        DISPATCH();
    }

#if !NOVA_COMPUTED_GOTO
        default:
            novai_error(vm, NOVA_VM_ERR_RUNTIME, "unknown opcode");
            return vm->status;
        }
    }
#endif

    return NOVA_VM_OK;
}

#if NOVA_COMPUTED_GOTO
#pragma GCC diagnostic pop
#endif

/* ============================================================
 * PART 13: PUBLIC API
 * ============================================================ */

NovaVM *nova_vm_create(void) {
    NovaVM *vm = (NovaVM *)malloc(sizeof(NovaVM));
    if (vm == NULL) {
        return NULL;
    }

    vm->stack = (NovaValue *)malloc(NOVA_INITIAL_STACK_SIZE * sizeof(NovaValue));
    if (vm->stack == NULL) {
        free(vm);
        return NULL;
    }

    vm->stack_top = vm->stack;
    vm->stack_size = NOVA_INITIAL_STACK_SIZE;
    vm->frame_count = 0;
    vm->open_upvalues = NULL;
    vm->cfunc_base = NULL;
    vm->meta_stop_frame = -1;
    vm->running_coroutine = NULL;
    vm->saved_stacks = NULL;
    vm->task_queue = NULL;
    vm->task_count = 0;
    vm->task_capacity = 0;
    vm->string_mt = NULL;
    vm->status = NOVA_VM_OK;
    vm->diag_code = 0;
    vm->error_msg = NULL;
    vm->error_jmp = NULL;
    vm->bytes_allocated = sizeof(NovaVM) + NOVA_INITIAL_STACK_SIZE * sizeof(NovaValue);

    /* Initialize GC state (must happen before any allocation) */
    nova_gc_init(vm);

    /* Initialize string interning pool (DAGGER-backed).
     * Must be created before any nova_string_new() calls.
     * Uses default memcmp key comparison. */
    {
        DaggerTable *sp = dagger_create(256, NULL);
        if (sp == NULL) {
            free(vm->stack);
            free(vm);
            return NULL;
        }
        /* When the pool is destroyed, free each NovaString value */
        dagger_set_value_destroy(sp, free);
        vm->string_pool = sp;
        vm->string_bytes = 0;
    }

    /* Create globals table */
    vm->globals = novai_table_new(vm);
    if (vm->globals == NULL) {
        free(vm->stack);
        free(vm);
        return NULL;
    }

    return vm;
}

/**
 * @brief Destroy a VM instance and free all associated resources.
 *
 * Shuts down the GC, destroys the string interning pool, and frees
 * the stack and VM struct.  Safe to call with NULL.
 *
 * @param vm  VM instance to destroy (may be NULL)
 */
void nova_vm_destroy(NovaVM *vm) {
    if (vm == NULL) {
        return;
    }

    /* Detach open upvalues from the chain (GC shutdown frees them) */
    vm->open_upvalues = NULL;

    /* Free async task queue (tasks themselves are GC objects) */
    nova_async_cleanup(vm);

    /* GC shutdown frees all GC-managed objects (tables, closures,
     * upvalues) via the all_objects chain.  Interned strings are
     * NOT on all_objects — they are owned by the string pool. */
    nova_gc_shutdown(vm);
    vm->globals = NULL;

    /* Destroy the string interning pool.  The value_destroy callback
     * (free) releases each NovaString that was allocated for interning.
     * This must happen AFTER gc_shutdown so that GC objects can still
     * reference string data during their own freeing (they don't
     * dereference strings during free, but ordering is cleaner). */
    if (vm->string_pool != NULL) {
        dagger_destroy((DaggerTable *)vm->string_pool);
        vm->string_pool = NULL;
    }

    /* Free non-GC resources */
    free(vm->stack);
    free(vm->error_msg);
    free(vm);
}

/**
 * @brief Execute a compiled proto in the VM.
 *
 * Sets up the top-level call frame for @p proto and runs the
 * bytecode dispatch loop until completion or error.
 *
 * @param vm     VM instance (must not be NULL)
 * @param proto  Compiled prototype to execute (must not be NULL)
 *
 * @return NOVA_VM_OK on success, or an error code.
 */
int nova_vm_execute(NovaVM *vm, const NovaProto *proto) {
    if (vm == NULL || proto == NULL) {
        return NOVA_VM_ERR_NULLPTR;
    }

    vm->status = NOVA_VM_OK;

    NTRACE(VM, "nova_vm_execute proto=<%s> max_stack=%d params=%d",
           proto->source ? proto->source : "(top-level)",
           proto->max_stack, proto->num_params);

    /* Ensure stack space (+1 for return value guard slot) */
    if (novai_stack_ensure(vm, proto->max_stack + 11) != 0) {
        return vm->status;
    }

    /* Set up initial frame.
     *
     * Reserve slot 0 as a return-value receptor.  The RETURN
     * opcode writes results to frame->base - 1, so base must
     * be at vm->stack + 1 to avoid a buffer underflow when the
     * top-level proto executes a 'return <expr>' statement.  */
    NovaCallFrame *frame = &vm->frames[0];
    frame->proto = proto;
    frame->closure = NULL;
    frame->base = vm->stack + 1;   /* slot 0 = return receptor */
    frame->ip = proto->code;
    frame->num_results = -1;  /* Variable results */
    frame->num_args = 0;
    frame->varargs = NULL;
    frame->num_varargs = 0;

    vm->stack[0] = nova_value_nil();  /* Clear guard slot */
    vm->frame_count = 1;
    /* stack_top must cover the full register window so GC root
     * scanning sees all live values (locals, temporaries, etc.).
     * Using num_params left higher registers invisible to GC.
     * Nil-fill slots above params so GC doesn't see stale ptrs. */
    if (proto->max_stack > proto->num_params) {
        REG_WINDOW_NIL_FILL(&frame->base[proto->num_params],
                            (size_t)(proto->max_stack - proto->num_params),
                            nova_value_nil());
    }
    vm->stack_top = frame->base + proto->max_stack;

    return novai_execute(vm);
}

/**
 * @brief Call a function on the VM stack.
 *
 * Expects the function followed by @p nargs arguments on the stack.
 * Supports closures, C functions, and __call metamethods.
 * On return, results replace the function slot on the stack.
 *
 * @param vm        VM instance (must not be NULL)
 * @param nargs     Number of arguments pushed after the function
 * @param nresults  Expected number of results (-1 = all)
 *
 * @return NOVA_VM_OK on success, or an error code.
 */
int nova_vm_call(NovaVM *vm, int nargs, int nresults) {
    if (vm == NULL) {
        return NOVA_VM_ERR_NULLPTR;
    }

    NovaValue *func_slot = vm->stack_top - nargs - 1;
    NovaValue func = *func_slot;

    NTRACE(CALL, "nova_vm_call nargs=%d nresults=%d type=%d",
           nargs, nresults, nova_typeof(func));
    NTRACE_STACK(vm, "nova_vm_call entry");

    if (nova_is_cfunction(func)) {
        /* Stack layout: [func][arg1][arg2]...  stack_top points after args */
        NovaValue *save_cfunc_base = vm->cfunc_base;
        vm->cfunc_base = func_slot + 1;
        vm->stack_top = func_slot + 1 + nargs;
        int got = nova_as_cfunction(func)(vm);
        vm->cfunc_base = save_cfunc_base;
        if (got < 0 || vm->status != NOVA_VM_OK) {
            return vm->status;
        }
        /* cfunc pushed 'got' results at stack_top - got */
        /* Move results to func_slot (replacing the function) */
        NovaValue *results = vm->stack_top - got;
        for (int i = 0; i < got; i++) {
            func_slot[i] = results[i];
        }
        /* Pad with nil if needed */
        if (nresults >= 0) {
            for (int i = got; i < nresults; i++) {
                func_slot[i] = nova_value_nil();
            }
            vm->stack_top = func_slot + nresults;
        } else {
            vm->stack_top = func_slot + got;
        }
        return NOVA_VM_OK;
    }

    if (!nova_is_function(func)) {
        /* Try __call metamethod for non-function/non-cfunction types */
        NovaValue callable;
        if (nova_meta_call(vm, func, &callable) != 0) {
            return vm->status;
        }
        /* __call resolved: shift args right, insert original as arg 1 */
        if (novai_stack_ensure(vm, 1) != 0) {
            return vm->status;
        }
        /* func_slot may have moved after stack_ensure */
        func_slot = vm->stack_top - nargs - 1;
        for (int i = nargs; i >= 0; i--) {
            func_slot[1 + i + 1] = func_slot[1 + i];
        }
        func_slot[0] = callable;
        func_slot[1] = func;
        nargs++;
        vm->stack_top++;
        func = callable;

        /* Re-dispatch: callable could be cfunction or closure */
        if (nova_is_cfunction(func)) {
            NovaValue *save_cfunc_base = vm->cfunc_base;
            vm->cfunc_base = func_slot + 1;
            vm->stack_top = func_slot + 1 + nargs;
            int got = nova_as_cfunction(func)(vm);
            vm->cfunc_base = save_cfunc_base;
            if (got < 0 || vm->status != NOVA_VM_OK) {
                return vm->status;
            }
            NovaValue *results = vm->stack_top - got;
            for (int i = 0; i < got; i++) {
                func_slot[i] = results[i];
            }
            if (nresults >= 0) {
                for (int i = got; i < nresults; i++) {
                    func_slot[i] = nova_value_nil();
                }
                vm->stack_top = func_slot + nresults;
            } else {
                vm->stack_top = func_slot + got;
            }
            return NOVA_VM_OK;
        }
        if (!nova_is_function(func)) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "attempt to call a %s value",
                     nova_vm_typename(nova_typeof(func)));
            vm->diag_code = NOVA_E2008;
            novai_error(vm, NOVA_VM_ERR_TYPE, buf);
            return vm->status;
        }
    }

    NovaClosure *cl = nova_as_closure(func);
    const NovaProto *proto = cl->proto;

    if (vm->frame_count >= NOVA_MAX_CALL_DEPTH) {
        novai_error(vm, NOVA_VM_ERR_STACKOVERFLOW, "call stack overflow");
        return vm->status;
    }

    if (novai_stack_ensure(vm, proto->max_stack + 10) != 0) {
        return vm->status;
    }

    /* Set meta_stop_frame so the inner novai_execute stops when
     * this closure returns, instead of leaking into parent frames.
     * frame_count (before push) is where RETURN should stop. */
    int saved_stop = vm->meta_stop_frame;
    vm->meta_stop_frame = vm->frame_count;

    NovaCallFrame *frame = &vm->frames[vm->frame_count++];
    frame->proto = proto;
    frame->closure = cl;
    frame->base = func_slot + 1;
    frame->ip = proto->code;
    frame->num_results = nresults;
    frame->num_args = nargs;
    frame->varargs = NULL;
    frame->num_varargs = 0;

    /* Nil-fill uninitialized registers so GC sees no stale ptrs */
    if (proto->max_stack > nargs) {
        REG_WINDOW_NIL_FILL(&frame->base[nargs],
                            (size_t)(proto->max_stack - nargs),
                            nova_value_nil());
    }
    /* Ensure GC can see callee's full register window */
    vm->stack_top = frame->base + proto->max_stack;

    int result = novai_execute(vm);
    vm->meta_stop_frame = saved_stop;

    NTRACE(CALL, "nova_vm_call returned status=%d", result);
    NTRACE_STACK(vm, "nova_vm_call exit");

    return result;
}

/**
 * @brief Protected call - catches runtime errors via setjmp/longjmp.
 *
 * Same calling convention as nova_vm_call(), but errors are caught
 * instead of propagated. On error, the error message string is
 * pushed onto the stack at the function's original position.
 *
 * @param vm        VM instance (must not be NULL)
 * @param nargs     Number of arguments on stack
 * @param nresults  Expected results (-1 = all)
 *
 * @return NOVA_VM_OK on success, error code on failure.
 */
int nova_vm_pcall(NovaVM *vm, int nargs, int nresults) {
    if (vm == NULL) {
        return NOVA_VM_ERR_NULLPTR;
    }

    /* Save the function slot offset (not pointer, because stack
     * may be reallocated by nova_vm_call, invalidating pointers
     * saved before setjmp). */
    ptrdiff_t func_slot_off = (vm->stack_top - nargs - 1) - vm->stack;

    /* Save state for recovery */
    NovaErrorJmp ej;
    ej.previous = vm->error_jmp;
    ej.status = NOVA_VM_OK;
    ej.frame_count = vm->frame_count;
    ej.stack_top = vm->stack_top;
    vm->error_jmp = &ej;

    int saved_status = vm->status;
    int saved_meta_stop = vm->meta_stop_frame;
    NovaValue *saved_cfunc_base = vm->cfunc_base;

    if (setjmp(ej.buf) == 0) {
        /* Normal path: attempt the call */
        int result = nova_vm_call(vm, nargs, nresults);
        vm->error_jmp = ej.previous;
        return result;
    }

    /* Error path: longjmp landed here.
     * Re-derive func_slot from offset (stack may have moved). */
    NovaValue *func_slot = vm->stack + func_slot_off;

    vm->error_jmp = ej.previous;
    vm->meta_stop_frame = saved_meta_stop;  /* Restore: nova_vm_call sets
                                             * this but longjmp skips its
                                             * cleanup, leaving it stale */
    vm->cfunc_base = saved_cfunc_base;      /* Restore: __call dispatch may
                                             * have modified cfunc_base */

    /* Unwind call frames back to the saved depth */
    while (vm->frame_count > ej.frame_count) {
        vm->frame_count--;
        NovaCallFrame *f = &vm->frames[vm->frame_count];
        /* Close upvalues for unwound frames */
        novai_close_upvalues(vm, f->base);
        /* Free vararg buffers */
        if (f->varargs != NULL) {
            free(f->varargs);
            f->varargs = NULL;
            f->num_varargs = 0;
        }
    }

    /* Push the error message at the function slot position */
    const char *err = vm->error_msg;
    if (err == NULL) {
        err = "(unknown error)";
    }

    /* Place error msg string at func_slot[0], set stack_top after it */
    NovaString *estr = novai_string_new(vm, err, strlen(err));
    if (estr != NULL) {
        func_slot[0] = nova_value_string(estr);
    } else {
        func_slot[0] = nova_value_nil();
    }
    vm->stack_top = func_slot + 1;

    int err_code = ej.status;
    vm->status = saved_status;  /* Restore status so VM can continue */
    return err_code;
}

/* ============================================================
 * PART 14: STACK OPERATIONS
 * ============================================================ */

/**
 * @brief Push nil onto the VM stack.
 *
 * @param vm  VM instance (may be NULL)
 */
void nova_vm_push_nil(NovaVM *vm) {
    if (vm == NULL) return;
    if (novai_stack_ensure(vm, 1) != 0) return;
    *vm->stack_top++ = nova_value_nil();
}

/**
 * @brief Push a boolean value onto the VM stack.
 *
 * @param vm  VM instance (may be NULL)
 * @param b   Boolean value (0 = false, nonzero = true)
 */
void nova_vm_push_bool(NovaVM *vm, int b) {
    if (vm == NULL) return;
    if (novai_stack_ensure(vm, 1) != 0) return;
    *vm->stack_top++ = nova_value_bool(b);
}

/**
 * @brief Push an integer value onto the VM stack.
 *
 * @param vm  VM instance (may be NULL)
 * @param n   Integer value to push
 */
void nova_vm_push_integer(NovaVM *vm, nova_int_t n) {
    if (vm == NULL) return;
    if (novai_stack_ensure(vm, 1) != 0) return;
    *vm->stack_top++ = nova_value_integer(n);
}

/**
 * @brief Push a floating-point number onto the VM stack.
 *
 * @param vm  VM instance (may be NULL)
 * @param n   Number value to push
 */
void nova_vm_push_number(NovaVM *vm, nova_number_t n) {
    if (vm == NULL) return;
    if (novai_stack_ensure(vm, 1) != 0) return;
    *vm->stack_top++ = nova_value_number(n);
}

/**
 * @brief Push a string onto the VM stack.
 *
 * The string is copied and interned.  If allocation fails,
 * nil is pushed instead.
 *
 * @param vm   VM instance (may be NULL)
 * @param s    Pointer to string data
 * @param len  Length of string in bytes
 */
void nova_vm_push_string(NovaVM *vm, const char *s, size_t len) {
    if (vm == NULL) return;
    if (novai_stack_ensure(vm, 1) != 0) return;
    NovaString *str = novai_string_new(vm, s, len);
    if (str == NULL) {
        *vm->stack_top++ = nova_value_nil();
    } else {
        *vm->stack_top++ = nova_value_string(str);
    }
}

/**
 * @brief Push a C function onto the VM stack.
 *
 * @param vm  VM instance (may be NULL)
 * @param fn  C function pointer to push
 */
void nova_vm_push_cfunction(NovaVM *vm, nova_cfunc_t fn) {
    if (vm == NULL) return;
    if (novai_stack_ensure(vm, 1) != 0) return;
    *vm->stack_top++ = nova_value_cfunction(fn);
}

/**
 * @brief Push an arbitrary NovaValue onto the VM stack.
 *
 * @param vm  VM instance (may be NULL)
 * @param v   Value to push
 */
void nova_vm_push_value(NovaVM *vm, NovaValue v) {
    if (vm == NULL) return;
    if (novai_stack_ensure(vm, 1) != 0) return;
    *vm->stack_top++ = v;
}

/**
 * @brief Get a value from the VM stack by index.
 *
 * Positive indices are relative to the base (0 = first argument).
 * Negative indices are relative to the stack top (-1 = top).
 * Returns nil if the index is out of range.
 *
 * @param vm   VM instance (may be NULL)
 * @param idx  Stack index (positive = from base, negative = from top)
 *
 * @return The value at the given index, or nil.
 */
NovaValue nova_vm_get(NovaVM *vm, int idx) {
    if (vm == NULL) {
        return nova_value_nil();
    }

    NovaValue *base_ptr = (vm->cfunc_base != NULL) ? vm->cfunc_base : vm->stack;
    NovaValue *slot = NULL;
    if (idx >= 0) {
        slot = base_ptr + idx;
    } else {
        slot = vm->stack_top + idx;
    }

    if (slot < vm->stack || slot >= vm->stack_top) {
        return nova_value_nil();
    }
    return *slot;
}

/**
 * @brief Get the number of values on the VM stack.
 *
 * @param vm  VM instance (may be NULL)
 *
 * @return Number of values on the stack, or 0 if vm is NULL.
 */
int nova_vm_get_top(NovaVM *vm) {
    if (vm == NULL) {
        return 0;
    }
    NovaValue *base_ptr = (vm->cfunc_base != NULL) ? vm->cfunc_base : vm->stack;
    return (int)(vm->stack_top - base_ptr);
}

/**
 * @brief Set the stack top to a specific index.
 *
 * Positive indices set the top relative to the base.  Negative
 * indices are relative to the current top.  Grows with nil if needed.
 *
 * @param vm   VM instance (may be NULL)
 * @param idx  Target stack index
 */
void nova_vm_set_top(NovaVM *vm, int idx) {
    if (vm == NULL) {
        return;
    }
    NovaValue *base_ptr = (vm->cfunc_base != NULL) ? vm->cfunc_base : vm->stack;
    if (idx >= 0) {
        NovaValue *target = base_ptr + idx;
        while (vm->stack_top < target) {
            nova_vm_push_nil(vm);
        }
        vm->stack_top = target;
    } else {
        vm->stack_top += idx;
        if (vm->stack_top < base_ptr) {
            vm->stack_top = base_ptr;
        }
    }
}

/**
 * @brief Pop n values from the VM stack.
 *
 * @param vm  VM instance (may be NULL)
 * @param n   Number of values to pop (must be > 0)
 */
void nova_vm_pop(NovaVM *vm, int n) {
    if (vm == NULL || n <= 0) {
        return;
    }
    NovaValue *base_ptr = (vm->cfunc_base != NULL) ? vm->cfunc_base : vm->stack;
    vm->stack_top -= n;
    if (vm->stack_top < base_ptr) {
        vm->stack_top = base_ptr;
    }
}

/* ============================================================
 * PART 15: GLOBAL TABLE OPERATIONS
 * ============================================================ */

/**
 * @brief Set a global variable by name.
 *
 * @param vm    VM instance (must not be NULL)
 * @param name  Variable name (must not be NULL)
 * @param val   Value to assign
 */
void nova_vm_set_global(NovaVM *vm, const char *name, NovaValue val) {
    if (vm == NULL || name == NULL) {
        return;
    }
    NovaString *key = novai_string_new(vm, name, strlen(name));
    if (key == NULL) {
        return;
    }
    novai_table_set_str(vm, vm->globals, key, val);
}

/**
 * @brief Get a global variable by name.
 *
 * @param vm    VM instance (must not be NULL)
 * @param name  Variable name (must not be NULL)
 *
 * @return The global's value, or nil if not found.
 */
NovaValue nova_vm_get_global(NovaVM *vm, const char *name) {
    if (vm == NULL || name == NULL) {
        return nova_value_nil();
    }
    /* With string interning, just intern the name and look it up.
     * nova_string_new returns the canonical pointer (no temp alloc). */
    NovaString *key = novai_string_new(vm, name, strlen(name));
    if (key == NULL) {
        return nova_value_nil();
    }
    return novai_table_get_str(vm->globals, key);
}

/* ============================================================
 * PART 16: ERROR AND TYPE UTILITIES
 * ============================================================ */

const char *nova_vm_typename(NovaValueType type) {
    switch (type) {
        case NOVA_TYPE_NIL:       return "nil";
        case NOVA_TYPE_BOOL:      return "boolean";
        case NOVA_TYPE_INTEGER:   return "integer";
        case NOVA_TYPE_NUMBER:    return "number";
        case NOVA_TYPE_STRING:    return "string";
        case NOVA_TYPE_TABLE:     return "table";
        case NOVA_TYPE_FUNCTION:  return "function";
        case NOVA_TYPE_CFUNCTION: return "cfunction";
        case NOVA_TYPE_USERDATA:  return "userdata";
        case NOVA_TYPE_THREAD:    return "thread";
        default:                  return "unknown";
    }
}

/**
 * @brief Get the last error message from the VM.
 *
 * Returns the stored error message, or a default string based
 * on the VM status code.
 *
 * @param vm  VM instance (may be NULL)
 *
 * @return Error message string (never NULL).
 */
const char *nova_vm_error(const NovaVM *vm) {
    if (vm == NULL) {
        return "null VM";
    }
    if (vm->error_msg != NULL) {
        return vm->error_msg;
    }
    switch (vm->status) {
        case NOVA_VM_OK:              return "no error";
        case NOVA_VM_ERR_RUNTIME:     return "runtime error";
        case NOVA_VM_ERR_MEMORY:      return "memory allocation failed";
        case NOVA_VM_ERR_STACKOVERFLOW: return "stack overflow";
        case NOVA_VM_ERR_TYPE:        return "type error";
        case NOVA_VM_ERR_DIVZERO:     return "division by zero";
        case NOVA_VM_ERR_NULLPTR:     return "null pointer";
        default:                      return "unknown error";
    }
}

/* ============================================================
 * PART 17: PUBLIC ERROR AND TABLE APIS
 * ============================================================ */

#include <stdarg.h>

/**
 * @brief Auto-classify an error message into a fine-grained diagnostic code.
 *
 * Detects common stdlib error patterns and returns the appropriate
 * NovaErrorCode so diag_code is set without changing call sites.
 *
 * @param msg  Formatted error message
 * @return NovaErrorCode, or 0 if no pattern matches
 */
static int novai_auto_classify(const char *msg) {
    if (msg == NULL) { return 0; }

    /* "bad argument #N to 'func' (... expected ...)" → argument type error */
    if (strncmp(msg, "bad argument", 12) == 0) {
        if (strstr(msg, "value expected") != NULL) { return NOVA_E2022; }
        return NOVA_E2021;
    }

    /* Memory errors */
    if (strstr(msg, "out of memory") != NULL ||
        strstr(msg, "memory allocation") != NULL ||
        strstr(msg, "allocation failed") != NULL) {
        return NOVA_E2003;
    }

    /* Module loading */
    if (strstr(msg, "module") != NULL &&
        strstr(msg, "not found") != NULL) {
        return NOVA_E2023;
    }

    /* SQL errors */
    if (strncmp(msg, "sql.", 4) == 0) { return NOVA_E3006; }

    /* Network errors */
    if (strncmp(msg, "net.", 4) == 0 ||
        strncmp(msg, "net:", 4) == 0) {
        return NOVA_E3005;
    }

    /* File I/O errors */
    if (strstr(msg, "cannot open") != NULL ||
        strstr(msg, "cannot read") != NULL ||
        strstr(msg, "cannot seek") != NULL) {
        return NOVA_E3007;
    }

    /* Format / parse / codec errors */
    if (strstr(msg, ".decode:") != NULL ||
        strstr(msg, ".encode:") != NULL ||
        strstr(msg, ".load:") != NULL) {
        return NOVA_E3008;
    }

    return 0;
}

/**
 * @brief Raise a formatted runtime error.
 *
 * Formats the message with printf-style arguments and sets
 * the VM into an error state via novai_error().
 *
 * @param vm   VM instance (must not be NULL)
 * @param fmt  printf-style format string
 * @param ...  Format arguments
 */
void nova_vm_raise_error(NovaVM *vm, const char *fmt, ...) {
    if (vm == NULL || fmt == NULL) {
        return;
    }

    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n < 0) {
        n = 0;
    }

    vm->diag_code = novai_auto_classify(buf);
    novai_error(vm, NOVA_VM_ERR_RUNTIME, buf);
    (void)n;
}

void nova_vm_raise_error_ex(NovaVM *vm, int vm_err, int diag,
                            const char *fmt, ...) {
    if (vm == NULL || fmt == NULL) {
        return;
    }

    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n < 0) {
        n = 0;
    }

    vm->diag_code = diag;
    novai_error(vm, vm_err, buf);
    (void)n;
}

/**
 * @brief Create a new table and push it onto the stack.
 *
 * If table allocation fails, nil is pushed instead.
 *
 * @param vm  VM instance (may be NULL)
 */
void nova_vm_push_table(NovaVM *vm) {
    if (vm == NULL) {
        return;
    }
    if (novai_stack_ensure(vm, 1) != 0) {
        return;
    }

    NovaTable *t = novai_table_new(vm);
    if (t == NULL) {
        *vm->stack_top++ = nova_value_nil();
    } else {
        *vm->stack_top++ = nova_value_table(t);
    }
}

/**
 * @brief Set a named field on a table at the given stack index.
 *
 * Pops the value from the top of the stack and assigns it to
 * the field @p name in the table at @p idx.  No-op if the
 * value at @p idx is not a table.
 *
 * @param vm    VM instance (must not be NULL)
 * @param idx   Stack index of the table
 * @param name  Field name (must not be NULL)
 */
void nova_vm_set_field(NovaVM *vm, int idx, const char *name) {
    if (vm == NULL || name == NULL) {
        return;
    }

    /* Get the table */
    NovaValue tval = nova_vm_get(vm, idx);
    if (!nova_is_table(tval)) {
        return;
    }

    /* Ensure there is a value on the stack */
    if (vm->stack_top <= vm->stack) {
        return;
    }

    /* Read value from TOS WITHOUT popping yet — keeps it GC-safe
     * while novai_string_new may trigger nova_gc_check. */
    NovaValue val = vm->stack_top[-1];

    /* Create the key string (may trigger GC; value still on stack) */
    NovaString *key = novai_string_new(vm, name, strlen(name));
    if (key == NULL) {
        return;
    }

    /* Set the field (table resize here is also GC-safe: value on stack) */
    novai_table_set_str(vm, nova_as_table(tval), key, val);

    /* NOW pop the value — it's safely stored in the table */
    vm->stack_top--;
}

/* ============================================================
 * nova_vm_execute_frames  -- run VM until frame_count drops
 *                            to 'stop_at'.
 *
 * Used by nova_meta.c to invoke Nova closures from C during
 * metamethod dispatch.  The caller must have already pushed
 * the call frame; this function just resumes execution and
 * returns when RETURN pops back to the 'stop_at' level.
 * ============================================================ */
int nova_vm_execute_frames(NovaVM *vm, int stop_at) {
    if (vm == NULL) {
        return NOVA_VM_ERR_NULLPTR;
    }

    /* Save the previous stop boundary (supports nesting) */
    int saved_stop = vm->meta_stop_frame;
    vm->meta_stop_frame = stop_at;

    int result = novai_execute(vm);

    /* Restore previous boundary */
    vm->meta_stop_frame = saved_stop;
    return result;
}

/* ============================================================
 * PUBLIC WRAPPERS for nova_meta.c
 * ============================================================ */

int nova_table_raw_set_int(NovaVM *vm, NovaTable *t,
                           nova_int_t key, NovaValue val) {
    return novai_table_set_int(vm, t, key, val);
}

int nova_table_raw_set_str(NovaVM *vm, NovaTable *t,
                           NovaString *key, NovaValue val) {
    return novai_table_set_str(vm, t, key, val);
}

/**
 * @brief Intern a string in the VM's string pool.
 *
 * Returns the canonical interned pointer for the given string
 * data, allocating a new NovaString if one does not already exist.
 *
 * @param vm   VM instance (must not be NULL)
 * @param s    String data
 * @param len  Length of string in bytes
 *
 * @return Interned NovaString pointer, or NULL on allocation failure.
 */
NovaString *nova_vm_intern_string(NovaVM *vm, const char *s, size_t len) {
    return novai_string_new(vm, s, len);
}

/* ============================================================
 * PART 32: STACK TRACEBACK
 * ============================================================ */

/**
 * @brief Build a stack traceback string.
 *
 * Walks the call frame stack from `level` down to frame 0 and
 * produces a multi-line string similar to Lua's debug.traceback():
 *
 *   stack traceback:
 *       script.n:10: in function 'foo'
 *       script.n:5: in function 'bar'
 *       script.n:1: in main chunk
 *
 * @param vm     VM instance (must not be NULL)
 * @param msg    Optional message to prepend (may be NULL)
 * @param level  Frame level to start from (0 = top, 1 = caller)
 * @return Heap-allocated string or NULL.  Caller must free().
 *
 * COMPLEXITY: O(frame_count)
 * THREAD SAFETY: Not thread-safe
 */
char *nova_vm_traceback(const NovaVM *vm, const char *msg, int level) {
    if (vm == NULL) {
        return NULL;
    }

    /* Pre-allocate a buffer (grows as needed) */
    size_t cap = 512;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (buf == NULL) {
        return NULL;
    }

    /* Helper: append a string literal (no printf needed) */
    #define TB_PUTS(s) do { \
        size_t sl_ = strlen(s); \
        while (len + sl_ >= cap) { \
            cap *= 2; \
            char *tmp_ = (char *)realloc(buf, cap); \
            if (tmp_ == NULL) { free(buf); return NULL; } \
            buf = tmp_; \
        } \
        memcpy(buf + len, s, sl_); \
        len += sl_; \
        buf[len] = '\0'; \
    } while (0)

    /* Helper: append formatted text (with args) */
    #define TB_FMT(fmt, ...) do { \
        int n_ = snprintf(buf + len, cap - len, fmt, __VA_ARGS__); \
        if (n_ < 0) { n_ = 0; } \
        while (len + (size_t)n_ >= cap) { \
            cap *= 2; \
            char *tmp_ = (char *)realloc(buf, cap); \
            if (tmp_ == NULL) { free(buf); return NULL; } \
            buf = tmp_; \
            n_ = snprintf(buf + len, cap - len, fmt, __VA_ARGS__); \
            if (n_ < 0) { n_ = 0; } \
        } \
        len += (size_t)n_; \
    } while (0)

    /* Prepend user message if provided */
    if (msg != NULL && msg[0] != '\0') {
        TB_FMT("%s\n", msg);
    }

    TB_PUTS("stack traceback:");

    int start = vm->frame_count - 1;
    if (level > 0 && level <= start) {
        start = start - level;
    }

    for (int i = start; i >= 0; i--) {
        const NovaCallFrame *f = &vm->frames[i];
        const NovaProto *proto = f->proto;

        if (proto == NULL) {
            TB_PUTS("\n\t[C]: in ?");
            continue;
        }

        const char *source = proto->source ? proto->source : "?";
        uint32_t line = 0;

        if (f->ip != NULL && proto->lines.line_numbers != NULL) {
            uint32_t pc = (uint32_t)(f->ip - proto->code);
            /* For saved frames (not the topmost), ip points to the
             * instruction AFTER the CALL, so use pc-1 for the
             * calling line.  For the topmost frame (i == start),
             * ip was saved to the current CALL instruction.          */
            if (i < start && pc > 0) {
                pc--;
            }
            if (pc < proto->lines.count) {
                line = proto->lines.line_numbers[pc];
            }
        }

        /* Function name: use closure's proto, or "main chunk" */
        if (i == 0) {
            TB_FMT("\n\t%s:%u: in main chunk", source, line);
        } else if (f->closure != NULL && f->closure->proto != NULL &&
                   f->closure->proto->source != NULL) {
            /* Try to find the function name from locals in the
             * parent frame.  As a fallback, use "function <src:line>" */
            const NovaProto *cp = f->closure->proto;
            TB_FMT("\n\t%s:%u: in function <%s:%u>",
                   source, line,
                   cp->source ? cp->source : "?",
                   cp->line_defined);
        } else {
            TB_FMT("\n\t%s:%u: in function <unknown>", source, line);
        }
    }

    #undef TB_PUTS
    #undef TB_FMT

    return buf;
}
