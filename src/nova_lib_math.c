/**
 * @file nova_lib_math.c
 * @brief Nova Language - Math Standard Library
 *
 * Provides mathematical functions and constants as the "math"
 * module table. All trigonometric functions work in radians.
 *
 * Functions:
 *   math.abs(x)         Absolute value
 *   math.ceil(x)        Ceiling
 *   math.floor(x)       Floor
 *   math.sqrt(x)        Square root
 *   math.sin(x)         Sine
 *   math.cos(x)         Cosine
 *   math.tan(x)         Tangent
 *   math.asin(x)        Arc sine
 *   math.acos(x)        Arc cosine
 *   math.atan(y [,x])   Arc tangent (atan2 if x given)
 *   math.exp(x)         e^x
 *   math.log(x [,b])    Natural log (or log base b)
 *   math.pow(x, y)      x^y      (also x^y operator)
 *   math.fmod(x, y)     Float modulus
 *   math.max(...)        Maximum of arguments
 *   math.min(...)        Minimum of arguments
 *   math.random([m [,n]])  Random number
 *   math.randomseed(x)    Seed the RNG
 *   math.tointeger(x)     Convert to exact integer or nil
 *   math.type(x)          "integer", "float", or false
 *
 * Constants:
 *   math.pi              Pi (3.14159...)
 *   math.huge            Positive infinity
 *   math.maxinteger      Maximum integer value
 *   math.mininteger      Minimum integer value
 *
 * @author Anthony Taliento
 * @date 2026-02-08
 * @version 0.2.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * DEPENDENCIES:
 *   - nova_lib.h
 *   - <math.h>
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#include "nova/nova_lib.h"
#include "nova/nova_vm.h"

#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <limits.h>
#include <float.h>

/* ============================================================
 * SINGLE-ARGUMENT MATH WRAPPERS
 * ============================================================ */

#define NOVA_MATH_FUNC1(cname, cfunc)                             \
    static int nova_math_##cname(NovaVM *vm) {                    \
        nova_number_t x = 0.0;                                    \
        if (!nova_lib_check_number(vm, 0, &x)) {                  \
            return -1;                                             \
        }                                                          \
        nova_vm_push_number(vm, cfunc(x));                         \
        return 1;                                                  \
    }

NOVA_MATH_FUNC1(abs,   fabs)
NOVA_MATH_FUNC1(ceil,  ceil)
NOVA_MATH_FUNC1(floor, floor)
NOVA_MATH_FUNC1(sqrt,  sqrt)
NOVA_MATH_FUNC1(sin,   sin)
NOVA_MATH_FUNC1(cos,   cos)
NOVA_MATH_FUNC1(tan,   tan)
NOVA_MATH_FUNC1(asin,  asin)
NOVA_MATH_FUNC1(acos,  acos)
NOVA_MATH_FUNC1(exp,   exp)

/* ============================================================
 * ATAN (1 or 2 arguments)
 * ============================================================ */

static int nova_math_atan(NovaVM *vm) {
    nova_number_t y = 0.0;
    if (!nova_lib_check_number(vm, 0, &y)) {
        return -1;
    }

    int nargs = nova_vm_get_top(vm);
    if (nargs >= 2) {
        nova_number_t x = 0.0;
        if (!nova_lib_check_number(vm, 1, &x)) {
            return -1;
        }
        nova_vm_push_number(vm, atan2(y, x));
    } else {
        nova_vm_push_number(vm, atan(y));
    }
    return 1;
}

/* ============================================================
 * LOG (1 or 2 arguments)
 * ============================================================ */

static int nova_math_log(NovaVM *vm) {
    nova_number_t x = 0.0;
    if (!nova_lib_check_number(vm, 0, &x)) {
        return -1;
    }

    int nargs = nova_vm_get_top(vm);
    if (nargs >= 2) {
        nova_number_t base = 0.0;
        if (!nova_lib_check_number(vm, 1, &base)) {
            return -1;
        }
        nova_vm_push_number(vm, log(x) / log(base));
    } else {
        nova_vm_push_number(vm, log(x));
    }
    return 1;
}

