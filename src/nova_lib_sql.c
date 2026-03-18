/**
 * @file nova_lib_sql.c
 * @brief Nova Language - SQL Database Library (SQLite3 backend)
 *
 * Provides SQLite3 database operations as the "sql" module, enabled via:
 *
 *   #import sql
 *
 * Functions:
 *   sql.open(path)                Open/create a database → db handle (int)
 *   sql.close(db)                 Close a database
 *   sql.exec(db, query)           Execute DDL/DML (returns affected rows)
 *   sql.query(db, query)          Execute SELECT → table of row tables
 *   sql.insert(db, table, data)   Insert a table/row into a SQL table
 *   sql.tables(db)                List all table names → table of strings
 *   sql.columns(db, table)        List columns → table of {name, type}
 *   sql.begin(db)                 Start a transaction
 *   sql.commit(db)                Commit a transaction
 *   sql.rollback(db)              Rollback a transaction
 *   sql.last_insert_id(db)        Last inserted row ID
 *   sql.changes(db)               Number of rows changed by last statement
 *   sql.escape(str)               Escape a string for safe SQL embedding
 *
 * Database handles are simple integer indices into an internal pool.
 * Maximum 16 simultaneous open databases.
 *
 * Example:
 *   #import sql
 *   local db = sql.open("myapp.db")
 *   sql.exec(db, "CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, name TEXT, age INTEGER)")
 *   sql.insert(db, "users", {name = "Alice", age = 30})
 *   local rows = sql.query(db, "SELECT * FROM users")
 *   for i = 0, #rows - 1 do
 *       printf("  %s (age %d)\n", rows[i].name, rows[i].age)
 *   end
 *   sql.close(db)
 *
 * @author Anthony Taliento
 * @date 2026-02-12
 * @version 1.0.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 *
 * DEPENDENCIES:
 *   - nova_lib.h (library registration)
 *   - nova_vm.h  (VM API)
 *   - sqlite3    (database engine)
 *
 * THREAD SAFETY:
 *   Not thread-safe. Single VM instance per call.
 */

#include "nova/nova_lib.h"
#include "nova/nova_vm.h"

#include <zorya/sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * INTERNAL: Database handle pool
 * ============================================================ */

#define NOVA_SQL_MAX_DBS 16

static sqlite3 *nova_sql_dbs[NOVA_SQL_MAX_DBS];
static int       nova_sql_initialized = 0;

static void novai_sql_init(void) {
    if (nova_sql_initialized) { return; }
    memset(nova_sql_dbs, 0, sizeof(nova_sql_dbs));
    nova_sql_initialized = 1;
}

static int novai_sql_alloc(sqlite3 *db) {
    novai_sql_init();
    for (int i = 0; i < NOVA_SQL_MAX_DBS; i++) {
        if (nova_sql_dbs[i] == NULL) {
            nova_sql_dbs[i] = db;
            return i;
        }
    }
    return -1;
}

static sqlite3 *novai_sql_get(int idx) {
    novai_sql_init();
    if (idx < 0 || idx >= NOVA_SQL_MAX_DBS) { return NULL; }
    return nova_sql_dbs[idx];
}

static void novai_sql_release(int idx) {
    if (idx >= 0 && idx < NOVA_SQL_MAX_DBS) {
        nova_sql_dbs[idx] = NULL;
    }
}

/* ============================================================
 * sql.open(path)
 *
 * Opens or creates a SQLite database.
 * Pass ":memory:" for an in-memory database.
 * Returns: integer database handle
 * ============================================================ */

static int nova_sql_open(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) { return -1; }
    const char *path = nova_lib_check_string(vm, 0);
    if (path == NULL) { return -1; }

    sqlite3 *db = NULL;
    int rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK) {
        const char *err = db ? sqlite3_errmsg(db) : "unknown error";
        nova_vm_raise_error(vm, "sql.open: %s", err);
        if (db != NULL) { sqlite3_close(db); }
        return -1;
    }

    /* Enable WAL mode for better concurrent read performance */
    (void)sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);

    int handle = novai_sql_alloc(db);
    if (handle < 0) {
        sqlite3_close(db);
        nova_vm_raise_error(vm, "sql.open: too many open databases (max %d)",
                            NOVA_SQL_MAX_DBS);
        return -1;
    }

    nova_vm_push_integer(vm, (nova_int_t)handle);
    return 1;
}

