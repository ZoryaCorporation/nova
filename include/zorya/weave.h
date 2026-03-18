/**
 * @file weave.h
 * @brief WEAVE - The Third Sister: Zorya Corporation's String Library
 *
 * @author Anthony Taliento
 * @date 2025-12-18
 * @version 1.0.0
 *
 * @copyright Copyright (c) 2025 Zorya Corporation
 * @license MIT
 *
 * DESCRIPTION:
 *   WEAVE is the string component of Zorya's foundational trinity:
 *   - NXH:    The hasher (bytes -> identity)
 *   - DAGGER: The retriever (identity -> value)
 *   - WEAVE:  The weaver (string construction and manipulation)
 *
 *   Together these three form a complete foundation for any system
 *   requiring high-performance string and data handling.
 *
 * DESIGN PHILOSOPHY:
 *   "UNIX is basically a simple operating system, but you have to be
 *    a genius to understand the simplicity." - Dennis Ritchie
 *
 *   Weave solves the hard problem of strings in C, then makes it simple:
 *   - Fat strings with O(1) length
 *   - Interned strings for O(1) comparison
 *   - Deferred concatenation to avoid malloc churn
 *   - Encoding-agnostic (bytes, not codepoints)
 *   - Zero-copy where possible, single-copy where necessary
 *
 * COMPONENTS:
 *   Weave  - Mutable fat string (len + cap + data)
 *   Tablet - Interned string pool (backed by DAGGER)
 *   Cord   - Deferred concatenation (rope-lite)
 *
 * NAMING ETYMOLOGY:
 *   PIE *webh- "to weave, to braid" -> womb, weft, web, weave
 *   Tablet: The cards with holes used in tablet weaving
 *   Cord: A bundle of threads woven together
 *
 * NOTE:
 *   I am aware that PIE is an 'academic' conlang at best by ignoring Basque, circumpolar, and Uralic languages.
 *   If we are going to hypothesize that PIE was spoken as far back as 7000 - 8000BCE, which places it in the neolithic time period.
 *   That is 10,000 - 12,000 years ago, therefore the PIE language model which is primarily based on comparative linguistics
 *   favoring grecco-roman languages heavily over other languages. The problem here? The hellenistic and roman peoples as we know them didn't exist in the neolithic time period.
 *   So we are with PIE using very young languages. To give you a quick analogy, that's like building a house with trees that haven't even sprouted yet.
 *   I'm well aware tha this has nothing to do with the actual code implentations. This is my small contribution to the field of history, anthropology, and linguistics.
 *   In the off chance that any lingustic researchers ever read this, please devote resources and urge your colleagues
 *   to research and study lingustic substrata and fossils present in various living, and dead languages!
 *
 * ADDTIONALLY:
 *   Any researchers that are interested in any of Zorya Corporation's work I will personally and happily work with you to further your field of study.
 *   If you are a linguist, historian, anthropologist, (or studying any field at all for that matter), I and Zorya will always stand with, support and collaborate
 *   with you or your team to in any way that I can.
 *   Get in contact with me and we can work together!
 *
 *
 * THREAD SAFETY:
 *   Not thread-safe. Use external synchronization or per-thread instances.
 *
 * DEPENDENCIES:
 *   - nxh.h (hashing)
 *   - dagger.h (Tablet's backing store)
 *   - zorya_arena.h (Tablet's memory pool)
 *
 * USAGE:
 *   // Simple string operations
 *   Weave *w = weave_new("hello");
 *   weave_append(w, " world");
 *   printf("%s\n", weave_cstr(w));  // "hello world"
 *   weave_free(w);
 *
 *   // Interned strings (O(1) comparison)
 *   Tablet *T = tablet_create();
 *   const Weave *a = tablet_intern(T, "config");
 *   const Weave *b = tablet_intern(T, "config");
 *   assert(a == b);  // Same pointer!
 *   tablet_destroy(T);
 *
 *   // Deferred concatenation
 *   Cord *c = cord_new();
 *   cord_append(c, "gcc ");
 *   cord_append(c, "-Wall ");
 *   cord_append(c, "-o main");
 *   Weave *cmd = cord_to_weave(c);  // Single allocation
 *   cord_free(c);
 */

#ifndef WEAVE_H
#define WEAVE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * VERSION
 * ============================================================ */

#define WEAVE_VERSION_MAJOR 1
#define WEAVE_VERSION_MINOR 0
#define WEAVE_VERSION_PATCH 0
#define WEAVE_VERSION_STRING "1.0.0"

/* ============================================================
 * CONFIGURATION
 * ============================================================ */

/**
 * Initial capacity for new Weave strings.
 * Strings smaller than this won't trigger reallocation on first append.
 */