/* ============================================================
 * POW, FMOD
 * ============================================================ */

static int nova_math_pow(NovaVM *vm) {
    nova_number_t x = 0.0, y = 0.0;
    if (!nova_lib_check_number(vm, 0, &x)) {
        return -1;
    }
    if (!nova_lib_check_number(vm, 1, &y)) {
        return -1;
    }
    nova_vm_push_number(vm, pow(x, y));
    return 1;
}

static int nova_math_fmod(NovaVM *vm) {
    nova_number_t x = 0.0, y = 0.0;
    if (!nova_lib_check_number(vm, 0, &x)) {
        return -1;
    }
    if (!nova_lib_check_number(vm, 1, &y)) {
        return -1;
    }
    nova_vm_push_number(vm, fmod(x, y));
    return 1;
}

/* ============================================================
 * MIN, MAX (variadic)
 * ============================================================ */

static int nova_math_max(NovaVM *vm) {
    int nargs = nova_vm_get_top(vm);
    if (nargs == 0) {
        nova_vm_raise_error(vm, "bad argument #1 to 'max' (value expected)");
        return -1;
    }

    nova_number_t best = 0.0;
    if (!nova_lib_check_number(vm, 0, &best)) {
        return -1;
    }

    for (int i = 1; i < nargs; i++) {
        nova_number_t val = 0.0;
        if (!nova_lib_check_number(vm, i, &val)) {
            return -1;
        }
        if (val > best) {
            best = val;
        }
    }

    nova_vm_push_number(vm, best);
    return 1;
}

static int nova_math_min(NovaVM *vm) {
    int nargs = nova_vm_get_top(vm);
    if (nargs == 0) {
        nova_vm_raise_error(vm, "bad argument #1 to 'min' (value expected)");
        return -1;
    }

    nova_number_t best = 0.0;
    if (!nova_lib_check_number(vm, 0, &best)) {
        return -1;
    }

    for (int i = 1; i < nargs; i++) {
        nova_number_t val = 0.0;
        if (!nova_lib_check_number(vm, i, &val)) {
            return -1;
        }
        if (val < best) {
            best = val;
        }
    }

    nova_vm_push_number(vm, best);
    return 1;
}

/* ============================================================
 * RANDOM
 * ============================================================ */

static int nova_math_random_seeded = 0;

static int nova_math_random(NovaVM *vm) {
    int nargs = nova_vm_get_top(vm);

    /* Lazy-seed on first call */
    if (!nova_math_random_seeded) {
        srand((unsigned int)time(NULL));
        nova_math_random_seeded = 1;
    }

    if (nargs == 0) {
        /* Return [0, 1) */
        nova_vm_push_number(vm, (nova_number_t)rand() / ((nova_number_t)RAND_MAX + 1.0));
        return 1;
    }

    nova_int_t m = 0;
    if (!nova_lib_check_integer(vm, 0, &m)) {
        return -1;
    }

    if (nargs == 1) {
        /* Return [1, m] */
        if (m < 1) {
            nova_vm_raise_error(vm, "bad argument #1 to 'random' "
                               "(interval is empty)");
            return -1;
        }
        nova_vm_push_integer(vm, (nova_int_t)(rand() % (int)m) + 1);
        return 1;
    }

    /* nargs >= 2: return [m, n] */
    nova_int_t n = 0;
    if (!nova_lib_check_integer(vm, 1, &n)) {
        return -1;
    }

    if (m > n) {
        nova_vm_raise_error(vm, "bad argument #2 to 'random' "
                           "(interval is empty)");
        return -1;
    }

    nova_int_t range = n - m + 1;
    nova_vm_push_integer(vm, (nova_int_t)(rand() % (int)range) + m);
    return 1;
}

static int nova_math_randomseed(NovaVM *vm) {
    nova_int_t seed = 0;
    if (!nova_lib_check_integer(vm, 0, &seed)) {
        return -1;
    }
    srand((unsigned int)seed);
    nova_math_random_seeded = 1;
    return 0;
}

