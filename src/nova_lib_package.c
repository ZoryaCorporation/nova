/**
 * @file nova_lib_package.c
 * @brief Nova Language - Package / Module System
 *
 * Implements the require() function and the package table,
 * providing Nova's module loading system.
 *
 * Module search order:
 *   1. package.preload[name] — pre-registered loader functions
 *   2. package.loaded[name]  — previously loaded modules (cache)
 *   3. Search package.path for .n and .no files
 *
 * package.path patterns use '?' as the module name placeholder:
 *   "./?.n;./?.no;/usr/local/lib/nova/?.n"
 *
 * When a module is found and loaded:
 *   - The file is compiled and executed
 *   - If it returns a value, that becomes the module
 *   - The result is cached in package.loaded[name]
 *   - Subsequent require() calls return the cached value
 *
 * @author Anthony Taliento
 * @date 2026-02-07
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * DEPENDENCIES:
 *   - nova_lib.h
 *   - nova_vm.h
 *   - nova_lex.h, nova_pp.h, nova_parse.h, nova_compile.h
 *   - nova_opt.h, nova_codegen.h, nova_proto.h
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#include "nova/nova_lib.h"
#include "nova/nova_vm.h"
#include "nova/nova_lex.h"
#include "nova/nova_pp.h"
#include "nova/nova_parse.h"
#include "nova/nova_compile.h"
#include "nova/nova_opt.h"
#include "nova/nova_codegen.h"
#include "nova/nova_proto.h"
#include "nova/nova_conf.h"
#include "nova/nova_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <zorya/pal.h>   /* zorya_is_file() — portable file existence check */

/* ============================================================
 * INTERNAL CONSTANTS
 * ============================================================ */

/** Default module search path */
#define NOVA_DEFAULT_PATH \
    "./?.n;./?.no;./lib/?.n;./lib/?.no;" \
    "/usr/local/lib/nova/?.n;/usr/local/lib/nova/?.no"

/** Maximum module name length */
#define NOVA_MAX_MODULE_NAME 256

/** Maximum path length */
#define NOVA_MAX_PATH 4096

/* ============================================================
 * INTERNAL: FILE EXISTENCE CHECK
 * ============================================================ */

static int novai_file_exists(const char *path) {
    return zorya_is_file(path);
}

/* ============================================================
 * INTERNAL: READ FILE
 * ============================================================ */

static char *novai_read_file_pkg(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0) {
        fclose(f);
        return NULL;
    }

    char *data = (char *)malloc((size_t)size + 1);
    if (data == NULL) {
        fclose(f);
        return NULL;
    }

    size_t nread = fread(data, 1, (size_t)size, f);
    fclose(f);

    data[nread] = '\0';
    if (out_size != NULL) {
        *out_size = nread;
    }
    return data;
}

/* ============================================================
 * INTERNAL: COMPILE AND EXECUTE A MODULE FILE
 * ============================================================ */

/**
 * @brief Load and execute a .n source file as a module.
 *
 * Returns 1 on success (result pushed on stack), 0 on failure.
 */
static int novai_load_source_module(NovaVM *vm, const char *path) {
    size_t source_size = 0;
    NTRACE(MODULE, "load_source_module path='%s'", path);
    NTRACE_ENTER("load_source_module");
    char *source = novai_read_file_pkg(path, &source_size);
    if (source == NULL) {
        NTRACE(MODULE, "load_source_module FAIL: cannot read '%s'", path);
        NTRACE_LEAVE();
        return 0;
    }

    /* Preprocessor */
    NovaPP *pp = nova_pp_create();
    if (pp == NULL) {
        free(source);
        return 0;
    }

    if (nova_pp_process_string(pp, source, source_size, path) != 0) {
        nova_pp_destroy(pp);
        free(source);
        return 0;
    }

    /* Parser */
    NovaParser parser;
    if (nova_parser_init(&parser, pp) != 0) {
        nova_pp_destroy(pp);
        free(source);
        return 0;
    }

    if (nova_parse_row(&parser, path) != 0) {
        nova_parser_free(&parser);
        nova_pp_destroy(pp);
        free(source);
        return 0;
    }

    /* Compiler */
    NovaProto *proto = nova_compile(&parser.table, path);
    if (proto == NULL) {
        nova_parser_free(&parser);
        nova_pp_destroy(pp);
        free(source);
        return 0;
    }

    /* Optimize */
    nova_optimize(proto, 1);

    /* Wrap proto in a closure and execute via nova_vm_call()
     * so the return value is properly placed on the calling
     * stack (nova_vm_execute clobbers the stack base and
     * cannot be used for nested module loading). */
    NovaClosure *cl = nova_closure_new(vm, proto);
    if (cl == NULL) {
        nova_proto_destroy(proto);
        nova_parser_free(&parser);
        nova_pp_destroy(pp);
        free(source);
        return 0;
    }

    nova_vm_push_value(vm, nova_value_closure(cl));
    int status = nova_vm_call(vm, 0, 1);

    /* NOTE: Do NOT call nova_proto_destroy(proto) here!
     * Closures created during module execution hold references
     * to sub-protos.  The GC will reclaim them when the closures
     * become unreachable.  Only free the compiler temporaries. */
    nova_parser_free(&parser);
    nova_pp_destroy(pp);
    free(source);

    if (status != NOVA_VM_OK) {
        NTRACE(MODULE, "load_source_module FAIL: execute returned %d", status);
        NTRACE_LEAVE();
        return 0;
    }

    /* nova_vm_call leaves 1 result on the stack at the
     * closure's former position. */
    NTRACE(MODULE, "load_source_module SUCCESS path='%s'", path);
    NTRACE_STACK(vm, "after load_source_module");
    NTRACE_LEAVE();
    return 1;
}