#ifndef WEAVE_INITIAL_CAP
#define WEAVE_INITIAL_CAP 32
#endif

/**
 * Growth factor numerator/denominator (default 3/2 = 1.5x growth).
 * Using integer math to avoid floating point.
 */
#ifndef WEAVE_GROW_NUM
#define WEAVE_GROW_NUM 3
#endif
#ifndef WEAVE_GROW_DEN
#define WEAVE_GROW_DEN 2
#endif

/**
 * Initial chunk capacity for Cord.
 */
#ifndef CORD_INITIAL_CHUNKS
#define CORD_INITIAL_CHUNKS 8
#endif

/**
 * Initial bucket count for Tablet's hash table.
 */
#ifndef TABLET_INITIAL_BUCKETS
#define TABLET_INITIAL_BUCKETS 256
#endif

/* ============================================================
 * PERFORMANCE PRIMES - The Weaving Constants
 * ============================================================
 *
 * These primes are carefully selected for string operations and
 * validated to avoid collision with NXH primes (≥20 bit XOR distance).
 *
 * Each prime serves a specific purpose in WEAVE's algorithms:
 * - STITCH: String concatenation mixing
 * - THREAD: Cord deferred concatenation
 * - PATTERN: Pattern matching and search
 * - INTERN: Tablet string interning
 * - SCAN: Byte-level scanning operations
 *
 * Selection criteria (matching NXH methodology):
 * 1. Large 64-bit values (avoid short cycles)
 * 2. Good bit distribution (Hamming weight 28-40)
 * 3. Odd numbers (required for modular arithmetic)
 * 4. Low XOR collision with NXH primes (≥20 bits different)
 * 5. Good avalanche properties when rotated/XORed
 *
 * CRITICAL: Do not modify WITHOUT VERIFYING COLLISION RESISTANCE!
 *           These primes WILL NOT BE CHANGED.
 *
 * Collision validation: 2025-12-21
 * - STITCH:  27+ bit XOR distance from NXH primes ✓
 * - THREAD:  22+ bit XOR distance (actual prime) ✓
 * - PATTERN: 22+ bit XOR distance ✓
 * - INTERN:  21+ bit XOR distance ✓
 * - SCAN:    25+ bit XOR distance ✓
 */

/** String concatenation mixing prime */
#define WEAVE_PRIME_STITCH   0xA54FF53A5F1D36F1ULL

/** Cord deferred concatenation prime (verified prime) */
#define WEAVE_PRIME_THREAD   0xBD3AF235E7B4ECF7ULL

/** Pattern matching and substring search prime */
#define WEAVE_PRIME_PATTERN  0xD1B54A32D192ED57ULL

/** Tablet string interning prime */
#define WEAVE_PRIME_INTERN   0xE95C90F7B3A64C8BULL

/** Byte scanning operations prime */
#define WEAVE_PRIME_SCAN     0xF4B3C8A6E1D79265ULL

/* ============================================================
 * PCM INTEGRATION - Performance Critical Macros
 * ============================================================
 *
 * WEAVE inherits optimizations from PCM for hot-path operations:
 * - Branch prediction hints (LIKELY/UNLIKELY)
 * - Compiler intrinsics (INLINE, HOT, COLD)
 * - Bit manipulation (ROTL64, POPCOUNT, CLZ)
 * - Memory hints (PREFETCH, ALIGNED)
 *
 * This gives WEAVE access to the same CPU-level optimizations
 * that make NXH and DAGGER fast.
 */

#include "zorya/pcm.h"

/* Hot-path function markers (no static - let caller decide) */
#define WEAVE_INLINE      inline
#define WEAVE_HOT         ZORYA_HOT
#define WEAVE_COLD        ZORYA_COLD
#define WEAVE_PURE        ZORYA_PURE

/* Branch prediction (use on common cases) */
#define WEAVE_LIKELY(x)   ZORYA_LIKELY(x)
#define WEAVE_UNLIKELY(x) ZORYA_UNLIKELY(x)

/* Bit operations for hash mixing */
#define WEAVE_ROTL64(x, n) ZORYA_ROTL64(x, n)
#define WEAVE_ROTR64(x, n) ZORYA_ROTR64(x, n)
#define WEAVE_POPCOUNT(x)  ZORYA_POPCOUNT64(x)
#define WEAVE_CLZ(x)       ZORYA_CLZ64(x)
#define WEAVE_CTZ(x)       ZORYA_CTZ64(x)

/* Memory prefetch for sequential scanning */
#define WEAVE_PREFETCH(addr)       ZORYA_PREFETCH(addr)
#define WEAVE_PREFETCH_WRITE(addr) ZORYA_PREFETCH(addr)

/* Alignment for cache-friendly allocations */
#define WEAVE_ALIGNED(n)  ZORYA_ALIGNED(n)
#define WEAVE_ALIGN_UP(x, a) ZORYA_ALIGN_UP(x, a)

