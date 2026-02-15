/**
 * @file test_vm_basic.c
 * @brief Basic VM tests for Nova Language
 *
 * Tests VM initialization, stack operations, and simple bytecode execution.
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
 * TEST: VM CREATION / DESTRUCTION
 * ============================================================ */

static int test_vm_create_destroy(void) {
    NovaVM *vm = nova_vm_create();
    ASSERT(vm != NULL);
    nova_vm_destroy(vm);
    return 1;
}

/* ============================================================
 * TEST: STACK PUSH/POP
 * ============================================================ */

static int test_stack_push_nil(void) {
    NovaVM *vm = nova_vm_create();
    ASSERT(vm != NULL);

    nova_vm_push_nil(vm);
    ASSERT(nova_vm_get_top(vm) == 1);

    NovaValue v = nova_vm_get(vm, 0);
    ASSERT(v.type == NOVA_TYPE_NIL);

    nova_vm_destroy(vm);
    return 1;
}

static int test_stack_push_bool(void) {
    NovaVM *vm = nova_vm_create();
    ASSERT(vm != NULL);

    nova_vm_push_bool(vm, 1);
    nova_vm_push_bool(vm, 0);
    ASSERT(nova_vm_get_top(vm) == 2);

    NovaValue v0 = nova_vm_get(vm, 0);
    NovaValue v1 = nova_vm_get(vm, 1);
    ASSERT(v0.type == NOVA_TYPE_BOOL);
    ASSERT(v0.as.boolean == 1);
    ASSERT(v1.type == NOVA_TYPE_BOOL);
    ASSERT(v1.as.boolean == 0);

    nova_vm_destroy(vm);
    return 1;
}

static int test_stack_push_integer(void) {
    NovaVM *vm = nova_vm_create();
    ASSERT(vm != NULL);

    nova_vm_push_integer(vm, 42);
    nova_vm_push_integer(vm, -100);
    nova_vm_push_integer(vm, 0x7FFFFFFFFFFFFFFFLL);

    ASSERT(nova_vm_get_top(vm) == 3);

    NovaValue v0 = nova_vm_get(vm, 0);
    NovaValue v1 = nova_vm_get(vm, 1);
    NovaValue v2 = nova_vm_get(vm, 2);

    ASSERT(v0.type == NOVA_TYPE_INTEGER);
    ASSERT_EQ_INT(v0.as.integer, 42);
    ASSERT_EQ_INT(v1.as.integer, -100);
    ASSERT_EQ_INT(v2.as.integer, 0x7FFFFFFFFFFFFFFFLL);

    nova_vm_destroy(vm);
    return 1;
}

static int test_stack_push_number(void) {
    NovaVM *vm = nova_vm_create();
    ASSERT(vm != NULL);

    nova_vm_push_number(vm, 3.14159);
    ASSERT(nova_vm_get_top(vm) == 1);

    NovaValue v = nova_vm_get(vm, 0);
    ASSERT(v.type == NOVA_TYPE_NUMBER);
    ASSERT(v.as.number > 3.14 && v.as.number < 3.15);

    nova_vm_destroy(vm);
    return 1;
}

static int test_stack_push_string(void) {
    NovaVM *vm = nova_vm_create();
    ASSERT(vm != NULL);

    nova_vm_push_string(vm, "Hello, Nova!", 12);
    ASSERT(nova_vm_get_top(vm) == 1);

    NovaValue v = nova_vm_get(vm, 0);
    ASSERT(v.type == NOVA_TYPE_STRING);
    ASSERT(v.as.string != NULL);
    ASSERT(v.as.string->length == 12);
    ASSERT(memcmp(v.as.string->data, "Hello, Nova!", 12) == 0);

    nova_vm_destroy(vm);
    return 1;
}

