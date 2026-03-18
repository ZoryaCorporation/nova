/**
 * @file nova_codegen.h
 * @brief Nova Language - Bytecode Serialization (.no format)
 *
 * The code generator serializes compiled NovaProto trees to the
 * portable .no binary format and deserializes them back. This is
 * the final stage of the compilation pipeline and the first stage
 * when loading pre-compiled bytecode.
 *
 * Pipeline:
 *   Save: NovaProto --> nova_codegen_save() --> file.no
 *   Load: file.no   --> nova_codegen_load() --> NovaProto
 *
 * The .no format is:
 *   - Cross-platform (explicit little-endian encoding)
 *   - Versioned (format version in header for forward compat)
 *   - Checksummed (NXH64 integrity check)
 *   - Debug-optional (line maps and locals can be stripped)
 *
 * @author Anthony Taliento
 * @date 2026-02-07
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#ifndef NOVA_CODEGEN_H
#define NOVA_CODEGEN_H

#include "nova_proto.h"

#include <stdint.h>
#include <stddef.h>

/* ============================================================
 * FORMAT CONSTANTS
 * ============================================================ */

/** Magic number: "NOVA" in big-endian ASCII */
#define NOVA_CODEGEN_MAGIC       ((uint32_t)0x4E4F5641)

/** Format version 1.0 */
#define NOVA_CODEGEN_VERSION_MAJOR  1
#define NOVA_CODEGEN_VERSION_MINOR  0

/** Portable platform tag (no platform-specific features) */
#define NOVA_CODEGEN_PLATFORM_PORTABLE  0x00000000

/** EOF sentinel marker: 0xDEAD4E56 ("DEAD" + 'N'+'V' for NoVa) */
#define NOVA_CODEGEN_EOF_MARKER  ((uint32_t)0xDEAD4E56)

/** Header size in bytes (before payload) */
#define NOVA_CODEGEN_HEADER_SIZE  28

/* ============================================================
 * FLAGS
 *
 * Stored in the 2-byte flags field of the .no header.
 * Also used as parameters to save/dump functions.
 * ============================================================ */

/** Data is little-endian (always set in v1; BE not supported) */
#define NOVA_CODEGEN_FLAG_LE      0x0000

/** Debug info (line maps, local names) is present */
#define NOVA_CODEGEN_FLAG_DEBUG   0x0002

/** Debug info has been stripped */
#define NOVA_CODEGEN_FLAG_STRIP   0x0004

/* ============================================================
 * ERROR CODES
 * ============================================================ */

#define NOVA_CODEGEN_OK            0   /* Success                     */
#define NOVA_CODEGEN_ERR_IO       -1   /* File I/O error              */
#define NOVA_CODEGEN_ERR_NOMEM    -2   /* Allocation failure          */
#define NOVA_CODEGEN_ERR_MAGIC    -3   /* Bad magic number            */
#define NOVA_CODEGEN_ERR_VERSION  -4   /* Unsupported format version  */
#define NOVA_CODEGEN_ERR_CORRUPT  -5   /* Checksum mismatch/truncated */
#define NOVA_CODEGEN_ERR_EOF      -6   /* Unexpected end of data      */
#define NOVA_CODEGEN_ERR_NULLPTR  -7   /* NULL argument               */

/* ============================================================
 * PUBLIC API
 * ============================================================ */

/**
 * @brief Save a compiled proto tree to a .no file.
 *
 * Serializes the proto and all sub-protos to the Nova bytecode
 * format and writes to the specified path. The file is written
 * atomically (built in memory, then one fwrite).
 *
 * @param proto  Root prototype to serialize (must not be NULL)
 * @param path   Output file path (must not be NULL)
 * @param flags  Combination of NOVA_CODEGEN_FLAG_* values.
 *               Use NOVA_CODEGEN_FLAG_DEBUG (default) to include
 *               debug info, or NOVA_CODEGEN_FLAG_STRIP to omit it.
 *
 * @return NOVA_CODEGEN_OK on success, negative error code on failure.
 *
 * @pre proto != NULL && path != NULL
 *
 * COMPLEXITY: O(n) where n = total instructions + constants
 * THREAD SAFETY: Not thread-safe
 */
int nova_codegen_save(const NovaProto *proto, const char *path, int flags);

/**
 * @brief Load a .no file into a proto tree.
 *
 * Reads the specified .no file, verifies the header (magic,
 * version, checksum), and deserializes the proto tree.
 *
 * @param path       Input file path (must not be NULL)
 * @param error_out  Output: error code (may be NULL)
 *
 * @return Root prototype on success (caller owns, use
 *         nova_proto_destroy to free), or NULL on failure.
 *
 * @pre path != NULL
 *
 * COMPLEXITY: O(n) where n = file size
 * THREAD SAFETY: Not thread-safe
 */
NovaProto *nova_codegen_load(const char *path, int *error_out);

/**
 * @brief Serialize a proto tree to a memory buffer.
 *
 * Same format as nova_codegen_save, but outputs to a heap-allocated
 * buffer instead of a file. Caller must free *buf_out.
 *
 * @param proto     Root prototype to serialize (must not be NULL)
 * @param buf_out   Output: pointer to allocated buffer
 * @param size_out  Output: size of buffer in bytes
 * @param flags     Combination of NOVA_CODEGEN_FLAG_* values
 *
 * @return NOVA_CODEGEN_OK on success, negative error code on failure.
 *
 * @pre proto != NULL && buf_out != NULL && size_out != NULL
 *
 * COMPLEXITY: O(n)
 * THREAD SAFETY: Not thread-safe
 */
int nova_codegen_dump(const NovaProto *proto, uint8_t **buf_out,
                      size_t *size_out, int flags);

/**
 * @brief Deserialize a proto tree from a memory buffer.
 *
 * @param buf        Input buffer containing .no format data
 * @param size       Size of buffer in bytes
 * @param error_out  Output: error code (may be NULL)
 *
 * @return Root prototype on success (caller owns), or NULL on failure.
 *
 * @pre buf != NULL && size >= NOVA_CODEGEN_HEADER_SIZE + 4
 *
 * COMPLEXITY: O(n)
 * THREAD SAFETY: Not thread-safe
 */
NovaProto *nova_codegen_undump(const uint8_t *buf, size_t size,
                               int *error_out);

/**
 * @brief Get a human-readable error message for a codegen error code.
 *
 * @param error  Error code (NOVA_CODEGEN_ERR_*)
 * @return Static string describing the error (never NULL)
 */
const char *nova_codegen_strerror(int error);

#endif /* NOVA_CODEGEN_H */