/* Power-of-2 capacity optimization */
#define WEAVE_NEXT_POW2(x) ZORYA_NEXT_POWER_OF_2_64(x)
#define WEAVE_IS_POW2(x)   ZORYA_IS_POWER_OF_2(x)

/* Unreachable code elimination */
#define WEAVE_UNREACHABLE() ZORYA_UNREACHABLE()

/* ============================================================
 * FORWARD DECLARATIONS
 * ============================================================ */

typedef struct Weave Weave;
typedef struct Tablet Tablet;
typedef struct Cord Cord;

/* ============================================================
 * WEAVE - Mutable Fat String
 * ============================================================
 *
 * A Weave is a length-prefixed, capacity-tracked, NUL-terminated
 * string. It knows its length (O(1)), knows its capacity, and
 * can grow efficiently.
 *
 * Memory layout:
 *   +-------+-------+-------+-------------------+
 *   |  len  |  cap  | flags |    data[cap]      |
 *   +-------+-------+-------+-------------------+
 *                           ^
 *                           Always NUL-terminated at data[len]
 *
 * The data[] is a flexible array member, so the entire Weave
 * is a single allocation.
 */

/* Weave flags */
#define WEAVE_FLAG_NONE      0x00
#define WEAVE_FLAG_INTERNED  0x01  /* Owned by Tablet, do not free */
#define WEAVE_FLAG_READONLY  0x02  /* Mutation prohibited */
#define WEAVE_FLAG_STATIC    0x04  /* Points to static data */

/* ------------------------------------------------------------
 * Creation and Destruction
 * ------------------------------------------------------------ */

/**
 * @brief Create a new Weave from a NUL-terminated C string
 *
 * @param Source string (NULL creates empty Weave)
 * @return New Weave or NULL on allocation failure
 *
 * @example
 *   Weave *w = weave_new("hello");
 *   // Use w...
 *   weave_free(w);
 */
Weave* weave_new(const char *s);

/**
 * @brief Create a new Weave from bytes (may contain NUL)
 *
 * @param s   Source bytes (NULL with len=0 creates empty Weave)
 * @param len Number of bytes to copy
 * @return New Weave or NULL on allocation failure
 */
Weave* weave_new_len(const char *s, size_t len);

/**
 * @brief Create an empty Weave with pre-allocated capacity
 *
 * @param cap Initial capacity (will be at least WEAVE_INITIAL_CAP)
 * @return New empty Weave or NULL on allocation failure
 *
 * Use this when you know approximately how much you'll append.
 */
Weave* weave_with_cap(size_t cap);

/**
 * @brief Create a deep copy of a Weave
 *
 * @param w Source Weave (NULL returns NULL)
 * @return New Weave with same content, or NULL on failure
 */
Weave* weave_dup(const Weave *w);

/**
 * @brief Free a Weave
 *
 * @param w Weave to free (NULL is safe)
 *
 * @note Does nothing if w is interned (owned by Tablet)
 */
void weave_free(Weave *w);

/* ------------------------------------------------------------
 * Access - All O(1)
 * ------------------------------------------------------------ */

/**
 * @brief Get the underlying C string (NUL-terminated)
 *
 * @param w Weave to access
 * @return Pointer to string data, or "" if w is NULL
 *
 * The returned pointer is valid until the Weave is mutated or freed.
 */
const char* weave_cstr(const Weave *w);

/**
 * @brief Get the byte length (not including NUL terminator)
 *
 * @param w Weave to query
 * @return Length in bytes, or 0 if w is NULL
 */
size_t weave_len(const Weave *w);

/**
 * @brief Get the allocated capacity
 *
 * @param w Weave to query
 * @return Capacity in bytes, or 0 if w is NULL
 */
size_t weave_cap(const Weave *w);

/**
 * @brief Check if Weave is empty
 *
 * @param w Weave to check
 * @return true if NULL or zero-length
 */
bool weave_empty(const Weave *w);

/**
 * @brief Get character at index (bounds-checked)
 *
 * @param w Weave to access
 * @param i Index (0-based)
 * @return Character at index, or '\0' if out of bounds
 */
char weave_at(const Weave *w, size_t i);

/**
 * @brief Get Weave flags
 *
 * @param w Weave to query
 * @return Flags bitmask
 */
uint8_t weave_flags(const Weave *w);

/**
 * @brief Check if Weave is interned
 *
 * @param w Weave to check
 * @return true if owned by a Tablet
 */
bool weave_is_interned(const Weave *w);

/* ------------------------------------------------------------
 * Mutation - Returns self for chaining
 * ------------------------------------------------------------ */

