/**
 * @file nova_lib_table.c
 * @brief Nova Language - Table Standard Library
 *
 * Provides table manipulation functions as the "table" module.
 *
 * Functions:
 *   table.insert(t, [pos,] value)  Insert value into array part
 *   table.remove(t [, pos])        Remove from array part
 *   table.concat(t [,sep [,i [,j]]])  Concatenate array elements
 *   table.sort(t [,comp])          Sort array part (basic)
 *   table.move(t, f, e, dest [,t2])  Move elements
 *   table.pack(...)                Pack arguments into table
 *   table.unpack(t [,i [,j]])      Unpack (alias for global unpack)
 *
 * NOTE: These operate on the array part of tables. Full DAGGER
 * integration for the hash part is planned for Phase 9.
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
 *   - nova_vm.h
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 */

#include "nova/nova_lib.h"
#include "nova/nova_vm.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================
 * INSERT
 * ============================================================ */

/**
 * @brief table.insert(t, [pos,] value) - Insert into array.
 *
 * If pos is given, shifts elements up.
 * If pos is omitted, appends at end.
 *
 * NOTE: Simplified - works with the existing array_used counter.
 * Full implementation with gap handling arrives with DAGGER tables.
 */
static int nova_table_insert(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) {
        return -1;
    }

    NovaValue tv = nova_vm_get(vm, 0);
    if (!nova_is_table(tv)) {
        nova_vm_raise_error(vm, "bad argument #1 to 'insert' (table expected)");
        return -1;
    }

    NovaTable *t = nova_as_table(tv);
    int nargs = nova_vm_get_top(vm);

    if (nargs == 2) {
        /* table.insert(t, value) - append */
        NovaValue val = nova_vm_get(vm, 1);
        uint32_t pos = nova_table_array_len(t);

        /* Grow array if needed */
        if (nova_table_grow_array(vm, t, pos + 1) != 0) {
            nova_vm_raise_error(vm, "memory allocation failed in 'insert'");
            return -1;
        }

        /* PERF: direct array write for append — set_int would work
         * but grow_array already guaranteed slot exists */
        nova_table_array_ptr(t)[pos] = val;
        nova_table_array_len(t) = pos + 1;
    } else {
        /* table.insert(t, pos, value) */
        nova_int_t pos = 0;
        if (!nova_lib_check_integer(vm, 1, &pos)) {
            return -1;
        }
        NovaValue val = nova_vm_get(vm, 2);

        if (pos < 0 || pos > (nova_int_t)nova_table_array_len(t)) {
            nova_vm_raise_error(vm, "bad argument #2 to 'insert' (position out of bounds)");
            return -1;
        }

        /* Grow array if needed */
        if (nova_table_grow_array(vm, t, nova_table_array_len(t) + 1) != 0) {
            nova_vm_raise_error(vm, "memory allocation failed in 'insert'");
            return -1;
        }

        /* PERF: direct array shift — bulk memmove semantics require
         * raw pointer access for performance */
        NovaValue *arr = nova_table_array_ptr(t);
        uint32_t idx = (uint32_t)pos;
        uint32_t used = nova_table_array_len(t);
        for (uint32_t i = used; i > idx; i--) {
            arr[i] = arr[i - 1];
        }
        arr[idx] = val;
        nova_table_array_len(t)++;
    }

    return 0;  /* No results */
}

/* ============================================================
 * REMOVE
 * ============================================================ */

/**
 * @brief table.remove(t [, pos]) - Remove from array.
 *
 * Removes element at pos (default: last), shifts down.
 * Returns the removed value.
 */
static int nova_table_remove(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    NovaValue tv = nova_vm_get(vm, 0);
    if (!nova_is_table(tv)) {
        nova_vm_raise_error(vm, "bad argument #1 to 'remove' (table expected)");
        return -1;
    }

    NovaTable *t = nova_as_table(tv);

    if (nova_table_array_len(t) == 0) {
        nova_vm_push_nil(vm);
        return 1;
    }

    int nargs = nova_vm_get_top(vm);
    nova_int_t pos = (nova_int_t)(nova_table_array_len(t) - 1);  /* Default: last (0-based) */

    if (nargs >= 2) {
        if (!nova_lib_check_integer(vm, 1, &pos)) {
            return -1;
        }
    }

    if (pos < 0 || pos >= (nova_int_t)nova_table_array_len(t)) {
        nova_vm_raise_error(vm, "bad argument #2 to 'remove' (position out of bounds)");
        return -1;
    }

    uint32_t idx = (uint32_t)pos;
    NovaValue removed = nova_table_get_int(t, pos);

    /* PERF: direct array shift — bulk memmove semantics require
     * raw pointer access for performance */
    NovaValue *arr = nova_table_array_ptr(t);
    uint32_t used = nova_table_array_len(t);
    for (uint32_t i = idx; i < used - 1; i++) {
        arr[i] = arr[i + 1];
    }
    arr[used - 1] = nova_value_nil();
    nova_table_array_len(t)--;

    /* Push removed value */
    switch (nova_typeof(removed)) {
        case NOVA_TYPE_NIL:     nova_vm_push_nil(vm); break;
        case NOVA_TYPE_BOOL:    nova_vm_push_bool(vm, nova_as_bool(removed)); break;
        case NOVA_TYPE_INTEGER: nova_vm_push_integer(vm, nova_as_integer(removed)); break;
        case NOVA_TYPE_NUMBER:  nova_vm_push_number(vm, nova_as_number(removed)); break;
        case NOVA_TYPE_STRING:
            nova_vm_push_string(vm, nova_str_data(nova_as_string(removed)),
                                nova_str_len(nova_as_string(removed)));
            break;
        default: nova_vm_push_nil(vm); break;
    }
    return 1;
}

