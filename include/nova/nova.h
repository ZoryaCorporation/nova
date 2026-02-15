/**
 * @file nova.h
 * @brief Nova Language - Master Public Header
 *
 * Single-include header for embedding Nova in C applications.
 * This is the primary entry point for the Nova C API.
 *
 *   #include <nova/nova.h>
 *
 * @author Anthony Taliento
 * @date 2026-02-05
 * @version 0.1.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 *
 * DESCRIPTION:
 *   Nova is a lightweight, embeddable scripting language inspired by Lua.
 *   Built from scratch with preprocessor support, cross-platform bytecode,
 *   and a clean C API. Powered by the Zorya C SDK (NXH, DAGGER, Weave).
 *
 * DEPENDENCIES:
 *   - zorya/nxh.h    (NXH64 hash function)
 *   - zorya/dagger.h (DAGGER hash tables)
 *   - zorya/weave.h  (Weave string library)
 *   - zorya/pcm.h    (Performance-critical macros)
 */

#ifndef NOVA_H
#define NOVA_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "nova_conf.h"

/* ============================================================
 * FORWARD DECLARATIONS
 * ============================================================ */

typedef struct NovaState    NovaState;
typedef struct NovaValue    NovaValue;
typedef struct NovaTable    NovaTable;
typedef struct NovaString   NovaString;
typedef struct NovaClosure  NovaClosure;
typedef struct NovaProto    NovaProto;
typedef struct NovaUpvalue  NovaUpvalue;
typedef struct NovaThread   NovaThread;
typedef struct NovaUserdata NovaUserdata;

/* ============================================================
 * FUNDAMENTAL TYPES
 *
 * nova_number_t, nova_int_t, nova_uint_t are now defined in
 * nova_conf.h so that headers like nova_lex.h can use them
 * without pulling in the full nova.h master header.
 * ============================================================ */

/** C function callable from Nova */
typedef int (*nova_cfunc_t)(NovaState *N);

/** Memory allocator function signature */
typedef void *(*nova_alloc_fn)(void *userdata, void *ptr,
                               size_t old_size, size_t new_size);

/** Error handler function signature */
typedef void (*nova_error_fn)(NovaState *N, const char *msg, void *userdata);

/** Debug hook function signature */
typedef void (*nova_hook_fn)(NovaState *N, int event, int line);

/* ============================================================
 * TYPE TAGS
 * ============================================================ */

/**
 * @brief Nova value type enumeration
 *
 * Every Nova value carries a type tag. This enum defines all
 * possible types in the Nova type system.
 */
typedef enum {
    NOVA_TYPE_NIL       = 0,    /**< nil value                      */
    NOVA_TYPE_BOOL      = 1,    /**< Boolean (true/false)           */
    NOVA_TYPE_INTEGER   = 2,    /**< Machine integer (nova_int_t)   */
    NOVA_TYPE_NUMBER    = 3,    /**< IEEE 754 double (nova_number_t)*/
    NOVA_TYPE_STRING    = 4,    /**< Interned string (Weave-backed) */
    NOVA_TYPE_TABLE     = 5,    /**< Table (DAGGER-backed)          */
    NOVA_TYPE_FUNCTION  = 6,    /**< Nova closure                   */
    NOVA_TYPE_CFUNCTION = 7,    /**< Raw C function                 */
    NOVA_TYPE_USERDATA  = 8,    /**< Full userdata (GC-managed)     */
    NOVA_TYPE_LUSERDATA = 9,    /**< Light userdata (raw pointer)   */
    NOVA_TYPE_THREAD    = 10,   /**< Coroutine thread               */

    NOVA_TYPE_COUNT     = 11    /**< Number of types (internal)     */
} NovaType;

/* ============================================================
 * STATUS CODES
 * ============================================================ */

/**
 * @brief Status codes returned by Nova API functions
 */
typedef enum {
    NOVA_OK             =  0,   /**< Success                        */
    NOVA_ERROR_RUNTIME  = -1,   /**< Runtime error                  */
    NOVA_ERROR_SYNTAX   = -2,   /**< Syntax/parse error             */
    NOVA_ERROR_MEMORY   = -3,   /**< Memory allocation failure      */
    NOVA_ERROR_HANDLER  = -4,   /**< Error in error handler         */
    NOVA_ERROR_FILE     = -5,   /**< File I/O error                 */
    NOVA_ERROR_BYTECODE = -6,   /**< Invalid bytecode               */
    NOVA_ERROR_PP       = -7,   /**< Preprocessor error             */
    NOVA_YIELD          =  1    /**< Coroutine yielded              */
} nova_status_t;