/**
 * @brief Append a C string
 *
 * @param w Weave to modify
 * @param s String to append (NULL appends nothing)
 * @return w, or NULL if reallocation failed
 *
 * @example
 *   weave_append(weave_append(w, "hello"), " world");
 */
Weave* weave_append(Weave *w, const char *s);

/**
 * @brief Append bytes
 *
 * @param w   Weave to modify
 * @param s   Bytes to append
 * @param len Number of bytes
 * @return w, or NULL if reallocation failed
 */
Weave* weave_append_len(Weave *w, const char *s, size_t len);

/**
 * @brief Append a single character
 *
 * @param w Weave to modify
 * @param c Character to append
 * @return w, or NULL if reallocation failed
 */
Weave* weave_append_char(Weave *w, char c);

/**
 * @brief Append another Weave
 *
 * @param w     Weave to modify
 * @param other Weave to append (NULL appends nothing)
 * @return w, or NULL if reallocation failed
 */
Weave* weave_append_weave(Weave *w, const Weave *other);

/**
 * @brief Prepend a C string
 *
 * @param w Weave to modify
 * @param s String to prepend (NULL prepends nothing)
 * @return w, or NULL if reallocation failed
 *
 * @note O(n) operation - shifts existing content
 */
Weave* weave_prepend(Weave *w, const char *s);

/**
 * @brief Prepend bytes
 *
 * @param w   Weave to modify
 * @param s   Bytes to prepend
 * @param len Number of bytes
 * @return w, or NULL if reallocation failed
 */
Weave* weave_prepend_len(Weave *w, const char *s, size_t len);

/**
 * @brief Clear content (keep capacity)
 *
 * @param w Weave to clear
 */
void weave_clear(Weave *w);

/**
 * @brief Truncate to specified length
 *
 * @param w   Weave to modify
 * @param len New length (if >= current, no change)
 */
void weave_truncate(Weave *w, size_t len);

/**
 * @brief Ensure capacity for future appends
 *
 * @param w       Weave to modify
 * @param min_cap Minimum capacity needed
 * @return w, or NULL if reallocation failed
 */
Weave* weave_reserve(Weave *w, size_t min_cap);

/**
 * @brief Shrink capacity to fit content
 *
 * @param w Weave to modify
 * @return w (may be reallocated), or NULL on failure
 */
Weave* weave_shrink(Weave *w);

/* ------------------------------------------------------------
 * Operations - Return NEW Weave (caller frees)
 * ------------------------------------------------------------ */

/**
 * @brief Extract substring
 *
 * @param w     Source Weave
 * @param start Start index (0-based)
 * @param len   Maximum length to extract
 * @return New Weave with substring, or empty if out of range
 */
Weave* weave_substr(const Weave *w, size_t start, size_t len);

/**
 * @brief Extract substring from start to end
 *
 * @param w     Source Weave
 * @param start Start index (inclusive)
 * @param end   End index (exclusive, -1 means end of string)
 * @return New Weave with substring
 */
Weave* weave_slice(const Weave *w, size_t start, int64_t end);

/**
 * @brief Replace first occurrence
 *
 * @param w     Source Weave
 * @param old   String to find
 * @param new_s Replacement string
 * @return New Weave with replacement, or dup if not found
 */
Weave* weave_replace(const Weave *w, const char *old, const char *new_s);

/**
 * @brief Replace all occurrences
 *
 * @param w     Source Weave
 * @param old   String to find
 * @param new_s Replacement string
 * @return New Weave with all replacements
 */
Weave* weave_replace_all(const Weave *w, const char *old, const char *new_s);

/**
 * @brief Trim leading and trailing whitespace
 *
 * @param w Source Weave
 * @return New trimmed Weave
 */
Weave* weave_trim(const Weave *w);

/**
 * @brief Trim specified characters from both ends
 *
 * @param w     Source Weave
 * @param chars Characters to trim
 * @return New trimmed Weave
 */
Weave* weave_trim_chars(const Weave *w, const char *chars);

/**
 * @brief Trim leading characters
 *
 * @param w     Source Weave
 * @param chars Characters to trim (NULL for whitespace)
 * @return New trimmed Weave
 */
Weave* weave_trim_left(const Weave *w, const char *chars);

/**
 * @brief Trim trailing characters
 *
 * @param w     Source Weave
 * @param chars Characters to trim (NULL for whitespace)
 * @return New trimmed Weave
 */
Weave* weave_trim_right(const Weave *w, const char *chars);

/**
 * @brief Convert to uppercase (ASCII only)
 *
 * @param w Source Weave
 * @return New uppercase Weave
 */
Weave* weave_to_upper(const Weave *w);

/**
 * @brief Convert to lowercase (ASCII only)
 *
 * @param w Source Weave
 * @return New lowercase Weave
 */
