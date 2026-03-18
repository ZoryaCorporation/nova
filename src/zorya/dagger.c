/**
 * @file dagger.c
 * @brief DAGGER - Implementation file
 * 
 * Core hash table operations for DAGGER.
 * The header contains type definitions and inline helpers.
 * This file contains the heavier operations.
 * 
 * Link with: -ldagger -lnxh
 * 
 * @author Anthony Taliento
 * @date 2025-11-30
 * @version 2.0.0
 * 
 * @copyright Copyright 2025 Zorya Corporation
 * @license Apache-2.0
 */

#include "zorya/dagger.h"

/* ============================================================
 * TABLE LIFECYCLE
 * ============================================================ */

DaggerTable *dagger_create(size_t initial_capacity, dagger_key_eq_fn key_eq) {
    DaggerTable *T = (DaggerTable *)calloc(1, sizeof(DaggerTable));
    if (T == NULL) {
        return NULL;
    }
    
    /* Ensure capacity is power of 2 and >= minimum */
    if (initial_capacity < DAGGER_MIN_CAPACITY) {
        initial_capacity = DAGGER_INITIAL_CAPACITY;
    }
    initial_capacity = dagger_next_pow2(initial_capacity);
    
    T->entries = (DaggerEntry *)calloc(initial_capacity, sizeof(DaggerEntry));
    if (T->entries == NULL) {
        free(T);
        return NULL;
    }
    
    T->capacity = initial_capacity;
    T->mask = initial_capacity - 1;
    T->count = 0;
    
    T->seed_primary = NXH_SEED_DEFAULT;
    T->seed_alternate = NXH_SEED_ALT;
    
    T->key_eq = key_eq ? key_eq : dagger_key_eq_default;
    T->value_destroy = NULL;
    T->key_destroy = NULL;
    
    T->max_psl = 0;
    T->cuckoo_count = 0;
    T->resize_count = 0;
    T->total_probes = 0;
    T->total_lookups = 0;
    
    return T;
}

void dagger_destroy(DaggerTable *T) {
    if (T == NULL) return;
    
    if (T->entries != NULL) {
        /* Call destructors if set */
        if (T->value_destroy != NULL || T->key_destroy != NULL) {
            for (size_t i = 0; i < T->capacity; i++) {
                if (T->entries[i].occupied) {
                    if (T->value_destroy != NULL && T->entries[i].value != NULL) {
                        T->value_destroy(T->entries[i].value);
                    }
                    if (T->key_destroy != NULL && T->entries[i].key != NULL) {
                        T->key_destroy(T->entries[i].key, T->entries[i].key_len);
                    }
                }
            }
        }
        free(T->entries);
    }
    
    free(T);
}

void dagger_set_value_destroy(DaggerTable *T, dagger_value_destroy_fn destroy_fn) {
    if (T != NULL) {
        T->value_destroy = destroy_fn;
    }
}

void dagger_set_key_destroy(DaggerTable *T, dagger_key_destroy_fn destroy_fn) {
    if (T != NULL) {
        T->key_destroy = destroy_fn;
    }
}

/* ============================================================
 * RESIZE OPERATION
 * ============================================================ */

dagger_result_t dagger_resize(DaggerTable *T, size_t new_capacity) {
    if (T == NULL) {
        return DAGGER_ERROR_NULLPTR;
    }
    
    new_capacity = dagger_next_pow2(new_capacity);
    if (new_capacity < DAGGER_MIN_CAPACITY) {
        new_capacity = DAGGER_MIN_CAPACITY;
    }
    
    /* Save old entries */
    DaggerEntry *old_entries = T->entries;
    size_t old_capacity = T->capacity;
    
    /* Allocate new entries */
    T->entries = (DaggerEntry *)calloc(new_capacity, sizeof(DaggerEntry));
    if (T->entries == NULL) {
        T->entries = old_entries;
        return DAGGER_ERROR_NOMEM;
    }
    
    T->capacity = new_capacity;
    T->mask = new_capacity - 1;
    T->count = 0;
    T->max_psl = 0;
    T->cuckoo_count = 0;
    T->resize_count++;
    
    /* Re-insert all entries */
    for (size_t i = 0; i < old_capacity; i++) {
        if (old_entries[i].occupied) {
            dagger_result_t r = dagger_insert_internal(
                T,
                old_entries[i].key,
                old_entries[i].key_len,
                old_entries[i].value,
                old_entries[i].hash_primary,
                old_entries[i].hash_alternate,
                1
            );
            
            if (r != DAGGER_OK) {
                /* Restore old table on failure */
                free(T->entries);
                T->entries = old_entries;
                T->capacity = old_capacity;
                T->mask = old_capacity - 1;
                return r;
            }
        }
    }
    
    free(old_entries);
    return DAGGER_OK;
}

