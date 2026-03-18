/**
 * @file nova_vm.h
 * @brief Nova Language - Virtual Machine Types and API
 *
 * Register-based bytecode virtual machine with:
 *   - Tagged-union value representation (NaN-boxing planned for v0.2)
 *   - 256 registers per call frame (8-bit addressing)
 *   - Computed goto dispatch (GCC/Clang) with switch fallback
 *   - Open upvalue chain for closure variable capture
 *   - Growable value stack with call frame array
 *
 * The VM executes NovaProto trees produced by the compiler pipeline
 * (lexer -> preprocessor -> parser -> compiler -> optimizer -> codegen).
 * It provides the core execution engine; the C API layer, GC, and
 * standard library are separate modules that build atop this.
 *
 * NOTE: This header replaces the earlier draft that included
 * nova_object.h and nova_code.h. Those files are design-phase
 * drafts. The actual pipeline uses nova_opcode.h and nova_proto.h.
 * The old nova_vm.h is preserved as nova_vm.h.draft_backup.
 *
 * @author Anthony Taliento
 * @date 2026-02-07
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * DEPENDENCIES:
 *   - nova_proto.h (NovaProto, NovaConstant)
 *   - nova_opcode.h (NovaInstruction, opcode macros)
 *   - nova_conf.h (limits, numeric types)
 *
 * THREAD SAFETY:
 *   Not thread-safe. Each thread should own a separate NovaVM.
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#ifndef NOVA_VM_H
#define NOVA_VM_H

#include "nova_conf.h"
#include "nova_opcode.h"
#include "nova_proto.h"

#include <setjmp.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

/* ============================================================
 * GARBAGE COLLECTOR INFRASTRUCTURE
 *
 * Every heap-allocated object embeds a NovaGCHeader as its first
 * field, allowing generic traversal via the all_objects list.
 *
 * Tri-color invariant:
 *   WHITE: unreached by current mark pass
 *   GRAY:  reached, but children not yet scanned
 *   BLACK: fully scanned (all children gray or black)
 *
 * Two white colors (WHITE0, WHITE1) enable Lua-style flip-flop:
 * "current white" alternates each cycle, so newly allocated
 * objects are always the current white without needing to
 * traverse the entire object list at sweep start.
 * ============================================================ */

/** GC colors for tri-color marking */
typedef enum {
    NOVA_GC_WHITE0 = 0,     /**< White (unreached), generation A  */
    NOVA_GC_WHITE1 = 1,     /**< White (unreached), generation B  */
    NOVA_GC_GRAY   = 2,     /**< Reached, children not scanned    */
    NOVA_GC_BLACK  = 3      /**< Fully scanned                    */
} NovaGCColor;

/** GC state machine phases (prefixed PHASE_ to avoid clashing with
 * NOVA_GC_PAUSE config macro in nova_conf.h) */
typedef enum {
    NOVA_GC_PHASE_PAUSE = 0,    /**< Idle, waiting for threshold      */
    NOVA_GC_PHASE_MARK  = 1,    /**< Incremental marking in progress  */
    NOVA_GC_PHASE_SWEEP = 2     /**< Sweeping dead objects             */
} NovaGCPhase;

/**
 * @brief GC header embedded as the FIRST field in every heap object.
 *
 * This must be the first field so that (NovaGCHeader*)obj == obj,
 * enabling safe casting between object types and the generic header.
 * 24 bytes on 64-bit (two pointers + 4 bytes metadata + 4 bytes size).
 */
typedef struct NovaGCHeader {
    struct NovaGCHeader *gc_next;    /**< Next object in all_objects chain */
    struct NovaGCHeader *gc_gray;    /**< Next object in gray list (mark)  */
    uint8_t              gc_type;    /**< NovaValueType tag for this obj   */
    uint8_t              gc_color;   /**< NovaGCColor                      */
    uint8_t              gc_flags;   /**< Object-specific flags            */
    uint8_t              _gc_pad;    /**< Alignment padding                */
    uint32_t             gc_size;    /**< Allocation size (for accounting) */
} NovaGCHeader;

/** Initialize a GC header on a freshly allocated object */
#define NOVA_GC_INIT(hdr, typ, col, sz) do { \
    (hdr)->gc_next  = NULL;                  \
    (hdr)->gc_gray  = NULL;                  \
    (hdr)->gc_type  = (uint8_t)(typ);        \
    (hdr)->gc_color = (uint8_t)(col);        \
    (hdr)->gc_flags = 0;                     \
    (hdr)->_gc_pad  = 0;                     \
    (hdr)->gc_size  = (uint32_t)(sz);        \
} while (0)

/** Access the GC header of any heap object (must be first field) */
#define NOVA_GC_HDR(obj) ((NovaGCHeader *)(obj))

/** Check if an object is white (either white) */
#define NOVA_GC_IS_WHITE(hdr) ((hdr)->gc_color <= NOVA_GC_WHITE1)

/** Check if an object is gray */
#define NOVA_GC_IS_GRAY(hdr)  ((hdr)->gc_color == NOVA_GC_GRAY)

/** Check if an object is black */
#define NOVA_GC_IS_BLACK(hdr) ((hdr)->gc_color == NOVA_GC_BLACK)

/* ============================================================
 * VALUE TYPE TAGS
 *
 * Runtime type tags. These extend the compile-time NovaConstTag
 * with heap-allocated object types.
 * ============================================================ */

typedef enum {
    NOVA_TYPE_NIL       = 0,
    NOVA_TYPE_BOOL      = 1,
    NOVA_TYPE_INTEGER   = 2,
    NOVA_TYPE_NUMBER    = 3,
    NOVA_TYPE_STRING    = 4,
    NOVA_TYPE_TABLE     = 5,
    NOVA_TYPE_FUNCTION  = 6,    /* Nova closure                   */
    NOVA_TYPE_CFUNCTION = 7,    /* C function                     */
    NOVA_TYPE_USERDATA  = 8,
    NOVA_TYPE_THREAD    = 9     /* Coroutine                      */
} NovaValueType;

/* ============================================================
 * FORWARD DECLARATIONS
 * ============================================================ */

typedef struct NovaVM       NovaVM;
typedef struct NovaString   NovaString;
typedef struct NovaTable    NovaTable;
typedef struct NovaClosure  NovaClosure;
typedef struct NovaUpvalue  NovaUpvalue;
typedef struct NovaCoroutine NovaCoroutine;

/* ============================================================
 * C FUNCTION SIGNATURE
 *
 * All C functions callable from Nova have this signature.
 * Receives the VM pointer, returns number of results pushed.
 * ============================================================ */

typedef int (*nova_cfunc_t)(NovaVM *vm);

