/**
 * @file nxh.h
 * @brief NXH (Nexus Hash) - ZORYA's High-Performance Hash Function
 * 
 * NXH is a fast, non-cryptographic hash function designed for hash table
 * keys and data integrity checks. It provides:
 * 
 *   - Excellent avalanche properties (single bit change affects ~50% of output)
 *   - High throughput on modern CPUs (~10 GB/s on x86-64)
 *   - 64-bit output with optional 32-bit variant
 *   - Seed support for hash table randomization
 *   - Two independent hash functions for Cuckoo hashing (NXH64 + NXH64_ALT)
 * 
 * WARNING: NXH is NOT cryptographically secure. Do not use for:
 *   - Password hashing
 *   - Digital signatures
 *   - Any security-critical application
 *
 * For cryptographic hashing, use nxhc.h (NXH-Crypto) instead.
 *
 * USAGE:
 *   #include <zorya/nxh.h>
 *   uint64_t hash = nxh64(data, length, NXH_SEED_DEFAULT);
 *
 * LINKING:
 *   gcc myapp.c -lnxh -o myapp
 *   OR for header-only mode: #define NXH_IMPLEMENTATION before include
 *
 * @author Anthony Taliento
 * @date 2026-02-11
 * @version 2.0.0
 * 
 * @copyright Copyright 2026 Zorya Corporation
 * @license Apache-2.0
 */

#ifndef NXH_H_
#define NXH_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * NXH CONSTANTS - The Nexus Primes
 * ============================================================ */

/** Primary mixing prime - Golden ratio derivative */
#define NXH_PRIME_NEXUS   0x9E3779B185EBCA87ULL

/** Bit avalanche catalyst */
#define NXH_PRIME_VOID    0xC2B2AE3D27D4EB4FULL

/** Secondary mixer */
#define NXH_PRIME_ECHO    0x165667B19E3779F9ULL

/** Finalization prime */
#define NXH_PRIME_PULSE   0x85EBCA77C2B2AE63ULL

/** Tail processing prime */
#define NXH_PRIME_DRIFT   0x27D4EB2F165667C5ULL

/** Alternate prime 1 - For second hash function */
#define NXH_PRIME_ALT_1   0x517CC1B727220A95ULL

/** Alternate prime 2 - For second hash function */
#define NXH_PRIME_ALT_2   0x71D67FFFEDA60000ULL

/** Default seed value */
#define NXH_SEED_DEFAULT  0ULL

/** Alternate seed for second hash function */
#define NXH_SEED_ALT      0xDEADBEEFCAFEBABEULL

/* ============================================================
 * VERSION INFO
 * ============================================================ */

#define NXH_VERSION_MAJOR 2
#define NXH_VERSION_MINOR 0
#define NXH_VERSION_PATCH 0
#define NXH_VERSION_STRING "2.0.0"

/* ============================================================
 * INLINE HOT-PATH HELPERS
 * 
 * These stay in the header for maximum performance in hot loops.
 * The compiler can inline these directly into calling code.
 * ============================================================ */

/**
 * @brief Rotate left (64-bit)
 */
static inline uint64_t nxh_rotl64(uint64_t x, int r) {
    return (x << r) | (x >> (64 - r));
}

/**
 * @brief Rotate right (64-bit)
 */
static inline uint64_t nxh_rotr64(uint64_t x, int r) {
    return (x >> r) | (x << (64 - r));
}

/**
 * @brief Read 64-bit value (little-endian, unaligned safe)
 */