/* ============================================================
 * CONFIGURATION STRUCTURE
 * ============================================================ */

/**
 * @brief Configuration for creating a Nova state
 *
 * Pass to nova_open_with() for custom configuration.
 * All fields have sane defaults if zero-initialized.
 */
typedef struct {
    nova_alloc_fn   allocator;          /**< Custom allocator (NULL = malloc) */
    void           *alloc_userdata;     /**< Passed to allocator              */
    nova_error_fn   error_handler;      /**< Error callback (NULL = stderr)   */
    void           *error_userdata;     /**< Passed to error_handler          */
    size_t          initial_stack;      /**< Initial stack size (0 = default) */
    int             gc_pause;           /**< GC pause percentage (0 = default)*/
    int             gc_step_mul;        /**< GC step multiplier (0 = default) */
    int             array_base;         /**< Array base index (0 or 1)        */
    int             open_stdlibs;       /**< Auto-open standard libraries?    */
} NovaConfig;

/* ============================================================
 * LIBRARY REGISTRATION
 * ============================================================ */

/**
 * @brief Entry in a library registration table
 *
 * Used with nova_open_lib() to register C functions.
 * Terminated by a {NULL, NULL} sentinel.
 */
typedef struct {
    const char    *name;    /**< Function name in Nova            */
    nova_cfunc_t   func;    /**< C function pointer               */
} NovaReg;

/* Sentinel for NovaReg arrays */
#define NOVA_REG_END { NULL, NULL }

/* ============================================================
 * GC CONTROL COMMANDS
 * ============================================================ */

typedef enum {
    NOVA_GC_STOP        = 0,    /**< Stop the GC                    */
    NOVA_GC_RESTART     = 1,    /**< Restart the GC                 */
    NOVA_GC_COLLECT     = 2,    /**< Force a full collection cycle   */
    NOVA_GC_COUNT       = 3,    /**< Get memory usage (KB)           */
    NOVA_GC_COUNTB      = 4,    /**< Get memory usage (bytes mod 1K) */
    NOVA_GC_STEP        = 5,    /**< Run incremental step            */
    NOVA_GC_SETPAUSE    = 6,    /**< Set GC pause percentage         */
    NOVA_GC_SETSTEPMUL  = 7,    /**< Set GC step multiplier          */
    NOVA_GC_ISRUNNING   = 8,    /**< Check if GC is running          */
    NOVA_GC_GENERATIONAL = 9    /**< Switch to generational mode     */
} nova_gc_command_t;

/* ============================================================
 * STATE LIFECYCLE
 * ============================================================ */

/**
 * @brief Create a new Nova state with default configuration
 *
 * @return New state, or NULL on allocation failure
 *
 * @post Caller must call nova_close() when done
 */
NovaState *nova_open(void);

/**
 * @brief Create a new Nova state with custom configuration
 *
 * @param config Configuration (NULL fields use defaults)
 * @return New state, or NULL on allocation failure
 *
 * @pre config != NULL
 * @post Caller must call nova_close() when done
 */
NovaState *nova_open_with(const NovaConfig *config);

/**
 * @brief Destroy a Nova state and free all resources
 *
 * @param N State to destroy (NULL is safe/no-op)
 */
void nova_close(NovaState *N);

/* ============================================================
 * LOADING AND EXECUTION
 * ============================================================ */

/**
 * @brief Load a Nova source file (.n or .lua)
 *
 * Lexes, preprocesses, parses, and compiles the file.
 * Pushes the compiled chunk as a function on the stack.
 *
 * @param N     Nova state
 * @param path  File path (must not be NULL)
 * @return NOVA_OK on success, error code on failure
 */
nova_status_t nova_load_file(NovaState *N, const char *path);

/**
 * @brief Load a Nova source string
 *
 * @param N       Nova state
 * @param source  Source code (must not be NULL)
 * @return NOVA_OK on success, error code on failure
 */
nova_status_t nova_load_string(NovaState *N, const char *source);

/**
 * @brief Load a named Nova source string
 *
 * @param N       Nova state
 * @param source  Source code
 * @param len     Length (0 = strlen)
 * @param name    Chunk name for error messages
 * @return NOVA_OK on success, error code on failure
 */