/* ============================================================
 * RUNTIME VALUE REPRESENTATION
 *
 * Two compile-time modes controlled by NOVA_NAN_BOXING:
 *
 *   NOVA_NAN_BOXING=1 (x86_64, ARM64):
 *     NovaValue is a uint64_t. All values packed into 64 bits
 *     using IEEE 754 quiet-NaN space for type tags. 8 bytes.
 *
 *   NOVA_NAN_BOXING=0 (portable fallback):
 *     NovaValue is a struct { type + union }. 16 bytes.
 *
 * Both modes present the SAME abstraction layer (see below).
 * Consumer code MUST use the nova_is/nova_as/nova_value
 * macros and constructors exclusively.
 * ============================================================ */

#if NOVA_NAN_BOXING
/* --------------------------------------------------------
 * NaN-Boxing Layout (64-bit)
 *
 * IEEE 754: bits 62:52 = 0x7FF → NaN. Bit 51 → quiet NaN.
 * QNAN = 0x7FFC000000000000 (quiet NaN + 2 tag bits clear)
 *
 * Encoding:
 *   Double:    Any bit pattern where (v & QNAN) != QNAN
 *   Nil:       QNAN | TAG_NIL
 *   Bool:      QNAN | TAG_BOOL | (0 or 1)
 *   Integer:   SIGN_BIT | QNAN | (48-bit value)
 *   Object:    QNAN | TAG_OBJ  | (48-bit NovaGCHeader*)
 *   CFunction: QNAN | TAG_CFUNC | (48-bit function pointer)
 *
 * The sign bit is reserved for integer tagging.
 * Object sub-types (string, table, closure, etc.) are resolved
 * by dereferencing the GC header's gc_type field.
 * -------------------------------------------------------- */

/** Quiet NaN bit pattern we commandeer */
#define NOVA_QNAN       ((uint64_t)0x7FFC000000000000ULL)

/** Sign bit (used exclusively for integer tagging) */
#define NOVA_SIGN_BIT   ((uint64_t)0x8000000000000000ULL)

/** Type tags packed into bits 48..51 of the NaN payload */
#define NOVA_TAG_NIL    ((uint64_t)0x0001000000000000ULL)
#define NOVA_TAG_BOOL   ((uint64_t)0x0002000000000000ULL)
#define NOVA_TAG_OBJ    ((uint64_t)0x0003000000000000ULL)
#define NOVA_TAG_CFUNC  ((uint64_t)0x0004000000000000ULL)

/** Mask for isolating the 4-bit type tag field (bits 48..51) */
#define NOVA_TAG_MASK   ((uint64_t)0x000F000000000000ULL)

/** 48-bit pointer/payload extraction mask */
#define NOVA_PAYLOAD_MASK ((uint64_t)0x0000FFFFFFFFFFFFULL)

/** NaN-boxed value: a single uint64_t */
typedef uint64_t NovaValue;

/** Sentinel values */
#define NOVA_VALUE_NIL    (NOVA_QNAN | NOVA_TAG_NIL)
#define NOVA_VALUE_TRUE   (NOVA_QNAN | NOVA_TAG_BOOL | (uint64_t)1)
#define NOVA_VALUE_FALSE  (NOVA_QNAN | NOVA_TAG_BOOL | (uint64_t)0)

#else /* !NOVA_NAN_BOXING — Portable tagged union */

typedef struct NovaValue {
    NovaValueType type;
    union {
        int            boolean;
        nova_int_t     integer;
        nova_number_t  number;
        NovaString    *string;
        NovaTable     *table;
        NovaClosure   *closure;
        nova_cfunc_t   cfunc;
        void          *userdata;
        NovaCoroutine *coroutine;
    } as;
} NovaValue;

/** Sentinel values (compound literals are fine in C99) */
#define NOVA_VALUE_NIL   ((NovaValue){ .type = NOVA_TYPE_NIL, .as = {0} })
#define NOVA_VALUE_TRUE  ((NovaValue){ .type = NOVA_TYPE_BOOL, .as.boolean = 1 })
#define NOVA_VALUE_FALSE ((NovaValue){ .type = NOVA_TYPE_BOOL, .as.boolean = 0 })

#endif /* NOVA_NAN_BOXING */

/* ============================================================
 * STRING OBJECT
 *
 * Heap-allocated, immutable after creation.
 * Hash computed once at creation via NXH64.
 * Flexible array member for inline data storage.
 * ============================================================ */

struct NovaString {
    NovaGCHeader gc;        /* GC header (MUST be first field)    */
    size_t    length;       /* Byte length (not including NUL)    */
    uint64_t  hash;         /* NXH64 hash (computed at creation)  */
    char      data[];       /* Flexible array: NUL-terminated     */
};

/* ============================================================
 * TABLE OBJECT
 *
 * Minimal array + hash hybrid for Phase 7.
 * Array part: integer keys [0..array_used-1]
 * Hash part: linear-probing open-addressing table
 * DAGGER integration is planned for Phase 9.
 * ============================================================ */

/** Hash table entry */
typedef struct {
    NovaValue key;
    NovaValue value;
    int       occupied;     /* 1 if slot is in use                */
} NovaTableEntry;

struct NovaTable {
    NovaGCHeader gc;                /* GC header (MUST be first field)  */
    NovaValue      *array;          /* Array part                     */
    uint32_t        array_size;     /* Array part allocated capacity  */
    uint32_t        array_used;     /* Highest integer key + 1        */
    NovaTableEntry *hash;           /* Hash part                      */
    uint32_t        hash_size;      /* Hash capacity (power of 2)     */
    uint32_t        hash_used;      /* Number of occupied slots       */
    NovaTable      *metatable;      /* Metatable (NULL if none)       */
};

/* ============================================================
 * UPVALUE (Closure Variable Capture)
 *
 * While the local is on the stack: location = &stack[reg]
 * After scope ends (CLOSE): location = &closed, value copied.
 * Open upvalues form a linked list sorted by stack position.
 * ============================================================ */

struct NovaUpvalue {
    NovaGCHeader gc;                /* GC header (MUST be first field)  */
    NovaValue          *location;   /* Points to stack or &closed     */
    NovaValue           closed;     /* Holds value after local dies   */
    struct NovaUpvalue *next;       /* Next open upvalue in chain     */
};

/* ============================================================
 * CLOSURE (Function + Captured Upvalues)
 * ============================================================ */

struct NovaClosure {
    NovaGCHeader gc;                /* GC header (MUST be first field)  */
    const NovaProto  *proto;        /* Function prototype             */
    NovaUpvalue     **upvalues;     /* Captured upvalue array         */
    uint8_t           upvalue_count;/* Number of upvalues             */
};

/* ============================================================
 * COROUTINE STATUS
 * ============================================================ */

typedef enum {
    NOVA_CO_SUSPENDED = 0,  /* Created or yielded, waiting to resume  */
    NOVA_CO_RUNNING   = 1,  /* Currently executing                    */
    NOVA_CO_DEAD      = 2,  /* Finished or errored, cannot resume     */
    NOVA_CO_NORMAL    = 3   /* Resumed another coroutine (suspended)  */
} NovaCoStatus;