static inline uint64_t nxh_read64(const uint8_t *p) {
    return (uint64_t)p[0]        | ((uint64_t)p[1] << 8)  |
           ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

/**
 * @brief Read 32-bit value (little-endian, unaligned safe)
 */
static inline uint32_t nxh_read32(const uint8_t *p) {
    return (uint32_t)p[0]        | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/**
 * @brief The Nexus Mix - Core mixing function (HOT PATH)
 */
static inline uint64_t nxh_mix(uint64_t acc, uint64_t input) {
    acc += input * NXH_PRIME_VOID;
    acc = nxh_rotl64(acc, 31);
    acc *= NXH_PRIME_NEXUS;
    return acc;
}

/**
 * @brief The Nexus Merge - Combines accumulators (HOT PATH)
 */
static inline uint64_t nxh_merge(uint64_t h, uint64_t v) {
    v *= NXH_PRIME_VOID;
    v = nxh_rotl64(v, 31);
    v *= NXH_PRIME_NEXUS;
    h ^= v;
    h = h * NXH_PRIME_NEXUS + NXH_PRIME_PULSE;
    return h;
}

/**
 * @brief Avalanche finalization (HOT PATH)
 */
static inline uint64_t nxh_avalanche(uint64_t h) {
    h ^= h >> 33;
    h *= NXH_PRIME_VOID;
    h ^= h >> 29;
    h *= NXH_PRIME_ECHO;
    h ^= h >> 32;
    return h;
}

/**
 * @brief Alternate mixing function for Cuckoo (HOT PATH)
 */
static inline uint64_t nxh_mix_alt(uint64_t acc, uint64_t input) {
    acc += input * NXH_PRIME_ALT_2;
    acc = nxh_rotl64(acc, 27);
    acc *= NXH_PRIME_ALT_1;
    return acc;
}

/**
 * @brief Alternate avalanche finalization (HOT PATH)
 */
static inline uint64_t nxh_avalanche_alt(uint64_t h) {
    h ^= h >> 31;
    h *= NXH_PRIME_ALT_1;
    h ^= h >> 27;
    h *= NXH_PRIME_ALT_2;
    h ^= h >> 33;
    return h;
}

/* ============================================================
 * LIBRARY API (implemented in nxh.c, linked via libnxh.a)
 * 
 * For header-only mode, define NXH_IMPLEMENTATION before include.
 * ============================================================ */

#ifndef NXH_IMPLEMENTATION

/**
 * @brief NXH64 - Hash arbitrary data to 64-bit value
 * 
 * @param data   Pointer to data to hash (can be NULL if len == 0)
 * @param len    Length of data in bytes
 * @param seed   Hash seed (use NXH_SEED_DEFAULT for standard hashing)
 * @return       64-bit hash value
 */
uint64_t nxh64(const void *data, size_t len, uint64_t seed);

/**
 * @brief NXH64_ALT - Alternate hash for Cuckoo hashing
 * 
 * Produces a statistically independent hash from nxh64().
 * Used as second hash function in DAGGER tables.
 */
uint64_t nxh64_alt(const void *data, size_t len, uint64_t seed);

/**
 * @brief Hash a null-terminated string
 */
uint64_t nxh_string(const char *str);

/**
 * @brief Hash a null-terminated string (alternate)
 */
uint64_t nxh_string_alt(const char *str);

/**
 * @brief Hash a 64-bit integer directly
 */
uint64_t nxh_int64(uint64_t value);

/**
 * @brief Hash a 32-bit integer directly
 */
uint64_t nxh_int32(uint32_t value);

/**
 * @brief Combine two hashes into one
 */
uint64_t nxh_combine(uint64_t h1, uint64_t h2);

/**
 * @brief NXH32 - 32-bit variant
 */
uint32_t nxh32(const void *data, size_t len, uint32_t seed);

/**
 * @brief Hash pointer value
 */
uint64_t nxh_ptr(const void *ptr);

#else /* NXH_IMPLEMENTATION - Header-only mode */

/* ============================================================
 * IMPLEMENTATION (for header-only usage)
 * ============================================================ */

uint64_t nxh64(const void *data, size_t len, uint64_t seed) {
    const uint8_t *p = (const uint8_t *)data;
    const uint8_t *end = p + len;
    uint64_t h64;

    if (len >= 32) {
        const uint8_t *limit = end - 32;
        
        uint64_t v1 = seed + NXH_PRIME_NEXUS + NXH_PRIME_VOID;
        uint64_t v2 = seed + NXH_PRIME_VOID;
        uint64_t v3 = seed;
        uint64_t v4 = seed - NXH_PRIME_NEXUS;

        do {
            v1 = nxh_mix(v1, nxh_read64(p));      p += 8;
            v2 = nxh_mix(v2, nxh_read64(p));      p += 8;
            v3 = nxh_mix(v3, nxh_read64(p));      p += 8;
            v4 = nxh_mix(v4, nxh_read64(p));      p += 8;
        } while (p <= limit);

        h64 = nxh_rotl64(v1, 1) + nxh_rotl64(v2, 7) + 
              nxh_rotl64(v3, 12) + nxh_rotl64(v4, 18);
        
        h64 = nxh_merge(h64, v1);
        h64 = nxh_merge(h64, v2);
        h64 = nxh_merge(h64, v3);
        h64 = nxh_merge(h64, v4);
    } else {
        h64 = seed + NXH_PRIME_DRIFT;
    }

    h64 += (uint64_t)len;

    while (p + 8 <= end) {
        uint64_t k1 = nxh_read64(p);
        k1 *= NXH_PRIME_VOID;
        k1 = nxh_rotl64(k1, 31);
        k1 *= NXH_PRIME_NEXUS;
        h64 ^= k1;
        h64 = nxh_rotl64(h64, 27) * NXH_PRIME_NEXUS + NXH_PRIME_PULSE;
        p += 8;
    }

    if (p + 4 <= end) {
        h64 ^= (uint64_t)nxh_read32(p) * NXH_PRIME_NEXUS;
        h64 = nxh_rotl64(h64, 23) * NXH_PRIME_VOID + NXH_PRIME_ECHO;
        p += 4;
    }

    while (p < end) {
        h64 ^= (uint64_t)(*p) * NXH_PRIME_DRIFT;
        h64 = nxh_rotl64(h64, 11) * NXH_PRIME_NEXUS;
        p++;
    }

    return nxh_avalanche(h64);
}

uint64_t nxh64_alt(const void *data, size_t len, uint64_t seed) {
    const uint8_t *p = (const uint8_t *)data;
    const uint8_t *end = p + len;
    uint64_t h64;

    if (len >= 32) {
        const uint8_t *limit = end - 32;
        
        uint64_t v1 = seed + NXH_PRIME_ALT_1 + NXH_PRIME_ALT_2;
        uint64_t v2 = seed + NXH_PRIME_ALT_2;
        uint64_t v3 = seed;
        uint64_t v4 = seed - NXH_PRIME_ALT_1;

        do {
            v1 = nxh_mix_alt(v1, nxh_read64(p));      p += 8;
            v2 = nxh_mix_alt(v2, nxh_read64(p));      p += 8;
            v3 = nxh_mix_alt(v3, nxh_read64(p));      p += 8;
            v4 = nxh_mix_alt(v4, nxh_read64(p));      p += 8;
        } while (p <= limit);

        h64 = nxh_rotl64(v1, 3) + nxh_rotl64(v2, 11) + 
              nxh_rotl64(v3, 17) + nxh_rotl64(v4, 23);
        
        h64 ^= v1 * NXH_PRIME_ALT_1;
        h64 ^= v2 * NXH_PRIME_ALT_2;
        h64 ^= v3 * NXH_PRIME_ALT_1;
        h64 ^= v4 * NXH_PRIME_ALT_2;
    } else {
        h64 = seed + NXH_PRIME_ALT_1;
    }

    h64 += (uint64_t)len;

    while (p + 8 <= end) {
        uint64_t k1 = nxh_read64(p) * NXH_PRIME_ALT_2;
        k1 = nxh_rotl64(k1, 29);
        k1 *= NXH_PRIME_ALT_1;
        h64 ^= k1;
        h64 = nxh_rotl64(h64, 25) * NXH_PRIME_ALT_1 + NXH_PRIME_ALT_2;
        p += 8;
    }

    if (p + 4 <= end) {
        h64 ^= (uint64_t)nxh_read32(p) * NXH_PRIME_ALT_1;
        h64 = nxh_rotl64(h64, 21) * NXH_PRIME_ALT_2;
        p += 4;
    }

    while (p < end) {
        h64 ^= (uint64_t)(*p) * NXH_PRIME_ALT_2;
        h64 = nxh_rotl64(h64, 13) * NXH_PRIME_ALT_1;
        p++;
    }

    return nxh_avalanche_alt(h64);
}

uint64_t nxh_string(const char *str) {
    if (str == NULL) {
        return nxh_avalanche(NXH_PRIME_NEXUS);
    }
    size_t len = 0;
    const char *p = str;
    while (*p++) len++;
    return nxh64(str, len, NXH_SEED_DEFAULT);
}

uint64_t nxh_string_alt(const char *str) {
    if (str == NULL) {
        return nxh_avalanche_alt(NXH_PRIME_ALT_1);
    }
    size_t len = 0;
    const char *p = str;
    while (*p++) len++;
    return nxh64_alt(str, len, NXH_SEED_ALT);
}

uint64_t nxh_int64(uint64_t value) {
    return nxh_avalanche(value * NXH_PRIME_NEXUS + NXH_PRIME_VOID);
}

uint64_t nxh_int32(uint32_t value) {
    return nxh_avalanche((uint64_t)value * NXH_PRIME_NEXUS + NXH_PRIME_ECHO);
}

uint64_t nxh_combine(uint64_t h1, uint64_t h2) {
    h1 ^= h2 + NXH_PRIME_NEXUS + (h1 << 6) + (h1 >> 2);
    return h1;
}

uint32_t nxh32(const void *data, size_t len, uint32_t seed) {
    uint64_t h = nxh64(data, len, seed);
    return (uint32_t)(h ^ (h >> 32));
}

uint64_t nxh_ptr(const void *ptr) {
    return nxh_int64((uint64_t)(uintptr_t)ptr);
}

#endif /* NXH_IMPLEMENTATION */

#ifdef __cplusplus
}
#endif

#endif /* NXH_H_ */
