/**
 * @file nova_lib_data.c
 * @brief Nova Language - Data Processing Standard Library
 *
 * Exposes the NDP (Nova Data Processor) codecs as per-format modules:
 *
 *   #import json  ->  json.decode(text)  json.encode(value [, opts])
 *   #import csv   ->  csv.decode(text [, opts])  csv.encode(value [, opts])
 *   #import tsv   ->  tsv.decode(text [, opts])  tsv.encode(value [, opts])
 *   #import ini   ->  ini.decode(text)  ini.encode(value [, opts])
 *   #import toml  ->  toml.decode(text)  toml.encode(value [, opts])
 *   #import html  ->  html.decode(text [, opts])  html.encode(value [, opts])
 *   #import data  ->  ALL of the above + data.detect(text)
 *
 * Each format module provides:
 *   <fmt>.decode(text [, options])   -- string -> table
 *   <fmt>.encode(value [, options])  -- table -> string
 *   <fmt>.load(filename [, options]) -- file -> table
 *   <fmt>.save(filename, value [, options]) -- table -> file
 *
 * The "data" meta-module also provides:
 *   data.decode(text, format [, options])
 *   data.encode(value, format [, options])
 *   data.load(filename, format [, options])
 *   data.save(filename, value, format [, options])
 *   data.detect(text)
 *
 * @author Anthony Taliento
 * @date 2026-02-11
 * @version 2.0.0
 *
 * @copyright Copyright (c) 2026 Zorya Corporation
 * @license MIT
 *
 * ZORYA-C COMPLIANCE: v2.0.0
 *
 * DEPENDENCIES:
 *   - nova_ndp.h / nova_ndp.c (codec engine)
 *   - nova_lib.h (registration infrastructure)
 *   - nova_pp.h (NOVA_IMPORT_* flags)
 *
 * THREAD SAFETY:
 *   Not thread-safe. Operates on a single VM instance.
 */

#include "nova/nova_lib.h"
#include "nova/nova_ndp.h"
#include "nova/nova_nini.h"
#include "nova/nova_pp.h"
#include "nova/nova_vm.h"

/* Forward declarations for non-data modules opened via #import */
#ifndef NOVA_NO_NET
extern int nova_open_net(NovaVM *vm);
#endif
extern int nova_open_sql(NovaVM *vm);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * HELPER: Parse options table into NdpOptions
 * ============================================================ */

static void ndp_read_options(NovaVM *vm, int opts_idx, NdpOptions *opts) {
    if (opts_idx >= nova_vm_get_top(vm)) {
        return;
    }
    NovaValue ov = nova_vm_get(vm, opts_idx);
    if (!nova_is_table(ov) || nova_as_table(ov) == NULL) {
        return;
    }

    NovaTable *t = nova_as_table(ov);
    uint32_t iter = 0;
    NovaValue hk, hv;
    while (nova_table_next(t, &iter, &hk, &hv)) {
        if (!nova_is_string(hk)) {
            continue;
        }
        const char *k = nova_str_data(nova_as_string(hk));
        NovaValue v = hv;

        if (strcmp(k, "delimiter") == 0 && nova_is_string(v) &&
            nova_str_len(nova_as_string(v)) > 0) {
            opts->csv_delimiter = nova_str_data(nova_as_string(v))[0];
        } else if (strcmp(k, "header") == 0 && nova_is_bool(v)) {
            opts->csv_header = nova_as_bool(v);
        } else if (strcmp(k, "pretty") == 0 && nova_is_bool(v)) {
            opts->pretty = nova_as_bool(v);
        } else if (strcmp(k, "indent") == 0 && nova_is_integer(v)) {
            opts->indent = (int)nova_as_integer(v);
        } else if (strcmp(k, "typed") == 0 && nova_is_bool(v)) {
            opts->ini_typed = nova_as_bool(v);
        } else if (strcmp(k, "text_only") == 0 && nova_is_bool(v)) {
            opts->html_text_only = nova_as_bool(v);
        } else if (strcmp(k, "strict") == 0 && nova_is_bool(v)) {
            opts->json_strict = nova_as_bool(v);
        } else if (strcmp(k, "quote") == 0 && nova_is_string(v) &&
                   nova_str_len(nova_as_string(v)) > 0) {
            opts->csv_quote = nova_str_data(nova_as_string(v))[0];
        }
    }
}