/* NOTE: NovaCoroutine struct defined after NovaCallFrame and
 * NovaErrorJmp (which it depends on). See below. */

/* ============================================================
 * CALL FRAME
 *
 * Each function call pushes a frame. Contains saved IP,
 * base register pointer, closure reference, and result count.
 * ============================================================ */

typedef struct {
    const NovaProto       *proto;   /* Function being executed        */
    const NovaInstruction *ip;      /* Saved instruction pointer      */
    NovaValue             *base;    /* Base of frame's registers      */
    NovaClosure           *closure; /* Closure (for upvalue access)   */
    int                    num_results; /* Expected results from caller */
    int                    num_args;    /* Actual args passed by caller */
    NovaValue             *varargs;    /* Saved vararg values (heap)   */
    int                    num_varargs; /* Count of saved varargs       */
} NovaCallFrame;

/* ============================================================
 * VM STATUS CODES
 * ============================================================ */

#define NOVA_VM_OK              0
#define NOVA_VM_ERR_RUNTIME     1
#define NOVA_VM_ERR_MEMORY      2
#define NOVA_VM_ERR_STACKOVERFLOW 3
#define NOVA_VM_ERR_TYPE        4
#define NOVA_VM_ERR_DIVZERO     5
#define NOVA_VM_ERR_NULLPTR     6
#define NOVA_VM_YIELD           7   /* Execution suspended by yield   */

/* ============================================================
 * PROTECTED CALL ERROR RECOVERY
 *
 * Linked list of setjmp buffers for pcall/xpcall error
 * recovery. When novai_error() fires and error_jmp is set,
 * it longjmps back to the most recent protected frame.
 * ============================================================ */

typedef struct NovaErrorJmp {
    struct NovaErrorJmp *previous;  /* Enclosing protected frame    */
    jmp_buf              buf;       /* setjmp/longjmp buffer        */
    int                  status;    /* Error code after longjmp     */
    int                  frame_count; /* Saved frame_count           */
    NovaValue           *stack_top;   /* Saved stack_top             */
} NovaErrorJmp;

/* ============================================================
 * COROUTINE (Thread) OBJECT
 *
 * Each coroutine owns its own value stack and call frame
 * array, but shares the global table and GC with the parent
 * VM. This enables cooperative multitasking via resume/yield.
 * ============================================================ */

#define NOVA_COROUTINE_STACK_SIZE  256
#define NOVA_COROUTINE_MAX_FRAMES  64

struct NovaCoroutine {
    NovaGCHeader    gc;             /* GC header (MUST be first field)  */
    NovaValue      *stack;          /* Own value stack (heap-allocated) */
    NovaValue      *stack_top;      /* First free slot in own stack    */
    size_t          stack_size;     /* Allocated stack slots           */
    NovaCallFrame  *frames;         /* Own call frame array (heap)     */
    int             frame_count;    /* Current call depth              */
    int             max_frames;     /* Allocated frame slots           */
    NovaClosure    *body;           /* The function this coroutine runs*/
    NovaVM         *vm;             /* Parent VM (shared globals/GC)   */
    NovaCoStatus    status;         /* Coroutine state machine         */
    NovaUpvalue    *open_upvalues;  /* Coroutine-local upvalue chain   */
    NovaErrorJmp   *error_jmp;      /* Coroutine-local pcall chain     */
    int             meta_stop_frame;/* Frame boundary for nested calls */
    int             nresults;       /* Number of results from last yield*/

    /* == Async support (pending args from async function call) = */
    NovaValue      *pending_args;   /* Stashed args for first resume   */
    int             pending_nargs;  /* Number of stashed arguments     */
};

/* ============================================================
 * SAVED STACK REFERENCE (for GC root marking during coroutines)
 *
 * When a coroutine is resumed, the caller's stack and frames
 * are saved on the C stack (local variables) and become invisible
 * to the GC.  NovaSavedStackRef records a pointer to each saved
 * stack so the GC can mark those values as roots.
 *
 * This struct lives ON THE C STACK (as a local variable in
 * nova_coroutine_resume).  It is valid for exactly the lifetime
 * of the coroutine execution and is automatically popped when
 * the coroutine yields or returns.
 *
 * Handles nesting: A resumes B resumes C — each level pushes
 * its own NovaSavedStackRef, forming a linked list from the
 * innermost (current) to the outermost (main thread) saved state.
 * ============================================================ */

typedef struct NovaSavedStackRef {
    NovaValue           *stack;       /**< Caller's stack base        */
    NovaValue           *stack_top;   /**< Caller's stack top         */
    size_t               stack_size;  /**< Caller's stack allocation  */
    int                  frame_count; /**< Caller's frame count       */
    const NovaCallFrame *frames;      /**< Caller's saved frames      */
    struct NovaSavedStackRef *prev;   /**< Previous saved stack       */
} NovaSavedStackRef;

/* ============================================================
 * VM STATE
 *
 * The central runtime structure. Owns the value stack, call
 * frames, global table, and open upvalue chain.
 * GC fields are placeholder stubs for Phase 8.
 * ============================================================ */

struct NovaVM {
    /* == Value stack ========================================== */
    NovaValue     *stack;           /* Growable value stack         */
    NovaValue     *stack_top;       /* First free slot              */
    size_t         stack_size;      /* Allocated stack slots        */

    /* == Call frames ========================================== */
    NovaCallFrame  frames[NOVA_MAX_CALL_DEPTH];
    int            frame_count;     /* Current depth                */

    /* == Global environment =================================== */
    NovaTable     *globals;

    /* == Open upvalue chain (sorted by stack position desc) === */
    NovaUpvalue   *open_upvalues;

    /* == C function call context ============================== */
    NovaValue     *cfunc_base;      /* Rebase point for cfunc args  */

    /* == Metamethod pipeline state ============================ */
    int            meta_stop_frame; /* -1 = no stop; else stop here */

    /* == Coroutine state ====================================== */
    NovaCoroutine *running_coroutine; /* Currently executing coroutine */

    /* == Saved caller stacks (GC root chain) ================== */
    /* When a coroutine is resumed, the caller's stack and frames
     * are saved on the C stack.  The GC must mark those saved
     * stacks to prevent live objects from being swept.  This chain
     * links all saved caller contexts, enabling the GC to traverse
     * them as additional root sets.  Handles nested coroutine
     * resumes (A resumes B resumes C) by chaining nodes. */
    struct NovaSavedStackRef *saved_stacks;

    /* == Async task scheduler ================================= */
    /* Round-robin task queue for spawned async tasks. Tasks are
     * coroutines registered via OP_SPAWN or async.spawn(). The
     * event loop (async.run) resumes tasks in order until all
     * are DEAD. */
    NovaCoroutine **task_queue;     /* Array of spawned task ptrs  */
    int             task_count;     /* Number of active tasks      */
    int             task_capacity;  /* Allocated slots             */