Weave* weave_to_lower(const Weave *w);

/**
 * @brief Repeat string n times
 *
 * @param w Source Weave
 * @param n Number of repetitions
 * @return New Weave with repeated content
 */
Weave* weave_repeat(const Weave *w, size_t n);

/**
 * @brief Reverse string (byte-level, not Unicode-aware)
 *
 * @param w Source Weave
 * @return New reversed Weave
 */
Weave* weave_reverse(const Weave *w);

/* ------------------------------------------------------------
 * Search
 * ------------------------------------------------------------ */

/**
 * @brief Find first occurrence of substring
 *
 * @param w      Weave to search
 * @param needle String to find
 * @return Index of first match, or -1 if not found
 */
int64_t weave_find(const Weave *w, const char *needle);

/**
 * @brief Find first occurrence starting from offset
 *
 * @param w      Weave to search
 * @param needle String to find
 * @param start  Start index
 * @return Index of match, or -1 if not found
 */
int64_t weave_find_from(const Weave *w, const char *needle, size_t start);

/**
 * @brief Find last occurrence of substring
 *
 * @param w      Weave to search
 * @param needle String to find
 * @return Index of last match, or -1 if not found
 */
int64_t weave_rfind(const Weave *w, const char *needle);

/**
 * @brief Find first occurrence of character
 *
 * @param w Weave to search
 * @param c Character to find
 * @return Index of first match, or -1 if not found
 */
int64_t weave_find_char(const Weave *w, char c);

/**
 * @brief Find last occurrence of character
 *
 * @param w Weave to search
 * @param c Character to find
 * @return Index of last match, or -1 if not found
 */
int64_t weave_rfind_char(const Weave *w, char c);

/**
 * @brief Find first character from set
 *
 * @param w     Weave to search
 * @param chars Set of characters to find
 * @return Index of first match, or -1 if none found
 */
int64_t weave_find_any(const Weave *w, const char *chars);

/**
 * @brief Find first character NOT in set
 *
 * @param w     Weave to search
 * @param chars Set of characters to skip
 * @return Index of first non-matching char, or -1 if all match
 */
int64_t weave_find_not(const Weave *w, const char *chars);

/**
 * @brief Check if contains substring
 *
 * @param w      Weave to search
 * @param needle String to find
 * @return true if found
 */
bool weave_contains(const Weave *w, const char *needle);

/**
 * @brief Check if starts with prefix
 *
 * @param w      Weave to check
 * @param prefix Prefix to match
 * @return true if w starts with prefix
 */
bool weave_starts_with(const Weave *w, const char *prefix);

/**
 * @brief Check if ends with suffix
 *
 * @param w      Weave to check
 * @param suffix Suffix to match
 * @return true if w ends with suffix
 */
bool weave_ends_with(const Weave *w, const char *suffix);

/**
 * @brief Count occurrences of substring
 *
 * @param w      Weave to search
 * @param needle String to count
 * @return Number of non-overlapping occurrences
 */
size_t weave_count(const Weave *w, const char *needle);

/* ------------------------------------------------------------
 * Comparison
 * ------------------------------------------------------------ */

/**
 * @brief Compare two Weaves (strcmp-style)
 *
 * @param a First Weave
 * @param b Second Weave
 * @return <0 if a<b, 0 if a==b, >0 if a>b
 */
int weave_cmp(const Weave *a, const Weave *b);

/**
 * @brief Compare Weave with C string
 *
 * @param w Weave to compare
 * @param s C string to compare
 * @return <0, 0, or >0
 */
int weave_cmp_cstr(const Weave *w, const char *s);

/**
 * @brief Case-insensitive compare (ASCII only)
 *
 * @param a First Weave
 * @param b Second Weave
 * @return <0, 0, or >0
 */
int weave_casecmp(const Weave *a, const Weave *b);

/**
 * @brief Check equality
 *
 * @param a First Weave
 * @param b Second Weave
 * @return true if equal
 */
bool weave_eq(const Weave *a, const Weave *b);

/**
 * @brief Check equality with C string
 *
 * @param w Weave to compare
 * @param s C string to compare
 * @return true if equal
 */
bool weave_eq_cstr(const Weave *w, const char *s);

/**
 * @brief Case-insensitive equality (ASCII only)
 *
 * @param a First Weave
 * @param b Second Weave
 * @return true if case-insensitively equal
 */
bool weave_caseeq(const Weave *a, const Weave *b);

/* ------------------------------------------------------------
 * Hashing (Uses NXH)
 * ------------------------------------------------------------ */

/**
 * @brief Compute hash of Weave content
 *
 * @param w Weave to hash
 * @return 64-bit hash value
 *
 * Uses NXH internally for high-quality hashing.
 */
uint64_t weave_hash(const Weave *w);

