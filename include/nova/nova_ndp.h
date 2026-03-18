/**
 * @file nova_ndp.h
 * @brief Nova Data Processor (NDP) - Unified Multi-Format Codec
 *
 * Provides decode (text -> Nova table) and encode (Nova table -> text)
 * for multiple structured data formats through a single API:
 *
 *   - JSON  (RFC 8259, recursive descent)
 *   - CSV   (RFC 4180, delimiter-configurable for TSV)
 *   - INI   (sections + key=value with typed values)
 *   - TOML  (typed config with arrays and nested tables)
 *   - HTML  (tag-soup tolerant, produces table tree)
 *   - NINI  (Nova INI — enhanced INI with arrays, interpolation, tasks)
 *
 * All codecs share a common string builder, number parser, and
 * escape handler. Format detection is available for auto-decode.
 *
 * Nova API (exposed via nova_lib_data.c):
 *   data.decode(text, format [, opts])  -- string -> table
 *   data.encode(value, format [, opts]) -- table -> string
 *   data.load(filename, format [, opts])-- file -> table
 *   data.save(filename, value, format [, opts]) -- table -> file
 *   data.detect(text)                   -- guess format name
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
 *   - nova_vm.h (NovaVM, value types, stack API)
 *
 * THREAD SAFETY:
 *   Not thread-safe. Each call operates on a single VM instance.
 */

#ifndef NOVA_NDP_H
#define NOVA_NDP_H

#include <stddef.h>

/* Forward declarations (avoid pulling in nova_vm.h for users
   who only need format detection or the string builder) */
struct NovaVM;

/* ============================================================
 * FORMAT IDENTIFIERS
 * ============================================================ */

typedef enum {
    NDP_FORMAT_JSON = 0,
    NDP_FORMAT_CSV  = 1,
    NDP_FORMAT_TSV  = 2,
    NDP_FORMAT_INI  = 3,
    NDP_FORMAT_TOML = 4,
    NDP_FORMAT_HTML = 5,
    NDP_FORMAT_YAML = 6,
    NDP_FORMAT_NINI = 7,
    NDP_FORMAT_UNKNOWN = -1
} NdpFormat;

/* ============================================================
 * CODEC OPTIONS
 *
 * A single options struct covers all formats. Each codec reads
 * only the fields relevant to it; the rest are ignored.
 * ============================================================ */

typedef struct {
    NdpFormat format;

    /* CSV / TSV options */
    char      csv_delimiter;    /**< Field delimiter (default ',')       */
    int       csv_header;       /**< 1 = first row is header names       */
    char      csv_quote;        /**< Quote character (default '"')        */

    /* JSON options */
    int       json_strict;      /**< 1 = no trailing commas, no comments  */

    /* Encode options */
    int       pretty;           /**< 1 = human-readable output            */
    int       indent;           /**< Spaces per indent level (default 2)  */

    /* INI options */
    int       ini_typed;        /**< 1 = parse int/float/bool values      */

    /* HTML options */
    int       html_text_only;   /**< 1 = extract text content only        */

    /* NINI options */
    int       nini_interpolate; /**< 1 = resolve ${var} references         */
    int       nini_tasks_only;  /**< 1 = parse only [task:*] sections      */

} NdpOptions;

/**
 * @brief Initialize an NdpOptions struct with defaults.
 *
 * @param opts  Options to initialize (must not be NULL)
 *
 * @pre opts != NULL
 * @post All fields set to sensible defaults
 */
void ndp_options_init(NdpOptions *opts);

/* ============================================================
 * STRING BUILDER
 *
 * Growable byte buffer used by all encoders. Also useful for
 * general string construction in C code.
 * ============================================================ */

typedef struct {
    char   *data;       /**< Buffer data (heap-allocated) */
    size_t  len;        /**< Current length               */
    size_t  cap;        /**< Allocated capacity            */
} NdpBuf;

/**
 * @brief Initialize a string builder.
 * @param buf  Buffer to initialize (must not be NULL)
 */
void ndp_buf_init(NdpBuf *buf);

/**
 * @brief Free a string builder's memory.
 * @param buf  Buffer to free (may be NULL)
 */
void ndp_buf_free(NdpBuf *buf);

/**
 * @brief Append raw bytes to the buffer.
 * @param buf   Target buffer
 * @param data  Bytes to append
 * @param len   Number of bytes
 */
void ndp_buf_append(NdpBuf *buf, const char *data, size_t len);

/**
 * @brief Append a null-terminated string to the buffer.
 * @param buf  Target buffer
 * @param s    String to append
 */
void ndp_buf_puts(NdpBuf *buf, const char *s);

/**
 * @brief Append a single character to the buffer.
 * @param buf  Target buffer
 * @param c    Character to append
 */
void ndp_buf_putc(NdpBuf *buf, char c);

/**
 * @brief Append a printf-formatted string to the buffer.
 * @param buf  Target buffer
 * @param fmt  Format string
 * @param ...  Arguments
 */
void ndp_buf_printf(NdpBuf *buf, const char *fmt, ...);

/* ============================================================
 * CORE DECODE / ENCODE API
 *
 * Decode: Parses text and pushes a Nova value onto the VM stack.
 *         Returns 0 on success, -1 on error (error msg in errbuf).
 *
 * Encode: Reads a Nova value from VM stack position and writes
 *         the formatted text into an NdpBuf.
 *         Returns 0 on success, -1 on error.
 * ============================================================ */

/**
 * @brief Decode text into a Nova value (pushed onto VM stack).
 *
 * @param vm      VM instance (must not be NULL)
 * @param text    Input text to parse (must not be NULL)
 * @param len     Length of input text
 * @param opts    Codec options (must not be NULL, format field selects codec)
 * @param errbuf  Error message buffer (256 bytes, filled on error)
 *
 * @return 0 on success (value pushed on stack), -1 on error
 *
 * @pre vm != NULL && text != NULL && opts != NULL
 */
int ndp_decode(struct NovaVM *vm, const char *text, size_t len,
               const NdpOptions *opts, char *errbuf);

/**
 * @brief Encode a Nova value to text.
 *
 * @param vm      VM instance (must not be NULL)
 * @param idx     Stack index of value to encode
 * @param opts    Codec options (must not be NULL, format field selects codec)
 * @param out     Output buffer (caller must ndp_buf_init before, ndp_buf_free after)
 * @param errbuf  Error message buffer (256 bytes, filled on error)
 *
 * @return 0 on success (text in out->data), -1 on error
 *
 * @pre vm != NULL && opts != NULL && out != NULL
 */
int ndp_encode(struct NovaVM *vm, int idx, const NdpOptions *opts,
               NdpBuf *out, char *errbuf);

/* ============================================================
 * FORMAT UTILITIES
 * ============================================================ */

/**
 * @brief Detect the format of a text string.
 *
 * Heuristic analysis of the first few hundred bytes to guess
 * the data format. Not 100% accurate but good for auto-detect.
 *
 * @param text  Input text
 * @param len   Length of text
 * @return Detected format, or NDP_FORMAT_UNKNOWN
 */
NdpFormat ndp_detect(const char *text, size_t len);

/**
 * @brief Get the human-readable name for a format.
 * @param fmt  Format identifier
 * @return Static string: "json", "csv", "tsv", "ini", "toml", "html"
 */
const char *ndp_format_name(NdpFormat fmt);

/**
 * @brief Parse a format name string to its identifier.
 * @param name  Format name (case-insensitive)
 * @return Format identifier, or NDP_FORMAT_UNKNOWN
 */
NdpFormat ndp_format_from_name(const char *name);

#endif /* NOVA_NDP_H */