/* ============================================================
 * INTERNAL INSERT
 * ============================================================ */

dagger_result_t dagger_insert_internal(
    DaggerTable *T,
    const void *key, uint32_t key_len,
    void *value,
    uint64_t h1, uint64_t h2,
    int allow_replace
) {
    DaggerEntry entry = {
        .hash_primary = h1,
        .hash_alternate = h2,
        .key = key,
        .value = value,
        .key_len = key_len,
        .psl = 0,
        .occupied = 1,
        .in_cuckoo = 0,
        ._pad = 0
    };
    
    size_t idx = dagger_slot(T, h1);
    size_t cuckoo_cycles = 0;
    int in_cuckoo_phase = 0;
    
    while (1) {
        DaggerEntry *slot = &T->entries[idx];
        
        /* Empty slot - insert here */
        if (!slot->occupied) {
            *slot = entry;
            slot->in_cuckoo = (uint8_t)in_cuckoo_phase;
            T->count++;
            if (in_cuckoo_phase) T->cuckoo_count++;
            if (entry.psl > T->max_psl) T->max_psl = entry.psl;
            return DAGGER_OK;
        }
        
        /* Key exists? */
        if (slot->hash_primary == h1 && slot->key_len == key_len) {
            if (T->key_eq(slot->key, slot->key_len, key, key_len)) {
                if (allow_replace) {
                    /* Destroy old value */
                    if (T->value_destroy != NULL && slot->value != NULL) {
                        T->value_destroy(slot->value);
                    }
                    /* Destroy old key and replace with new key */
                    if (T->key_destroy != NULL && slot->key != NULL) {
                        T->key_destroy(slot->key, slot->key_len);
                    }
                    slot->key = key;
                    slot->key_len = key_len;
                    slot->value = value;
                    return DAGGER_OK;
                }
                return DAGGER_EXISTS;
            }
        }
        
        /* Robin Hood: steal from richer entry */
        if (entry.psl > slot->psl) {
            /* Swap entries */
            DaggerEntry tmp = *slot;
            *slot = entry;
            slot->in_cuckoo = (uint8_t)in_cuckoo_phase;
            entry = tmp;
        }
        
        /* Advance probe */
        entry.psl++;
        idx = (idx + 1) & T->mask;
        
        /* Check if we need to switch to Cuckoo */
        if (!in_cuckoo_phase && entry.psl > DAGGER_PSL_THRESHOLD) {
            /* Switch to Cuckoo: use alternate hash */
            in_cuckoo_phase = 1;
            entry.psl = 0;
            idx = dagger_slot(T, entry.hash_alternate);
            cuckoo_cycles = 0;
        }
        
        /* Check Cuckoo cycle limit */
        if (in_cuckoo_phase) {
            cuckoo_cycles++;
            if (cuckoo_cycles > DAGGER_MAX_CUCKOO_CYCLES) {
                return DAGGER_ERROR_FULL;  /* Need resize */
            }
        }
    }
}

/* ============================================================
 * CORE OPERATIONS
 * ============================================================ */

dagger_result_t dagger_set(
    DaggerTable *T,
    const void *key, uint32_t key_len,
    void *value,
    int replace
) {
    if (T == NULL || key == NULL) {
        return DAGGER_ERROR_NULLPTR;
    }
    if (key_len == 0) {
        return DAGGER_ERROR_INVALID;
    }
    
    /* Check if resize needed */
    size_t threshold = (T->capacity * DAGGER_LOAD_FACTOR_PERCENT) / 100;
    if (T->count >= threshold) {
        dagger_result_t r = dagger_resize(T, T->capacity * DAGGER_GROWTH_FACTOR);
        if (r != DAGGER_OK) {
            return r;
        }
    }
    
    /* Compute both hashes */
    uint64_t h1 = nxh64(key, key_len, T->seed_primary);
    uint64_t h2 = nxh64_alt(key, key_len, T->seed_alternate);
    
    dagger_result_t r = dagger_insert_internal(T, key, key_len, value, h1, h2, replace);
    
    /* If insert failed due to table full, resize and retry */
    if (r == DAGGER_ERROR_FULL) {
        r = dagger_resize(T, T->capacity * DAGGER_GROWTH_FACTOR);
        if (r != DAGGER_OK) return r;
        
        /* Recompute hashes with potentially new seeds */
        h1 = nxh64(key, key_len, T->seed_primary);
        h2 = nxh64_alt(key, key_len, T->seed_alternate);
        r = dagger_insert_internal(T, key, key_len, value, h1, h2, replace);
    }
    
    return r;
}

