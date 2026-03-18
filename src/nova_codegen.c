/**
 * @file nova_codegen.c
 * @brief Nova Language - Bytecode Serialization (.no format)
 *
 * Serializes NovaProto trees to the portable .no binary format
 * and deserializes them back. All multi-byte values are encoded
 * little-endian regardless of host byte order.
 *
 * Format overview (28-byte header + payload + 4-byte EOF marker):
 *   [Magic:4][Version:2][Flags:2][Platform:4][Timestamp:8][Checksum:8]
 *   [Proto (recursive)][EOF Marker:4]
 *
 * @author Anthony Taliento
 * @date 2026-02-07
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * DEPENDENCIES:
 *   - nova_codegen.h (public API, format constants)
 *   - nova_proto.h   (NovaProto, NovaConstant types)
 *   - nova_opcode.h  (NovaInstruction)
 *   - zorya/nxh.h    (NXH64 checksum)
 *
 * THREAD SAFETY:
 *   Not thread-safe. Each thread should serialize separate protos.
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#include "nova/nova_codegen.h"
#include "nova/nova_opcode.h"
#include "nova/nova_proto.h"
#include "nova/nova_conf.h"
#include "zorya/nxh.h"

#include <stdint.h>
#include <stdlib.h>   /* malloc, realloc, free */
#include <string.h>   /* memcpy, memset */
#include <stdio.h>    /* fopen, fread, fwrite, fclose, fseek, ftell */
#include <time.h>     /* time() for timestamp */

/* ============================================================
 * PART 1: WRITE BUFFER
 *
 * Growable byte buffer for building the .no file in memory.
 * All write helpers append to this buffer. Once complete,
 * the buffer is written to disk in one fwrite() call.
 * ============================================================ */

/** Initial buffer capacity (4KB) */
#define NOVAI_BUF_INIT_CAP  4096

/** Growable write buffer */
typedef struct {
    uint8_t *data;      /* Heap-allocated buffer              */
    size_t   size;      /* Bytes written so far               */
    size_t   capacity;  /* Allocated capacity                 */
    int      error;     /* Set to non-zero on first failure   */
} NovaCodegenBuf;

/**
 * @brief Initialize a write buffer.
 *
 * @param buf  Buffer to initialize
 */
static void novai_buf_init(NovaCodegenBuf *buf) {
    if (buf == NULL) {
        return;
    }
    buf->data = (uint8_t *)malloc(NOVAI_BUF_INIT_CAP);
    buf->size = 0;
    buf->capacity = NOVAI_BUF_INIT_CAP;
    buf->error = 0;
    if (buf->data == NULL) {
        buf->capacity = 0;
        buf->error = NOVA_CODEGEN_ERR_NOMEM;
    }
}

/**
 * @brief Free a write buffer's data.
 *
 * @param buf  Buffer to free
 */
static void novai_buf_free(NovaCodegenBuf *buf) {
    if (buf != NULL) {
        free(buf->data);
        buf->data = NULL;
        buf->size = 0;
        buf->capacity = 0;
    }
}

/**
 * @brief Ensure the buffer has room for `needed` more bytes.
 *
 * @param buf     Write buffer
 * @param needed  Additional bytes needed
 * @return 1 on success, 0 on allocation failure
 */
static int novai_buf_ensure(NovaCodegenBuf *buf, size_t needed) {
    if (buf->error != 0) {
        return 0;
    }

    size_t required = buf->size + needed;
    if (required <= buf->capacity) {
        return 1;
    }

    /* Grow geometrically (2x) */
    size_t new_cap = buf->capacity;
    while (new_cap < required) {
        new_cap *= 2;
    }

    uint8_t *new_data = (uint8_t *)realloc(buf->data, new_cap);
    if (new_data == NULL) {
        buf->error = NOVA_CODEGEN_ERR_NOMEM;
        return 0;
    }

    buf->data = new_data;
    buf->capacity = new_cap;
    return 1;
}

/* ============================================================
 * PART 2: WRITE HELPERS (Little-Endian)
 *
 * Each helper appends bytes to the buffer in LE order.
 * On failure, sets buf->error and returns silently.
 * ============================================================ */

static void novai_write_bytes(NovaCodegenBuf *buf, const void *data,
                              size_t len) {
    if (buf->error != 0 || len == 0) {
        return;
    }
    if (novai_buf_ensure(buf, len) == 0) {
        return;
    }
    memcpy(buf->data + buf->size, data, len);
    buf->size += len;
}

static void novai_write_u8(NovaCodegenBuf *buf, uint8_t v) {
    novai_write_bytes(buf, &v, 1);
}

static void novai_write_u16(NovaCodegenBuf *buf, uint16_t v) {
    uint8_t bytes[2];
    bytes[0] = (uint8_t)(v & 0xFFu);
    bytes[1] = (uint8_t)((v >> 8) & 0xFFu);
    novai_write_bytes(buf, bytes, 2);
}

