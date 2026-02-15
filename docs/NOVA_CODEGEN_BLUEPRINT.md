# Nova Code Generator Blueprint

Design reference for `nova_codegen.h` and `nova_codegen.c`.
Serializes NovaProto trees to the `.no` binary format and loads them back.

## Binary Format (.no) -- from NOVA_DESIGN.md Section 5.2

```
Offset  Size  Field
------  ----  -----
0x00    4     Magic: "NOVA" (0x4E4F5641, big-endian ASCII)
0x04    1     Format major version (0x01)
0x05    1     Format minor version (0x00)
0x06    2     Flags (little-endian)
                bit 0: 0=little-endian data, 1=big-endian data
                bit 1: 1=debug info present
                bit 2: 1=stripped (no debug)
                bits 3-15: reserved (0)
0x08    4     Platform tag (0x00000000 = portable)
0x0C    8     Timestamp (uint64_t, unix epoch, little-endian)
0x14    8     NXH64 checksum of all bytes after this field
0x1C    --    === Payload starts here ===
```

**Total header: 28 bytes (0x1C)**

### Payload Sections (in order)

1. **String Table**
   - `uint32_t count` — number of strings
   - For each string: `uint16_t length` + `uint8_t data[length]` (no NUL)

2. **Main Proto** (recursive)
   - Encoded inline; sub-protos are nested recursively

3. **EOF Marker**: `uint32_t 0xDEADN0VA` (0xDEAD4E56 if we use ASCII for N=0x4E, but let's use 0xDEADN0VA as the doc says — actually let's pick a clean constant: `0xDEAD4E56` which is DEAD + 'NV' for NoVa)

Actually, let's use `0xDEAD0A0E` — or simply define it clearly. The design doc says `0xDEADNOVA`. Since that's not valid hex, we'll use `0xDEAD4E56` (DEAD + 'N'=0x4E + 'V'=0x56).

Wait — let's just use the literal approach:
- `NOVA_CODEGEN_EOF_MARKER  0xDEADBABE` — simple, memorable, standard sentinel.

No, let's stay on brand: `0x4E4F5600` — "NOV\0". Or just define a 4-byte marker. Let me keep it simple and use `0xDEADNOVA` concept but pick `0xDEAD4E56` for the actual hex value.

## Proto Serialization Format

Each proto is serialized recursively:

```
Proto {
    /* Metadata */
    uint8_t   num_params
    uint8_t   is_vararg
    uint8_t   is_async
    uint8_t   max_stack
    uint32_t  line_defined
    uint32_t  last_line
    
    /* Source name (index into string table, or inline) */
    uint16_t  source_length     (0 if NULL)
    uint8_t   source_data[source_length]
    
    /* Instructions */
    uint32_t  code_count
    uint32_t  code[code_count]   (each 4 bytes, little-endian)
    
    /* Constants */
    uint32_t  const_count
    Constant  constants[const_count] {
        uint8_t tag   (0=nil, 1=bool, 2=integer, 3=number, 4=string)
        /* tag 0 (nil):     no data */
        /* tag 1 (bool):    uint8_t value */
        /* tag 2 (integer): int64_t value (8 bytes LE) */
        /* tag 3 (number):  double value (8 bytes LE, IEEE 754) */
        /* tag 4 (string):  uint16_t length + uint8_t data[length] */
    }
    
    /* Upvalues */
    uint8_t   upvalue_count
    Upvalue   upvalues[upvalue_count] {
        uint8_t index
        uint8_t in_stack
    }
    
    /* Sub-prototypes */
    uint32_t  proto_count
    Proto     protos[proto_count]   (recursive!)
    
    /* Debug info (if flags bit 1 set) */
    DebugInfo {
        /* Line numbers */
        uint32_t  line_count     (should == code_count)
        uint32_t  lines[line_count]
        
        /* Local variables */
        uint32_t  local_count
        Local     locals[local_count] {
            uint16_t name_length
            uint8_t  name_data[name_length]
            uint32_t start_pc
            uint32_t end_pc
            uint8_t  reg
        }
        
        /* Upvalue names */
        uint8_t  upval_name_count   (should == upvalue_count)
        UpvalName upval_names[upval_name_count] {
            uint16_t name_length    (0 if NULL)
            uint8_t  name_data[name_length]
        }
    }
}
```

## File Structure

