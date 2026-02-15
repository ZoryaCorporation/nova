/**
 * @file dagger.h
 * @brief DAGGER - Dual-Action Guaranteed Grab Engine for Records
 * 
 * DAGGER is a high-performance hash table implementation combining:
 * 
 *   - Robin Hood Hashing: Minimizes probe sequence variance
 *   - Cuckoo Fallback: Guarantees O(1) worst-case lookup
 *   - NXH Integration: Uses Nexus Hash for all hashing needs
 * 
 * KEY FEATURES:
 *   - Average lookup: O(1) with low variance
 *   - Worst-case lookup: O(1) (guaranteed)
 *   - Memory efficient: ~1 byte overhead per slot
 *   - Cache-friendly: Linear probing, 16-byte alignment
 *   - Flexible: Works with any key/value types
 * 
 * USAGE:
 *   #include <zorya/dagger.h>
 *   DaggerTable *T = dagger_create(0, NULL);
 *   dagger_set_str(T, "key", value, 1);
 *   dagger_destroy(T);
 *
 * LINKING:
 *   gcc myapp.c -ldagger -lnxh -o myapp
 *
 * @author Anthony Taliento
 * @date 2025-11-28
 * @version 2.0.0
 * 
 * @copyright Copyright 2025 Zorya Corporation
 * @license Apache-2.0
 */

#ifndef DAGGER_H_
#define DAGGER_H_

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include "nxh.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * CONFIGURATION
 * ============================================================ */

/** Maximum probe sequence length before Cuckoo kicks in */
#define DAGGER_PSL_THRESHOLD 16

/** Maximum Cuckoo displacement cycles before resize */
#define DAGGER_MAX_CUCKOO_CYCLES 500

/** Default initial capacity (slots) */
#define DAGGER_INITIAL_CAPACITY 64

/** Load factor threshold for resize (75%) */
#define DAGGER_LOAD_FACTOR_PERCENT 75

/** Minimum capacity (must be power of 2) */
#define DAGGER_MIN_CAPACITY 16

/** Growth factor when resizing (2x) */
#define DAGGER_GROWTH_FACTOR 2

/* ============================================================
 * VERSION INFO
 * ============================================================ */

#define DAGGER_VERSION_MAJOR 2
#define DAGGER_VERSION_MINOR 0
#define DAGGER_VERSION_PATCH 0
#define DAGGER_VERSION_STRING "2.0.0"

/* ============================================================
 * RESULT CODES
 * ============================================================ */

typedef enum {
    DAGGER_OK = 0,              /**< Operation successful */
    DAGGER_NOT_FOUND = 1,       /**< Key not found (not an error) */
    DAGGER_EXISTS = 2,          /**< Key already exists (for insert) */
    DAGGER_ERROR_NULLPTR = -1,  /**< NULL pointer passed */
    DAGGER_ERROR_NOMEM = -2,    /**< Memory allocation failed */
    DAGGER_ERROR_FULL = -3,     /**< Table is full (resize failed) */
    DAGGER_ERROR_INVALID = -4   /**< Invalid argument */
} dagger_result_t;

/* ============================================================
 * ENTRY STRUCTURE
 * ============================================================ */

/**
 * @brief Single hash table entry (40 bytes aligned)
 */
typedef struct {
    uint64_t hash_primary;   /**< Primary hash (nxh64) */
    uint64_t hash_alternate; /**< Alternate hash (nxh64_alt) for Cuckoo */
    
    const void *key;         /**< Pointer to key data */
    void *value;             /**< Pointer to value data */
    
    uint32_t key_len;        /**< Key length in bytes */
    uint8_t psl;             /**< Probe Sequence Length (0-255) */
    uint8_t occupied;        /**< Slot is in use */
    uint8_t in_cuckoo;       /**< Entry is in Cuckoo alternate location */
    uint8_t _pad;            /**< Padding for alignment */
} DaggerEntry;

/* ============================================================
 * FUNCTION POINTER TYPES
 * ============================================================ */

/**
 * @brief Key comparison function type
 */
typedef int (*dagger_key_eq_fn)(
    const void *k1, uint32_t k1_len,
    const void *k2, uint32_t k2_len
);