static int test_stack_pop(void) {
    NovaVM *vm = nova_vm_create();
    ASSERT(vm != NULL);

    nova_vm_push_integer(vm, 1);
    nova_vm_push_integer(vm, 2);
    nova_vm_push_integer(vm, 3);
    ASSERT(nova_vm_get_top(vm) == 3);

    nova_vm_pop(vm, 2);
    ASSERT(nova_vm_get_top(vm) == 1);

    NovaValue v = nova_vm_get(vm, 0);
    ASSERT_EQ_INT(v.as.integer, 1);

    nova_vm_destroy(vm);
    return 1;
}

static int test_stack_negative_index(void) {
    NovaVM *vm = nova_vm_create();
    ASSERT(vm != NULL);

    nova_vm_push_integer(vm, 10);
    nova_vm_push_integer(vm, 20);
    nova_vm_push_integer(vm, 30);

    NovaValue top = nova_vm_get(vm, -1);
    NovaValue mid = nova_vm_get(vm, -2);
    NovaValue bot = nova_vm_get(vm, -3);

    ASSERT_EQ_INT(top.as.integer, 30);
    ASSERT_EQ_INT(mid.as.integer, 20);
    ASSERT_EQ_INT(bot.as.integer, 10);

    nova_vm_destroy(vm);
    return 1;
}

/* ============================================================
 * TEST: GLOBALS
 * ============================================================ */

static int test_global_set_get(void) {
    NovaVM *vm = nova_vm_create();
    ASSERT(vm != NULL);

    nova_vm_set_global(vm, "answer", nova_value_integer(42));
    NovaValue v = nova_vm_get_global(vm, "answer");

    ASSERT(v.type == NOVA_TYPE_INTEGER);
    ASSERT_EQ_INT(v.as.integer, 42);

    nova_vm_destroy(vm);
    return 1;
}

static int test_global_missing(void) {
    NovaVM *vm = nova_vm_create();
    ASSERT(vm != NULL);

    NovaValue v = nova_vm_get_global(vm, "does_not_exist");
    ASSERT(v.type == NOVA_TYPE_NIL);

    nova_vm_destroy(vm);
    return 1;
}

/* ============================================================
 * TEST: C FUNCTION CALL
 * ============================================================ */

static int cfunc_add_two(NovaVM *vm) {
    /* Get argument from stack */
    NovaValue arg = nova_vm_get(vm, -1);
    if (arg.type != NOVA_TYPE_INTEGER) {
        return -1;
    }

    /* Push result */
    nova_vm_pop(vm, 1);  /* Remove argument */
    nova_vm_push_integer(vm, arg.as.integer + 2);
    return 1;  /* One result */
}

static int test_cfunction_call(void) {
    NovaVM *vm = nova_vm_create();
    ASSERT(vm != NULL);

    /* Push function and argument */
    nova_vm_push_cfunction(vm, cfunc_add_two);
    nova_vm_push_integer(vm, 40);

    /* Call with 1 arg, expect 1 result */
    int status = nova_vm_call(vm, 1, 1);
    ASSERT(status == NOVA_VM_OK);
    ASSERT(nova_vm_get_top(vm) == 1);

    NovaValue result = nova_vm_get(vm, 0);
    ASSERT(result.type == NOVA_TYPE_INTEGER);
    ASSERT_EQ_INT(result.as.integer, 42);

    nova_vm_destroy(vm);
    return 1;
}

/* ============================================================
 * TEST: SIMPLE BYTECODE EXECUTION
 * ============================================================ */

static int test_execute_loadint(void) {
    NovaVM *vm = nova_vm_create();
    ASSERT(vm != NULL);

    /* Create a simple proto that loads an integer */
    NovaProto *proto = nova_proto_create();
    ASSERT(proto != NULL);

    /* LOADINT R0, 42 */
    NovaInstruction code[2];
    code[0] = NOVA_ENCODE_ASBX(NOVA_OP_LOADINT, 0, 42);
    code[1] = NOVA_ENCODE_ABC(NOVA_OP_RETURN0, 0, 0, 0);

    proto->code = code;
    proto->code_count = 2;
    proto->max_stack = 1;

    int status = nova_vm_execute(vm, proto);
    printf("(status=%d) ", status);
    ASSERT(status == NOVA_VM_OK);

    /* Clean up - don't free code since it's on stack */
    proto->code = NULL;
    proto->code_count = 0;
    nova_proto_destroy(proto);
    nova_vm_destroy(vm);
    return 1;
}