dagger_result_t dagger_get(
    const DaggerTable *T,
    const void *key, uint32_t key_len,
    void **value
) {
    if (T == NULL || key == NULL) {
        return DAGGER_ERROR_NULLPTR;
    }
    if (key_len == 0) {
        return DAGGER_ERROR_INVALID;
    }
    
    /* Increment lookup counter (cast away const for stats) */
    ((DaggerTable *)T)->total_lookups++;
    
    uint64_t h1 = nxh64(key, key_len, T->seed_primary);
    size_t idx = dagger_slot(T, h1);
    size_t probes = 0;
    
    /* Phase 1: Robin Hood linear probing */
    while (1) {
        const DaggerEntry *slot = &T->entries[idx];
        probes++;
        
        if (!slot->occupied) {
            /* Empty slot - not found */
            ((DaggerTable *)T)->total_probes += probes;
            return DAGGER_NOT_FOUND;
        }
        
        if (slot->hash_primary == h1 && slot->key_len == key_len) {
            if (T->key_eq(slot->key, slot->key_len, key, key_len)) {
                /* Found! */
                if (value != NULL) {
                    *value = slot->value;
                }
                ((DaggerTable *)T)->total_probes += probes;
                return DAGGER_OK;
            }
        }
        
        /* If current entry's PSL is less than our probe count,
         * the key doesn't exist (Robin Hood guarantee) */
        if (slot->psl < probes - 1) {
            break;
        }
        
        if (probes > DAGGER_PSL_THRESHOLD + 1) {
            break;  /* Switch to Cuckoo lookup */
        }
        
        idx = (idx + 1) & T->mask;
    }
    
    /* Phase 2: Check Cuckoo alternate location */
    uint64_t h2 = nxh64_alt(key, key_len, T->seed_alternate);
    idx = dagger_slot(T, h2);
    
    for (size_t i = 0; i <= DAGGER_PSL_THRESHOLD; i++) {
        const DaggerEntry *slot = &T->entries[idx];
        probes++;
        
        if (!slot->occupied) {
            ((DaggerTable *)T)->total_probes += probes;
            return DAGGER_NOT_FOUND;
        }
        
        if (slot->in_cuckoo && slot->hash_alternate == h2 && slot->key_len == key_len) {
            if (T->key_eq(slot->key, slot->key_len, key, key_len)) {
                if (value != NULL) {
                    *value = slot->value;
                }
                ((DaggerTable *)T)->total_probes += probes;
                return DAGGER_OK;
            }
        }
        
        idx = (idx + 1) & T->mask;
    }
    
    ((DaggerTable *)T)->total_probes += probes;
    return DAGGER_NOT_FOUND;
}

int dagger_contains(const DaggerTable *T, const void *key, uint32_t key_len) {
    return dagger_get(T, key, key_len, NULL) == DAGGER_OK ? 1 : 0;
}

dagger_result_t dagger_remove(DaggerTable *T, const void *key, uint32_t key_len) {
    if (T == NULL || key == NULL) {
        return DAGGER_ERROR_NULLPTR;
    }
    if (key_len == 0) {
        return DAGGER_ERROR_INVALID;
    }
    
    uint64_t h1 = nxh64(key, key_len, T->seed_primary);
    size_t idx = dagger_slot(T, h1);
    size_t probes = 0;
    
    /* Find the entry */
    int found = 0;
    int in_cuckoo = 0;
    
    /* Phase 1: Robin Hood search */
    while (probes <= DAGGER_PSL_THRESHOLD) {
        DaggerEntry *slot = &T->entries[idx];
        probes++;
        
        if (!slot->occupied) {
            break;
        }
        
        if (slot->hash_primary == h1 && slot->key_len == key_len) {
            if (T->key_eq(slot->key, slot->key_len, key, key_len)) {
                found = 1;
                in_cuckoo = slot->in_cuckoo;
                break;
            }
        }
        
        if (slot->psl < probes - 1) {
            break;
        }
        
        idx = (idx + 1) & T->mask;
    }
    
    /* Phase 2: Check Cuckoo if not found */
    if (!found) {
        uint64_t h2 = nxh64_alt(key, key_len, T->seed_alternate);
        idx = dagger_slot(T, h2);
        
        for (size_t i = 0; i <= DAGGER_PSL_THRESHOLD && !found; i++) {
            DaggerEntry *slot = &T->entries[idx];
            
            if (!slot->occupied) {
                break;
            }
            
            if (slot->in_cuckoo && slot->hash_alternate == h2 && slot->key_len == key_len) {
                if (T->key_eq(slot->key, slot->key_len, key, key_len)) {
                    found = 1;
                    in_cuckoo = 1;
                    break;
                }
            }
            
            idx = (idx + 1) & T->mask;
        }
    }
    
    if (!found) {
        return DAGGER_NOT_FOUND;
    }
    
    /* Call destructors */
    DaggerEntry *slot = &T->entries[idx];
    if (T->value_destroy != NULL && slot->value != NULL) {
        T->value_destroy(slot->value);
    }
    if (T->key_destroy != NULL && slot->key != NULL) {
        T->key_destroy(slot->key, slot->key_len);
    }
    
    /* Mark as empty */
    memset(slot, 0, sizeof(DaggerEntry));
    T->count--;
    if (in_cuckoo) T->cuckoo_count--;
    
    /* Backward shift deletion to maintain Robin Hood invariant */
    size_t next_idx = (idx + 1) & T->mask;
    while (T->entries[next_idx].occupied && T->entries[next_idx].psl > 0) {
        T->entries[idx] = T->entries[next_idx];
        T->entries[idx].psl--;
        
        memset(&T->entries[next_idx], 0, sizeof(DaggerEntry));
        
        idx = next_idx;
        next_idx = (next_idx + 1) & T->mask;
    }
    
    return DAGGER_OK;
}