/* ------------------------------------------------------------
 * Split and Join
 * ------------------------------------------------------------ */

/**
 * @brief Split by delimiter
 *
 * @param w     Weave to split
 * @param delim Delimiter string
 * @param count Output: number of parts
 * @return Array of Weave pointers (caller frees with weave_free_array)
 */
Weave** weave_split(const Weave *w, const char *delim, size_t *count);

/**
 * @brief Split by any character in set
 *
 * @param w     Weave to split
 * @param chars Characters to split on
 * @param count Output: number of parts
 * @return Array of Weave pointers
 */
Weave** weave_split_any(const Weave *w, const char *chars, size_t *count);

/**
 * @brief Split into lines
 *
 * @param w     Weave to split
 * @param count Output: number of lines
 * @return Array of Weave pointers (handles \n, \r\n, \r)
 */
Weave** weave_lines(const Weave *w, size_t *count);

/**
 * @brief Join C strings with separator
 *
 * @param parts Array of C strings
 * @param count Number of strings
 * @param sep   Separator (NULL for empty)
 * @return New joined Weave
 */
Weave* weave_join(const char **parts, size_t count, const char *sep);

/**
 * @brief Join Weaves with separator
 *
 * @param parts Array of Weave pointers
 * @param count Number of Weaves
 * @param sep   Separator (NULL for empty)
 * @return New joined Weave
 */
Weave* weave_join_weave(Weave **parts, size_t count, const char *sep);

/**
 * @brief Free array of Weaves
 *
 * @param arr   Array to free
 * @param count Number of elements
 */
void weave_free_array(Weave **arr, size_t count);

/* ------------------------------------------------------------
 * Formatting
 * ------------------------------------------------------------ */

/**
 * @brief Create formatted string (printf-style)
 *
 * @param fmt Format string
 * @param ... Format arguments
 * @return New formatted Weave, or NULL on error
 */
Weave* weave_fmt(const char *fmt, ...);

/**
 * @brief Append formatted string
 *
 * @param w   Weave to modify
 * @param fmt Format string
 * @param ... Format arguments
 * @return w, or NULL on error
 */
Weave* weave_append_fmt(Weave *w, const char *fmt, ...);

/* ============================================================
 * TABLET - Interned String Pool
 * ============================================================
 *
 * A Tablet is a pool of unique, immutable strings. When you intern
 * a string, you get back a canonical pointer - the same string
 * content always returns the same pointer.
 *
 * This enables O(1) string comparison via pointer equality.
 *
 * Backed by DAGGER for O(1) lookup and Arena for memory efficiency.
 */

/* ------------------------------------------------------------
 * Creation and Destruction
 * ------------------------------------------------------------ */

/**
 * @brief Create a new Tablet
 *
 * @return New Tablet or NULL on allocation failure
 */
Tablet* tablet_create(void);

/**
 * @brief Create with specified initial capacity
 *
 * @param initial_buckets Initial hash table size
 * @return New Tablet or NULL on allocation failure
 */
Tablet* tablet_create_sized(size_t initial_buckets);

/**
 * @brief Destroy Tablet and all interned strings
 *
 * @param T Tablet to destroy (NULL is safe)
 *
 * @warning All interned string pointers become invalid!
 */
void tablet_destroy(Tablet *T);

/* ------------------------------------------------------------
 * Interning
 * ------------------------------------------------------------ */

/**
 * @brief Intern a C string
 *
 * @param T Tablet to intern into
 * @param s String to intern (NULL returns NULL)
 * @return Canonical Weave pointer, or NULL on failure
 *
 * If the string already exists, returns existing pointer.
 * If new, creates and stores a new interned Weave.
 *
 * @note Returned Weave is owned by Tablet - do NOT free it!
 */
const Weave* tablet_intern(Tablet *T, const char *s);

/**
 * @brief Intern bytes
 *
 * @param T   Tablet to intern into
 * @param s   Bytes to intern
 * @param len Length in bytes
 * @return Canonical Weave pointer, or NULL on failure
 */
const Weave* tablet_intern_len(Tablet *T, const char *s, size_t len);

/**
 * @brief Intern an existing Weave
 *
 * @param T Tablet to intern into
 * @param w Weave to intern
 * @return Canonical Weave pointer
 *
 * If w's content already exists, returns existing pointer.
 */
const Weave* tablet_intern_weave(Tablet *T, const Weave *w);

/* ------------------------------------------------------------
 * Lookup
 * ------------------------------------------------------------ */

/**
 * @brief Lookup without interning
 *
 * @param T Tablet to search
 * @param s String to find
 * @return Existing Weave pointer, or NULL if not interned
 */
const Weave* tablet_lookup(const Tablet *T, const char *s);