static int test_execute_add(void) {
    NovaVM *vm = nova_vm_create();
    ASSERT(vm != NULL);

    NovaProto *proto = nova_proto_create();
    ASSERT(proto != NULL);

    /* 
     * LOADINT R0, 10
     * LOADINT R1, 32
     * ADD R0, R0, R1
     * RETURN0
     */
    NovaInstruction code[4];
    code[0] = NOVA_ENCODE_ASBX(NOVA_OP_LOADINT, 0, 10);
    code[1] = NOVA_ENCODE_ASBX(NOVA_OP_LOADINT, 1, 32);
    code[2] = NOVA_ENCODE_ABC(NOVA_OP_ADD, 0, 0, 1);
    code[3] = NOVA_ENCODE_ABC(NOVA_OP_RETURN0, 0, 0, 0);

    proto->code = code;
    proto->code_count = 4;
    proto->max_stack = 2;

    int status = nova_vm_execute(vm, proto);
    printf("(status=%d) ", status);
    ASSERT(status == NOVA_VM_OK);

    proto->code = NULL;
    proto->code_count = 0;
    nova_proto_destroy(proto);
    nova_vm_destroy(vm);
    return 1;
}

/* ============================================================
 * TEST: TYPENAME
 * ============================================================ */

static int test_typename(void) {
    ASSERT(strcmp(nova_vm_typename(NOVA_TYPE_NIL), "nil") == 0);
    ASSERT(strcmp(nova_vm_typename(NOVA_TYPE_BOOL), "boolean") == 0);
    ASSERT(strcmp(nova_vm_typename(NOVA_TYPE_INTEGER), "integer") == 0);
    ASSERT(strcmp(nova_vm_typename(NOVA_TYPE_NUMBER), "number") == 0);
    ASSERT(strcmp(nova_vm_typename(NOVA_TYPE_STRING), "string") == 0);
    ASSERT(strcmp(nova_vm_typename(NOVA_TYPE_TABLE), "table") == 0);
    ASSERT(strcmp(nova_vm_typename(NOVA_TYPE_FUNCTION), "function") == 0);
    ASSERT(strcmp(nova_vm_typename(NOVA_TYPE_CFUNCTION), "cfunction") == 0);
    return 1;
}

/* ============================================================
 * MAIN
 * ============================================================ */

int main(void) {
    printf("\n");
    printf("==============================================\n");
    printf("  NOVA VM BASIC TESTS\n");
    printf("==============================================\n\n");

    printf("VM Lifecycle:\n");
    TEST(vm_create_destroy);

    printf("\nStack Operations:\n");
    TEST(stack_push_nil);
    TEST(stack_push_bool);
    TEST(stack_push_integer);
    TEST(stack_push_number);
    TEST(stack_push_string);
    TEST(stack_pop);
    TEST(stack_negative_index);

    printf("\nGlobal Table:\n");
    TEST(global_set_get);
    TEST(global_missing);

    printf("\nC Function Interface:\n");
    TEST(cfunction_call);

    printf("\nBytecode Execution:\n");
    TEST(execute_loadint);
    TEST(execute_add);

    printf("\nUtilities:\n");
    TEST(typename);

    printf("\n==============================================\n");
    printf("  RESULTS: %d/%d tests passed\n", tests_passed, tests_run);
    printf("==============================================\n\n");

    return (tests_passed == tests_run) ? 0 : 1;
}