/* ============================================================
 * HELPER: Read a file into a buffer
 * ============================================================ */

static char *ndp_read_file(const char *filename, size_t *out_len,
                           char *errbuf, size_t errbuf_sz) {
    FILE *f = fopen(filename, "rb");
    if (f == NULL) {
        (void)snprintf(errbuf, errbuf_sz, "cannot open '%s'", filename);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize < 0 || fsize > 100 * 1024 * 1024) {
        fclose(f);
        (void)snprintf(errbuf, errbuf_sz, "file too large or seek error");
        return NULL;
    }

    char *content = (char *)malloc((size_t)fsize + 1);
    if (content == NULL) {
        fclose(f);
        (void)snprintf(errbuf, errbuf_sz, "out of memory");
        return NULL;
    }

    size_t nread = fread(content, 1, (size_t)fsize, f);
    fclose(f);
    content[nread] = '\0';
    *out_len = nread;
    return content;
}

/* ============================================================
 * HELPER: Write buffer to file
 * ============================================================ */

static int ndp_write_file(const char *filename, const char *data,
                          size_t len, char *errbuf, size_t errbuf_sz) {
    FILE *f = fopen(filename, "wb");
    if (f == NULL) {
        (void)snprintf(errbuf, errbuf_sz,
                       "cannot open '%s' for writing", filename);
        return -1;
    }

    if (len > 0 && data != NULL) {
        size_t written = fwrite(data, 1, len, f);
        if (written != len) {
            fclose(f);
            (void)snprintf(errbuf, errbuf_sz, "write error");
            return -1;
        }
    }
    fclose(f);
    return 0;
}

/* ============================================================
 * PER-FORMAT FUNCTION GENERATORS
 *
 * Each macro creates decode/encode/load/save for one format.
 * The format is baked in -- no format string parameter needed.
 * ============================================================ */

#define DEFINE_FORMAT_DECODE(FNAME, FMT_ENUM, FMT_NAME)                  \
static int nova_##FNAME##_decode(NovaVM *vm) {                           \
    if (nova_lib_check_args(vm, 1) != 0) { return -1; }                 \
    NovaValue text_val = nova_vm_get(vm, 0);                             \
    if (!nova_is_string(text_val)) {                             \
        nova_vm_raise_error(vm,                                          \
            FMT_NAME ".decode: argument 1 must be a string");            \
        return -1;                                                       \
    }                                                                    \
    NdpOptions opts;                                                     \
    ndp_options_init(&opts);                                             \
    opts.format = (FMT_ENUM);                                            \
    ndp_read_options(vm, 1, &opts);                                      \
    char errbuf[256];                                                    \
    errbuf[0] = '\0';                                                    \
    if (ndp_decode(vm, nova_str_data(nova_as_string(text_val)),                         \
                   nova_str_len(nova_as_string(text_val)), &opts, errbuf) != 0) {    \
        nova_vm_raise_error(vm, FMT_NAME ".decode: %s", errbuf);         \
        return -1;                                                       \
    }                                                                    \
    return 1;                                                            \
}

#define DEFINE_FORMAT_ENCODE(FNAME, FMT_ENUM, FMT_NAME)                  \
static int nova_##FNAME##_encode(NovaVM *vm) {                           \
    if (nova_lib_check_args(vm, 1) != 0) { return -1; }                 \
    NdpOptions opts;                                                     \
    ndp_options_init(&opts);                                             \
    opts.format = (FMT_ENUM);                                            \
    ndp_read_options(vm, 1, &opts);                                      \
    NdpBuf out;                                                          \
    ndp_buf_init(&out);                                                  \
    char errbuf[256];                                                    \
    errbuf[0] = '\0';                                                    \
    if (ndp_encode(vm, 0, &opts, &out, errbuf) != 0) {                  \
        ndp_buf_free(&out);                                              \
        nova_vm_raise_error(vm, FMT_NAME ".encode: %s", errbuf);         \
        return -1;                                                       \
    }                                                                    \
    nova_vm_push_string(vm, out.data ? out.data : "", out.len);          \
    ndp_buf_free(&out);                                                  \
    return 1;                                                            \
}