/**
 * @brief Lookup by length
 *
 * @param T   Tablet to search
 * @param s   Bytes to find
 * @param len Length in bytes
 * @return Existing Weave pointer, or NULL if not interned
 */
const Weave* tablet_lookup_len(const Tablet *T, const char *s, size_t len);

/**
 * @brief Check if string is interned
 *
 * @param T Tablet to search
 * @param s String to check
 * @return true if interned
 */
bool tablet_contains(const Tablet *T, const char *s);

/* ------------------------------------------------------------
 * Statistics
 * ------------------------------------------------------------ */

/**
 * @brief Get number of interned strings
 *
 * @param T Tablet to query
 * @return Number of unique strings
 */
size_t tablet_count(const Tablet *T);

/**
 * @brief Get total memory usage
 *
 * @param T Tablet to query
 * @return Bytes used for interned strings
 */
size_t tablet_memory(const Tablet *T);

/* ------------------------------------------------------------
 * Comparison Macro
 * ------------------------------------------------------------ */

/**
 * @brief O(1) equality for interned strings
 *
 * Only valid for strings from the SAME Tablet!
 */
#define tablet_eq(a, b) ((a) == (b))

/* ============================================================
 * CORD - Deferred Concatenation
 * ============================================================
 *
 * A Cord is a lightweight rope structure for building strings
 * without repeated malloc/copy cycles. Instead of copying,
 * it accumulates pointers to string chunks.
 *
 * Only when you need the final C string do you "materialize"
 * the Cord into a single Weave (one allocation).
 *
 * Ideal for:
 * - Template expansion with many substitutions
 * - Building command lines
 * - Any heavy string concatenation
 */

/* ------------------------------------------------------------
 * Creation and Destruction
 * ------------------------------------------------------------ */

/**
 * @brief Create a new empty Cord
 *
 * @return New Cord or NULL on allocation failure
 */
Cord* cord_new(void);

/**
 * @brief Create with initial capacity
 *
 * @param chunk_cap Initial chunk array capacity
 * @return New Cord or NULL on allocation failure
 */
Cord* cord_with_cap(size_t chunk_cap);

/**
 * @brief Free a Cord and its owned chunks
 *
 * @param c Cord to free (NULL is safe)
 */
void cord_free(Cord *c);

/**
 * @brief Reset Cord to empty (keep capacity)
 *
 * @param c Cord to clear
 */
void cord_clear(Cord *c);

/* ------------------------------------------------------------
 * Building - All O(1) amortized
 * ------------------------------------------------------------ */

/**
 * @brief Append C string (copies the string)
 *
 * @param c Cord to modify
 * @param s String to append (NULL is no-op)
 * @return c, or NULL on failure
 */
Cord* cord_append(Cord *c, const char *s);

/**
 * @brief Append bytes (copies the data)
 *
 * @param c   Cord to modify
 * @param s   Bytes to append
 * @param len Number of bytes
 * @return c, or NULL on failure
 */
Cord* cord_append_len(Cord *c, const char *s, size_t len);

/**
 * @brief Append Weave (references, no copy)
 *
 * @param c Cord to modify
 * @param w Weave to append (must outlive Cord or be interned)
 * @return c, or NULL on failure
 *
 * @note The Weave is NOT copied - it's referenced. The Weave must
 *       remain valid until the Cord is freed or materialized.
 */
Cord* cord_append_weave(Cord *c, const Weave *w);

/**
 * @brief Append another Cord (references all chunks)
 *
 * @param c     Cord to modify
 * @param other Cord to append
 * @return c, or NULL on failure
 */
Cord* cord_append_cord(Cord *c, const Cord *other);

/**
 * @brief Append single character
 *
 * @param c  Cord to modify
 * @param ch Character to append
 * @return c, or NULL on failure
 */
Cord* cord_append_char(Cord *c, char ch);

/**
 * @brief Append formatted string
 *
 * @param c   Cord to modify
 * @param fmt Format string
 * @param ... Format arguments
 * @return c, or NULL on failure
 */
Cord* cord_append_fmt(Cord *c, const char *fmt, ...);

/* ------------------------------------------------------------
 * Access
 * ------------------------------------------------------------ */

/**
 * @brief Get total length of Cord
 *
 * @param c Cord to query
 * @return Total length in bytes (O(1))
 */
size_t cord_len(const Cord *c);

/**
 * @brief Get number of chunks
 *
 * @param c Cord to query
 * @return Number of string chunks
 */
size_t cord_chunk_count(const Cord *c);

/**
 * @brief Check if Cord is empty
 *
 * @param c Cord to check
 * @return true if empty
 */
bool cord_empty(const Cord *c);

/* ------------------------------------------------------------
 * Materialization
 * ------------------------------------------------------------ */