    /* == Error state ========================================== */
    int            status;          /* NOVA_VM_* status code        */
    int            diag_code;       /* NovaErrorCode for diagnostics*/
    char          *error_msg;       /* Heap-allocated error string  */
    NovaErrorJmp  *error_jmp;       /* Protected call chain (pcall) */

    /* == Garbage collector ===================================== */
    NovaGCHeader  *all_objects;     /* Head of all GC objects list  */
    NovaGCHeader  *gray_list;       /* Objects marked gray (to scan)*/
    size_t         bytes_allocated; /* Current live bytes           */
    size_t         gc_threshold;    /* Next GC trigger threshold    */
    int            gc_pause;        /* Pause between cycles (%)     */
    int            gc_step_mul;     /* Work per step multiplier     */
    uint8_t        gc_phase;        /* NovaGCPhase state machine    */
    uint8_t        gc_current_white;/* Current white (0 or 1)       */
    uint8_t        gc_running;      /* 1 if GC is active            */
    uint8_t        gc_emergency;    /* 1 during emergency collect   */
    NovaGCHeader **gc_sweep_pos;    /* Ptr-to-ptr into all_objects   */
    size_t         gc_estimate;     /* Estimate of live bytes       */
    size_t         gc_debt;         /* Accumulated alloc debt       */

    /* == Per-type metatables (GC-rooted) ===================== */
    NovaTable     *string_mt;       /* Shared string metatable      */

    /* == String interning (DAGGER-backed) ===================== */
    void          *string_pool;     /* DaggerTable* — intern table  */
    size_t         string_bytes;    /* Bytes in interned strings    */
};

/* ============================================================
 * VM LIFECYCLE
 * ============================================================ */

/**
 * @brief Create a new VM instance with default configuration.
 *
 * Allocates the value stack (NOVA_INITIAL_STACK_SIZE),
 * creates the global table, and initializes all state.
 *
 * @return New VM, or NULL on allocation failure.
 *
 * COMPLEXITY: O(1)
 * THREAD SAFETY: Not thread-safe
 */
NovaVM *nova_vm_create(void);

/**
 * @brief Destroy a VM and free all owned resources.
 *
 * Frees the value stack, global table, open upvalues,
 * error message, and the VM struct itself.
 *
 * @param vm  VM to destroy (NULL is safe, no-op).
 *
 * COMPLEXITY: O(n) where n = allocated objects
 * THREAD SAFETY: Not thread-safe
 */
void nova_vm_destroy(NovaVM *vm);

/* ============================================================
 * EXECUTION
 * ============================================================ */

/**
 * @brief Execute a compiled prototype as a top-level chunk.
 *
 * Creates a closure from the proto, pushes it as frame 0,
 * and runs the dispatch loop until RETURN.
 *
 * @param vm     VM instance (must not be NULL)
 * @param proto  Compiled prototype (must not be NULL)
 *
 * @return NOVA_VM_OK on success, or a NOVA_VM_ERR_* code.
 *
 * COMPLEXITY: O(instructions executed)
 * THREAD SAFETY: Not thread-safe
 */
int nova_vm_execute(NovaVM *vm, const NovaProto *proto);

/**
 * @brief Create a new closure from a compiled prototype.
 *
 * Allocates a closure object, links it to the GC, and
 * initializes upvalue slots to NULL. Used by the module
 * loader to wrap a compiled proto for nova_vm_call().
 *
 * @param vm     VM instance (used for GC; may be NULL)
 * @param proto  Compiled prototype (must not be NULL)
 * @return New closure, or NULL on allocation failure.
 */
NovaClosure *nova_closure_new(NovaVM *vm, const NovaProto *proto);

/**
 * @brief Call a function already on the stack.
 *
 * Expects the function at stack[top - nargs - 1], followed
 * by nargs arguments. After return, results are at the
 * function's former position.
 *
 * @param vm        VM instance
 * @param nargs     Number of arguments on stack
 * @param nresults  Expected results (-1 = all)
 *
 * @return NOVA_VM_OK or error code.
 */
int nova_vm_call(NovaVM *vm, int nargs, int nresults);

/**
 * @brief Protected call (like pcall in Lua).
 *
 * Calls a function in protected mode, catching runtime errors.
 * On success, returns NOVA_VM_OK with results on the stack.
 * On error, returns the error code and pushes an error message
 * string at the function's original position.
 *
 * Stack layout before: [func][arg1][arg2]...[argN]  (stack_top)
 * Stack after success:  [result1][result2]...
 * Stack after error:    [error_message_string]
 *
 * @param vm        VM instance
 * @param nargs     Number of arguments on stack
 * @param nresults  Expected results (-1 = all)
 *
 * @return NOVA_VM_OK on success, error code on failure.
 */
int nova_vm_pcall(NovaVM *vm, int nargs, int nresults);

/* ============================================================
 * STACK OPERATIONS
 *
 * These form the foundation of the C API for pushing values
 * onto the stack and reading them back.
 * ============================================================ */

/** Push a nil value */
void nova_vm_push_nil(NovaVM *vm);

/** Push a boolean value */
void nova_vm_push_bool(NovaVM *vm, int b);

/** Push an integer value */
void nova_vm_push_integer(NovaVM *vm, nova_int_t val);

/** Push a number (float) value */
void nova_vm_push_number(NovaVM *vm, nova_number_t val);

/**
 * @brief Push a string value (creates a new NovaString).
 * @param vm   VM instance
 * @param s    String data (must not be NULL if len > 0)
 * @param len  String length in bytes
 */
void nova_vm_push_string(NovaVM *vm, const char *s, size_t len);

/** Push a C function */
void nova_vm_push_cfunction(NovaVM *vm, nova_cfunc_t fn);

/**
 * @brief Push an arbitrary NovaValue onto the stack.
 *
 * Handles all value types including tables, closures, and userdata.
 * This is the generic push — prefer type-specific pushes when the
 * type is known at compile time.
 *
 * @param vm  VM instance (must not be NULL)
 * @param v   Value to push
 */
void nova_vm_push_value(NovaVM *vm, NovaValue v);

/**
 * @brief Get a value at the given stack index.
 *
 * Positive indices index from frame base (0 = first register).
 * Negative indices are relative to top (-1 = top element).
 *
 * @param vm   VM instance
 * @param idx  Stack index
 * @return The value, or nil if index is out of bounds.
 */
NovaValue nova_vm_get(NovaVM *vm, int idx);

/** Get number of values on the stack above the current frame base */
int nova_vm_get_top(NovaVM *vm);

/**
 * @brief Set the stack top, filling with nil or discarding.
 * @param vm   VM instance
 * @param idx  New top index
 */
void nova_vm_set_top(NovaVM *vm, int idx);