nova_status_t nova_load_buffer(NovaState *N, const char *source,
                               size_t len, const char *name);

/**
 * @brief Load compiled bytecode (.no format)
 *
 * @param N     Nova state
 * @param data  Bytecode data
 * @param len   Data length in bytes
 * @return NOVA_OK on success, error code on failure
 */
nova_status_t nova_load_bytecode(NovaState *N, const void *data, size_t len);

/**
 * @brief Call a function on the stack
 *
 * @param N         Nova state
 * @param nargs     Number of arguments pushed above the function
 * @param nresults  Number of expected results (-1 = all)
 * @return NOVA_OK on success
 */
nova_status_t nova_call(NovaState *N, int nargs, int nresults);

/**
 * @brief Protected call (catches errors)
 *
 * Like nova_call() but catches runtime errors. On error,
 * pushes error message onto the stack.
 *
 * @param N         Nova state
 * @param nargs     Number of arguments
 * @param nresults  Number of expected results (-1 = all)
 * @return NOVA_OK on success, error code on failure
 */
nova_status_t nova_pcall(NovaState *N, int nargs, int nresults);

/**
 * @brief Compile a source file to bytecode (.no)
 *
 * @param N         Nova state
 * @param inpath    Source file path (.n)
 * @param outpath   Output path (.no) (NULL = derive from inpath)
 * @param strip     Strip debug information?
 * @return NOVA_OK on success, error code on failure
 */
nova_status_t nova_compile_file(NovaState *N, const char *inpath,
                                const char *outpath, int strip);

/* ============================================================
 * STACK OPERATIONS
 * ============================================================ */

int         nova_get_top(NovaState *N);
void        nova_set_top(NovaState *N, int idx);
void        nova_pop(NovaState *N, int n);
void        nova_push_value(NovaState *N, int idx);
void        nova_remove(NovaState *N, int idx);
void        nova_insert(NovaState *N, int idx);
void        nova_replace(NovaState *N, int idx);
void        nova_copy(NovaState *N, int from, int to);
int         nova_check_stack(NovaState *N, int n);
int         nova_abs_index(NovaState *N, int idx);

/* ============================================================
 * PUSH VALUES (C → Nova Stack)
 * ============================================================ */

void        nova_push_nil(NovaState *N);
void        nova_push_bool(NovaState *N, int b);
void        nova_push_integer(NovaState *N, nova_int_t value);
void        nova_push_number(NovaState *N, nova_number_t value);
const char *nova_push_string(NovaState *N, const char *s);
const char *nova_push_lstring(NovaState *N, const char *s, size_t len);
const char *nova_push_fstring(NovaState *N, const char *fmt, ...);
void        nova_push_cfunction(NovaState *N, nova_cfunc_t fn);
void       *nova_push_userdata(NovaState *N, size_t size);
void        nova_push_light_userdata(NovaState *N, void *ptr);

/* ============================================================
 * TYPE QUERIES
 * ============================================================ */

NovaType    nova_type(NovaState *N, int idx);
const char *nova_typename(NovaState *N, int idx);
const char *nova_typename_of(NovaType type);

int         nova_is_nil(NovaState *N, int idx);
int         nova_is_bool(NovaState *N, int idx);
int         nova_is_integer(NovaState *N, int idx);
int         nova_is_number(NovaState *N, int idx);
int         nova_is_string(NovaState *N, int idx);
int         nova_is_table(NovaState *N, int idx);
int         nova_is_function(NovaState *N, int idx);
int         nova_is_cfunction(NovaState *N, int idx);
int         nova_is_userdata(NovaState *N, int idx);
int         nova_is_thread(NovaState *N, int idx);
int         nova_is_none_or_nil(NovaState *N, int idx);

/* ============================================================
 * GET VALUES (Nova Stack → C)
 * ============================================================ */

int           nova_to_bool(NovaState *N, int idx);
nova_int_t    nova_to_integer(NovaState *N, int idx);
nova_number_t nova_to_number(NovaState *N, int idx);
const char   *nova_to_string(NovaState *N, int idx, size_t *len);
nova_cfunc_t  nova_to_cfunction(NovaState *N, int idx);
void         *nova_to_userdata(NovaState *N, int idx);
NovaThread   *nova_to_thread(NovaState *N, int idx);

/* ============================================================
 * TABLE OPERATIONS
 * ============================================================ */