#define DEFINE_FORMAT_LOAD(FNAME, FMT_ENUM, FMT_NAME)                    \
static int nova_##FNAME##_load(NovaVM *vm) {                             \
    if (nova_lib_check_args(vm, 1) != 0) { return -1; }                 \
    const char *filename = nova_lib_check_string(vm, 0);                 \
    if (filename == NULL) { return -1; }                                 \
    NdpOptions opts;                                                     \
    ndp_options_init(&opts);                                             \
    opts.format = (FMT_ENUM);                                            \
    ndp_read_options(vm, 1, &opts);                                      \
    char errbuf[256];                                                    \
    errbuf[0] = '\0';                                                    \
    size_t content_len = 0;                                              \
    char *content = ndp_read_file(filename, &content_len,                \
                                  errbuf, sizeof(errbuf));               \
    if (content == NULL) {                                               \
        nova_vm_raise_error(vm, FMT_NAME ".load: %s", errbuf);           \
        return -1;                                                       \
    }                                                                    \
    int rc = ndp_decode(vm, content, content_len, &opts, errbuf);        \
    free(content);                                                       \
    if (rc != 0) {                                                       \
        nova_vm_raise_error(vm, FMT_NAME ".load: %s", errbuf);           \
        return -1;                                                       \
    }                                                                    \
    return 1;                                                            \
}

#define DEFINE_FORMAT_SAVE(FNAME, FMT_ENUM, FMT_NAME)                    \
static int nova_##FNAME##_save(NovaVM *vm) {                             \
    if (nova_lib_check_args(vm, 2) != 0) { return -1; }                 \
    const char *filename = nova_lib_check_string(vm, 0);                 \
    if (filename == NULL) { return -1; }                                 \
    NdpOptions opts;                                                     \
    ndp_options_init(&opts);                                             \
    opts.format = (FMT_ENUM);                                            \
    ndp_read_options(vm, 2, &opts);                                      \
    NdpBuf out;                                                          \
    ndp_buf_init(&out);                                                  \
    char errbuf[256];                                                    \
    errbuf[0] = '\0';                                                    \
    if (ndp_encode(vm, 1, &opts, &out, errbuf) != 0) {                  \
        ndp_buf_free(&out);                                              \
        nova_vm_raise_error(vm, FMT_NAME ".save: %s", errbuf);           \
        return -1;                                                       \
    }                                                                    \
    if (ndp_write_file(filename, out.data, out.len,                      \
                       errbuf, sizeof(errbuf)) != 0) {                   \
        ndp_buf_free(&out);                                              \
        nova_vm_raise_error(vm, FMT_NAME ".save: %s", errbuf);           \
        return -1;                                                       \
    }                                                                    \
    ndp_buf_free(&out);                                                  \
    nova_vm_push_bool(vm, 1);                                            \
    return 1;                                                            \
}

#define DEFINE_FORMAT_ALL(FNAME, FMT_ENUM, FMT_NAME) \
    DEFINE_FORMAT_DECODE(FNAME, FMT_ENUM, FMT_NAME)  \
    DEFINE_FORMAT_ENCODE(FNAME, FMT_ENUM, FMT_NAME)  \
    DEFINE_FORMAT_LOAD(FNAME, FMT_ENUM, FMT_NAME)    \
    DEFINE_FORMAT_SAVE(FNAME, FMT_ENUM, FMT_NAME)

/* ============================================================
 * INSTANTIATE PER-FORMAT FUNCTIONS
 * ============================================================ */

DEFINE_FORMAT_ALL(json, NDP_FORMAT_JSON, "json")
DEFINE_FORMAT_ALL(csv,  NDP_FORMAT_CSV,  "csv")
DEFINE_FORMAT_ALL(ini,  NDP_FORMAT_INI,  "ini")
DEFINE_FORMAT_ALL(toml, NDP_FORMAT_TOML, "toml")
DEFINE_FORMAT_ALL(html, NDP_FORMAT_HTML, "html")
DEFINE_FORMAT_ALL(yaml, NDP_FORMAT_YAML, "yaml")

/* NINI: manual wrappers (calls standalone nova_nini.c directly) */