void dagger_clear(DaggerTable *T) {
    if (T == NULL) return;
    
    /* Call destructors if set */
    if (T->value_destroy != NULL || T->key_destroy != NULL) {
        for (size_t i = 0; i < T->capacity; i++) {
            if (T->entries[i].occupied) {
                if (T->value_destroy != NULL && T->entries[i].value != NULL) {
                    T->value_destroy(T->entries[i].value);
                }
                if (T->key_destroy != NULL && T->entries[i].key != NULL) {
                    T->key_destroy(T->entries[i].key, T->entries[i].key_len);
                }
            }
        }
    }
    
    memset(T->entries, 0, T->capacity * sizeof(DaggerEntry));
    T->count = 0;
    T->max_psl = 0;
    T->cuckoo_count = 0;
}

/* ============================================================
 * ITERATION
 * ============================================================ */

size_t dagger_foreach(const DaggerTable *T, dagger_iter_fn fn, void *ctx) {
    if (T == NULL || fn == NULL) return 0;
    
    size_t count = 0;
    for (size_t i = 0; i < T->capacity; i++) {
        if (T->entries[i].occupied) {
            count++;
            if (fn(T->entries[i].key, T->entries[i].key_len, 
                   T->entries[i].value, ctx) != 0) {
                break;
            }
        }
    }
    return count;
}

/* ============================================================
 * STATISTICS
 * ============================================================ */

void dagger_stats(const DaggerTable *T, DaggerStats *stats) {
    if (T == NULL || stats == NULL) return;
    
    stats->capacity = T->capacity;
    stats->count = T->count;
    stats->max_psl = T->max_psl;
    stats->cuckoo_count = T->cuckoo_count;
    stats->resize_count = T->resize_count;
    stats->load_factor = T->capacity > 0 ? 
        (double)T->count / (double)T->capacity : 0.0;
    stats->avg_probes = T->total_lookups > 0 ?
        (double)T->total_probes / (double)T->total_lookups : 0.0;
}

/* ============================================================
 * STRING KEY CONVENIENCE FUNCTIONS
 * ============================================================ */

dagger_result_t dagger_set_str(DaggerTable *T, const char *key, void *value, int replace) {
    if (key == NULL) return DAGGER_ERROR_NULLPTR;
    size_t len = 0;
    const char *p = key;
    while (*p++) len++;
    return dagger_set(T, key, (uint32_t)len, value, replace);
}

dagger_result_t dagger_get_str(const DaggerTable *T, const char *key, void **value) {
    if (key == NULL) return DAGGER_ERROR_NULLPTR;
    size_t len = 0;
    const char *p = key;
    while (*p++) len++;
    return dagger_get(T, key, (uint32_t)len, value);
}

int dagger_contains_str(const DaggerTable *T, const char *key) {
    return dagger_get_str(T, key, NULL) == DAGGER_OK ? 1 : 0;
}

dagger_result_t dagger_remove_str(DaggerTable *T, const char *key) {
    if (key == NULL) return DAGGER_ERROR_NULLPTR;
    size_t len = 0;
    const char *p = key;
    while (*p++) len++;
    return dagger_remove(T, key, (uint32_t)len);
}
