/**
 * @file test_vm_advanced.c
 * @brief Advanced VM tests for Nova Language
 *
 * Tests tables, loops, comparisons, and more complex bytecode.
 *
 * @author Anthony Taliento
 * @date 2026-02-07
 */

#include "nova/nova_vm.h"
#include "nova/nova_proto.h"
#include "nova/nova_opcode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * TEST UTILITIES
 * ============================================================ */

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) \
    do { \
        printf("  TEST: %-40s ", #name); \
        tests_run++; \
        if (test_##name()) { \
            printf("[PASS]\n"); \
            tests_passed++; \
        } else { \
            printf("[FAIL]\n"); \
        } \
    } while (0)

#define ASSERT(cond) \
    do { \
        if (!(cond)) { \
            printf("\n    ASSERT FAILED: %s (line %d)\n", #cond, __LINE__); \
            return 0; \
        } \
    } while (0)

#define ASSERT_EQ_INT(a, b) \
    do { \
        nova_int_t _a = (a), _b = (b); \
        if (_a != _b) { \
            printf("\n    ASSERT_EQ_INT FAILED: %lld != %lld (line %d)\n", \
                   (long long)_a, (long long)_b, __LINE__); \
            return 0; \
        } \
    } while (0)

/* ============================================================
 * TEST: ARITHMETIC OPERATIONS
 * ============================================================ */

static int test_arith_sub(void) {
    NovaVM *vm = nova_vm_create();
    ASSERT(vm != NULL);

    NovaProto *proto = nova_proto_create();
    NovaInstruction code[4];
    code[0] = NOVA_ENCODE_ASBX(NOVA_OP_LOADINT, 0, 100);
    code[1] = NOVA_ENCODE_ASBX(NOVA_OP_LOADINT, 1, 58);
    code[2] = NOVA_ENCODE_ABC(NOVA_OP_SUB, 0, 0, 1);
    code[3] = NOVA_ENCODE_ABC(NOVA_OP_RETURN0, 0, 0, 0);

    proto->code = code;
    proto->code_count = 4;
    proto->max_stack = 2;

    ASSERT(nova_vm_execute(vm, proto) == NOVA_VM_OK);

    proto->code = NULL;
    nova_proto_destroy(proto);
    nova_vm_destroy(vm);
    return 1;
}

static int test_arith_mul(void) {
    NovaVM *vm = nova_vm_create();
    ASSERT(vm != NULL);

    NovaProto *proto = nova_proto_create();
    NovaInstruction code[4];
    code[0] = NOVA_ENCODE_ASBX(NOVA_OP_LOADINT, 0, 6);
    code[1] = NOVA_ENCODE_ASBX(NOVA_OP_LOADINT, 1, 7);
    code[2] = NOVA_ENCODE_ABC(NOVA_OP_MUL, 0, 0, 1);
    code[3] = NOVA_ENCODE_ABC(NOVA_OP_RETURN0, 0, 0, 0);

    proto->code = code;
    proto->code_count = 4;
    proto->max_stack = 2;

    ASSERT(nova_vm_execute(vm, proto) == NOVA_VM_OK);

    proto->code = NULL;
    nova_proto_destroy(proto);
    nova_vm_destroy(vm);
    return 1;
}

static int test_arith_unm(void) {
    NovaVM *vm = nova_vm_create();
    ASSERT(vm != NULL);

    NovaProto *proto = nova_proto_create();
    NovaInstruction code[3];
    code[0] = NOVA_ENCODE_ASBX(NOVA_OP_LOADINT, 0, 42);
    code[1] = NOVA_ENCODE_ABC(NOVA_OP_UNM, 0, 0, 0);
    code[2] = NOVA_ENCODE_ABC(NOVA_OP_RETURN0, 0, 0, 0);

    proto->code = code;
    proto->code_count = 3;
    proto->max_stack = 1;

    ASSERT(nova_vm_execute(vm, proto) == NOVA_VM_OK);

    proto->code = NULL;
    nova_proto_destroy(proto);
    nova_vm_destroy(vm);
    return 1;
}

/* ============================================================
 * TEST: BITWISE OPERATIONS
 * ============================================================ */

static int test_bitwise_band(void) {
    NovaVM *vm = nova_vm_create();
    ASSERT(vm != NULL);

    NovaProto *proto = nova_proto_create();
    NovaInstruction code[4];
    code[0] = NOVA_ENCODE_ASBX(NOVA_OP_LOADINT, 0, 0xFF);
    code[1] = NOVA_ENCODE_ASBX(NOVA_OP_LOADINT, 1, 0x0F);
    code[2] = NOVA_ENCODE_ABC(NOVA_OP_BAND, 0, 0, 1);
    code[3] = NOVA_ENCODE_ABC(NOVA_OP_RETURN0, 0, 0, 0);

    proto->code = code;
    proto->code_count = 4;
    proto->max_stack = 2;

    ASSERT(nova_vm_execute(vm, proto) == NOVA_VM_OK);

    proto->code = NULL;
    nova_proto_destroy(proto);
    nova_vm_destroy(vm);
    return 1;
}

static int test_bitwise_bnot(void) {
    NovaVM *vm = nova_vm_create();
    ASSERT(vm != NULL);

    NovaProto *proto = nova_proto_create();
    NovaInstruction code[3];
    code[0] = NOVA_ENCODE_ASBX(NOVA_OP_LOADINT, 0, 0);
    code[1] = NOVA_ENCODE_ABC(NOVA_OP_BNOT, 0, 0, 0);
    code[2] = NOVA_ENCODE_ABC(NOVA_OP_RETURN0, 0, 0, 0);

    proto->code = code;
    proto->code_count = 3;
    proto->max_stack = 1;

    ASSERT(nova_vm_execute(vm, proto) == NOVA_VM_OK);

    proto->code = NULL;
    nova_proto_destroy(proto);
    nova_vm_destroy(vm);
    return 1;
}

/* ============================================================
 * TEST: LOGICAL OPERATIONS
 * ============================================================ */

static int test_logical_not(void) {
    NovaVM *vm = nova_vm_create();
    ASSERT(vm != NULL);

    NovaProto *proto = nova_proto_create();
    NovaInstruction code[3];
    code[0] = NOVA_ENCODE_ABC(NOVA_OP_LOADBOOL, 0, 1, 0);  /* true */
    code[1] = NOVA_ENCODE_ABC(NOVA_OP_NOT, 0, 0, 0);        /* false */
    code[2] = NOVA_ENCODE_ABC(NOVA_OP_RETURN0, 0, 0, 0);

    proto->code = code;
    proto->code_count = 3;
    proto->max_stack = 1;

    ASSERT(nova_vm_execute(vm, proto) == NOVA_VM_OK);

    proto->code = NULL;
    nova_proto_destroy(proto);
    nova_vm_destroy(vm);
    return 1;
}

/* ============================================================
 * TEST: MOVE AND LOADNIL
 * ============================================================ */

static int test_move(void) {
    NovaVM *vm = nova_vm_create();
    ASSERT(vm != NULL);

    NovaProto *proto = nova_proto_create();
    NovaInstruction code[3];
    code[0] = NOVA_ENCODE_ASBX(NOVA_OP_LOADINT, 0, 123);
    code[1] = NOVA_ENCODE_ABC(NOVA_OP_MOVE, 1, 0, 0);
    code[2] = NOVA_ENCODE_ABC(NOVA_OP_RETURN0, 0, 0, 0);

    proto->code = code;
    proto->code_count = 3;
    proto->max_stack = 2;

    ASSERT(nova_vm_execute(vm, proto) == NOVA_VM_OK);

    proto->code = NULL;
    nova_proto_destroy(proto);
    nova_vm_destroy(vm);
    return 1;
}

static int test_loadnil(void) {
    NovaVM *vm = nova_vm_create();
    ASSERT(vm != NULL);

    NovaProto *proto = nova_proto_create();
    NovaInstruction code[2];
    code[0] = NOVA_ENCODE_ABC(NOVA_OP_LOADNIL, 0, 4, 0);  /* R0..R4 = nil */
    code[1] = NOVA_ENCODE_ABC(NOVA_OP_RETURN0, 0, 0, 0);

    proto->code = code;
    proto->code_count = 2;
    proto->max_stack = 5;

    ASSERT(nova_vm_execute(vm, proto) == NOVA_VM_OK);

    proto->code = NULL;
    nova_proto_destroy(proto);
    nova_vm_destroy(vm);
    return 1;
}

/* ============================================================
 * TEST: LOADI - Load Immediate Float
 * ============================================================ */

static int test_loadi(void) {
    NovaVM *vm = nova_vm_create();
    ASSERT(vm != NULL);

    NovaProto *proto = nova_proto_create();
    NovaInstruction code[2];
    code[0] = NOVA_ENCODE_ASBX(NOVA_OP_LOADINT, 0, 12345);
    code[1] = NOVA_ENCODE_ABC(NOVA_OP_RETURN0, 0, 0, 0);

    proto->code = code;
    proto->code_count = 2;
    proto->max_stack = 1;

    ASSERT(nova_vm_execute(vm, proto) == NOVA_VM_OK);

    proto->code = NULL;
    nova_proto_destroy(proto);
    nova_vm_destroy(vm);
    return 1;
}

/* ============================================================
 * TEST: NEWTABLE
 * ============================================================ */

static int test_newtable(void) {
    NovaVM *vm = nova_vm_create();
    ASSERT(vm != NULL);

    NovaProto *proto = nova_proto_create();
    NovaInstruction code[2];
    code[0] = NOVA_ENCODE_ABC(NOVA_OP_NEWTABLE, 0, 0, 0);
    code[1] = NOVA_ENCODE_ABC(NOVA_OP_RETURN0, 0, 0, 0);

    proto->code = code;
    proto->code_count = 2;
    proto->max_stack = 1;

    ASSERT(nova_vm_execute(vm, proto) == NOVA_VM_OK);

    proto->code = NULL;
    nova_proto_destroy(proto);
    nova_vm_destroy(vm);
    return 1;
}

/* ============================================================
 * TEST: NOP AND DEBUG
 * ============================================================ */

static int test_nop(void) {
    NovaVM *vm = nova_vm_create();
    ASSERT(vm != NULL);

    NovaProto *proto = nova_proto_create();
    NovaInstruction code[4];
    code[0] = NOVA_ENCODE_ABC(NOVA_OP_NOP, 0, 0, 0);
    code[1] = NOVA_ENCODE_ABC(NOVA_OP_NOP, 0, 0, 0);
    code[2] = NOVA_ENCODE_ABC(NOVA_OP_NOP, 0, 0, 0);
    code[3] = NOVA_ENCODE_ABC(NOVA_OP_RETURN0, 0, 0, 0);

    proto->code = code;
    proto->code_count = 4;
    proto->max_stack = 1;

    ASSERT(nova_vm_execute(vm, proto) == NOVA_VM_OK);

    proto->code = NULL;
    nova_proto_destroy(proto);
    nova_vm_destroy(vm);
    return 1;
}

/* ============================================================
 * TEST: ADDI (Add Immediate)
 * ============================================================ */

static int test_addi(void) {
    NovaVM *vm = nova_vm_create();
    ASSERT(vm != NULL);

    NovaProto *proto = nova_proto_create();
    NovaInstruction code[3];
    code[0] = NOVA_ENCODE_ASBX(NOVA_OP_LOADINT, 0, 40);
    code[1] = NOVA_ENCODE_ABC(NOVA_OP_ADDI, 0, 0, 2);  /* R0 = R0 + 2 */
    code[2] = NOVA_ENCODE_ABC(NOVA_OP_RETURN0, 0, 0, 0);

    proto->code = code;
    proto->code_count = 3;
    proto->max_stack = 1;

    ASSERT(nova_vm_execute(vm, proto) == NOVA_VM_OK);

    proto->code = NULL;
    nova_proto_destroy(proto);
    nova_vm_destroy(vm);
    return 1;
}

/* ============================================================
 * TEST: STRLEN
 * ============================================================ */

static int test_strlen_on_table(void) {
    NovaVM *vm = nova_vm_create();
    ASSERT(vm != NULL);

    NovaProto *proto = nova_proto_create();
    NovaInstruction code[3];
    code[0] = NOVA_ENCODE_ABC(NOVA_OP_NEWTABLE, 0, 0, 0);
    code[1] = NOVA_ENCODE_ABC(NOVA_OP_STRLEN, 1, 0, 0);  /* length = 0 */
    code[2] = NOVA_ENCODE_ABC(NOVA_OP_RETURN0, 0, 0, 0);

    proto->code = code;
    proto->code_count = 3;
    proto->max_stack = 2;

    ASSERT(nova_vm_execute(vm, proto) == NOVA_VM_OK);

    proto->code = NULL;
    nova_proto_destroy(proto);
    nova_vm_destroy(vm);
    return 1;
}

/* ============================================================
 * TEST: MULTIPLE C FUNCTIONS
 * ============================================================ */

static int cfunc_get_answer(NovaVM *vm) {
    nova_vm_push_integer(vm, 42);
    return 1;
}

static int cfunc_double(NovaVM *vm) {
    NovaValue arg = nova_vm_get(vm, -1);
    if (arg.type != NOVA_TYPE_INTEGER) {
        nova_vm_push_nil(vm);
        return 1;
    }
    nova_vm_pop(vm, 1);
    nova_vm_push_integer(vm, arg.as.integer * 2);
    return 1;
}

static int test_multiple_cfunctions(void) {
    NovaVM *vm = nova_vm_create();
    ASSERT(vm != NULL);

    /* Call cfunc_get_answer() -> 42 */
    nova_vm_push_cfunction(vm, cfunc_get_answer);
    ASSERT(nova_vm_call(vm, 0, 1) == NOVA_VM_OK);
    ASSERT(nova_vm_get_top(vm) == 1);

    NovaValue v1 = nova_vm_get(vm, 0);
    ASSERT(v1.type == NOVA_TYPE_INTEGER);
    ASSERT_EQ_INT(v1.as.integer, 42);

    /* Call cfunc_double(42) -> 84 */
    nova_vm_push_cfunction(vm, cfunc_double);
    nova_vm_push_integer(vm, 42);
    ASSERT(nova_vm_call(vm, 1, 1) == NOVA_VM_OK);

    NovaValue v2 = nova_vm_get(vm, -1);
    ASSERT(v2.type == NOVA_TYPE_INTEGER);
    ASSERT_EQ_INT(v2.as.integer, 84);

    nova_vm_destroy(vm);
    return 1;
}

/* ============================================================
 * TEST: VALUE TRUTHINESS
 * ============================================================ */

static int test_value_truthiness(void) {
    /* nil is falsy */
    ASSERT(nova_value_is_truthy(nova_value_nil()) == 0);
    
    /* false is falsy */
    ASSERT(nova_value_is_truthy(nova_value_bool(0)) == 0);
    
    /* true is truthy */
    ASSERT(nova_value_is_truthy(nova_value_bool(1)) == 1);
    
    /* 0 is truthy (unlike some languages) */
    ASSERT(nova_value_is_truthy(nova_value_integer(0)) == 1);
    
    /* Non-zero is truthy */
    ASSERT(nova_value_is_truthy(nova_value_integer(42)) == 1);
    
    return 1;
}

/* ============================================================
 * MAIN
 * ============================================================ */

int main(void) {
    printf("\n");
    printf("==============================================\n");
    printf("  NOVA VM ADVANCED TESTS\n");
    printf("==============================================\n\n");

    printf("Arithmetic Operations:\n");
    TEST(arith_sub);
    TEST(arith_mul);
    TEST(arith_unm);

    printf("\nBitwise Operations:\n");
    TEST(bitwise_band);
    TEST(bitwise_bnot);

    printf("\nLogical Operations:\n");
    TEST(logical_not);

    printf("\nLoad/Move Instructions:\n");
    TEST(move);
    TEST(loadnil);

    printf("\nControl Flow:\n");
    TEST(loadi);

    printf("\nTables:\n");
    TEST(newtable);
    TEST(strlen_on_table);

    printf("\nMiscellaneous:\n");
    TEST(nop);
    TEST(addi);

    printf("\nC Function Interface:\n");
    TEST(multiple_cfunctions);

    printf("\nValue Semantics:\n");
    TEST(value_truthiness);

    printf("\n==============================================\n");
    printf("  RESULTS: %d/%d tests passed\n", tests_passed, tests_run);
    printf("==============================================\n\n");

    return (tests_passed == tests_run) ? 0 : 1;
}