```
nova_codegen.c (~1400 lines estimated)
├── Part 0: File header + includes + constants
├── Part 1: Write helpers (write_u8, write_u16, write_u32, write_u64, write_double, write_bytes, write_string)
├── Part 2: Read helpers (read_u8, read_u16, read_u32, read_u64, read_double, read_bytes, read_string)
├── Part 3: Proto serialization (write_proto recursive)
├── Part 4: Proto deserialization (read_proto recursive)
├── Part 5: Top-level save (.no file writer)
├── Part 6: Top-level load (.no file reader + verification)
├── Part 7: Memory buffer save/load variants
├── Part 8: Dump to buffer (for embedding)
```

## nova_codegen.h API

```c
/* Save a proto tree to a .no file */
int nova_codegen_save(const NovaProto *proto, const char *path, int flags);

/* Load a proto tree from a .no file */
NovaProto *nova_codegen_load(const char *path, int *error_out);

/* Save to memory buffer (caller frees) */
int nova_codegen_dump(const NovaProto *proto, uint8_t **buf_out, 
                      size_t *size_out, int flags);

/* Load from memory buffer */
NovaProto *nova_codegen_undump(const uint8_t *buf, size_t size,
                               int *error_out);
```

## Flags

```c
#define NOVA_CODEGEN_FLAG_DEBUG   0x0002  /* Include debug info */
#define NOVA_CODEGEN_FLAG_STRIP   0x0004  /* Strip debug info */
```

Default: debug info included.

## Error Codes

```c
#define NOVA_CODEGEN_OK           0
#define NOVA_CODEGEN_ERR_IO      -1   /* File I/O error */
#define NOVA_CODEGEN_ERR_NOMEM   -2   /* Allocation failure */
#define NOVA_CODEGEN_ERR_MAGIC   -3   /* Bad magic number */
#define NOVA_CODEGEN_ERR_VERSION -4   /* Unsupported format version */
#define NOVA_CODEGEN_ERR_CORRUPT -5   /* Checksum mismatch or truncated */
#define NOVA_CODEGEN_ERR_EOF     -6   /* Unexpected end of data */
```

## Write Buffer

Use a growable buffer to avoid multiple file writes:

```c
typedef struct {
    uint8_t  *data;
    size_t    size;
    size_t    capacity;
} NovaCodegenBuf;
```

All write_* functions append to this buffer. When done, compute NXH64 
checksum over the payload, patch it into the header, write entire buffer 
to file in one fwrite() call.

## Read State

```c
typedef struct {
    const uint8_t *data;
    size_t         size;
    size_t         pos;
    int            flags;    /* From header */
    int            error;    /* Set on first error */
} NovaCodegenReader;
```

All read_* functions advance pos. On any error, set reader->error and return
zero/NULL. Callers check reader->error after each section.

## Checksum Strategy

1. Write header with checksum = 0
2. Write all payload sections into buffer
3. Compute NXH64 over bytes [0x1C .. end] (everything after checksum field)
4. Patch checksum at offset 0x14
5. Write buffer to file

On load:
1. Read full file into memory
2. Extract checksum from header
3. Compute NXH64 over bytes [0x1C .. end-4] (before EOF marker)
4. Compare. If mismatch, return NOVA_CODEGEN_ERR_CORRUPT

## Endianness

All multi-byte values are little-endian. The write helpers encode as LE
regardless of host byte order. The read helpers decode from LE.

```c
static void novai_write_u32(NovaCodegenBuf *buf, uint32_t v) {
    uint8_t bytes[4];
    bytes[0] = (uint8_t)(v & 0xFF);
    bytes[1] = (uint8_t)((v >> 8) & 0xFF);
    bytes[2] = (uint8_t)((v >> 16) & 0xFF);
    bytes[3] = (uint8_t)((v >> 24) & 0xFF);
    novai_write_bytes(buf, bytes, 4);
}
```

## Key Design Decisions

1. **Recursive proto encoding**: Sub-protos are nested inline. No separate
   proto table with indices. Simpler to implement, slightly larger output,
   but matches the recursive NovaProto tree structure naturally.

2. **String table**: NOT a shared top-level string table (unlike the design
   doc). Instead, strings are written inline in each constant pool and debug
   section. Simplifies implementation significantly. A shared string table
   can be added as a v1.1 optimization.

3. **Debug info is always included by default**. Use NOVA_CODEGEN_FLAG_STRIP
   to omit it. This matches `gcc -g` vs `strip` philosophy.

4. **EOF marker**: Validates that the file wasn't truncated.

5. **One-shot memory buffer**: Build entire output in memory, compute
   checksum, then write. For typical Nova scripts (<1MB bytecode), this is
   fine. Streaming write can be added later if needed.