/** Pop n values from the stack */
void nova_vm_pop(NovaVM *vm, int n);

/* ============================================================
 * GLOBAL TABLE
 * ============================================================ */

/** Set a global variable by name */
void nova_vm_set_global(NovaVM *vm, const char *name, NovaValue val);

/** Get a global variable by name (returns nil if not found) */
NovaValue nova_vm_get_global(NovaVM *vm, const char *name);

/* ============================================================
 * VALUE HELPERS
 * ============================================================ */

/** Get the type name as a static string */
const char *nova_vm_typename(NovaValueType type);

/** Get the current error message, or NULL if no error */
const char *nova_vm_error(const NovaVM *vm);

/**
 * @brief Raise a runtime error from C code.
 *
 * Sets the VM status to NOVA_VM_ERR_RUNTIME and stores
 * the formatted message. C functions should return -1 after
 * calling this to abort execution.
 *
 * @param vm   VM instance
 * @param fmt  printf-style format string
 * @param ...  Format arguments
 */
void nova_vm_raise_error(NovaVM *vm, const char *fmt, ...);

/**
 * @brief Raise a runtime error with a specific diagnostic code.
 *
 * Like nova_vm_raise_error() but stores a fine-grained NovaErrorCode
 * in vm->diag_code for rich diagnostic rendering. The coarse VM
 * status is set to the given vm_err code.
 *
 * @param vm       VM instance
 * @param vm_err   Coarse error code (NOVA_VM_ERR_*)
 * @param diag     Fine-grained diagnostic code (NOVA_E2xxx etc.)
 * @param fmt      printf-style format string
 * @param ...      Format arguments
 */
void nova_vm_raise_error_ex(NovaVM *vm, int vm_err, int diag,
                            const char *fmt, ...);

/**
 * @brief Build a stack traceback string.
 *
 * Walks the call frame stack and builds a multi-line string showing
 * the call chain.  The returned string is heap-allocated; the caller
 * must free() it.
 *
 * @param vm     VM instance (must not be NULL)
 * @param msg    Optional message to prepend (may be NULL)
 * @param level  Frame level to start from (0 = current, 1 = caller)
 * @return Heap-allocated traceback string, or NULL on failure.
 *         Caller must free().
 */
char *nova_vm_traceback(const NovaVM *vm, const char *msg, int level);

/**
 * @brief Create a new empty table and push it on the stack.
 * @param vm  VM instance
 */
void nova_vm_push_table(NovaVM *vm);

/**
 * @brief Set a field on a table at stack index.
 *
 * Pops the top value and sets it as table[name].
 *
 * @param vm    VM instance
 * @param idx   Stack index of the table
 * @param name  Field name
 */
void nova_vm_set_field(NovaVM *vm, int idx, const char *name);

/* ============================================================
 * VALUE CONSTRUCTORS (inline)
 * ============================================================ */

/* ============================================================
 * INTERNAL VM HELPERS (used by nova_meta.c)
 *
 * These are NOT private -- they are part of the VM's internal API
 * consumed by the metamethod pipeline.  User code should prefer
 * the higher-level stack-based API above.
 * ============================================================ */

/**
 * @brief Set a VM error code and message.
 *
 * Called by the VM and metamethod pipeline to report errors.
 * Does NOT longjmp -- caller must check vm->status or return.
 *
 * @param vm    VM instance
 * @param code  Error code (NOVA_VM_ERR_*)
 * @param msg   Error message (static or owned)
 */
void novai_error(NovaVM *vm, int code, const char *msg);

/**
 * @brief Raw table set by integer key (bypasses metamethods).
 *
 * @param vm   VM instance
 * @param t    Target table
 * @param key  Integer key (1-based for array part)
 * @param val  Value to set
 * @return     0 on success, -1 on error
 */
int nova_table_raw_set_int(NovaVM *vm, NovaTable *t,
                           nova_int_t key, NovaValue val);

/**
 * @brief Raw table set by string key (bypasses metamethods).
 *
 * @param vm   VM instance
 * @param t    Target table
 * @param key  String key
 * @param val  Value to set
 * @return     0 on success, -1 on error
 */
int nova_table_raw_set_str(NovaVM *vm, NovaTable *t,
                           NovaString *key, NovaValue val);

/**
 * @brief Create/intern a string in the VM.
 *
 * @param vm   VM instance
 * @param s    String data
 * @param len  String length
 * @return     Interned NovaString, or NULL on OOM
 */
NovaString *nova_vm_intern_string(NovaVM *vm, const char *s, size_t len);

/**
 * @brief Execute VM until frame_count drops to the target level.
 *
 * Used by the metamethod pipeline to call Nova closures from C.
 * Runs the current top frame's bytecode and returns when
 * frame_count == target_frame_count (i.e., the called function
 * returned).
 *
 * @param vm                  VM instance
 * @param target_frame_count  Stop when frame_count reaches this
 * @return                    NOVA_VM_OK or error code
 */
int nova_vm_execute_frames(NovaVM *vm, int target_frame_count);

/* ============================================================
 * VALUE CONSTRUCTORS (inline, dual-mode)
 *
 * These construct NovaValues from C values. Under NaN-boxing,
 * values are packed into 64 bits. Under tagged union, they fill
 * the struct fields. Consumer code uses these exclusively.
 * ============================================================ */

#if NOVA_NAN_BOXING

static inline NovaValue nova_value_nil(void) {
    return NOVA_VALUE_NIL;
}

static inline NovaValue nova_value_bool(int b) {
    return b ? NOVA_VALUE_TRUE : NOVA_VALUE_FALSE;
}

static inline NovaValue nova_value_integer(nova_int_t i) {
    return NOVA_SIGN_BIT | NOVA_QNAN
         | ((uint64_t)i & NOVA_PAYLOAD_MASK);
}

static inline NovaValue nova_value_number(nova_number_t n) {
    NovaValue v = 0;
    memcpy(&v, &n, sizeof(nova_number_t));
    return v;
}

static inline NovaValue nova_value_string(NovaString *s) {
    return NOVA_QNAN | NOVA_TAG_OBJ
         | ((uint64_t)(uintptr_t)s & NOVA_PAYLOAD_MASK);
}

static inline NovaValue nova_value_table(NovaTable *t) {
    return NOVA_QNAN | NOVA_TAG_OBJ
         | ((uint64_t)(uintptr_t)t & NOVA_PAYLOAD_MASK);
}

static inline NovaValue nova_value_closure(NovaClosure *cl) {
    return NOVA_QNAN | NOVA_TAG_OBJ
         | ((uint64_t)(uintptr_t)cl & NOVA_PAYLOAD_MASK);
}

static inline NovaValue nova_value_cfunction(nova_cfunc_t fn) {
    return NOVA_QNAN | NOVA_TAG_CFUNC
         | ((uint64_t)(uintptr_t)fn & NOVA_PAYLOAD_MASK);
}