/* ============================================================
 * CONCAT
 * ============================================================ */

/**
 * @brief table.concat(t [, sep [, i [, j]]]) - Join array elements.
 *
 * Concatenates array elements to a string with separator.
 * Default sep="", i=1, j=#t.
 */
static int nova_table_concat(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) {
        return -1;
    }

    NovaValue tv = nova_vm_get(vm, 0);
    if (!nova_is_table(tv)) {
        nova_vm_raise_error(vm, "bad argument #1 to 'concat' (table expected)");
        return -1;
    }

    NovaTable *t = nova_as_table(tv);
    int nargs = nova_vm_get_top(vm);

    const char *sep = "";
    size_t sep_len = 0;
    nova_int_t i = 0;
    nova_int_t j = (nova_int_t)nova_table_array_len(t) - 1;

    if (nargs >= 2) {
        NovaValue sv = nova_vm_get(vm, 1);
        if (nova_is_string(sv)) {
            sep = nova_str_data(nova_as_string(sv));
            sep_len = nova_str_len(nova_as_string(sv));
        }
    }
    if (nargs >= 3) {
        NovaValue iv = nova_vm_get(vm, 2);
        if (nova_is_integer(iv)) {
            i = nova_as_integer(iv);
        }
    }
    if (nargs >= 4) {
        NovaValue jv = nova_vm_get(vm, 3);
        if (nova_is_integer(jv)) {
            j = nova_as_integer(jv);
        }
    }

    if (i > j) {
        nova_vm_push_string(vm, "", 0);
        return 1;
    }

    /* Calculate total length */
    size_t total = 0;
    for (nova_int_t k = i; k <= j; k++) {
        if (k < 0 || (uint32_t)k >= nova_table_array_len(t)) {
            continue;
        }
        NovaValue elem = nova_table_get_int(t, k);
        if (nova_is_string(elem)) {
            total += nova_str_len(nova_as_string(elem));
        } else if (nova_is_integer(elem)) {
            total += 21;
        } else if (nova_is_number(elem)) {
            total += 32;
        } else {
            nova_vm_raise_error(vm, "invalid value (table) at index %lld in "
                               "table for 'concat'", (long long)k);
            return -1;
        }
        if (k < j) {
            total += sep_len;
        }
    }

    char *buf = (char *)malloc(total + 1);
    if (buf == NULL) {
        nova_vm_raise_error(vm, "memory allocation failed in 'concat'");
        return -1;
    }

    char *p = buf;
    for (nova_int_t k = i; k <= j; k++) {
        if (k > i && sep_len > 0) {
            memcpy(p, sep, sep_len);
            p += sep_len;
        }
        if (k < 0 || (uint32_t)k >= nova_table_array_len(t)) {
            continue;
        }
        NovaValue elem = nova_table_get_int(t, (uint32_t)k);
        if (nova_is_string(elem)) {
            memcpy(p, nova_str_data(nova_as_string(elem)), nova_str_len(nova_as_string(elem)));
            p += nova_str_len(nova_as_string(elem));
        } else if (nova_is_integer(elem)) {
            int n = sprintf(p, "%lld", (long long)nova_as_integer(elem));
            p += n;
        } else if (nova_is_number(elem)) {
            int n = sprintf(p, "%.14g", nova_as_number(elem));
            p += n;
        }
    }
    *p = '\0';

    size_t len = (size_t)(p - buf);
    nova_vm_push_string(vm, buf, len);
    free(buf);
    return 1;
}

/* ============================================================
 * SORT (Insertion sort with optional comparator)
 *
 * table.sort(t [, comp])
 * Sorts array part of t in-place using < (default) or
 * comp(a,b) returning true when a should come before b.
 * ============================================================ */

/**
 * @brief Default less-than comparison for sort.
 *
 * Compares two NovaValues using < semantics:
 *   integer < integer, number coercion, string lexicographic.
 *
 * @param a  Left operand
 * @param b  Right operand
 *
 * @return 1 if a < b, 0 otherwise (including incompatible types).
 */
