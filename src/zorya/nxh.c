/**
 * @file nxh.c
 * @brief NXH (Nexus Hash) - Implementation
 * 
 * Fast, non-cryptographic hash function implementation.
 * Link with: -lnxh
 * 
 * @author Anthony Taliento
 * @date 2025-11-30
 * @version 2.0.0
 * 
 * @copyright Copyright 2025 Zorya Corporation
 * @license Apache-2.0
 */

#include "zorya/nxh.h"

/* ============================================================
 * NXH64 - Primary Hash Function
 * ============================================================ */

uint64_t nxh64(const void *data, size_t len, uint64_t seed) {
    const uint8_t *p = (const uint8_t *)data;
    const uint8_t *end = p + len;
    uint64_t h64 = 0;

    if (data == NULL && len != 0) {
        return nxh_avalanche(seed ^ NXH_PRIME_NEXUS);
    }

    if (len >= 32) {
        /* Large input: use 4 parallel accumulators for ILP */
        const uint8_t *limit = end - 32;
        
        uint64_t v1 = seed + NXH_PRIME_NEXUS + NXH_PRIME_VOID;
        uint64_t v2 = seed + NXH_PRIME_VOID;
        uint64_t v3 = seed;
        uint64_t v4 = seed - NXH_PRIME_NEXUS;

        /* Process 32-byte blocks */
        do {
            v1 = nxh_mix(v1, nxh_read64(p));      p += 8;
            v2 = nxh_mix(v2, nxh_read64(p));      p += 8;
            v3 = nxh_mix(v3, nxh_read64(p));      p += 8;
            v4 = nxh_mix(v4, nxh_read64(p));      p += 8;
        } while (p <= limit);

        /* Merge accumulators with different rotations */
        h64 = nxh_rotl64(v1, 1) + nxh_rotl64(v2, 7) + 
              nxh_rotl64(v3, 12) + nxh_rotl64(v4, 18);
        
        h64 = nxh_merge(h64, v1);
        h64 = nxh_merge(h64, v2);
        h64 = nxh_merge(h64, v3);
        h64 = nxh_merge(h64, v4);
    } else {
        /* Small input: simple initialization */
        h64 = seed + NXH_PRIME_DRIFT;
    }

    /* Add length to prevent length-extension attacks */
    h64 += (uint64_t)len;

    /* Process remaining 8-byte blocks */
    while (p + 8 <= end) {
        uint64_t k1 = nxh_read64(p);
        k1 *= NXH_PRIME_VOID;
        k1 = nxh_rotl64(k1, 31);
        k1 *= NXH_PRIME_NEXUS;
        h64 ^= k1;
        h64 = nxh_rotl64(h64, 27) * NXH_PRIME_NEXUS + NXH_PRIME_PULSE;
        p += 8;
    }

    /* Process remaining 4-byte block */
    if (p + 4 <= end) {
        h64 ^= (uint64_t)nxh_read32(p) * NXH_PRIME_NEXUS;
        h64 = nxh_rotl64(h64, 23) * NXH_PRIME_VOID + NXH_PRIME_ECHO;
        p += 4;
    }

    /* Process remaining bytes individually */
    while (p < end) {
        h64 ^= (uint64_t)(*p) * NXH_PRIME_DRIFT;
        h64 = nxh_rotl64(h64, 11) * NXH_PRIME_NEXUS;
        p++;
    }

    return nxh_avalanche(h64);
}

/* ============================================================
 * NXH64_ALT - Alternate Hash (for Cuckoo/DAGGER)
 * ============================================================ */

uint64_t nxh64_alt(const void *data, size_t len, uint64_t seed) {
    const uint8_t *p = (const uint8_t *)data;
    const uint8_t *end = p + len;
    uint64_t h64 = 0;

    if (data == NULL && len != 0) {
        return nxh_avalanche_alt(seed ^ NXH_PRIME_ALT_1);
    }

    if (len >= 32) {
        const uint8_t *limit = end - 32;
        
        /* Different initialization constants */
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

        /* Different rotation pattern */
        h64 = nxh_rotl64(v1, 3) + nxh_rotl64(v2, 11) + 
              nxh_rotl64(v3, 17) + nxh_rotl64(v4, 23);
        
        /* Simplified merge for alternate */
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

/* ============================================================
 * CONVENIENCE FUNCTIONS
 * ============================================================ */

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
    /* Fold 64-bit to 32-bit with mixing */
    return (uint32_t)(h ^ (h >> 32));
}

uint64_t nxh_ptr(const void *ptr) {
    return nxh_int64((uint64_t)(uintptr_t)ptr);
}