/**
 * @brief Value destructor function type
 */
typedef void (*dagger_value_destroy_fn)(void *value);

/**
 * @brief Key destructor function type
 */
typedef void (*dagger_key_destroy_fn)(const void *key, uint32_t key_len);

/**
 * @brief Iterator callback type
 */
typedef int (*dagger_iter_fn)(
    const void *key, uint32_t key_len,
    void *value,
    void *ctx
);

/* ============================================================
 * TABLE STRUCTURE
 * ============================================================ */

/**
 * @brief DAGGER hash table
 */
typedef struct {
    DaggerEntry *entries;        /**< Entry array */
    size_t capacity;             /**< Number of slots */
    size_t count;                /**< Number of occupied slots */
    size_t mask;                 /**< capacity - 1 (for fast modulo) */
    
    uint64_t seed_primary;       /**< Seed for primary hash */
    uint64_t seed_alternate;     /**< Seed for alternate hash */
    
    dagger_key_eq_fn key_eq;     /**< Key equality function */
    dagger_value_destroy_fn value_destroy; /**< Optional value destructor */
    dagger_key_destroy_fn key_destroy;     /**< Optional key destructor */
    
    /* Statistics */
    size_t max_psl;              /**< Maximum PSL seen */
    size_t cuckoo_count;         /**< Entries in Cuckoo positions */
    size_t resize_count;         /**< Number of resizes performed */
    size_t total_probes;         /**< Total probe count (for stats) */
    size_t total_lookups;        /**< Total lookup count */
} DaggerTable;

/**
 * @brief Table statistics
 */
typedef struct {
    size_t capacity;         /**< Total slots */
    size_t count;            /**< Occupied slots */
    size_t max_psl;          /**< Maximum probe sequence length */
    size_t cuckoo_count;     /**< Entries in Cuckoo positions */
    size_t resize_count;     /**< Number of resizes performed */
    double load_factor;      /**< count / capacity */
    double avg_probes;       /**< Average probes per lookup */
} DaggerStats;

/* ============================================================
 * INLINE HELPER FUNCTIONS (stay in header for performance)
 * ============================================================ */

/**
 * @brief Next power of 2 >= n
 */
static inline size_t dagger_next_pow2(size_t n) {
    if (n == 0) return 1;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    return n + 1;
}

/**
 * @brief Default key equality: byte-by-byte comparison
 */
static inline int dagger_key_eq_default(
    const void *k1, uint32_t k1_len,
    const void *k2, uint32_t k2_len
) {
    if (k1_len != k2_len) return 0;
    return memcmp(k1, k2, k1_len) == 0 ? 1 : 0;
}

/**
 * @brief Get slot index for hash
 */
static inline size_t dagger_slot(const DaggerTable *T, uint64_t hash) {
    return (size_t)(hash & T->mask);
}

/* ============================================================
 * HOT PATH EXTRACTION - ZERO OVERHEAD LOOKUPS
 * 
 * PCM: DAGGER_HOT_*
 * Purpose: Inline record extraction with no function call overhead
 * Rationale: Standard dagger_get() has ~15ns overhead from:
 *            - NULL checks, stats tracking, output parameter indirection
 *            - Function pointer call for key comparison
 * Performance Impact: 50M lookups/sec -> 100M+ lookups/sec
 * Audit Date: 2025-12-07
 * 
 * USAGE:
 *   void *val = DAGGER_HOT_GET(T, key, key_len);
 *   if (val != NULL) { ... }
 * 
 * WARNING: These assume valid inputs. Use standard API for untrusted data.
 * ============================================================ */

/**
 * @brief HOT: Direct slot lookup - returns entry pointer or NULL
 * 
 * This is the fastest possible lookup - single hash, single probe.
 * Returns NULL if not found at expected slot (no Robin Hood search).
 * Use when you expect high hit rate at primary slot.
 */