static int novai_nini_lib_decode(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) { return -1; }
    NovaValue text_val = nova_vm_get(vm, 0);
    if (!nova_is_string(text_val)) {
        nova_vm_raise_error(vm, "nini.decode: argument 1 must be a string");
        return -1;
    }
    NiniOptions opts;
    nova_nini_options_init(&opts);
    char errbuf[256];
    errbuf[0] = '\0';
    if (nova_nini_decode(vm, nova_str_data(nova_as_string(text_val)),
                         nova_str_len(nova_as_string(text_val)),
                         &opts, errbuf, sizeof(errbuf)) != 0) {
        nova_vm_raise_error(vm, "nini.decode: %s", errbuf);
        return -1;
    }
    return 1;
}

static int novai_nini_lib_encode(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) { return -1; }
    NiniOptions opts;
    nova_nini_options_init(&opts);
    char *result = NULL;
    size_t result_len = 0;
    char errbuf[256];
    errbuf[0] = '\0';
    if (nova_nini_encode(vm, 0, &opts,
                         &result, &result_len,
                         errbuf, sizeof(errbuf)) != 0) {
        nova_vm_raise_error(vm, "nini.encode: %s", errbuf);
        return -1;
    }
    nova_vm_push_string(vm, result ? result : "", result_len);
    free(result);
    return 1;
}

static int novai_nini_lib_load(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) { return -1; }
    const char *filename = nova_lib_check_string(vm, 0);
    if (filename == NULL) { return -1; }
    NiniOptions opts;
    nova_nini_options_init(&opts);
    char errbuf[256];
    errbuf[0] = '\0';
    size_t content_len = 0;
    char *content = ndp_read_file(filename, &content_len,
                                  errbuf, sizeof(errbuf));
    if (content == NULL) {
        nova_vm_raise_error(vm, "nini.load: %s", errbuf);
        return -1;
    }
    int rc = nova_nini_decode(vm, content, content_len,
                              &opts, errbuf, sizeof(errbuf));
    free(content);
    if (rc != 0) {
        nova_vm_raise_error(vm, "nini.load: %s", errbuf);
        return -1;
    }
    return 1;
}

static int novai_nini_lib_save(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) { return -1; }
    const char *filename = nova_lib_check_string(vm, 0);
    if (filename == NULL) { return -1; }
    NiniOptions opts;
    nova_nini_options_init(&opts);
    char *result = NULL;
    size_t result_len = 0;
    char errbuf[256];
    errbuf[0] = '\0';
    if (nova_nini_encode(vm, 1, &opts,
                         &result, &result_len,
                         errbuf, sizeof(errbuf)) != 0) {
        nova_vm_raise_error(vm, "nini.save: %s", errbuf);
        return -1;
    }
    if (ndp_write_file(filename, result, result_len,
                       errbuf, sizeof(errbuf)) != 0) {
        free(result);
        nova_vm_raise_error(vm, "nini.save: %s", errbuf);
        return -1;
    }
    free(result);
    nova_vm_push_bool(vm, 1);
    return 1;
}

/* TSV: CSV codec with tab delimiter default */
static int nova_tsv_decode(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) { return -1; }
    NovaValue text_val = nova_vm_get(vm, 0);
    if (!nova_is_string(text_val)) {
        nova_vm_raise_error(vm, "tsv.decode: argument 1 must be a string");
        return -1;
    }
    NdpOptions opts;
    ndp_options_init(&opts);
    opts.format = NDP_FORMAT_CSV;
    opts.csv_delimiter = '\t';
    ndp_read_options(vm, 1, &opts);
    char errbuf[256];
    errbuf[0] = '\0';
    if (ndp_decode(vm, nova_str_data(nova_as_string(text_val)),
                   nova_str_len(nova_as_string(text_val)), &opts, errbuf) != 0) {
        nova_vm_raise_error(vm, "tsv.decode: %s", errbuf);
        return -1;
    }
    return 1;
}