static void novai_write_u32(NovaCodegenBuf *buf, uint32_t v) {
    uint8_t bytes[4];
    bytes[0] = (uint8_t)(v & 0xFFu);
    bytes[1] = (uint8_t)((v >> 8) & 0xFFu);
    bytes[2] = (uint8_t)((v >> 16) & 0xFFu);
    bytes[3] = (uint8_t)((v >> 24) & 0xFFu);
    novai_write_bytes(buf, bytes, 4);
}

static void novai_write_u64(NovaCodegenBuf *buf, uint64_t v) {
    uint8_t bytes[8];
    bytes[0] = (uint8_t)(v & 0xFFu);
    bytes[1] = (uint8_t)((v >> 8) & 0xFFu);
    bytes[2] = (uint8_t)((v >> 16) & 0xFFu);
    bytes[3] = (uint8_t)((v >> 24) & 0xFFu);
    bytes[4] = (uint8_t)((v >> 32) & 0xFFu);
    bytes[5] = (uint8_t)((v >> 40) & 0xFFu);
    bytes[6] = (uint8_t)((v >> 48) & 0xFFu);
    bytes[7] = (uint8_t)((v >> 56) & 0xFFu);
    novai_write_bytes(buf, bytes, 8);
}

static void novai_write_i64(NovaCodegenBuf *buf, int64_t v) {
    novai_write_u64(buf, (uint64_t)v);
}

static void novai_write_double(NovaCodegenBuf *buf, double v) {
    uint64_t bits = 0;
    memcpy(&bits, &v, sizeof(bits));
    novai_write_u64(buf, bits);
}

/**
 * @brief Write a length-prefixed string (u16 length + data).
 *
 * @param buf   Write buffer
 * @param str   String data (may be NULL if len==0)
 * @param len   String length in bytes (max 65535)
 */
static void novai_write_string(NovaCodegenBuf *buf, const char *str,
                               uint32_t len) {
    uint16_t slen = (len > 0xFFFFu) ? 0xFFFFu : (uint16_t)len;
    novai_write_u16(buf, slen);
    if (slen > 0 && str != NULL) {
        novai_write_bytes(buf, str, slen);
    }
}

/* ============================================================
 * PART 3: READ STATE
 *
 * Stateful reader that walks a memory buffer sequentially.
 * On any error, sets reader->error and returns zero values.
 * ============================================================ */

/** Read state for deserialization */
typedef struct {
    const uint8_t *data;    /* Input buffer base             */
    size_t         size;    /* Total buffer size              */
    size_t         pos;     /* Current read position          */
    int            flags;   /* Flags from header              */
    int            error;   /* Set on first error             */
} NovaCodegenReader;

/**
 * @brief Check if `needed` bytes are available.
 *
 * @param r       Reader state
 * @param needed  Bytes needed
 * @return 1 if available, 0 if not (sets error)
 */
static int novai_read_check(NovaCodegenReader *r, size_t needed) {
    if (r->error != 0) {
        return 0;
    }
    if (r->pos + needed > r->size) {
        r->error = NOVA_CODEGEN_ERR_EOF;
        return 0;
    }
    return 1;
}

/* ============================================================
 * PART 4: READ HELPERS (Little-Endian)
 * ============================================================ */

static uint8_t novai_read_u8(NovaCodegenReader *r) {
    if (novai_read_check(r, 1) == 0) {
        return 0;
    }
    uint8_t v = r->data[r->pos];
    r->pos++;
    return v;
}