static int novai_sort_default_lt(NovaValue a, NovaValue b) {
    if (nova_is_integer(a) && nova_is_integer(b)) {
        return (nova_as_integer(a) < nova_as_integer(b));
    }
    if (nova_is_number(a) || nova_is_number(b)) {
        nova_number_t na = 0.0, nb = 0.0;
        if (nova_is_number(a)) na = nova_as_number(a);
        else if (nova_is_integer(a)) na = (nova_number_t)nova_as_integer(a);
        if (nova_is_number(b)) nb = nova_as_number(b);
        else if (nova_is_integer(b)) nb = (nova_number_t)nova_as_integer(b);
        return (na < nb);
    }
    if (nova_is_string(a) && nova_is_string(b)) {
        size_t la = nova_str_len(nova_as_string(a));
        size_t lb = nova_str_len(nova_as_string(b));
        size_t minlen = la < lb ? la : lb;
        int cmp = memcmp(nova_str_data(nova_as_string(a)), nova_str_data(nova_as_string(b)), minlen);
        return (cmp < 0) || (cmp == 0 && la < lb);
    }
    return 0;
}

/**
 * @brief Call user comparator comp(a, b) and return truthiness.
 *
 * Uses nova_vm_pcall for safety. On comparator error, propagates
 * via nova_vm_raise_error (longjmp to nearest error handler).
 *
 * @param vm    VM instance
 * @param comp  Comparator function (FUNCTION or CFUNCTION)
 * @param a     Left operand
 * @param b     Right operand
 *
 * @return 1 if comp(a,b) is truthy, 0 if falsy.
 */
static int novai_sort_call_comp(NovaVM *vm, NovaValue comp,
                                NovaValue a, NovaValue b) {
    NovaValue *result_area = vm->stack_top;

    nova_vm_push_value(vm, comp);
    nova_vm_push_value(vm, a);
    nova_vm_push_value(vm, b);

    int rc = nova_vm_pcall(vm, 2, 1);

    if (rc != NOVA_VM_OK) {
        /* Propagate comparator error.  result_area[0] has the msg. */
        const char *err = "error in sort comparator";
        if (nova_is_string(result_area[0])) {
            err = nova_str_data(nova_as_string(result_area[0]));
        }
        vm->stack_top = result_area;
        nova_vm_raise_error(vm, "%s", err);
        /* nova_vm_raise_error does longjmp, never returns */
        return 0;  /* unreachable, silences compiler */
    }

    NovaValue result = result_area[0];
    vm->stack_top = result_area;  /* restore stack */

    /* Truthiness: everything except nil and false is truthy */
    if (nova_is_nil(result)) {
        return 0;
    }
    if (nova_is_bool(result) && nova_as_bool(result) == 0) {
        return 0;
    }
    return 1;
}

static int nova_table_sort(NovaVM *vm) {
    int nargs_total = nova_vm_get_top(vm);
    if (nargs_total < 1) {
        nova_vm_raise_error(vm, "bad argument #1 to 'sort' (table expected)");
        return -1;
    }

    NovaValue tv = nova_vm_get(vm, 0);
    if (!nova_is_table(tv)) {
        nova_vm_raise_error(vm, "bad argument #1 to 'sort' (table expected)");
        return -1;
    }

    /* Optional comparator function (arg #2) */
    int has_comp = 0;
    NovaValue comp = nova_value_nil();
    if (nargs_total >= 2) {
        comp = nova_vm_get(vm, 1);
        if (!nova_is_function(comp) &&
            !nova_is_cfunction(comp)) {
            nova_vm_raise_error(vm,
                "bad argument #2 to 'sort' (function expected)");
            return -1;
        }
        has_comp = 1;
    }

    NovaTable *t = nova_as_table(tv);
    uint32_t n = nova_table_array_len(t);
    /* PERF: direct array access intentional for sort inner loop */
    NovaValue *arr = nova_table_array_ptr(t);

    /* Insertion sort (stable) */
    for (uint32_t i = 1; i < n; i++) {
        NovaValue key = arr[i];
        uint32_t j = i;

        while (j > 0) {
            NovaValue prev = arr[j - 1];
            int should_swap = 0;

            if (has_comp) {
                should_swap = novai_sort_call_comp(vm, comp, key, prev);
            } else {
                should_swap = novai_sort_default_lt(key, prev);
            }

            if (!should_swap) {
                break;
            }

            arr[j] = arr[j - 1];
            j--;
        }
        arr[j] = key;
    }

    return 0;  /* No results (sorts in-place) */
}

/* ============================================================
 * MOVE
 * ============================================================ */