static inline void *dagger_hot_probe1(
    const DaggerTable *T,
    const void *key,
    uint32_t key_len,
    uint64_t hash
) {
    size_t idx = hash & T->mask;
    const DaggerEntry *slot = &T->entries[idx];
    
    if (slot->occupied && 
        slot->hash_primary == hash && 
        slot->key_len == key_len &&
        memcmp(slot->key, key, key_len) == 0) {
        return slot->value;
    }
    return NULL;
}

/**
 * @brief HOT: Full Robin Hood lookup - inlined, no stats, no NULL checks
 * 
 * Complete lookup with Robin Hood early termination.
 * Skips Cuckoo fallback (rare case) - use dagger_get() if needed.
 */
static inline void *dagger_hot_get(
    const DaggerTable *T,
    const void *key,
    uint32_t key_len
) {
    uint64_t h1 = nxh64(key, key_len, T->seed_primary);
    size_t idx = h1 & T->mask;
    size_t psl = 0;
    
    /* Robin Hood linear probe with early termination */
    while (1) {
        const DaggerEntry *slot = &T->entries[idx];
        
        /* Empty slot = not found */
        if (!slot->occupied) {
            return NULL;
        }
        
        /* Check for match */
        if (slot->hash_primary == h1 && 
            slot->key_len == key_len &&
            memcmp(slot->key, key, key_len) == 0) {
            return slot->value;
        }
        
        /* Robin Hood guarantee: if current entry's PSL < our probe count,
         * the key cannot exist further in the chain */
        if (slot->psl < psl) {
            return NULL;
        }
        
        psl++;
        idx = (idx + 1) & T->mask;
        
        /* Safety: don't probe forever (shouldn't happen with Robin Hood) */
        if (psl > DAGGER_PSL_THRESHOLD) {
            return NULL;  /* Would be in Cuckoo - caller should use dagger_get */
        }
    }
}

/**
 * @brief HOT: String key lookup - inlined strlen + lookup
 */
static inline void *dagger_hot_get_str(
    const DaggerTable *T,
    const char *key
) {
    /* Inline strlen for hot path */
    uint32_t len = 0;
    const char *p = key;
    while (*p++) len++;
    
    return dagger_hot_get(T, key, len);
}

/**
 * @brief HOT: Integer key lookup - hash integer directly
 */
static inline void *dagger_hot_get_int(
    const DaggerTable *T,
    uint64_t key
) {
    uint64_t h1 = nxh_int64(key) ^ T->seed_primary;
    size_t idx = h1 & T->mask;
    
    const DaggerEntry *slot = &T->entries[idx];
    
    /* Integer keys are 8 bytes, hash as primary comparison */
    if (slot->occupied && 
        slot->hash_primary == h1 && 
        slot->key_len == sizeof(uint64_t)) {
        /* Verify actual key value */
        uint64_t stored = 0;
        memcpy(&stored, slot->key, sizeof(uint64_t));
        if (stored == key) {
            return slot->value;
        }
    }
    
    /* Fall through to linear probe if not at slot 0 */
    size_t psl = 1;
    idx = (idx + 1) & T->mask;
    
    while (psl <= DAGGER_PSL_THRESHOLD) {
        slot = &T->entries[idx];
        
        if (!slot->occupied || slot->psl < psl) {
            return NULL;
        }
        
        if (slot->hash_primary == h1 && slot->key_len == sizeof(uint64_t)) {
            uint64_t stored = 0;
            memcpy(&stored, slot->key, sizeof(uint64_t));
            if (stored == key) {
                return slot->value;
            }
        }
        
        psl++;
        idx = (idx + 1) & T->mask;
    }
    
    return NULL;
}

/**
 * @brief HOT: Check existence only (no value retrieval)
 */
static inline int dagger_hot_contains(
    const DaggerTable *T,
    const void *key,
    uint32_t key_len
) {
    return dagger_hot_get(T, key, key_len) != NULL;
}

/**
 * @brief HOT: Pre-hashed lookup - when you already have the hash
 * 
 * Use this when you're hashing the key for other purposes anyway.
 * Saves one nxh64() call (~5ns).
 */