static uint16_t novai_read_u16(NovaCodegenReader *r) {
    if (novai_read_check(r, 2) == 0) {
        return 0;
    }
    const uint8_t *p = r->data + r->pos;
    uint16_t v = (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
    r->pos += 2;
    return v;
}

static uint32_t novai_read_u32(NovaCodegenReader *r) {
    if (novai_read_check(r, 4) == 0) {
        return 0;
    }
    const uint8_t *p = r->data + r->pos;
    uint32_t v = (uint32_t)p[0]
               | ((uint32_t)p[1] << 8)
               | ((uint32_t)p[2] << 16)
               | ((uint32_t)p[3] << 24);
    r->pos += 4;
    return v;
}

static uint64_t novai_read_u64(NovaCodegenReader *r) {
    if (novai_read_check(r, 8) == 0) {
        return 0;
    }
    const uint8_t *p = r->data + r->pos;
    uint64_t v = (uint64_t)p[0]
               | ((uint64_t)p[1] << 8)
               | ((uint64_t)p[2] << 16)
               | ((uint64_t)p[3] << 24)
               | ((uint64_t)p[4] << 32)
               | ((uint64_t)p[5] << 40)
               | ((uint64_t)p[6] << 48)
               | ((uint64_t)p[7] << 56);
    r->pos += 8;
    return v;
}

static int64_t novai_read_i64(NovaCodegenReader *r) {
    return (int64_t)novai_read_u64(r);
}

static double novai_read_double(NovaCodegenReader *r) {
    uint64_t bits = novai_read_u64(r);
    double v = 0.0;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

/**
 * @brief Read a length-prefixed string (u16 length + data).
 *
 * Returns a heap-allocated copy with NUL terminator.
 * Caller must free the returned string. Returns NULL if length is 0.
 *
 * @param r        Reader state
 * @param len_out  Output: string length (may be NULL)
 * @return Heap-allocated string, or NULL
 */
static char *novai_read_string(NovaCodegenReader *r, uint32_t *len_out) {
    uint16_t slen = novai_read_u16(r);
    if (len_out != NULL) {
        *len_out = (uint32_t)slen;
    }
    if (slen == 0 || r->error != 0) {
        return NULL;
    }
    if (novai_read_check(r, slen) == 0) {
        return NULL;
    }

    char *str = (char *)malloc((size_t)slen + 1);
    if (str == NULL) {
        r->error = NOVA_CODEGEN_ERR_NOMEM;
        return NULL;
    }
    memcpy(str, r->data + r->pos, slen);
    str[slen] = '\0';
    r->pos += slen;
    return str;
}

/**
 * @brief Skip past a length-prefixed string without allocating.
 *
 * @param r  Reader state
 * @return String length that was skipped
 */
static uint16_t novai_skip_string(NovaCodegenReader *r) {
    uint16_t slen = novai_read_u16(r);
    if (slen > 0 && r->error == 0) {
        if (novai_read_check(r, slen) != 0) {
            r->pos += slen;
        }
    }
    return slen;
}

/* ============================================================
 * PART 5: PROTO SERIALIZATION (WRITE)
 *
 * Recursive: each sub-proto is serialized inline.
 * ============================================================ */

/**
 * @brief Serialize one proto (and all sub-protos) to the buffer.
 *
 * @param buf    Write buffer
 * @param proto  Proto to serialize
 * @param flags  NOVA_CODEGEN_FLAG_* (for debug info decision)
 */
static void novai_write_proto(NovaCodegenBuf *buf, const NovaProto *proto,
                              int flags) {
    uint32_t i = 0;

    if (buf->error != 0 || proto == NULL) {
        return;
    }

    /* ---- Metadata (4 bytes + 8 bytes) ---- */
    novai_write_u8(buf, proto->num_params);
    novai_write_u8(buf, proto->is_vararg);
    novai_write_u8(buf, proto->is_async);
    novai_write_u8(buf, proto->max_stack);
    novai_write_u32(buf, proto->line_defined);
    novai_write_u32(buf, proto->last_line);

    /* ---- Source name ---- */
    if (proto->source != NULL) {
        uint32_t slen = (uint32_t)strlen(proto->source);
        novai_write_string(buf, proto->source, slen);
    } else {
        novai_write_u16(buf, 0);
    }

    /* ---- Instructions ---- */
    novai_write_u32(buf, proto->code_count);
    for (i = 0; i < proto->code_count; i++) {
        novai_write_u32(buf, proto->code[i]);
    }

    /* ---- Constants ---- */
    novai_write_u32(buf, proto->const_count);
    for (i = 0; i < proto->const_count; i++) {
        const NovaConstant *k = &proto->constants[i];
        novai_write_u8(buf, (uint8_t)k->tag);

        switch (k->tag) {
            case NOVA_CONST_NIL:
                /* No data */
                break;
            case NOVA_CONST_BOOL:
                novai_write_u8(buf, (uint8_t)(k->as.boolean != 0 ? 1 : 0));
                break;
            case NOVA_CONST_INTEGER:
                novai_write_i64(buf, (int64_t)k->as.integer);
                break;
            case NOVA_CONST_NUMBER:
                novai_write_double(buf, (double)k->as.number);
                break;
            case NOVA_CONST_STRING:
                novai_write_string(buf, k->as.string.data,
                                   k->as.string.length);
                break;
            default:
                /* Unknown constant tag -- write as nil */
                break;
        }
    }

    /* ---- Upvalues ---- */
    novai_write_u8(buf, proto->upvalue_count);
    for (i = 0; i < (uint32_t)proto->upvalue_count; i++) {
        novai_write_u8(buf, proto->upvalues[i].index);
        novai_write_u8(buf, proto->upvalues[i].in_stack);
    }

    /* ---- Sub-prototypes (recursive) ---- */
    novai_write_u32(buf, proto->proto_count);
    for (i = 0; i < proto->proto_count; i++) {
        novai_write_proto(buf, proto->protos[i], flags);
    }

    /* ---- Debug info (optional) ---- */
    if ((flags & NOVA_CODEGEN_FLAG_STRIP) != 0) {
        /* Stripped: write zero counts as markers */
        novai_write_u32(buf, 0);  /* line_count = 0 */
        novai_write_u32(buf, 0);  /* local_count = 0 */
        novai_write_u8(buf, 0);   /* upval_name_count = 0 */
        return;
    }

    /* Line numbers */
    uint32_t line_count = proto->lines.count;
    novai_write_u32(buf, line_count);
    if (proto->lines.line_numbers != NULL) {
        for (i = 0; i < line_count; i++) {
            novai_write_u32(buf, proto->lines.line_numbers[i]);
        }
    }

    /* Local variables */
    novai_write_u32(buf, proto->local_count);
    for (i = 0; i < proto->local_count; i++) {
        const NovaLocalInfo *loc = &proto->locals[i];
        if (loc->name != NULL) {
            uint32_t nlen = (uint32_t)strlen(loc->name);
            novai_write_string(buf, loc->name, nlen);
        } else {
            novai_write_u16(buf, 0);
        }
        novai_write_u32(buf, loc->start_pc);
        novai_write_u32(buf, loc->end_pc);
        novai_write_u8(buf, loc->reg);
    }

    /* Upvalue names */
    novai_write_u8(buf, proto->upvalue_count);
    for (i = 0; i < (uint32_t)proto->upvalue_count; i++) {
        if (proto->upvalues[i].name != NULL) {
            uint32_t nlen = (uint32_t)strlen(proto->upvalues[i].name);
            novai_write_string(buf, proto->upvalues[i].name, nlen);
        } else {
            novai_write_u16(buf, 0);
        }
    }
}

/* ============================================================
 * PART 6: PROTO DESERIALIZATION (READ)
 *
 * Recursive: each sub-proto is deserialized inline.
 * On any error, partially constructed protos are destroyed.
 * ============================================================ */

/**
 * @brief Deserialize one proto (and all sub-protos) from the reader.
 *
 * @param r  Reader state
 * @return Newly allocated proto, or NULL on error
 */
static NovaProto *novai_read_proto(NovaCodegenReader *r) {
    uint32_t i = 0;

    if (r->error != 0) {
        return NULL;
    }

    NovaProto *proto = nova_proto_create();
    if (proto == NULL) {
        r->error = NOVA_CODEGEN_ERR_NOMEM;
        return NULL;
    }

    /* ---- Metadata ---- */
    proto->num_params = novai_read_u8(r);
    proto->is_vararg = novai_read_u8(r);
    proto->is_async = novai_read_u8(r);
    proto->max_stack = novai_read_u8(r);
    proto->line_defined = novai_read_u32(r);
    proto->last_line = novai_read_u32(r);

    if (r->error != 0) {
        goto fail;
    }

    /* ---- Source name ---- */
    {
        uint32_t slen = 0;
        char *source = novai_read_string(r, &slen);
        if (source != NULL) {
            /* Note: source string is now heap-allocated.
             * The proto doesn't own source strings normally
             * (they point into the interned pool), but for
             * loaded bytecode we need to manage this. Store
             * it and document that loaded protos own their
             * source string. */
            proto->source = source;
        }
    }

    if (r->error != 0) {
        goto fail;
    }

    /* ---- Instructions ---- */
    {
        uint32_t code_count = novai_read_u32(r);
        if (r->error != 0) {
            goto fail;
        }

        /* Safety limit: reject absurdly large code arrays */
        if (code_count > 0x1000000u) {  /* 16M instructions max */
            r->error = NOVA_CODEGEN_ERR_CORRUPT;
            goto fail;
        }

        if (code_count > 0) {
            proto->code = (NovaInstruction *)malloc(
                code_count * sizeof(NovaInstruction));
            if (proto->code == NULL) {
                r->error = NOVA_CODEGEN_ERR_NOMEM;
                goto fail;
            }
            proto->code_count = code_count;
            proto->code_capacity = code_count;

            for (i = 0; i < code_count; i++) {
                proto->code[i] = novai_read_u32(r);
            }
            if (r->error != 0) {
                goto fail;
            }
        }
    }

    /* ---- Constants ---- */
    {
        uint32_t const_count = novai_read_u32(r);
        if (r->error != 0) {
            goto fail;
        }

        if (const_count > NOVA_MAX_CONSTANTS) {
            r->error = NOVA_CODEGEN_ERR_CORRUPT;
            goto fail;
        }

        if (const_count > 0) {
            proto->constants = (NovaConstant *)malloc(
                const_count * sizeof(NovaConstant));
            if (proto->constants == NULL) {
                r->error = NOVA_CODEGEN_ERR_NOMEM;
                goto fail;
            }
            memset(proto->constants, 0,
                   const_count * sizeof(NovaConstant));
            proto->const_count = const_count;
            proto->const_capacity = const_count;

            for (i = 0; i < const_count; i++) {
                NovaConstant *k = &proto->constants[i];
                uint8_t tag = novai_read_u8(r);
                k->tag = (NovaConstTag)tag;

                switch (tag) {
                    case NOVA_CONST_NIL:
                        break;
                    case NOVA_CONST_BOOL:
                        k->as.boolean = novai_read_u8(r) != 0 ? 1 : 0;
                        break;
                    case NOVA_CONST_INTEGER:
                        k->as.integer = (nova_int_t)novai_read_i64(r);
                        break;
                    case NOVA_CONST_NUMBER:
                        k->as.number = (nova_number_t)novai_read_double(r);
                        break;
                    case NOVA_CONST_STRING: {
                        uint32_t slen = 0;
                        char *str = novai_read_string(r, &slen);
                        k->as.string.data = str;
                        k->as.string.length = slen;
                        break;
                    }
                    default:
                        /* Unknown tag: treat as nil, but continue */
                        k->tag = NOVA_CONST_NIL;
                        break;
                }

                if (r->error != 0) {
                    goto fail;
                }
            }
        }
    }

    /* ---- Upvalues ---- */
    {
        uint8_t upval_count = novai_read_u8(r);
        if (r->error != 0) {
            goto fail;
        }

        if (upval_count > 0) {
            proto->upvalues = (NovaUpvalDesc *)malloc(
                (size_t)upval_count * sizeof(NovaUpvalDesc));
            if (proto->upvalues == NULL) {
                r->error = NOVA_CODEGEN_ERR_NOMEM;
                goto fail;
            }
            memset(proto->upvalues, 0,
                   (size_t)upval_count * sizeof(NovaUpvalDesc));
            proto->upvalue_count = upval_count;

            for (i = 0; i < (uint32_t)upval_count; i++) {
                proto->upvalues[i].index = novai_read_u8(r);
                proto->upvalues[i].in_stack = novai_read_u8(r);
            }
            if (r->error != 0) {
                goto fail;
            }
        }
    }

    /* ---- Sub-prototypes (recursive) ---- */
    {
        uint32_t proto_count = novai_read_u32(r);
        if (r->error != 0) {
            goto fail;
        }

        if (proto_count > 0x10000u) {  /* 64K sub-protos max */
            r->error = NOVA_CODEGEN_ERR_CORRUPT;
            goto fail;
        }

        if (proto_count > 0) {
            proto->protos = (NovaProto **)malloc(
                proto_count * sizeof(NovaProto *));
            if (proto->protos == NULL) {
                r->error = NOVA_CODEGEN_ERR_NOMEM;
                goto fail;
            }
            memset(proto->protos, 0,
                   proto_count * sizeof(NovaProto *));
            proto->proto_count = proto_count;
            proto->proto_capacity = proto_count;

            for (i = 0; i < proto_count; i++) {
                proto->protos[i] = novai_read_proto(r);
                if (r->error != 0) {
                    goto fail;
                }
            }
        }
    }

    /* ---- Debug info ---- */
    {
        /* Line numbers */
        uint32_t line_count = novai_read_u32(r);
        if (r->error != 0) {
            goto fail;
        }

        if (line_count > 0) {
            proto->lines.line_numbers = (uint32_t *)malloc(
                line_count * sizeof(uint32_t));
            if (proto->lines.line_numbers == NULL) {
                r->error = NOVA_CODEGEN_ERR_NOMEM;
                goto fail;
            }
            proto->lines.count = line_count;

            for (i = 0; i < line_count; i++) {
                proto->lines.line_numbers[i] = novai_read_u32(r);
            }
            if (r->error != 0) {
                goto fail;
            }
        }

        /* Local variables */
        uint32_t local_count = novai_read_u32(r);
        if (r->error != 0) {
            goto fail;
        }

        if (local_count > 0) {
            proto->locals = (NovaLocalInfo *)malloc(
                local_count * sizeof(NovaLocalInfo));
            if (proto->locals == NULL) {
                r->error = NOVA_CODEGEN_ERR_NOMEM;
                goto fail;
            }
            memset(proto->locals, 0,
                   local_count * sizeof(NovaLocalInfo));
            proto->local_count = local_count;

            for (i = 0; i < local_count; i++) {
                NovaLocalInfo *loc = &proto->locals[i];
                uint32_t nlen = 0;
                loc->name = novai_read_string(r, &nlen);
                loc->start_pc = novai_read_u32(r);
                loc->end_pc = novai_read_u32(r);
                loc->reg = novai_read_u8(r);
                if (r->error != 0) {
                    goto fail;
                }
            }
        }

        /* Upvalue names */
        uint8_t upval_name_count = novai_read_u8(r);
        if (r->error != 0) {
            goto fail;
        }

        for (i = 0; i < (uint32_t)upval_name_count; i++) {
            if (i < (uint32_t)proto->upvalue_count) {
                uint32_t nlen = 0;
                char *name = novai_read_string(r, &nlen);
                proto->upvalues[i].name = name;
            } else {
                /* Extra names -- skip them */
                novai_skip_string(r);
            }
            if (r->error != 0) {
                goto fail;
            }
        }
    }

    return proto;

fail:
    nova_proto_destroy(proto);
    return NULL;
}

/* ============================================================
 * PART 7: TOP-LEVEL WRITE (HEADER + PROTO + EOF)
 * ============================================================ */

/**
 * @brief Build the complete .no buffer from a proto tree.
 *
 * Writes header with placeholder checksum, serializes the proto,
 * appends EOF marker, then patches the NXH64 checksum.
 *
 * @param proto     Root proto to serialize
 * @param buf       Write buffer (must be initialized)
 * @param flags     NOVA_CODEGEN_FLAG_*
 * @return NOVA_CODEGEN_OK or error code
 */
static int novai_build_buffer(const NovaProto *proto, NovaCodegenBuf *buf,
                              int flags) {
    if (proto == NULL || buf == NULL) {
        return NOVA_CODEGEN_ERR_NULLPTR;
    }

    /* ---- Header (28 bytes) ---- */

    /* Magic: "NOVA" */
    {
        uint8_t magic[4];
        magic[0] = (uint8_t)((NOVA_CODEGEN_MAGIC >> 24) & 0xFFu);
        magic[1] = (uint8_t)((NOVA_CODEGEN_MAGIC >> 16) & 0xFFu);
        magic[2] = (uint8_t)((NOVA_CODEGEN_MAGIC >> 8) & 0xFFu);
        magic[3] = (uint8_t)(NOVA_CODEGEN_MAGIC & 0xFFu);
        novai_write_bytes(buf, magic, 4);
    }

    /* Format version (2 bytes) */
    novai_write_u8(buf, NOVA_CODEGEN_VERSION_MAJOR);
    novai_write_u8(buf, NOVA_CODEGEN_VERSION_MINOR);

    /* Flags (2 bytes, LE) */
    {
        uint16_t file_flags = 0;
        if ((flags & NOVA_CODEGEN_FLAG_STRIP) == 0) {
            file_flags |= NOVA_CODEGEN_FLAG_DEBUG;
        }
        novai_write_u16(buf, file_flags);
    }

    /* Platform tag (4 bytes) */
    novai_write_u32(buf, NOVA_CODEGEN_PLATFORM_PORTABLE);

    /* Timestamp (8 bytes) */
    {
        time_t now = time(NULL);
        novai_write_u64(buf, (uint64_t)now);
    }

    /* Checksum placeholder (8 bytes, offset 0x14 = 20) */
    size_t checksum_offset = buf->size;
    novai_write_u64(buf, 0);  /* Will be patched */

    if (buf->error != 0) {
        return buf->error;
    }

    /* ---- Payload: Proto tree ---- */
    size_t payload_start = buf->size;
    novai_write_proto(buf, proto, flags);

    if (buf->error != 0) {
        return buf->error;
    }

    /* ---- EOF marker ---- */
    {
        uint8_t eof[4];
        eof[0] = (uint8_t)((NOVA_CODEGEN_EOF_MARKER >> 24) & 0xFFu);
        eof[1] = (uint8_t)((NOVA_CODEGEN_EOF_MARKER >> 16) & 0xFFu);
        eof[2] = (uint8_t)((NOVA_CODEGEN_EOF_MARKER >> 8) & 0xFFu);
        eof[3] = (uint8_t)(NOVA_CODEGEN_EOF_MARKER & 0xFFu);
        novai_write_bytes(buf, eof, 4);
    }

    if (buf->error != 0) {
        return buf->error;
    }

    /* ---- Patch NXH64 checksum ---- */
    /* Checksum covers everything from payload_start to end of buffer */
    {
        size_t payload_len = buf->size - payload_start;
        uint64_t checksum = nxh64(buf->data + payload_start, payload_len,
                                  (uint64_t)NOVA_CODEGEN_MAGIC);

        /* Write checksum at the reserved offset in LE */
        uint8_t ck_bytes[8];
        ck_bytes[0] = (uint8_t)(checksum & 0xFFu);
        ck_bytes[1] = (uint8_t)((checksum >> 8) & 0xFFu);
        ck_bytes[2] = (uint8_t)((checksum >> 16) & 0xFFu);
        ck_bytes[3] = (uint8_t)((checksum >> 24) & 0xFFu);
        ck_bytes[4] = (uint8_t)((checksum >> 32) & 0xFFu);
        ck_bytes[5] = (uint8_t)((checksum >> 40) & 0xFFu);
        ck_bytes[6] = (uint8_t)((checksum >> 48) & 0xFFu);
        ck_bytes[7] = (uint8_t)((checksum >> 56) & 0xFFu);
        memcpy(buf->data + checksum_offset, ck_bytes, 8);
    }

    return NOVA_CODEGEN_OK;
}

/* ============================================================
 * PART 8: TOP-LEVEL READ (HEADER VERIFY + PROTO)
 * ============================================================ */

/**
 * @brief Verify the .no header and set up the reader.
 *
 * @param r  Reader state (data and size must be set, pos=0)
 * @return NOVA_CODEGEN_OK or error code
 */
static int novai_verify_header(NovaCodegenReader *r) {
    /* Need at least header + EOF marker */
    if (r->size < NOVA_CODEGEN_HEADER_SIZE + 4) {
        return NOVA_CODEGEN_ERR_CORRUPT;
    }

    /* ---- Magic (4 bytes, big-endian ASCII) ---- */
    {
        uint8_t m0 = novai_read_u8(r);
        uint8_t m1 = novai_read_u8(r);
        uint8_t m2 = novai_read_u8(r);
        uint8_t m3 = novai_read_u8(r);

        uint32_t magic = ((uint32_t)m0 << 24) | ((uint32_t)m1 << 16)
                       | ((uint32_t)m2 << 8) | (uint32_t)m3;

        if (magic != NOVA_CODEGEN_MAGIC) {
            return NOVA_CODEGEN_ERR_MAGIC;
        }
    }

    /* ---- Format version (2 bytes) ---- */
    {
        uint8_t major = novai_read_u8(r);
        uint8_t minor = novai_read_u8(r);
        (void)minor;  /* Minor versions are backward compatible */

        if (major != NOVA_CODEGEN_VERSION_MAJOR) {
            return NOVA_CODEGEN_ERR_VERSION;
        }
    }

    /* ---- Flags (2 bytes, LE) ---- */
    {
        uint16_t file_flags = novai_read_u16(r);
        r->flags = (int)file_flags;
    }

    /* ---- Platform tag (4 bytes) ---- */
    {
        uint32_t platform = novai_read_u32(r);
        (void)platform;  /* Currently ignored; portable only */
    }

    /* ---- Timestamp (8 bytes) ---- */
    {
        uint64_t timestamp = novai_read_u64(r);
        (void)timestamp;  /* Informational only */
    }

    /* ---- Checksum (8 bytes) ---- */
    {
        uint64_t stored_checksum = novai_read_u64(r);

        /* Compute checksum over payload (everything after header) */
        size_t payload_start = NOVA_CODEGEN_HEADER_SIZE;
        size_t payload_len = r->size - payload_start;

        uint64_t computed = nxh64(r->data + payload_start, payload_len,
                                  (uint64_t)NOVA_CODEGEN_MAGIC);

        if (stored_checksum != computed) {
            return NOVA_CODEGEN_ERR_CORRUPT;
        }
    }

    /* ---- Verify EOF marker at end of file ---- */
    {
        size_t eof_pos = r->size - 4;
        uint32_t eof = ((uint32_t)r->data[eof_pos] << 24)
                     | ((uint32_t)r->data[eof_pos + 1] << 16)
                     | ((uint32_t)r->data[eof_pos + 2] << 8)
                     | (uint32_t)r->data[eof_pos + 3];

        if (eof != NOVA_CODEGEN_EOF_MARKER) {
            return NOVA_CODEGEN_ERR_CORRUPT;
        }
    }

    return NOVA_CODEGEN_OK;
}

/* ============================================================
 * PART 9: FILE I/O HELPERS
 * ============================================================ */

/**
 * @brief Read an entire file into a heap-allocated buffer.
 *
 * @param path      File path
 * @param buf_out   Output: pointer to buffer (caller frees)
 * @param size_out  Output: file size in bytes
 * @return NOVA_CODEGEN_OK or error code
 */
static int novai_read_file(const char *path, uint8_t **buf_out,
                           size_t *size_out) {
    if (path == NULL || buf_out == NULL || size_out == NULL) {
        return NOVA_CODEGEN_ERR_NULLPTR;
    }

    *buf_out = NULL;
    *size_out = 0;

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NOVA_CODEGEN_ERR_IO;
    }

    /* Get file size */
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NOVA_CODEGEN_ERR_IO;
    }

    long fsize = ftell(f);
    if (fsize < 0) {
        fclose(f);
        return NOVA_CODEGEN_ERR_IO;
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NOVA_CODEGEN_ERR_IO;
    }

    size_t size = (size_t)fsize;

    /* Allocate and read */
    uint8_t *buf = (uint8_t *)malloc(size);
    if (buf == NULL) {
        fclose(f);
        return NOVA_CODEGEN_ERR_NOMEM;
    }

    size_t read = fread(buf, 1, size, f);
    fclose(f);

    if (read != size) {
        free(buf);
        return NOVA_CODEGEN_ERR_IO;
    }

    *buf_out = buf;
    *size_out = size;
    return NOVA_CODEGEN_OK;
}