static inline NovaValue nova_value_coroutine(NovaCoroutine *co) {
    return NOVA_QNAN | NOVA_TAG_OBJ
         | ((uint64_t)(uintptr_t)co & NOVA_PAYLOAD_MASK);
}

static inline NovaValue nova_value_userdata(void *ud) {
    return NOVA_QNAN | NOVA_TAG_OBJ
         | ((uint64_t)(uintptr_t)ud & NOVA_PAYLOAD_MASK);
}

#else /* !NOVA_NAN_BOXING */

static inline NovaValue nova_value_nil(void) {
    NovaValue v;
    v.type = NOVA_TYPE_NIL;
    v.as.integer = 0;
    return v;
}

static inline NovaValue nova_value_bool(int b) {
    NovaValue v;
    v.type = NOVA_TYPE_BOOL;
    v.as.boolean = (b != 0) ? 1 : 0;
    return v;
}

static inline NovaValue nova_value_integer(nova_int_t i) {
    NovaValue v;
    v.type = NOVA_TYPE_INTEGER;
    v.as.integer = i;
    return v;
}

static inline NovaValue nova_value_number(nova_number_t n) {
    NovaValue v;
    v.type = NOVA_TYPE_NUMBER;
    v.as.number = n;
    return v;
}

static inline NovaValue nova_value_string(NovaString *s) {
    NovaValue v;
    v.type = NOVA_TYPE_STRING;
    v.as.string = s;
    return v;
}

static inline NovaValue nova_value_table(NovaTable *t) {
    NovaValue v;
    v.type = NOVA_TYPE_TABLE;
    v.as.table = t;
    return v;
}

static inline NovaValue nova_value_closure(NovaClosure *cl) {
    NovaValue v;
    v.type = NOVA_TYPE_FUNCTION;
    v.as.closure = cl;
    return v;
}

static inline NovaValue nova_value_cfunction(nova_cfunc_t fn) {
    NovaValue v;
    v.type = NOVA_TYPE_CFUNCTION;
    v.as.cfunc = fn;
    return v;
}

static inline NovaValue nova_value_coroutine(NovaCoroutine *co) {
    NovaValue v;
    v.type = NOVA_TYPE_THREAD;
    v.as.coroutine = co;
    return v;
}

static inline NovaValue nova_value_userdata(void *ud) {
    NovaValue v;
    v.type = NOVA_TYPE_USERDATA;
    v.as.userdata = ud;
    return v;
}

#endif /* NOVA_NAN_BOXING */

/* ============================================================
 * TRUTHINESS
 *
 * In Nova (like Lua): nil and false are falsy,
 * everything else is truthy (including 0 and "").
 * ============================================================ */

#if NOVA_NAN_BOXING

static inline int nova_value_is_truthy(NovaValue v) {
    if (v == NOVA_VALUE_NIL)   return 0;
    if (v == NOVA_VALUE_FALSE) return 0;
    return 1;
}

#else

static inline int nova_value_is_truthy(NovaValue v) {
    if (v.type == NOVA_TYPE_NIL) {
        return 0;
    }
    if (v.type == NOVA_TYPE_BOOL) {
        return v.as.boolean;
    }
    return 1;
}

#endif /* NOVA_NAN_BOXING */

/* ============================================================
 * VALUE ABSTRACTION LAYER (Phase 12.1 — NaN-Boxing)
 *
 * Type-checking and extraction macros that decouple ALL
 * consumer code from the internal value representation.
 *
 * Two complete implementations: NaN-boxed and tagged-union.
 * The 900+ call sites use ONLY these macros.
 *
 * USAGE:
 *   if (nova_is_string(v)) {
 *       NovaString *s = nova_as_string(v);
 *       printf("%s\n", nova_str_data(s));
 *   }
 * ============================================================ */

#if NOVA_NAN_BOXING

/* -- NaN-boxing: extract raw GC pointer from an OBJ-tagged value -- */
#define novai_as_gcptr(v) \
    ((NovaGCHeader *)(uintptr_t)((v) & NOVA_PAYLOAD_MASK))

/* -- Type queries (NaN-boxed) -------------------------------- */

#define nova_is_nil(v)       ((v) == NOVA_VALUE_NIL)
#define nova_is_bool(v)      (((v) & (NOVA_QNAN | NOVA_TAG_MASK)) == \
                              (NOVA_QNAN | NOVA_TAG_BOOL))
#define nova_is_integer(v)   (((v) & (NOVA_SIGN_BIT | NOVA_QNAN)) == \
                              (NOVA_SIGN_BIT | NOVA_QNAN))
#define nova_is_number(v)    (((v) & NOVA_QNAN) != NOVA_QNAN)
#define nova_is_cfunction(v) (((v) & (NOVA_SIGN_BIT | NOVA_QNAN | NOVA_TAG_MASK)) == \
                              (NOVA_QNAN | NOVA_TAG_CFUNC))

/* Object sub-type queries: check TAG_OBJ then gc_type */
#define novai_is_obj(v)      (((v) & (NOVA_QNAN | NOVA_TAG_MASK)) == \
                              (NOVA_QNAN | NOVA_TAG_OBJ))
#define novai_obj_gc_type(v) (novai_as_gcptr(v)->gc_type)

#define nova_is_string(v)    (novai_is_obj(v) && \
                              novai_obj_gc_type(v) == NOVA_TYPE_STRING)
#define nova_is_table(v)     (novai_is_obj(v) && \
                              novai_obj_gc_type(v) == NOVA_TYPE_TABLE)
#define nova_is_function(v)  (novai_is_obj(v) && \
                              novai_obj_gc_type(v) == NOVA_TYPE_FUNCTION)
#define nova_is_userdata(v)  (novai_is_obj(v) && \
                              novai_obj_gc_type(v) == NOVA_TYPE_USERDATA)
#define nova_is_thread(v)    (novai_is_obj(v) && \
                              novai_obj_gc_type(v) == NOVA_TYPE_THREAD)

/** True if value is integer or float */
#define nova_is_numeric(v)   (nova_is_integer(v) || nova_is_number(v))

/**
 * @brief Extract the type tag (NovaValueType) from a NaN-boxed value.
 *
 * This does multi-step decoding: check number first, then
 * check NaN tag bits, then dereference GC header for objects.
 */
static inline NovaValueType nova_typeof_nanbox(NovaValue v) {
    if (nova_is_number(v))     return NOVA_TYPE_NUMBER;
    if (nova_is_nil(v))        return NOVA_TYPE_NIL;
    if (nova_is_bool(v))       return NOVA_TYPE_BOOL;
    if (nova_is_integer(v))    return NOVA_TYPE_INTEGER;
    if (nova_is_cfunction(v))  return NOVA_TYPE_CFUNCTION;
    if (novai_is_obj(v))       return (NovaValueType)novai_obj_gc_type(v);
    return NOVA_TYPE_NIL;  /* unreachable for well-formed values */
}
#define nova_typeof(v)  nova_typeof_nanbox(v)