/* ============================================================
 * TYPE CHECKING
 * ============================================================ */

/**
 * @brief math.tointeger(x) - Convert to exact integer or nil.
 */
static int nova_math_tointeger(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    NovaValue v = nova_vm_get(vm, 0);

    if (nova_is_integer(v)) {
        nova_vm_push_integer(vm, nova_as_integer(v));
        return 1;
    }

    if (nova_is_number(v)) {
        nova_number_t n = nova_as_number(v);
        nova_number_t floored = floor(n);
        if (n == floored &&
            n >= (nova_number_t)INT64_MIN &&
            n <= (nova_number_t)INT64_MAX) {
            nova_vm_push_integer(vm, (nova_int_t)n);
            return 1;
        }
    }

    nova_vm_push_nil(vm);
    return 1;
}

/**
 * @brief math.type(x) - Return "integer", "float", or false.
 */
static int nova_math_type(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    NovaValue v = nova_vm_get(vm, 0);

    if (nova_is_integer(v)) {
        nova_vm_push_string(vm, "integer", 7);
        return 1;
    }
    if (nova_is_number(v)) {
        nova_vm_push_string(vm, "float", 5);
        return 1;
    }

    nova_vm_push_bool(vm, 0);
    return 1;
}

/* ============================================================
 * REGISTRATION TABLE
 * ============================================================ */

static const NovaLibReg nova_math_lib[] = {
    {"abs",         nova_math_abs},
    {"ceil",        nova_math_ceil},
    {"floor",       nova_math_floor},
    {"sqrt",        nova_math_sqrt},
    {"sin",         nova_math_sin},
    {"cos",         nova_math_cos},
    {"tan",         nova_math_tan},
    {"asin",        nova_math_asin},
    {"acos",        nova_math_acos},
    {"atan",        nova_math_atan},
    {"exp",         nova_math_exp},
    {"log",         nova_math_log},
    {"pow",         nova_math_pow},
    {"fmod",        nova_math_fmod},
    {"max",         nova_math_max},
    {"min",         nova_math_min},
    {"random",      nova_math_random},
    {"randomseed",  nova_math_randomseed},
    {"tointeger",   nova_math_tointeger},
    {"type",        nova_math_type},
    {NULL,          NULL}
};

/* ============================================================
 * MODULE OPENER
 * ============================================================ */

int nova_open_math(NovaVM *vm) {
    if (vm == NULL) {
        return -1;
    }

    /* Register math as a module table */
    nova_lib_register_module(vm, "math", nova_math_lib);

    /* Set constants on the math table.
     * nova_lib_register_module() already popped the table and set it
     * as a global, so we retrieve it back and push it onto the stack
     * to get a valid absolute index for nova_vm_set_field(). */
    NovaValue math_tbl = nova_vm_get_global(vm, "math");
    if (nova_is_table(math_tbl)) {
        nova_vm_push_value(vm, math_tbl);
        int tidx = nova_vm_get_top(vm) - 1;

        nova_vm_push_number(vm, 3.14159265358979323846);
        nova_vm_set_field(vm, tidx, "pi");

        nova_vm_push_number(vm, HUGE_VAL);
        nova_vm_set_field(vm, tidx, "huge");

        /* NaN-boxed integers use a 48-bit payload with sign extension
         * from bit 47.  The representable range is -(2^47) to (2^47 - 1),
         * NOT INT64_MAX / INT64_MIN which silently truncate.             */
        nova_vm_push_integer(vm, ((nova_int_t)1 << 47) - 1);   /* 140737488355327  */
        nova_vm_set_field(vm, tidx, "maxinteger");

        nova_vm_push_integer(vm, -((nova_int_t)1 << 47));      /* -140737488355328 */
        nova_vm_set_field(vm, tidx, "mininteger");

        nova_vm_set_top(vm, tidx);  /* Pop math table */
    }

    return 0;
}