/**
 * @brief Write a buffer to a file.
 *
 * @param path  File path
 * @param data  Buffer to write
 * @param size  Size in bytes
 * @return NOVA_CODEGEN_OK or error code
 */
static int novai_write_file(const char *path, const uint8_t *data,
                            size_t size) {
    if (path == NULL || data == NULL) {
        return NOVA_CODEGEN_ERR_NULLPTR;
    }

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return NOVA_CODEGEN_ERR_IO;
    }

    size_t written = fwrite(data, 1, size, f);
    int flush_err = fflush(f);
    fclose(f);

    if (written != size || flush_err != 0) {
        return NOVA_CODEGEN_ERR_IO;
    }

    return NOVA_CODEGEN_OK;
}

/* ============================================================
 * PART 10: PUBLIC API
 * ============================================================ */

/**
 * @brief Save a compiled proto tree to a .no file.
 *
 * @param proto  Root prototype to serialize (must not be NULL)
 * @param path   Output file path (must not be NULL)
 * @param flags  NOVA_CODEGEN_FLAG_* values
 *
 * @return NOVA_CODEGEN_OK on success, negative error code on failure.
 */
int nova_codegen_save(const NovaProto *proto, const char *path, int flags) {
    if (proto == NULL || path == NULL) {
        return NOVA_CODEGEN_ERR_NULLPTR;
    }

    NovaCodegenBuf buf;
    novai_buf_init(&buf);

    if (buf.error != 0) {
        return buf.error;
    }

    int err = novai_build_buffer(proto, &buf, flags);
    if (err != NOVA_CODEGEN_OK) {
        novai_buf_free(&buf);
        return err;
    }

    err = novai_write_file(path, buf.data, buf.size);
    novai_buf_free(&buf);
    return err;
}