static int nova_tsv_encode(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) { return -1; }
    NdpOptions opts;
    ndp_options_init(&opts);
    opts.format = NDP_FORMAT_CSV;
    opts.csv_delimiter = '\t';
    ndp_read_options(vm, 1, &opts);
    NdpBuf out;
    ndp_buf_init(&out);
    char errbuf[256];
    errbuf[0] = '\0';
    if (ndp_encode(vm, 0, &opts, &out, errbuf) != 0) {
        ndp_buf_free(&out);
        nova_vm_raise_error(vm, "tsv.encode: %s", errbuf);
        return -1;
    }
    nova_vm_push_string(vm, out.data ? out.data : "", out.len);
    ndp_buf_free(&out);
    return 1;
}

static int nova_tsv_load(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) { return -1; }
    const char *filename = nova_lib_check_string(vm, 0);
    if (filename == NULL) { return -1; }
    NdpOptions opts;
    ndp_options_init(&opts);
    opts.format = NDP_FORMAT_CSV;
    opts.csv_delimiter = '\t';
    ndp_read_options(vm, 1, &opts);
    char errbuf[256];
    errbuf[0] = '\0';
    size_t content_len = 0;
    char *content = ndp_read_file(filename, &content_len,
                                  errbuf, sizeof(errbuf));
    if (content == NULL) {
        nova_vm_raise_error(vm, "tsv.load: %s", errbuf);
        return -1;
    }
    int rc = ndp_decode(vm, content, content_len, &opts, errbuf);
    free(content);
    if (rc != 0) {
        nova_vm_raise_error(vm, "tsv.load: %s", errbuf);
        return -1;
    }
    return 1;
}

static int nova_tsv_save(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) { return -1; }
    const char *filename = nova_lib_check_string(vm, 0);
    if (filename == NULL) { return -1; }
    NdpOptions opts;
    ndp_options_init(&opts);
    opts.format = NDP_FORMAT_CSV;
    opts.csv_delimiter = '\t';
    ndp_read_options(vm, 2, &opts);
    NdpBuf out;
    ndp_buf_init(&out);
    char errbuf[256];
    errbuf[0] = '\0';
    if (ndp_encode(vm, 1, &opts, &out, errbuf) != 0) {
        ndp_buf_free(&out);
        nova_vm_raise_error(vm, "tsv.save: %s", errbuf);
        return -1;
    }
    if (ndp_write_file(filename, out.data, out.len,
                       errbuf, sizeof(errbuf)) != 0) {
        ndp_buf_free(&out);
        nova_vm_raise_error(vm, "tsv.save: %s", errbuf);
        return -1;
    }
    ndp_buf_free(&out);
    nova_vm_push_bool(vm, 1);
    return 1;
}

/* ============================================================
 * PER-FORMAT REGISTRATION TABLES
 * ============================================================ */

static const NovaLibReg nova_json_lib[] = {
    {"decode", nova_json_decode},
    {"encode", nova_json_encode},
    {"load",   nova_json_load},
    {"save",   nova_json_save},
    {NULL, NULL}
};

static const NovaLibReg nova_csv_lib[] = {
    {"decode", nova_csv_decode},
    {"encode", nova_csv_encode},
    {"load",   nova_csv_load},
    {"save",   nova_csv_save},
    {NULL, NULL}
};

static const NovaLibReg nova_tsv_lib[] = {
    {"decode", nova_tsv_decode},
    {"encode", nova_tsv_encode},
    {"load",   nova_tsv_load},
    {"save",   nova_tsv_save},
    {NULL, NULL}
};

static const NovaLibReg nova_ini_lib[] = {
    {"decode", nova_ini_decode},
    {"encode", nova_ini_encode},
    {"load",   nova_ini_load},
    {"save",   nova_ini_save},
    {NULL, NULL}
};

static const NovaLibReg nova_toml_lib[] = {
    {"decode", nova_toml_decode},
    {"encode", nova_toml_encode},
    {"load",   nova_toml_load},
    {"save",   nova_toml_save},
    {NULL, NULL}
};

static const NovaLibReg nova_html_lib[] = {
    {"decode", nova_html_decode},
    {"encode", nova_html_encode},
    {"load",   nova_html_load},
    {"save",   nova_html_save},
    {NULL, NULL}
};

static const NovaLibReg nova_yaml_lib[] = {
    {"decode", nova_yaml_decode},
    {"encode", nova_yaml_encode},
    {"load",   nova_yaml_load},
    {"save",   nova_yaml_save},
    {NULL, NULL}
};