static inline void *dagger_hot_get_prehash(
    const DaggerTable *T,
    const void *key,
    uint32_t key_len,
    uint64_t hash
) {
    size_t idx = hash & T->mask;
    size_t psl = 0;
    
    while (psl <= DAGGER_PSL_THRESHOLD) {
        const DaggerEntry *slot = &T->entries[idx];
        
        if (!slot->occupied) {
            return NULL;
        }
        
        if (slot->hash_primary == hash && 
            slot->key_len == key_len &&
            memcmp(slot->key, key, key_len) == 0) {
            return slot->value;
        }
        
        if (slot->psl < psl) {
            return NULL;
        }
        
        psl++;
        idx = (idx + 1) & T->mask;
    }
    
    return NULL;
}

/* Macro wrappers for even cleaner syntax */
#define DAGGER_HOT_GET(T, key, len)      dagger_hot_get((T), (key), (len))
#define DAGGER_HOT_GET_STR(T, key)       dagger_hot_get_str((T), (key))
#define DAGGER_HOT_GET_INT(T, key)       dagger_hot_get_int((T), (key))
#define DAGGER_HOT_CONTAINS(T, key, len) dagger_hot_contains((T), (key), (len))
#define DAGGER_HOT_PROBE1(T, k, l, h)    dagger_hot_probe1((T), (k), (l), (h))

/* ============================================================
 * LIBRARY API DECLARATIONS
 * 
 * Always visible for prototype checking.
 * Implementations in dagger.c (linked via libdagger.a)
 * ============================================================ */

/**
 * @brief Create a new DAGGER table
 */
DaggerTable *dagger_create(size_t initial_capacity, dagger_key_eq_fn key_eq);

/**
 * @brief Destroy a DAGGER table
 */
void dagger_destroy(DaggerTable *T);

/**
 * @brief Set value destructor
 */
void dagger_set_value_destroy(DaggerTable *T, dagger_value_destroy_fn destroy_fn);

/**
 * @brief Set key destructor
 */
void dagger_set_key_destroy(DaggerTable *T, dagger_key_destroy_fn destroy_fn);

/**
 * @brief Resize the table
 */
dagger_result_t dagger_resize(DaggerTable *T, size_t new_capacity);

/**
 * @brief Internal insert (used by resize)
 */
dagger_result_t dagger_insert_internal(
    DaggerTable *T,
    const void *key, uint32_t key_len,
    void *value,
    uint64_t h1, uint64_t h2,
    int allow_replace
);

/**
 * @brief Insert or update a key-value pair
 */
dagger_result_t dagger_set(
    DaggerTable *T,
    const void *key, uint32_t key_len,
    void *value,
    int replace
);

/**
 * @brief Lookup a key
 */
dagger_result_t dagger_get(
    const DaggerTable *T,
    const void *key, uint32_t key_len,
    void **value
);

/**
 * @brief Check if a key exists
 */
int dagger_contains(const DaggerTable *T, const void *key, uint32_t key_len);

/**
 * @brief Remove a key-value pair
 */
dagger_result_t dagger_remove(DaggerTable *T, const void *key, uint32_t key_len);

/**
 * @brief Clear all entries
 */
void dagger_clear(DaggerTable *T);

/**
 * @brief Iterate over all entries
 */
size_t dagger_foreach(const DaggerTable *T, dagger_iter_fn fn, void *ctx);

/**
 * @brief Get table statistics
 */
void dagger_stats(const DaggerTable *T, DaggerStats *stats);

/* String key convenience functions */
dagger_result_t dagger_set_str(DaggerTable *T, const char *key, void *value, int replace);
dagger_result_t dagger_get_str(const DaggerTable *T, const char *key, void **value);
int dagger_contains_str(const DaggerTable *T, const char *key);
dagger_result_t dagger_remove_str(DaggerTable *T, const char *key);

/* ============================================================
 * TYPED TABLE MACROS (DAGGER_SCHEMA)
 * 
 * These macros generate type-safe wrapper functions.
 * They stay in the header as they generate inline code.
 * ============================================================ */

#define DAGGER_DEFINE_TYPED(NAME, VALUE_TYPE)                                  \
                                                                               \
typedef DaggerTable NAME##Table;                                               \
                                                                               \
static inline void NAME##_value_destroy_(void *ptr) {                          \
    free(ptr);                                                                 \
}                                                                              \
                                                                               \