/* -- Value extraction (NaN-boxed) ----------------------------- */

#define nova_as_bool(v)      ((int)((v) & 1))
#define nova_as_integer(v)   ((nova_int_t)( \
    ((v) & NOVA_PAYLOAD_MASK) | \
    (((v) & ((uint64_t)1 << 47)) ? ~NOVA_PAYLOAD_MASK : 0)))

static inline nova_number_t nova_as_number_nanbox(NovaValue v) {
    nova_number_t n = 0.0;
    memcpy(&n, &v, sizeof(nova_number_t));
    return n;
}
#define nova_as_number(v)    nova_as_number_nanbox(v)

#define nova_as_string(v)    ((NovaString *)(uintptr_t)((v) & NOVA_PAYLOAD_MASK))
#define nova_as_table(v)     ((NovaTable *)(uintptr_t)((v) & NOVA_PAYLOAD_MASK))
#define nova_as_closure(v)   ((NovaClosure *)(uintptr_t)((v) & NOVA_PAYLOAD_MASK))
#define nova_as_cfunction(v) ((nova_cfunc_t)(uintptr_t)((v) & NOVA_PAYLOAD_MASK))
#define nova_as_userdata(v)  ((void *)(uintptr_t)((v) & NOVA_PAYLOAD_MASK))
#define nova_as_coroutine(v) ((NovaCoroutine *)(uintptr_t)((v) & NOVA_PAYLOAD_MASK))

#else /* !NOVA_NAN_BOXING — Tagged union */

/* -- Type queries (return int 0/1) -------------------------- */

#define nova_is_nil(v)       ((v).type == NOVA_TYPE_NIL)
#define nova_is_bool(v)      ((v).type == NOVA_TYPE_BOOL)
#define nova_is_integer(v)   ((v).type == NOVA_TYPE_INTEGER)
#define nova_is_number(v)    ((v).type == NOVA_TYPE_NUMBER)
#define nova_is_string(v)    ((v).type == NOVA_TYPE_STRING)
#define nova_is_table(v)     ((v).type == NOVA_TYPE_TABLE)
#define nova_is_function(v)  ((v).type == NOVA_TYPE_FUNCTION)
#define nova_is_cfunction(v) ((v).type == NOVA_TYPE_CFUNCTION)
#define nova_is_userdata(v)  ((v).type == NOVA_TYPE_USERDATA)
#define nova_is_thread(v)    ((v).type == NOVA_TYPE_THREAD)

/** True if value is integer or float */
#define nova_is_numeric(v)   (nova_is_integer(v) || nova_is_number(v))

/** Raw type tag (NovaValueType enum) */
#define nova_typeof(v)       ((NovaValueType)(v).type)

/* -- Value extraction (caller MUST check type first) -------- */

#define nova_as_bool(v)      ((v).as.boolean)
#define nova_as_integer(v)   ((v).as.integer)
#define nova_as_number(v)    ((v).as.number)
#define nova_as_string(v)    ((v).as.string)
#define nova_as_table(v)     ((v).as.table)
#define nova_as_closure(v)   ((v).as.closure)
#define nova_as_cfunction(v) ((v).as.cfunc)
#define nova_as_userdata(v)  ((v).as.userdata)
#define nova_as_coroutine(v) ((v).as.coroutine)

#endif /* NOVA_NAN_BOXING */

/* ============================================================
 * STRING ABSTRACTION LAYER (Phase 10.5b)
 *
 * When Weave strings replace NovaString, ONLY these change.
 * ============================================================ */

/** String data pointer (NUL-terminated) */
#define nova_str_data(s)     ((s)->data)

/** String byte length (excluding NUL) */
#define nova_str_len(s)      ((s)->length)

/** String hash (NXH64, computed at creation) */
#define nova_str_hash(s)     ((s)->hash)

/* ============================================================
 * TABLE ABSTRACTION LAYER (Phase 10.5c)
 *
 * Public API for table operations. When DAGGER replaces the
 * linear-probing hash, ONLY the implementations change.
 *
 * Declared here, defined in nova_vm.c (no longer static).
 * ============================================================ */

/** NXH64 seed for string hashing — single source of truth */
#define NOVA_STRING_SEED 0x4E6F7661ULL  /* "Nova" */

/* -- Table struct accessor macros (Phase 10.5d-g) ----------- */

/** Number of occupied array slots (logical length) */
#define nova_table_array_len(t)   ((t)->array_used)

/** Allocated array capacity */
#define nova_table_array_cap(t)   ((t)->array_size)

/** Number of occupied hash slots */
#define nova_table_hash_count(t)  ((t)->hash_used)

/** Allocated hash capacity */
#define nova_table_hash_cap(t)    ((t)->hash_size)

/** Get metatable pointer (may be NULL) */
#define nova_table_get_metatable(t)    ((t)->metatable)

/** Set metatable pointer (NULL to clear) */
#define nova_table_set_metatable(t, mt) ((t)->metatable = (mt))

/** Raw array pointer for bulk operations (sort, shift, etc.) */
#define nova_table_array_ptr(t)   ((t)->array)

/* -- Table function declarations ----------------------------- */

/** Create a new empty table */
NovaTable *nova_table_new(NovaVM *vm);

/** Free a table and its backing storage */
void nova_table_free(NovaVM *vm, NovaTable *t);

/** Get value by string key (returns nil if not found) */
NovaValue nova_table_get_str(NovaTable *t, const NovaString *key);

/** Set value by string key (returns 0 on success, -1 on error) */
int nova_table_set_str(NovaVM *vm, NovaTable *t,
                       NovaString *key, NovaValue val);

/** Get value by integer key (returns nil if not found) */
NovaValue nova_table_get_int(NovaTable *t, nova_int_t key);

/** Set value by integer key (returns 0 on success, -1 on error) */
int nova_table_set_int(NovaVM *vm, NovaTable *t,
                       nova_int_t key, NovaValue val);

/** Grow table array to hold at least `needed` slots */
int nova_table_grow_array(NovaVM *vm, NovaTable *t, uint32_t needed);

/**
 * @brief Lookup a table value by C string key.
 *
 * Hashes the raw C string and probes the hash table directly,
 * comparing by hash + length + bytes. Does NOT require the key
 * to be an interned NovaString. Used by metamethod lookups, etc.
 *
 * @param t        Table to search
 * @param key      C string key (NUL-terminated)
 * @param key_len  Byte length of key (excluding NUL)
 * @return Value associated with key, or nil if not found
 */
NovaValue nova_table_get_cstr(NovaTable *t, const char *key,
                              uint32_t key_len);

/** Create/intern a string (GC-managed, NUL-terminated) */
NovaString *nova_string_new(NovaVM *vm, const char *s, size_t len);