/**
 * @brief Load and execute a .no bytecode file as a module.
 *
 * Returns 1 on success (result pushed on stack), 0 on failure.
 */
static int novai_load_bytecode_module(NovaVM *vm, const char *path) {
    int load_err = 0;
    NovaProto *proto = nova_codegen_load(path, &load_err);
    if (proto == NULL) {
        return 0;
    }

    NovaClosure *cl = nova_closure_new(vm, proto);
    if (cl == NULL) {
        nova_proto_destroy(proto);
        return 0;
    }

    nova_vm_push_value(vm, nova_value_closure(cl));
    int status = nova_vm_call(vm, 0, 1);
    /* Do NOT destroy proto — closures reference sub-protos */

    return (status == NOVA_VM_OK) ? 1 : 0;
}

/* ============================================================
 * INTERNAL: SEARCH PATH FOR MODULE
 * ============================================================ */

/**
 * @brief Search package.path for a module file.
 *
 * Replaces '?' with the module name in each path template,
 * separated by ';'. Replaces '.' in module name with '/'.
 *
 * @param search_path  The path string (e.g., "./?.n;./lib/?.n")
 * @param name         Module name (e.g., "math.extra")
 * @param found_path   Output buffer (NOVA_MAX_PATH bytes)
 *
 * @return 1 if found, 0 if not.
 */
static int novai_search_path(const char *search_path, const char *name,
                             char *found_path) {
    if (search_path == NULL || name == NULL || found_path == NULL) {
        return 0;
    }

    NTRACE(MODULE, "search_path name='%s'", name);

    /* Convert module dots to directory separators */
    char modpath[NOVA_MAX_MODULE_NAME];
    size_t namelen = strlen(name);
    if (namelen >= NOVA_MAX_MODULE_NAME) {
        return 0;
    }
    for (size_t i = 0; i < namelen; i++) {
        modpath[i] = (name[i] == '.') ? '/' : name[i];
    }
    modpath[namelen] = '\0';

    /* Scan each template separated by ';' */
    const char *p = search_path;
    while (*p != '\0') {
        const char *end = strchr(p, ';');
        size_t tlen = (end != NULL) ? (size_t)(end - p) : strlen(p);

        /* Build candidate path: replace '?' with modpath */
        size_t out_pos = 0;
        for (size_t i = 0; i < tlen && out_pos < NOVA_MAX_PATH - 1; i++) {
            if (p[i] == '?') {
                size_t left = NOVA_MAX_PATH - 1 - out_pos;
                size_t copy = namelen < left ? namelen : left;
                memcpy(found_path + out_pos, modpath, copy);
                out_pos += copy;
            } else {
                found_path[out_pos++] = p[i];
            }
        }
        found_path[out_pos] = '\0';

        /* Check if file exists */
        if (novai_file_exists(found_path)) {
            NTRACE(MODULE, "search_path FOUND '%s'", found_path);
            return 1;
        }

        /* Move to next template */
        p += tlen;
        if (*p == ';') {
            p++;
        }
    }

    return 0;
}

/* ============================================================
 * PART 1: require()
 * ============================================================ */

/**
 * @brief require(modname) - Load and return a module.
 *
 * Search order:
 *   1. package.loaded[modname] — return cached
 *   2. package.preload[modname] — call loader
 *   3. Search package.path for file
 *
 * @return 1 result: the module value.
 */