/**
 * @brief Load a .no file into a proto tree.
 *
 * @param path       Input file path (must not be NULL)
 * @param error_out  Output: error code (may be NULL)
 *
 * @return Root prototype on success, or NULL on failure.
 */
NovaProto *nova_codegen_load(const char *path, int *error_out) {
    int err = NOVA_CODEGEN_OK;

    if (path == NULL) {
        err = NOVA_CODEGEN_ERR_NULLPTR;
        if (error_out != NULL) {
            *error_out = err;
        }
        return NULL;
    }

    /* Read entire file */
    uint8_t *file_data = NULL;
    size_t file_size = 0;

    err = novai_read_file(path, &file_data, &file_size);
    if (err != NOVA_CODEGEN_OK) {
        if (error_out != NULL) {
            *error_out = err;
        }
        return NULL;
    }

    /* Undump from buffer */
    NovaProto *proto = nova_codegen_undump(file_data, file_size, &err);
    free(file_data);

    if (error_out != NULL) {
        *error_out = err;
    }
    return proto;
}

/**
 * @brief Serialize a proto tree to a memory buffer.
 *
 * @param proto     Root prototype (must not be NULL)
 * @param buf_out   Output: pointer to allocated buffer
 * @param size_out  Output: buffer size in bytes
 * @param flags     NOVA_CODEGEN_FLAG_* values
 *
 * @return NOVA_CODEGEN_OK on success, negative error code on failure.
 */