/**
 * @brief Convert Cord to Weave (single allocation)
 *
 * @param c Cord to materialize
 * @return New Weave with concatenated content
 *
 * This is O(n) where n is total length - all chunks are
 * copied into a single contiguous Weave.
 */
Weave* cord_to_weave(const Cord *c);

/**
 * @brief Convert Cord to malloc'd C string
 *
 * @param c Cord to materialize
 * @return New NUL-terminated string (caller must free)
 */
char* cord_to_cstr(const Cord *c);

/**
 * @brief Write Cord content to buffer
 *
 * @param c      Cord to materialize
 * @param buf    Destination buffer
 * @param buflen Buffer size
 * @return Number of bytes written (excluding NUL), or required size if buf is NULL
 */
size_t cord_to_buf(const Cord *c, char *buf, size_t buflen);

/* ------------------------------------------------------------
 * Iteration (without materializing)
 * ------------------------------------------------------------ */

/**
 * @brief Chunk iterator callback
 *
 * @param chunk Pointer to chunk data
 * @param len   Chunk length
 * @param ctx   User context
 * @return true to continue, false to stop
 */
typedef bool (*cord_iter_fn)(const char *chunk, size_t len, void *ctx);

/**
 * @brief Iterate over chunks
 *
 * @param c   Cord to iterate
 * @param fn  Callback function
 * @param ctx User context passed to callback
 */
void cord_foreach(const Cord *c, cord_iter_fn fn, void *ctx);

/* ============================================================
 * WTC - Combined Operations
 * ============================================================
 *
 * Convenience functions that combine Weave, Tablet, and Cord
 * for common high-level operations.
 */

/**
 * @brief Variable lookup callback for interpolation
 *
 * @param var_name Variable name (without ${} delimiters)
 * @param ctx      User context
 * @return Variable value (can be static or from Tablet), or NULL if not found
 */
typedef const char* (*wtc_lookup_fn)(const char *var_name, void *ctx);

/**
 * @brief Interpolate variables in template string
 *
 * Replaces ${var} patterns with values from lookup function.
 *
 * @param template_str Template string with ${var} placeholders
 * @param lookup       Variable lookup function
 * @param ctx          Context passed to lookup
 * @return New Weave with substitutions, or NULL on error
 *
 * Features:
 * - ${var} - simple variable
 * - ${var:-default} - with default value
 * - $$ - literal $
 *
 * @example
 *   const char *lookup(const char *name, void *ctx) {
 *       if (strcmp(name, "CC") == 0) return "gcc";
 *       return NULL;
 *   }
 *   Weave *cmd = wtc_interpolate(weave_new("${CC} -o main"), lookup, NULL);
 *   // cmd = "gcc -o main"
 */
Weave* wtc_interpolate(const Weave *template_str, wtc_lookup_fn lookup, void *ctx);

/**
 * @brief Interpolate and intern result
 *
 * @param T            Tablet for interning
 * @param template_str Template string
 * @param lookup       Variable lookup function
 * @param ctx          Context for lookup
 * @return Interned Weave with substitutions
 */
const Weave* wtc_interpolate_intern(
    Tablet *T,
    const Weave *template_str,
    wtc_lookup_fn lookup,
    void *ctx
);

/**
 * @brief Format and intern in one step
 *
 * @param T   Tablet for interning
 * @param fmt Format string
 * @param ... Format arguments
 * @return Interned formatted Weave
 */
const Weave* wtc_format(Tablet *T, const char *fmt, ...);

/**
 * @brief Join and intern
 *
 * @param T     Tablet for interning
 * @param parts Array of C strings
 * @param count Number of strings
 * @param sep   Separator
 * @return Interned joined string
 */
const Weave* wtc_join(Tablet *T, const char **parts, size_t count, const char *sep);

/* ============================================================
 * OPTIONAL: UTF-8 UTILITIES
 * ============================================================
 *
 * Weave is byte-oriented by design. These optional utilities
 * provide UTF-8 awareness when needed.
 */

#ifdef WEAVE_UTF8

/**
 * @brief Count UTF-8 codepoints
 *
 * @param w Weave to analyze
 * @return Number of codepoints (not bytes, not graphemes)
 */
size_t weave_utf8_len(const Weave *w);

/**
 * @brief Validate UTF-8 encoding
 *
 * @param w Weave to validate
 * @return true if valid UTF-8
 */
bool weave_utf8_valid(const Weave *w);

/**
 * @brief Get byte offset of nth codepoint
 *
 * @param w Weave to search
 * @param n Codepoint index (0-based)
 * @return Byte offset, or -1 if n exceeds length
 */
int64_t weave_utf8_offset(const Weave *w, size_t n);

#endif /* WEAVE_UTF8 */

#ifdef __cplusplus
}
#endif

#endif /* WEAVE_H */