static const NovaLibReg nova_nini_lib[] = {
    {"decode", novai_nini_lib_decode},
    {"encode", novai_nini_lib_encode},
    {"load",   novai_nini_lib_load},
    {"save",   novai_nini_lib_save},
    {NULL, NULL}
};

/* ============================================================
 * data.* META-MODULE (generic format-string API)
 *
 * Available when #import data is used. Keeps the old-style API
 * where format is passed as a string argument.
 * ============================================================ */

static int nova_data_decode(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) { return -1; }
    NovaValue text_val = nova_vm_get(vm, 0);
    if (!nova_is_string(text_val)) {
        nova_vm_raise_error(vm, "data.decode: argument 1 must be a string");
        return -1;
    }
    const char *fmt_str = nova_lib_check_string(vm, 1);
    if (fmt_str == NULL) { return -1; }
    NdpFormat fmt = ndp_format_from_name(fmt_str);
    if (fmt == NDP_FORMAT_UNKNOWN) {
        nova_vm_raise_error(vm, "data.decode: unknown format '%s'", fmt_str);
        return -1;
    }
    NdpOptions opts;
    ndp_options_init(&opts);
    opts.format = fmt;
    if (fmt == NDP_FORMAT_TSV) { opts.csv_delimiter = '\t'; }
    ndp_read_options(vm, 2, &opts);
    char errbuf[256];
    errbuf[0] = '\0';
    if (ndp_decode(vm, nova_str_data(nova_as_string(text_val)),
                   nova_str_len(nova_as_string(text_val)), &opts, errbuf) != 0) {
        nova_vm_raise_error(vm, "data.decode: %s", errbuf);
        return -1;
    }
    return 1;
}

static int nova_data_encode(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) { return -1; }
    const char *fmt_str = nova_lib_check_string(vm, 1);
    if (fmt_str == NULL) { return -1; }
    NdpFormat fmt = ndp_format_from_name(fmt_str);
    if (fmt == NDP_FORMAT_UNKNOWN) {
        nova_vm_raise_error(vm, "data.encode: unknown format '%s'", fmt_str);
        return -1;
    }
    NdpOptions opts;
    ndp_options_init(&opts);
    opts.format = fmt;
    if (fmt == NDP_FORMAT_TSV) { opts.csv_delimiter = '\t'; }
    ndp_read_options(vm, 2, &opts);
    NdpBuf out;
    ndp_buf_init(&out);
    char errbuf[256];
    errbuf[0] = '\0';
    if (ndp_encode(vm, 0, &opts, &out, errbuf) != 0) {
        ndp_buf_free(&out);
        nova_vm_raise_error(vm, "data.encode: %s", errbuf);
        return -1;
    }
    nova_vm_push_string(vm, out.data ? out.data : "", out.len);
    ndp_buf_free(&out);
    return 1;
}

static int nova_data_load(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) { return -1; }
    const char *filename = nova_lib_check_string(vm, 0);
    if (filename == NULL) { return -1; }
    const char *fmt_str = nova_lib_check_string(vm, 1);
    if (fmt_str == NULL) { return -1; }
    NdpFormat fmt = ndp_format_from_name(fmt_str);
    if (fmt == NDP_FORMAT_UNKNOWN) {
        nova_vm_raise_error(vm, "data.load: unknown format '%s'", fmt_str);
        return -1;
    }
    NdpOptions opts;
    ndp_options_init(&opts);
    opts.format = fmt;
    if (fmt == NDP_FORMAT_TSV) { opts.csv_delimiter = '\t'; }
    ndp_read_options(vm, 2, &opts);
    char errbuf[256];
    errbuf[0] = '\0';
    size_t content_len = 0;
    char *content = ndp_read_file(filename, &content_len,
                                  errbuf, sizeof(errbuf));
    if (content == NULL) {
        nova_vm_raise_error(vm, "data.load: %s", errbuf);
        return -1;
    }
    int rc = ndp_decode(vm, content, content_len, &opts, errbuf);
    free(content);
    if (rc != 0) {
        nova_vm_raise_error(vm, "data.load: %s", errbuf);
        return -1;
    }
    return 1;
}