void        nova_new_table(NovaState *N);
void        nova_new_table_sized(NovaState *N, int narr, int nhash);
void        nova_get_table(NovaState *N, int idx);
void        nova_set_table(NovaState *N, int idx);
void        nova_get_field(NovaState *N, int idx, const char *key);
void        nova_set_field(NovaState *N, int idx, const char *key);
void        nova_get_index(NovaState *N, int idx, nova_int_t n);
void        nova_set_index(NovaState *N, int idx, nova_int_t n);
void        nova_raw_get(NovaState *N, int idx);
void        nova_raw_set(NovaState *N, int idx);
void        nova_raw_get_index(NovaState *N, int idx, nova_int_t n);
void        nova_raw_set_index(NovaState *N, int idx, nova_int_t n);
int         nova_next(NovaState *N, int idx);
size_t      nova_raw_len(NovaState *N, int idx);

/* ============================================================
 * GLOBAL TABLE
 * ============================================================ */

void        nova_get_global(NovaState *N, const char *name);
void        nova_set_global(NovaState *N, const char *name);

/* ============================================================
 * METATABLES
 * ============================================================ */

int         nova_get_metatable(NovaState *N, int idx);
int         nova_set_metatable(NovaState *N, int idx);

/* ============================================================
 * ERROR HANDLING
 * ============================================================ */

int         nova_error(NovaState *N);
int         nova_error_fmt(NovaState *N, const char *fmt, ...);

/* ============================================================
 * GC CONTROL
 * ============================================================ */

int         nova_gc(NovaState *N, int what, int data);

/* ============================================================
 * STANDARD LIBRARIES
 * ============================================================ */

void        nova_open_libs(NovaState *N);
void        nova_open_base(NovaState *N);
void        nova_open_string(NovaState *N);

/* ============================================================
 * BUILT-IN PRINTF
 *
 * Nova exposes printf() as a first-class global function.
 * Supports standard C format specifiers: %d, %i, %f, %s, %x,
 * %o, %e, %g, %c, %%, plus width/precision modifiers.
 *
 * From Nova code:
 *   printf("Player %s has %d HP\n", name, hp)
 *
 * This replaces the Lua anti-pattern:
 *   print(string.format("Player %s has %d HP", name, hp))
 * ============================================================ */

/**
 * @brief Format and print to stdout (C printf semantics)
 *
 * Available in Nova code as the global function printf().
 * Supports all standard C format specifiers.
 *
 * @param N   Nova state
 * @return Number of values pushed (0)
 */
int nova_lib_printf(NovaState *N);

/**
 * @brief Format and return string (C sprintf semantics)
 *
 * Available in Nova code as string.format() and also as
 * the global function sprintf().
 *
 * @param N   Nova state
 * @return Number of values pushed (1 = formatted string)
 */
int nova_lib_sprintf(NovaState *N);

/**
 * @brief Format and print to file (C fprintf semantics)
 *
 * Available in Nova code as io.printf() or fprintf().
 *
 * @param N   Nova state
 * @return Number of values pushed (0)
 */
int nova_lib_fprintf(NovaState *N);
void        nova_open_table(NovaState *N);
void        nova_open_math(NovaState *N);
void        nova_open_io(NovaState *N);
void        nova_open_os(NovaState *N);
void        nova_open_coroutine(NovaState *N);
void        nova_open_debug(NovaState *N);
void        nova_open_package(NovaState *N);

/**
 * @brief Register a library of C functions
 *
 * @param N     Nova state
 * @param name  Library name (creates global table)
 * @param lib   Array of NovaReg entries (terminated by NOVA_REG_END)
 */
void        nova_open_lib(NovaState *N, const char *name, const NovaReg *lib);

/* ============================================================
 * UTILITY MACROS
 * ============================================================ */

/** Pop one value from the stack */
#define nova_pop1(N) nova_pop(N, 1)

/** Check if stack index is valid (not none) */
#define nova_is_valid(N, idx) (nova_type(N, idx) != NOVA_TYPE_NIL || (idx) != 0)

/** Register a single C function as a global */
#define nova_register(N, name, fn) \
    (nova_push_cfunction(N, fn), nova_set_global(N, name))

/* ============================================================
 * VERSION QUERY
 * ============================================================ */

/**
 * @brief Get Nova version string
 * @return Version string (e.g., "Nova 0.1.0")
 */
const char *nova_version(void);

/**
 * @brief Get Nova version number
 * @return Packed version (major*10000 + minor*100 + patch)
 */
int nova_version_number(void);

#endif /* NOVA_H */