/* ============================================================
 * sql.close(db)
 * ============================================================ */

static int nova_sql_close(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) { return -1; }
    nova_int_t handle = 0;
    if (!nova_lib_check_integer(vm, 0, &handle)) { return -1; }

    sqlite3 *db = novai_sql_get((int)handle);
    if (db == NULL) {
        nova_vm_raise_error(vm, "sql.close: invalid database handle");
        return -1;
    }

    sqlite3_close(db);
    novai_sql_release((int)handle);
    nova_vm_push_bool(vm, 1);
    return 1;
}

/* ============================================================
 * sql.exec(db, query)
 *
 * Executes a DDL/DML statement (CREATE, INSERT, UPDATE, DELETE).
 * Returns: number of affected rows
 * ============================================================ */

static int nova_sql_exec(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) { return -1; }
    nova_int_t handle = 0;
    if (!nova_lib_check_integer(vm, 0, &handle)) { return -1; }
    const char *query = nova_lib_check_string(vm, 1);
    if (query == NULL) { return -1; }

    sqlite3 *db = novai_sql_get((int)handle);
    if (db == NULL) {
        nova_vm_raise_error(vm, "sql.exec: invalid database handle");
        return -1;
    }

    char *err_msg = NULL;
    int rc = sqlite3_exec(db, query, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        const char *msg = err_msg ? err_msg : sqlite3_errmsg(db);
        nova_vm_raise_error(vm, "sql.exec: %s", msg);
        if (err_msg != NULL) { sqlite3_free(err_msg); }
        return -1;
    }
    if (err_msg != NULL) { sqlite3_free(err_msg); }

    nova_vm_push_integer(vm, (nova_int_t)sqlite3_changes(db));
    return 1;
}

/* ============================================================
 * sql.query(db, query)
 *
 * Executes a SELECT and returns all result rows as a table
 * of tables. Each row is a table with column names as keys.
 *
 * Returns: table of row tables (0-indexed array)
 * ============================================================ */

static int nova_sql_query(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) { return -1; }
    nova_int_t handle = 0;
    if (!nova_lib_check_integer(vm, 0, &handle)) { return -1; }
    const char *query = nova_lib_check_string(vm, 1);
    if (query == NULL) { return -1; }

    sqlite3 *db = novai_sql_get((int)handle);
    if (db == NULL) {
        nova_vm_raise_error(vm, "sql.query: invalid database handle");
        return -1;
    }

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, query, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        nova_vm_raise_error(vm, "sql.query: %s", sqlite3_errmsg(db));
        return -1;
    }

    /* Create result array table and root it on the stack */
    nova_vm_push_table(vm);
    int results_idx = nova_vm_get_top(vm) - 1;
    NovaValue results_val = nova_vm_get(vm, results_idx);
    NovaTable *results = nova_as_table(results_val);
    if (results == NULL) {
        sqlite3_finalize(stmt);
        nova_vm_raise_error(vm, "sql.query: out of memory");
        return -1;
    }

    int col_count = sqlite3_column_count(stmt);
    nova_int_t row_idx = 0;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        /* Create row table — push to stack to root it during population */
        nova_vm_push_table(vm);
        int row_stack_idx = nova_vm_get_top(vm) - 1;
        NovaValue row_val = nova_vm_get(vm, row_stack_idx);
        NovaTable *row = nova_as_table(row_val);
        if (row == NULL) {
            sqlite3_finalize(stmt);
            nova_vm_raise_error(vm, "sql.query: out of memory");
            return -1;
        }

        for (int col = 0; col < col_count; col++) {
            const char *col_name = sqlite3_column_name(stmt, col);
            if (col_name == NULL) { continue; }

            NovaString *key = nova_string_new(vm, col_name,
                                              strlen(col_name));
            if (key == NULL) { continue; }

            int col_type = sqlite3_column_type(stmt, col);
            NovaValue val;

            switch (col_type) {
                case SQLITE_INTEGER:
                    val = nova_value_integer(
                        (nova_int_t)sqlite3_column_int64(stmt, col));
                    break;

                case SQLITE_FLOAT:
                    val = nova_value_number(
                        (nova_number_t)sqlite3_column_double(stmt, col));
                    break;

                case SQLITE_TEXT: {
                    const char *text =
                        (const char *)sqlite3_column_text(stmt, col);
                    int text_len = sqlite3_column_bytes(stmt, col);
                    if (text != NULL) {
                        val = nova_value_string(
                            nova_string_new(vm, text, (size_t)text_len));
                    } else {
                        val = NOVA_VALUE_NIL;
                    }
                    break;
                }

                case SQLITE_BLOB: {
                    /* Store blobs as strings (binary data) */
                    const void *blob = sqlite3_column_blob(stmt, col);
                    int blob_len = sqlite3_column_bytes(stmt, col);
                    if (blob != NULL) {
                        val = nova_value_string(
                            nova_string_new(vm, (const char *)blob,
                                            (size_t)blob_len));
                    } else {
                        val = NOVA_VALUE_NIL;
                    }
                    break;
                }

                case SQLITE_NULL:
                default:
                    val = NOVA_VALUE_NIL;
                    break;
            }

            (void)nova_table_set_str(vm, row, key, val);
        }

        /* Add row to results array (0-indexed), then pop row from stack */
        (void)nova_table_raw_set_int(vm, results, row_idx,
                                      nova_value_table(row));
        nova_vm_pop(vm, 1); /* pop row table — it's now held by results */
        row_idx++;
    }

    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        nova_vm_raise_error(vm, "sql.query: %s", sqlite3_errmsg(db));
        return -1;
    }

    /* results table is already on the stack at results_idx */
    return 1;
}