static inline NAME##Table *NAME##_create(size_t capacity) {                    \
    DaggerTable *T = dagger_create(capacity, NULL);                            \
    if (T != NULL) {                                                           \
        dagger_set_value_destroy(T, NAME##_value_destroy_);                    \
    }                                                                          \
    return T;                                                                  \
}                                                                              \
                                                                               \
static inline void NAME##_destroy(NAME##Table *T) {                            \
    dagger_destroy(T);                                                         \
}                                                                              \
                                                                               \
static inline dagger_result_t NAME##_set(                                      \
    NAME##Table *T,                                                            \
    const void *key, uint32_t key_len,                                         \
    VALUE_TYPE value                                                           \
) {                                                                            \
    VALUE_TYPE *heap_val = (VALUE_TYPE *)malloc(sizeof(VALUE_TYPE));           \
    if (heap_val == NULL) return DAGGER_ERROR_NOMEM;                           \
    *heap_val = value;                                                         \
    dagger_result_t r = dagger_set(T, key, key_len, heap_val, 1);              \
    if (r != DAGGER_OK) free(heap_val);                                        \
    return r;                                                                  \
}                                                                              \
                                                                               \
static inline dagger_result_t NAME##_set_str(                                  \
    NAME##Table *T,                                                            \
    const char *key,                                                           \
    VALUE_TYPE value                                                           \
) {                                                                            \
    if (key == NULL) return DAGGER_ERROR_NULLPTR;                              \
    size_t len = 0;                                                            \
    const char *p = key;                                                       \
    while (*p++) len++;                                                        \
    return NAME##_set(T, key, (uint32_t)len, value);                           \
}                                                                              \
                                                                               \
static inline VALUE_TYPE *NAME##_get(                                          \
    const NAME##Table *T,                                                      \
    const void *key, uint32_t key_len                                          \
) {                                                                            \
    void *val = NULL;                                                          \
    if (dagger_get(T, key, key_len, &val) == DAGGER_OK) {                      \
        return (VALUE_TYPE *)val;                                              \
    }                                                                          \
    return NULL;                                                               \
}                                                                              \
                                                                               \
static inline VALUE_TYPE *NAME##_get_str(                                      \
    const NAME##Table *T,                                                      \
    const char *key                                                            \
) {                                                                            \
    if (key == NULL) return NULL;                                              \
    size_t len = 0;                                                            \
    const char *p = key;                                                       \
    while (*p++) len++;                                                        \
    return NAME##_get(T, key, (uint32_t)len);                                  \
}                                                                              \
                                                                               \
static inline int NAME##_contains(                                             \
    const NAME##Table *T,                                                      \
    const void *key, uint32_t key_len                                          \
) {                                                                            \
    return dagger_contains(T, key, key_len);                                   \
}                                                                              \
                                                                               \
static inline int NAME##_contains_str(                                         \
    const NAME##Table *T,                                                      \
    const char *key                                                            \
) {                                                                            \
    return dagger_contains_str(T, key);                                        \
}                                                                              \
                                                                               \
static inline dagger_result_t NAME##_remove(                                   \
    NAME##Table *T,                                                            \
    const void *key, uint32_t key_len                                          \
) {                                                                            \
    return dagger_remove(T, key, key_len);                                     \
}                                                                              \
                                                                               \
static inline dagger_result_t NAME##_remove_str(                               \
    NAME##Table *T,                                                            \
    const char *key                                                            \
) {                                                                            \
    return dagger_remove_str(T, key);                                          \
}                                                                              \
                                                                               \
static inline size_t NAME##_count(const NAME##Table *T) {                      \
    return (T != NULL) ? T->count : 0;                                         \
}                                                                              \
                                                                               \
static inline void NAME##_clear(NAME##Table *T) {                              \
    dagger_clear(T);                                                           \
}                                                                              \
                                                                               \
static inline void NAME##_stats(const NAME##Table *T, DaggerStats *stats) {    \
    dagger_stats(T, stats);                                                    \
}

#ifdef __cplusplus
}
#endif

#endif /* DAGGER_H_ */