static int nova_base_require(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    /* Save stack_top at entry — the VM expects cfunction results to
     * be pushed starting at this position (base[A + 1 + nargs]).
     * All intermediate pushes during cache lookup and module loading
     * move stack_top; we must restore it before the final push. */
    NovaValue *result_slot = vm->stack_top;

    NovaValue namev = nova_vm_get(vm, 0);
    if (!nova_is_string(namev)) {
        nova_vm_raise_error(vm, "bad argument #1 to 'require' (string expected)");
        return -1;
    }

    const char *modname = nova_str_data(nova_as_string(namev));

    NTRACE(MODULE, "require('%s') START", modname);
    NTRACE_ENTER("require");
    NTRACE_STACK(vm, "require entry");

    /* --- Check package.loaded cache --- */
    NovaValue pkg = nova_vm_get_global(vm, "package");
    if (nova_is_table(pkg)) {
        /* Get package.loaded */
        nova_vm_push_value(vm, pkg);
        NovaValue loaded_key = nova_vm_get_global(vm, "_package_loaded");
        if (nova_is_table(loaded_key)) {
            /* Look up module in loaded table */
            NovaTable *ltbl = nova_as_table(loaded_key);
            /* Search hash part for the module name */
            NovaString *search_key = NULL;
            nova_vm_push_string(vm, modname, strlen(modname));
            NovaValue skv = nova_vm_get(vm, -1);
            vm->stack_top--;
            if (nova_is_string(skv)) {
                search_key = nova_as_string(skv);
            }
            if (search_key != NULL) {
                NovaValue found = nova_table_get_str(ltbl, search_key);
                if (!nova_is_nil(found)) {
                    /* Found in cache! */
                    NTRACE(MODULE, "require('%s') CACHE HIT", modname);
                    NTRACE_LEAVE();
                    vm->stack_top = result_slot;
                    nova_vm_push_value(vm, found);
                    return 1;
                }
            }
        }
    }

    /* --- Search package.path for the file --- */
    NovaValue pathv = nova_vm_get_global(vm, "_package_path");
    const char *search_path = NOVA_DEFAULT_PATH;
    if (nova_is_string(pathv)) {
        search_path = nova_str_data(nova_as_string(pathv));
    }

    char found_path[NOVA_MAX_PATH];
    if (!novai_search_path(search_path, modname, found_path)) {
        nova_vm_raise_error(vm,
            "module '%s' not found:\n\tno file in package.path", modname);
        return -1;
    }

    /* --- Load the module --- */
    int loaded = 0;
    size_t pathlen = strlen(found_path);

    /* Save stack position to detect return values */
    NovaValue *stack_before = vm->stack_top;

    if (pathlen >= 3 && strcmp(found_path + pathlen - 3, ".no") == 0) {
        loaded = novai_load_bytecode_module(vm, found_path);
    } else {
        loaded = novai_load_source_module(vm, found_path);
    }

    if (!loaded) {
        const char *err = nova_vm_error(vm);
        nova_vm_raise_error(vm, "error loading module '%s' from '%s': %s",
                           modname, found_path, err ? err : "unknown error");
        return -1;
    }

    /* Get the module return value */
    NovaValue module_val = nova_value_bool(1);  /* Default: true */
    if (vm->stack_top > stack_before) {
        module_val = *(vm->stack_top - 1);
    }

    NTRACE(MODULE, "require('%s') loaded from '%s'", modname, found_path);
    NTRACE_VALUE("module_val", module_val);
    NTRACE_STACK(vm, "after module load");

    /* --- Cache in package.loaded --- */
    NovaValue loaded_tbl = nova_vm_get_global(vm, "_package_loaded");
    if (nova_is_table(loaded_tbl)) {
        /* Store module_val in the loaded table */
        nova_vm_push_value(vm, loaded_tbl);
        nova_vm_push_value(vm, module_val);
        nova_vm_set_field(vm, -2, modname);
        vm->stack_top--;  /* Pop the loaded table */
    }

    /* Push the module value as result — restore stack_top first
     * so the result lands where the VM expects it. */
    vm->stack_top = result_slot;
    nova_vm_push_value(vm, module_val);
    NTRACE(MODULE, "require('%s') END", modname);
    NTRACE_LEAVE();
    return 1;
}

/* ============================================================
 * PART 2: PACKAGE TABLE SETUP
 * ============================================================ */

static const NovaLibReg nova_package_lib[] = {
    {NULL, NULL}  /* No module functions yet, just the table */
};

/**
 * @brief Initialize the package system.
 *
 * Creates:
 *   - require() global function
 *   - package table with path, loaded, preload
 *   - _package_loaded and _package_path internal globals
 */