/* ============================================================
 * sql.insert(db, table_name, data)
 *
 * Convenience function to INSERT a row from a Nova table.
 * The table keys become column names, values become values.
 *
 *   sql.insert(db, "users", {name = "Alice", age = 30})
 *   → INSERT INTO users (name, age) VALUES ('Alice', 30)
 *
 * Returns: last insert rowid
 * ============================================================ */

static int nova_sql_insert(NovaVM *vm) {
    if (nova_lib_check_args(vm, 3) != 0) { return -1; }
    nova_int_t handle = 0;
    if (!nova_lib_check_integer(vm, 0, &handle)) { return -1; }
    const char *table_name = nova_lib_check_string(vm, 1);
    if (table_name == NULL) { return -1; }

    NovaValue data_val = nova_vm_get(vm, 2);
    if (!nova_is_table(data_val) || nova_as_table(data_val) == NULL) {
        nova_vm_raise_error(vm, "sql.insert: argument 3 must be a table");
        return -1;
    }

    sqlite3 *db = novai_sql_get((int)handle);
    if (db == NULL) {
        nova_vm_raise_error(vm, "sql.insert: invalid database handle");
        return -1;
    }

    NovaTable *data = nova_as_table(data_val);

    /* Collect column names and values */
    /* We build: INSERT INTO table (col1, col2, ...) VALUES (?, ?, ...) */
    /* Then bind parameters for safety */

    /* First pass: count columns */
    int ncols = 0;
    uint32_t iter = 0;
    NovaValue hk, hv;
    while (nova_table_next(data, &iter, &hk, &hv)) {
        if (nova_is_string(hk)) { ncols++; }
    }

    if (ncols == 0) {
        nova_vm_raise_error(vm, "sql.insert: data table is empty");
        return -1;
    }

    /* Build SQL string */
    char sql_buf[4096];
    int pos = snprintf(sql_buf, sizeof(sql_buf), "INSERT INTO %s (", table_name);

    /* Temporary arrays for column names and values */
    const char *col_names[128];
    NovaValue   col_vals[128];
    int col_count = 0;

    iter = 0;
    while (nova_table_next(data, &iter, &hk, &hv) && col_count < 128) {
        if (!nova_is_string(hk)) { continue; }
        col_names[col_count] = nova_str_data(nova_as_string(hk));
        col_vals[col_count]  = hv;
        col_count++;
    }

    for (int i = 0; i < col_count; i++) {
        if (i > 0) { pos += snprintf(sql_buf + pos, sizeof(sql_buf) - (size_t)pos, ", "); }
        pos += snprintf(sql_buf + pos, sizeof(sql_buf) - (size_t)pos, "%s", col_names[i]);
    }
    pos += snprintf(sql_buf + pos, sizeof(sql_buf) - (size_t)pos, ") VALUES (");
    for (int i = 0; i < col_count; i++) {
        if (i > 0) { pos += snprintf(sql_buf + pos, sizeof(sql_buf) - (size_t)pos, ", "); }
        pos += snprintf(sql_buf + pos, sizeof(sql_buf) - (size_t)pos, "?");
    }
    (void)snprintf(sql_buf + pos, sizeof(sql_buf) - (size_t)pos, ")");

    /* Prepare statement */
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql_buf, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        nova_vm_raise_error(vm, "sql.insert: %s", sqlite3_errmsg(db));
        return -1;
    }

    /* Bind parameters */
    for (int i = 0; i < col_count; i++) {
        NovaValue v = col_vals[i];
        if (nova_is_nil(v)) {
            sqlite3_bind_null(stmt, i + 1);
        } else if (nova_is_integer(v)) {
            sqlite3_bind_int64(stmt, i + 1, (sqlite3_int64)nova_as_integer(v));
        } else if (nova_is_number(v)) {
            sqlite3_bind_double(stmt, i + 1, nova_as_number(v));
        } else if (nova_is_bool(v)) {
            sqlite3_bind_int(stmt, i + 1, nova_as_bool(v));
        } else if (nova_is_string(v)) {
            sqlite3_bind_text(stmt, i + 1,
                              nova_str_data(nova_as_string(v)),
                              (int)nova_str_len(nova_as_string(v)),
                              SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_null(stmt, i + 1);
        }
    }

    /* Execute */
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        nova_vm_raise_error(vm, "sql.insert: %s", sqlite3_errmsg(db));
        return -1;
    }

    nova_vm_push_integer(vm, (nova_int_t)sqlite3_last_insert_rowid(db));
    return 1;
}