static int nova_data_save(NovaVM *vm) {
    if (nova_lib_check_args(vm, 3) != 0) { return -1; }
    const char *filename = nova_lib_check_string(vm, 0);
    if (filename == NULL) { return -1; }
    const char *fmt_str = nova_lib_check_string(vm, 2);
    if (fmt_str == NULL) { return -1; }
    NdpFormat fmt = ndp_format_from_name(fmt_str);
    if (fmt == NDP_FORMAT_UNKNOWN) {
        nova_vm_raise_error(vm, "data.save: unknown format '%s'", fmt_str);
        return -1;
    }
    NdpOptions opts;
    ndp_options_init(&opts);
    opts.format = fmt;
    if (fmt == NDP_FORMAT_TSV) { opts.csv_delimiter = '\t'; }
    ndp_read_options(vm, 3, &opts);
    NdpBuf out;
    ndp_buf_init(&out);
    char errbuf[256];
    errbuf[0] = '\0';
    if (ndp_encode(vm, 1, &opts, &out, errbuf) != 0) {
        ndp_buf_free(&out);
        nova_vm_raise_error(vm, "data.save: %s", errbuf);
        return -1;
    }
    if (ndp_write_file(filename, out.data, out.len,
                       errbuf, sizeof(errbuf)) != 0) {
        ndp_buf_free(&out);
        nova_vm_raise_error(vm, "data.save: %s", errbuf);
        return -1;
    }
    ndp_buf_free(&out);
    nova_vm_push_bool(vm, 1);
    return 1;
}

static int nova_data_detect(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) { return -1; }
    NovaValue text_val = nova_vm_get(vm, 0);
    if (!nova_is_string(text_val)) {
        nova_vm_raise_error(vm, "data.detect: argument must be a string");
        return -1;
    }
    NdpFormat fmt = ndp_detect(nova_str_data(nova_as_string(text_val)),
                               nova_str_len(nova_as_string(text_val)));
    const char *name = ndp_format_name(fmt);
    nova_vm_push_string(vm, name, strlen(name));
    return 1;
}

static const NovaLibReg nova_data_lib[] = {
    {"decode",  nova_data_decode},
    {"encode",  nova_data_encode},
    {"load",    nova_data_load},
    {"save",    nova_data_save},
    {"detect",  nova_data_detect},
    {NULL,      NULL}
};

/* ============================================================
 * MODULE OPENERS
 * ============================================================ */

int nova_open_data_imports(NovaVM *vm, uint32_t import_flags) {
    if (vm == NULL) {
        return -1;
    }
    if (import_flags == 0) {
        return 0;
    }

    if (import_flags & NOVA_IMPORT_JSON) {
        nova_lib_register_module(vm, "json", nova_json_lib);
    }
    if (import_flags & NOVA_IMPORT_CSV) {
        nova_lib_register_module(vm, "csv", nova_csv_lib);
    }
    if (import_flags & NOVA_IMPORT_TSV) {
        nova_lib_register_module(vm, "tsv", nova_tsv_lib);
    }
    if (import_flags & NOVA_IMPORT_INI) {
        nova_lib_register_module(vm, "ini", nova_ini_lib);
    }
    if (import_flags & NOVA_IMPORT_TOML) {
        nova_lib_register_module(vm, "toml", nova_toml_lib);
    }
    if (import_flags & NOVA_IMPORT_HTML) {
        nova_lib_register_module(vm, "html", nova_html_lib);
    }
    if (import_flags & NOVA_IMPORT_YAML) {
        nova_lib_register_module(vm, "yaml", nova_yaml_lib);
    }
    if (import_flags & NOVA_IMPORT_NINI) {
        nova_lib_register_module(vm, "nini", nova_nini_lib);
    }
    if (import_flags & NOVA_IMPORT_NET) {
#ifndef NOVA_NO_NET
        (void)nova_open_net(vm);
#endif
    }
    if (import_flags & NOVA_IMPORT_SQL) {
        (void)nova_open_sql(vm);
    }
    if (import_flags & NOVA_IMPORT_DATA) {
        nova_lib_register_module(vm, "data", nova_data_lib);
    }

    return 0;
}

int nova_open_data(NovaVM *vm) {
    return nova_open_data_imports(vm, NOVA_IMPORT_ALL);
}