/** Compare two strings for equality (hash + length + bytes) */
int nova_string_equal(const NovaString *a, const NovaString *b);

/**
 * @brief Iterate to the next key-value pair in a table.
 *
 * Replaces all hand-rolled iteration loops over table internals.
 * Start with *iter_idx = 0. Returns 1 if a pair was found (and
 * advances *iter_idx), 0 when iteration is complete.
 *
 * Visits array part first (keys 0..array_used-1), then hash part.
 * Skips nil values in the array and unoccupied hash slots.
 *
 * @param t         Table to iterate
 * @param iter_idx  In/out iteration cursor (start at 0)
 * @param out_key   Output: key of current entry
 * @param out_val   Output: value of current entry
 * @return 1 if a pair was returned, 0 if iteration complete
 */
int nova_table_next(NovaTable *t, uint32_t *iter_idx,
                    NovaValue *out_key, NovaValue *out_val);

/**
 * @brief Find the iterator cursor for a given key.
 *
 * Locates `key` in the table and returns the iter_idx such that
 * calling nova_table_next() with it yields the entry AFTER `key`.
 * Returns 0 if key is nil (start from the beginning).
 * Returns (uint32_t)-1 if key is not found.
 *
 * @param t    Table to search
 * @param key  Key to locate
 * @return Iterator cursor, or (uint32_t)-1 on not found
 */
uint32_t nova_table_find_iter_idx(NovaTable *t, NovaValue key);

/* ============================================================
 * GARBAGE COLLECTOR API
 *
 * Incremental tri-color mark-sweep with tunable pacing.
 * Call nova_gc_step() after allocations to amortize GC work.
 * ============================================================ */

/**
 * @brief Link a newly allocated object into the VM's GC list.
 *
 * Must be called immediately after allocating any GC object.
 * Sets the object's color to the current white.
 *
 * @param vm   VM instance
 * @param hdr  GC header of the new object
 */
void nova_gc_link(NovaVM *vm, NovaGCHeader *hdr);

/**
 * @brief Perform an incremental GC step proportional to debt.
 *
 * Called automatically after allocations when debt exceeds
 * threshold. Can also be called manually via collectgarbage("step").
 *
 * @param vm  VM instance
 */
void nova_gc_step(NovaVM *vm);

/**
 * @brief Perform a full collection (stop-the-world).
 *
 * Completes all marking and sweeping in one call. Use sparingly;
 * prefer incremental steps for low-latency applications.
 *
 * @param vm  VM instance
 */
void nova_gc_full_collect(NovaVM *vm);

/**
 * @brief Initialize GC state in a newly created VM.
 *
 * @param vm  VM instance
 */
void nova_gc_init(NovaVM *vm);

/**
 * @brief Free all remaining GC objects during VM shutdown.
 *
 * @param vm  VM instance
 */
void nova_gc_shutdown(NovaVM *vm);

/**
 * @brief Write barrier: shade a mutated object gray if needed.
 *
 * Must be called when a black object gains a reference to a
 * white object (would violate the tri-color invariant).
 *
 * @param vm     VM instance
 * @param parent Object being mutated (the container)
 */
void nova_gc_barrier(NovaVM *vm, NovaGCHeader *parent);

/**
 * @brief Check if GC should run after an allocation of `size` bytes.
 *
 * Increments gc_debt and triggers a step if threshold exceeded.
 *
 * @param vm   VM instance
 * @param size Number of bytes just allocated
 */
void nova_gc_check(NovaVM *vm, size_t size);

/* ============================================================
 * COROUTINE API
 *
 * Create, resume, and yield coroutines. Built on top of the
 * VM execution engine with per-coroutine stack isolation.
 * ============================================================ */

/**
 * @brief Create a new coroutine from a closure.
 *
 * Allocates an independent stack and frame array for the
 * coroutine. The coroutine is initially SUSPENDED.
 *
 * @param vm   Parent VM (shared globals, GC)
 * @param body Closure to run inside the coroutine
 *
 * @return New coroutine, or NULL on allocation failure.
 */
NovaCoroutine *nova_coroutine_new(NovaVM *vm, NovaClosure *body);

/**
 * @brief Resume a suspended coroutine with arguments.
 *
 * Pushes nargs values from the caller's stack into the
 * coroutine, then runs it until it yields or returns.
 *
 * @param vm     Parent VM
 * @param co     Coroutine to resume (must be SUSPENDED)
 * @param nargs  Number of arguments on VM stack to pass
 *
 * @return NOVA_VM_OK if coroutine returned,
 *         NOVA_VM_YIELD if coroutine yielded,
 *         or an error code on failure.
 */
int nova_coroutine_resume(NovaVM *vm, NovaCoroutine *co, int nargs);

/**
 * @brief Yield the currently running coroutine.
 *
 * Called from within a running coroutine (via YIELD opcode
 * or coroutine.yield()). Suspends execution and returns
 * control to the resume caller.
 *
 * @param vm       Parent VM
 * @param nresults Number of values to yield
 *
 * @return NOVA_VM_YIELD (always).
 */
int nova_coroutine_yield(NovaVM *vm, int nresults);

/**
 * @brief Get the status string for a coroutine.
 *
 * @param co  Coroutine to query
 * @return "suspended", "running", "dead", or "normal"
 */
const char *nova_coroutine_status_str(NovaCoroutine *co);

/* ============================================================
 * ASYNC TASK SCHEDULER API
 *
 * Cooperative task scheduler built on the coroutine runtime.
 * Tasks are coroutines created from async function calls.
 * The scheduler runs spawned tasks round-robin until all
 * complete or error.
 * ============================================================ */

/**
 * @brief Enqueue a coroutine as a spawned task.
 *
 * @param vm   VM instance (must not be NULL)
 * @param task Coroutine to enqueue (must be SUSPENDED)
 * @return 0 on success, -1 on failure.
 */
int nova_async_enqueue(NovaVM *vm, NovaCoroutine *task);

/**
 * @brief Run one round-robin tick of all spawned tasks.
 *
 * Resumes each SUSPENDED task once. Dead tasks are removed.
 * Called during await to give spawned tasks CPU time.
 *
 * @param vm  VM instance
 */
void nova_async_tick(NovaVM *vm);

/**
 * @brief Run the async event loop until all tasks complete.
 *
 * Creates a root task from the given coroutine and runs it
 * plus all spawned tasks until everything finishes.
 *
 * @param vm   VM instance
 * @param root Root coroutine (the "main" async function)
 * @param nargs Number of arguments for the root task
 * @return NOVA_VM_OK or error code.
 */
int nova_async_run(NovaVM *vm, NovaCoroutine *root, int nargs);

/**
 * @brief Clean up the task queue (called by nova_vm_destroy).
 *
 * @param vm  VM instance
 */
void nova_async_cleanup(NovaVM *vm);

#endif /* NOVA_VM_H */