/* ============================================================
 * sql.tables(db)
 *
 * Returns a table of all user table names in the database.
 * ============================================================ */

static int nova_sql_tables(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) { return -1; }
    nova_int_t handle = 0;
    if (!nova_lib_check_integer(vm, 0, &handle)) { return -1; }

    sqlite3 *db = novai_sql_get((int)handle);
    if (db == NULL) {
        nova_vm_raise_error(vm, "sql.tables: invalid database handle");
        return -1;
    }

    const char *query =
        "SELECT name FROM sqlite_master WHERE type='table' "
        "AND name NOT LIKE 'sqlite_%' ORDER BY name";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, query, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        nova_vm_raise_error(vm, "sql.tables: %s", sqlite3_errmsg(db));
        return -1;
    }

    nova_vm_push_table(vm);
    int results_stack = nova_vm_get_top(vm) - 1;
    NovaValue results_v = nova_vm_get(vm, results_stack);
    NovaTable *results = nova_as_table(results_v);
    if (results == NULL) {
        sqlite3_finalize(stmt);
        nova_vm_raise_error(vm, "sql.tables: out of memory");
        return -1;
    }

    nova_int_t idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(stmt, 0);
        if (name != NULL) {
            NovaValue sv = nova_value_string(
                nova_string_new(vm, name, strlen(name)));
            (void)nova_table_raw_set_int(vm, results, idx, sv);
            idx++;
        }
    }

    sqlite3_finalize(stmt);
    /* results already on stack */
    return 1;
}

/* ============================================================
 * sql.columns(db, table_name)
 *
 * Returns column info: table of {name="...", type="...", ...}
 * ============================================================ */