int nova_open_package(NovaVM *vm) {
    if (vm == NULL) {
        return -1;
    }

    /* Register require() as a global */
    nova_vm_set_global(vm, "require",
                       nova_value_cfunction(nova_base_require));

    /* Register __require__() for #require PP directive.
     * The preprocessor emits: local x = __require__("mod")
     * which calls the same function as require(). */
    nova_vm_set_global(vm, "__require__",
                       nova_value_cfunction(nova_base_require));

    /* Create package.loaded table */
    nova_vm_push_table(vm);
    NovaValue loaded = nova_vm_get(vm, -1);
    vm->stack_top--;
    nova_vm_set_global(vm, "_package_loaded", loaded);

    /* Create package table */
    nova_vm_push_table(vm);

    /* Set package.path */
    nova_vm_push_string(vm, NOVA_DEFAULT_PATH, strlen(NOVA_DEFAULT_PATH));
    nova_vm_set_field(vm, -2, "path");

    /* Set package.loaded reference */
    nova_vm_push_value(vm, loaded);
    nova_vm_set_field(vm, -2, "loaded");

    /* Create package.preload */
    nova_vm_push_table(vm);
    nova_vm_set_field(vm, -2, "preload");

    /* Pop and set as global */
    NovaValue pkg_table = nova_vm_get(vm, -1);
    vm->stack_top--;
    nova_vm_set_global(vm, "package", pkg_table);

    /* Store path string for quick access */
    nova_vm_push_string(vm, NOVA_DEFAULT_PATH, strlen(NOVA_DEFAULT_PATH));
    NovaValue pathstr = nova_vm_get(vm, -1);
    vm->stack_top--;
    nova_vm_set_global(vm, "_package_path", pathstr);

    (void)nova_package_lib;
    return 0;
}

/* ============================================================
 * PART 3: SCRIPT DIRECTORY INJECTION
 * ============================================================ */

/**
 * @brief Prepend a script's directory to the package search path.
 *
 * Extracts the directory component of the given script path and
 * prepends "dir/?.n;dir/?.no;dir/lib/?.n;dir/lib/?.no;" to the
 * current _package_path global.  This allows require() to find
 * modules relative to the executing script rather than only
 * relative to the process working directory.
 *
 * @param vm    VM instance (must not be NULL)
 * @param path  Path to the script file (must not be NULL)
 */
void nova_package_set_script_dir(NovaVM *vm, const char *path) {
    if (vm == NULL || path == NULL) {
        return;
    }

    /* Find the last directory separator */
    const char *last_sep = strrchr(path, '/');
    if (last_sep == NULL) {
        /* No directory component -- script is in CWD, which
         * is already covered by the default "./?.n" patterns. */
        return;
    }

    /* Extract directory (including trailing slash) */
    size_t dir_len = (size_t)(last_sep - path) + 1;  /* +1 for '/' */
    if (dir_len >= NOVA_MAX_PATH - 64) {
        return;  /* Path too long, skip */
    }

    /* Build new search path:
     *   dir/?.n;dir/?.no;dir/lib/?.n;dir/lib/?.no;<existing> */
    char newpath[NOVA_MAX_PATH * 2];
    int wrote = snprintf(newpath, sizeof(newpath),
                         "%.*s?.n;%.*s?.no;%.*slib/?.n;%.*slib/?.no;",
                         (int)dir_len, path,
                         (int)dir_len, path,
                         (int)dir_len, path,
                         (int)dir_len, path);

    if (wrote < 0 || (size_t)wrote >= sizeof(newpath) - 1) {
        return;
    }

    /* Append existing package path */
    NovaValue oldpath = nova_vm_get_global(vm, "_package_path");
    if (nova_is_string(oldpath)) {
        size_t left = sizeof(newpath) - 1 - (size_t)wrote;
        size_t cplen = strlen(nova_str_data(nova_as_string(oldpath)));
        if (cplen > left) {
            cplen = left;
        }
        memcpy(newpath + wrote, nova_str_data(nova_as_string(oldpath)), cplen);
        newpath[wrote + (int)cplen] = '\0';
    }

    /* Update _package_path */
    size_t newlen = strlen(newpath);
    nova_vm_push_string(vm, newpath, newlen);
    NovaValue newval = nova_vm_get(vm, -1);
    vm->stack_top--;
    nova_vm_set_global(vm, "_package_path", newval);

    NTRACE(MODULE, "script_dir: prepended '%.*s' to package.path",
           (int)dir_len, path);
}