/**
 * @brief table.move(a1, f, e, t [, a2]) - Move elements.
 *
 * Copies a1[f..e] to a2[t..t+(e-f)] (a2 defaults to a1).
 * Handles overlapping regions correctly by choosing copy direction.
 * Returns the destination table.
 *
 * @param vm  Nova VM state (must not be NULL)
 *
 * @return 1 (pushes destination table), or -1 on error
 *
 * COMPLEXITY: O(e - f + 1)
 */
static int nova_table_move(NovaVM *vm) {
    if (nova_lib_check_args(vm, 4) != 0) {
        return -1;
    }

    NovaValue a1v = nova_vm_get(vm, 0);
    if (!nova_is_table(a1v)) {
        nova_vm_raise_error(vm, "bad argument #1 to 'move' (table expected)");
        return -1;
    }

    nova_int_t f = 0, e = 0, dest = 0;
    if (!nova_lib_check_integer(vm, 1, &f)) return -1;
    if (!nova_lib_check_integer(vm, 2, &e)) return -1;
    if (!nova_lib_check_integer(vm, 3, &dest)) return -1;

    NovaTable *src = nova_as_table(a1v);
    NovaTable *dst = src;  /* default: same table */

    /* Optional 5th argument: destination table */
    int nargs = nova_vm_get_top(vm);
    if (nargs >= 5) {
        NovaValue a2v = nova_vm_get(vm, 4);
        if (!nova_is_table(a2v)) {
            nova_vm_raise_error(vm,
                "bad argument #5 to 'move' (table expected)");
            return -1;
        }
        dst = nova_as_table(a2v);
    }

    /* Empty range: nothing to copy */
    if (f > e) {
        nova_vm_push_value(vm, nova_value_table(dst));
        return 1;
    }

    nova_int_t count = e - f + 1;

    /*
    ** Choose copy direction to handle overlapping regions correctly.
    ** Same table + dest > f  → copy backward to avoid overwriting
    **                          source elements before they're read.
    ** Otherwise              → copy forward (simpler).
    */
    if (src == dst && dest > f && dest <= e) {
        /* Backward copy */
        for (nova_int_t i = count - 1; i >= 0; i--) {
            nova_int_t si = f + i;
            nova_int_t di = dest + i;
            NovaValue val = nova_value_nil();
            if (si >= 0 && (uint32_t)si < nova_table_array_len(src)) {
                val = nova_table_get_int(src, (uint32_t)si);
            }
            nova_table_raw_set_int(vm, dst, di, val);
        }
    } else {
        /* Forward copy */
        for (nova_int_t i = 0; i < count; i++) {
            nova_int_t si = f + i;
            nova_int_t di = dest + i;
            NovaValue val = nova_value_nil();
            if (si >= 0 && (uint32_t)si < nova_table_array_len(src)) {
                val = nova_table_get_int(src, (uint32_t)si);
            }
            nova_table_raw_set_int(vm, dst, di, val);
        }
    }

    /* Return destination table */
    nova_vm_push_value(vm, nova_value_table(dst));
    return 1;
}

/* ============================================================
 * PACK
 * ============================================================ */

/**
 * @brief table.pack(...) - Pack arguments into a table.
 *
 * Returns {[1]=arg1, [2]=arg2, ..., n=nargs}.
 * NOTE: Simplified - returns nil until NEWTABLE push is available.
 */
static int nova_table_pack(NovaVM *vm) {
    int nargs = nova_vm_get_top(vm);

    /* Create a new table */
    nova_vm_push_table(vm);
    NovaValue tval = nova_vm_get(vm, -1);

    if (!nova_is_table(tval)) {
        return 1;
    }

    NovaTable *t = nova_as_table(tval);

    /* Grow array to hold all args */
    if (nargs > 0) {
        uint32_t new_size = (uint32_t)nargs;
        if (new_size < 8) new_size = 8;
        nova_table_grow_array(vm, t, new_size);
        /* PERF: direct array access intentional for bulk fill */
        NovaValue *arr = nova_table_array_ptr(t);
        for (int i = 0; i < nargs; i++) {
            arr[i] = nova_vm_get(vm, i);
        }
        nova_table_array_len(t) = (uint32_t)nargs;
    }

    /* The table is already on top of stack */
    return 1;
}

/* ============================================================
 * REGISTRATION TABLE
 * ============================================================ */

static const NovaLibReg nova_table_lib[] = {
    {"insert",  nova_table_insert},
    {"remove",  nova_table_remove},
    {"concat",  nova_table_concat},
    {"sort",    nova_table_sort},
    {"move",    nova_table_move},
    {"pack",    nova_table_pack},
    {NULL,      NULL}
};

/* ============================================================
 * MODULE OPENER
 * ============================================================ */

int nova_open_table(NovaVM *vm) {
    if (vm == NULL) {
        return -1;
    }

    nova_lib_register_module(vm, "table", nova_table_lib);
    return 0;
}