static int nova_sql_columns(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) { return -1; }
    nova_int_t handle = 0;
    if (!nova_lib_check_integer(vm, 0, &handle)) { return -1; }
    const char *table_name = nova_lib_check_string(vm, 1);
    if (table_name == NULL) { return -1; }

    sqlite3 *db = novai_sql_get((int)handle);
    if (db == NULL) {
        nova_vm_raise_error(vm, "sql.columns: invalid database handle");
        return -1;
    }

    char query[512];
    (void)snprintf(query, sizeof(query), "PRAGMA table_info(%s)", table_name);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, query, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        nova_vm_raise_error(vm, "sql.columns: %s", sqlite3_errmsg(db));
        return -1;
    }

    nova_vm_push_table(vm);
    int results_cidx = nova_vm_get_top(vm) - 1;
    NovaValue results_cv = nova_vm_get(vm, results_cidx);
    NovaTable *results = nova_as_table(results_cv);
    if (results == NULL) {
        sqlite3_finalize(stmt);
        nova_vm_raise_error(vm, "sql.columns: out of memory");
        return -1;
    }

    nova_int_t idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        nova_vm_push_table(vm);
        int ci_stack = nova_vm_get_top(vm) - 1;
        NovaValue ci_val = nova_vm_get(vm, ci_stack);
        NovaTable *col_info = nova_as_table(ci_val);
        if (col_info == NULL) {
            nova_vm_pop(vm, 1);
            continue;
        }

        /* cid, name, type, notnull, dflt_value, pk */
        const char *name = (const char *)sqlite3_column_text(stmt, 1);
        const char *type = (const char *)sqlite3_column_text(stmt, 2);
        int notnull = sqlite3_column_int(stmt, 3);
        int pk = sqlite3_column_int(stmt, 5);

        if (name != NULL) {
            NovaString *k = nova_string_new(vm, "name", 4);
            NovaValue v = nova_value_string(
                nova_string_new(vm, name, strlen(name)));
            if (k != NULL) { (void)nova_table_set_str(vm, col_info, k, v); }
        }
        if (type != NULL) {
            NovaString *k = nova_string_new(vm, "type", 4);
            NovaValue v = nova_value_string(
                nova_string_new(vm, type, strlen(type)));
            if (k != NULL) { (void)nova_table_set_str(vm, col_info, k, v); }
        }
        {
            NovaString *k = nova_string_new(vm, "notnull", 7);
            if (k != NULL) {
                (void)nova_table_set_str(vm, col_info, k,
                                          nova_value_bool(notnull));
            }
        }
        {
            NovaString *k = nova_string_new(vm, "pk", 2);
            if (k != NULL) {
                (void)nova_table_set_str(vm, col_info, k,
                                          nova_value_bool(pk));
            }
        }

        (void)nova_table_raw_set_int(vm, results, idx,
                                      nova_value_table(col_info));
        nova_vm_pop(vm, 1); /* pop col_info — held by results */
        idx++;
    }

    sqlite3_finalize(stmt);
    /* results already on stack */
    return 1;
}

/* ============================================================
 * sql.begin(db) / sql.commit(db) / sql.rollback(db)
 * ============================================================ */

static int nova_sql_begin(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) { return -1; }
    nova_int_t handle = 0;
    if (!nova_lib_check_integer(vm, 0, &handle)) { return -1; }

    sqlite3 *db = novai_sql_get((int)handle);
    if (db == NULL) {
        nova_vm_raise_error(vm, "sql.begin: invalid database handle");
        return -1;
    }

    char *err = NULL;
    if (sqlite3_exec(db, "BEGIN TRANSACTION", NULL, NULL, &err) != SQLITE_OK) {
        nova_vm_raise_error(vm, "sql.begin: %s", err ? err : "unknown error");
        if (err) { sqlite3_free(err); }
        return -1;
    }
    if (err) { sqlite3_free(err); }

    nova_vm_push_bool(vm, 1);
    return 1;
}

static int nova_sql_commit(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) { return -1; }
    nova_int_t handle = 0;
    if (!nova_lib_check_integer(vm, 0, &handle)) { return -1; }

    sqlite3 *db = novai_sql_get((int)handle);
    if (db == NULL) {
        nova_vm_raise_error(vm, "sql.commit: invalid database handle");
        return -1;
    }

    char *err = NULL;
    if (sqlite3_exec(db, "COMMIT", NULL, NULL, &err) != SQLITE_OK) {
        nova_vm_raise_error(vm, "sql.commit: %s", err ? err : "unknown error");
        if (err) { sqlite3_free(err); }
        return -1;
    }
    if (err) { sqlite3_free(err); }

    nova_vm_push_bool(vm, 1);
    return 1;
}