int nova_codegen_dump(const NovaProto *proto, uint8_t **buf_out,
                      size_t *size_out, int flags) {
    if (proto == NULL || buf_out == NULL || size_out == NULL) {
        return NOVA_CODEGEN_ERR_NULLPTR;
    }

    *buf_out = NULL;
    *size_out = 0;

    NovaCodegenBuf buf;
    novai_buf_init(&buf);

    if (buf.error != 0) {
        return buf.error;
    }

    int err = novai_build_buffer(proto, &buf, flags);
    if (err != NOVA_CODEGEN_OK) {
        novai_buf_free(&buf);
        return err;
    }

    /* Transfer ownership to caller */
    *buf_out = buf.data;
    *size_out = buf.size;
    /* Don't free buf -- caller owns the data now */

    return NOVA_CODEGEN_OK;
}

/**
 * @brief Deserialize a proto tree from a memory buffer.
 *
 * @param buf        Input buffer containing .no data
 * @param size       Buffer size in bytes
 * @param error_out  Output: error code (may be NULL)
 *
 * @return Root prototype on success, or NULL on failure.
 */
NovaProto *nova_codegen_undump(const uint8_t *buf, size_t size,
                               int *error_out) {
    int err = NOVA_CODEGEN_OK;

    if (buf == NULL) {
        err = NOVA_CODEGEN_ERR_NULLPTR;
        if (error_out != NULL) {
            *error_out = err;
        }
        return NULL;
    }

    NovaCodegenReader reader;
    reader.data = buf;
    reader.size = size;
    reader.pos = 0;
    reader.flags = 0;
    reader.error = 0;

    /* Verify header */
    err = novai_verify_header(&reader);
    if (err != NOVA_CODEGEN_OK) {
        if (error_out != NULL) {
            *error_out = err;
        }
        return NULL;
    }

    /* Deserialize proto tree */
    NovaProto *proto = novai_read_proto(&reader);
    if (proto == NULL || reader.error != 0) {
        if (proto != NULL) {
            nova_proto_destroy(proto);
        }
        err = (reader.error != 0) ? reader.error : NOVA_CODEGEN_ERR_CORRUPT;
        if (error_out != NULL) {
            *error_out = err;
        }
        return NULL;
    }

    if (error_out != NULL) {
        *error_out = NOVA_CODEGEN_OK;
    }
    return proto;
}

/**
 * @brief Get a human-readable error message for a codegen error code.
 *
 * @param error  Error code
 * @return Static string (never NULL)
 */
const char *nova_codegen_strerror(int error) {
    switch (error) {
        case NOVA_CODEGEN_OK:
            return "success";
        case NOVA_CODEGEN_ERR_IO:
            return "file I/O error";
        case NOVA_CODEGEN_ERR_NOMEM:
            return "memory allocation failed";
        case NOVA_CODEGEN_ERR_MAGIC:
            return "invalid .no file (bad magic number)";
        case NOVA_CODEGEN_ERR_VERSION:
            return "unsupported .no format version";
        case NOVA_CODEGEN_ERR_CORRUPT:
            return "corrupt .no file (checksum mismatch or truncated)";
        case NOVA_CODEGEN_ERR_EOF:
            return "unexpected end of .no data";
        case NOVA_CODEGEN_ERR_NULLPTR:
            return "NULL argument";
        default:
            return "unknown codegen error";
    }
}