static int nova_sql_rollback(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) { return -1; }
    nova_int_t handle = 0;
    if (!nova_lib_check_integer(vm, 0, &handle)) { return -1; }

    sqlite3 *db = novai_sql_get((int)handle);
    if (db == NULL) {
        nova_vm_raise_error(vm, "sql.rollback: invalid database handle");
        return -1;
    }

    char *err = NULL;
    if (sqlite3_exec(db, "ROLLBACK", NULL, NULL, &err) != SQLITE_OK) {
        nova_vm_raise_error(vm, "sql.rollback: %s",
                            err ? err : "unknown error");
        if (err) { sqlite3_free(err); }
        return -1;
    }
    if (err) { sqlite3_free(err); }

    nova_vm_push_bool(vm, 1);
    return 1;
}

/* ============================================================
 * sql.last_insert_id(db)
 * ============================================================ */

static int nova_sql_last_insert_id(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) { return -1; }
    nova_int_t handle = 0;
    if (!nova_lib_check_integer(vm, 0, &handle)) { return -1; }

    sqlite3 *db = novai_sql_get((int)handle);
    if (db == NULL) {
        nova_vm_raise_error(vm, "sql.last_insert_id: invalid database handle");
        return -1;
    }

    nova_vm_push_integer(vm, (nova_int_t)sqlite3_last_insert_rowid(db));
    return 1;
}

/* ============================================================
 * sql.changes(db)
 * ============================================================ */

static int nova_sql_changes_fn(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) { return -1; }
    nova_int_t handle = 0;
    if (!nova_lib_check_integer(vm, 0, &handle)) { return -1; }

    sqlite3 *db = novai_sql_get((int)handle);
    if (db == NULL) {
        nova_vm_raise_error(vm, "sql.changes: invalid database handle");
        return -1;
    }

    nova_vm_push_integer(vm, (nova_int_t)sqlite3_changes(db));
    return 1;
}

/* ============================================================
 * sql.escape(str)
 *
 * Escapes a string for safe embedding in SQL queries.
 * Doubles single quotes: ' → ''
 * ============================================================ */

static int nova_sql_escape_fn(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) { return -1; }
    const char *str = nova_lib_check_string(vm, 0);
    if (str == NULL) { return -1; }

    size_t len = strlen(str);
    /* Worst case: every char is a quote */
    char *escaped = (char *)malloc(len * 2 + 1);
    if (escaped == NULL) {
        nova_vm_raise_error(vm, "sql.escape: out of memory");
        return -1;
    }

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (str[i] == '\'') {
            escaped[j++] = '\'';
            escaped[j++] = '\'';
        } else {
            escaped[j++] = str[i];
        }
    }
    escaped[j] = '\0';

    nova_vm_push_string(vm, escaped, j);
    free(escaped);
    return 1;
}

/* ============================================================
 * MODULE REGISTRATION
 * ============================================================ */

static const NovaLibReg nova_sql_lib[] = {
    {"open",           nova_sql_open},
    {"close",          nova_sql_close},
    {"exec",           nova_sql_exec},
    {"query",          nova_sql_query},
    {"insert",         nova_sql_insert},
    {"tables",         nova_sql_tables},
    {"columns",        nova_sql_columns},
    {"begin",          nova_sql_begin},
    {"commit",         nova_sql_commit},
    {"rollback",       nova_sql_rollback},
    {"last_insert_id", nova_sql_last_insert_id},
    {"changes",        nova_sql_changes_fn},
    {"escape",         nova_sql_escape_fn},
    {NULL,             NULL}
};

/* ============================================================
 * MODULE OPENER
 * ============================================================ */

/**
 * @brief Open the sql library module.
 *
 * Registers the "sql" module table with all SQLite functions.
 *
 * @param vm  VM instance
 * @return 0 on success, -1 on failure
 */
int nova_open_sql(NovaVM *vm) {
    if (vm == NULL) { return -1; }
    nova_lib_register_module(vm, "sql", nova_sql_lib);
    return 0;
}
