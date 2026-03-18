/**
 * @file nova_lib_net.c
 * @brief Nova Language - Network I/O Library (libcurl backend)
 *
 * Provides HTTP/HTTPS networking as the "net" module, enabled via:
 *
 *   #import net
 *
 * Functions:
 *   net.get(url [, opts])           HTTP GET  → {status, body, headers}
 *   net.post(url, body [, opts])    HTTP POST → {status, body, headers}
 *   net.put(url, body [, opts])     HTTP PUT  → {status, body, headers}
 *   net.delete(url [, opts])        HTTP DELETE → {status, body, headers}
 *   net.patch(url, body [, opts])   HTTP PATCH → {status, body, headers}
 *   net.head(url [, opts])          HTTP HEAD → {status, headers}
 *   net.request(opts)               Generic request → {status, body, headers}
 *   net.url_encode(str)             URL-encode a string
 *   net.url_decode(str)             URL-decode a string
 *
 * Options table fields:
 *   headers  = {["Content-Type"] = "application/json", ...}
 *   timeout  = 30           (seconds, default 30)
 *   follow   = true         (follow redirects, default true)
 *   method   = "GET"        (for net.request)
 *   url      = "https://…"  (for net.request)
 *   body     = "..."        (for net.request)
 *   verbose  = false        (print debug info)
 *
 * Response table:
 *   status   = 200          (HTTP status code)
 *   body     = "..."        (response body string)
 *   headers  = {["content-type"] = "application/json", ...}
 *   url      = "https://…"  (effective URL after redirects)
 *   ok       = true         (status >= 200 and status < 300)
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
 *   - libcurl    (HTTP/HTTPS transport)
 *
 * THREAD SAFETY:
 *   Not thread-safe. Single VM instance per call.
 */

#ifndef NOVA_NO_NET

#include "nova/nova_lib.h"
#include "nova/nova_vm.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ============================================================
 * INTERNAL: Growable buffer for curl callbacks
 * ============================================================ */

typedef struct {
    char   *data;
    size_t  len;
    size_t  cap;
} NetBuf;

static void novai_netbuf_init(NetBuf *buf) {
    if (buf == NULL) { return; }
    buf->data = NULL;
    buf->len  = 0;
    buf->cap  = 0;
}

static void novai_netbuf_free(NetBuf *buf) {
    if (buf == NULL) { return; }
    free(buf->data);
    buf->data = NULL;
    buf->len  = 0;
    buf->cap  = 0;
}

static int novai_netbuf_append(NetBuf *buf, const char *data, size_t len) {
    if (buf == NULL || data == NULL || len == 0) { return 0; }
    size_t needed = buf->len + len + 1;
    if (needed > buf->cap) {
        size_t newcap = (buf->cap == 0) ? 4096 : buf->cap;
        while (newcap < needed) { newcap *= 2; }
        char *tmp = (char *)realloc(buf->data, newcap);
        if (tmp == NULL) { return -1; }
        buf->data = tmp;
        buf->cap  = newcap;
    }
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
    return 0;
}

/* ============================================================
 * INTERNAL: curl write callback
 * ============================================================ */

static size_t novai_curl_write_cb(char *ptr, size_t size, size_t nmemb,
                                  void *userdata) {
    size_t total = size * nmemb;
    NetBuf *buf = (NetBuf *)userdata;
    if (novai_netbuf_append(buf, ptr, total) != 0) {
        return 0; /* signal error to curl */
    }
    return total;
}

/* ============================================================
 * INTERNAL: curl header callback
 *
 * Parses "Key: Value\r\n" lines into a table.
 * ============================================================ */

typedef struct {
    NovaVM    *vm;
    NovaTable *headers;
} NetHeaderCtx;

static size_t novai_curl_header_cb(char *buffer, size_t size, size_t nmemb,
                                   void *userdata) {
    size_t total = size * nmemb;
    NetHeaderCtx *ctx = (NetHeaderCtx *)userdata;
    if (ctx == NULL || ctx->vm == NULL || ctx->headers == NULL) {
        return total;
    }

    /* Skip status lines and empty lines */
    if (total < 3 || buffer[0] == '\r' || buffer[0] == '\n') {
        return total;
    }
    if (memcmp(buffer, "HTTP/", 5) == 0) {
        return total;
    }

    /* Find the colon separator */
    const char *colon = (const char *)memchr(buffer, ':', total);
    if (colon == NULL) {
        return total;
    }

    size_t key_len = (size_t)(colon - buffer);
    const char *val_start = colon + 1;
    size_t val_total = total - key_len - 1;

    /* Trim leading whitespace from value */
    while (val_total > 0 && (*val_start == ' ' || *val_start == '\t')) {
        val_start++;
        val_total--;
    }
    /* Trim trailing \r\n */
    while (val_total > 0 && (val_start[val_total - 1] == '\r' ||
                             val_start[val_total - 1] == '\n')) {
        val_total--;
    }

    /* Lowercase the key (HTTP headers are case-insensitive) */
    char key_buf[256];
    size_t klen = key_len < sizeof(key_buf) - 1 ? key_len : sizeof(key_buf) - 1;
    for (size_t i = 0; i < klen; i++) {
        key_buf[i] = (char)tolower((unsigned char)buffer[i]);
    }
    key_buf[klen] = '\0';

    /* Set header in table */
    NovaString *key_str = nova_string_new(ctx->vm, key_buf, klen);
    if (key_str == NULL) { return total; }

    NovaValue val = nova_value_string(
        nova_string_new(ctx->vm, val_start, val_total));
    (void)nova_table_set_str(ctx->vm, ctx->headers, key_str, val);

    return total;
}

/* ============================================================
 * INTERNAL: Read options table
 * ============================================================ */

typedef struct {
    long    timeout;       /* seconds */
    int     follow;        /* follow redirects */
    int     verbose;       /* debug output */
    struct curl_slist *custom_headers;
} NetRequestOpts;

static void novai_net_opts_init(NetRequestOpts *opts) {
    if (opts == NULL) { return; }
    opts->timeout = 30;
    opts->follow  = 1;
    opts->verbose = 0;
    opts->custom_headers = NULL;
}

static void novai_net_opts_free(NetRequestOpts *opts) {
    if (opts == NULL) { return; }
    if (opts->custom_headers != NULL) {
        curl_slist_free_all(opts->custom_headers);
        opts->custom_headers = NULL;
    }
}

static void novai_net_read_opts(NovaVM *vm, int opts_idx,
                                NetRequestOpts *opts) {
    if (opts_idx >= nova_vm_get_top(vm)) { return; }
    NovaValue ov = nova_vm_get(vm, opts_idx);
    if (!nova_is_table(ov) || nova_as_table(ov) == NULL) { return; }

    NovaTable *t = nova_as_table(ov);
    uint32_t iter = 0;
    NovaValue hk, hv;
    while (nova_table_next(t, &iter, &hk, &hv)) {
        if (!nova_is_string(hk)) { continue; }
        const char *k = nova_str_data(nova_as_string(hk));

        if (strcmp(k, "timeout") == 0 && nova_is_integer(hv)) {
            opts->timeout = (long)nova_as_integer(hv);
        } else if (strcmp(k, "timeout") == 0 && nova_is_number(hv)) {
            opts->timeout = (long)nova_as_number(hv);
        } else if (strcmp(k, "follow") == 0 && nova_is_bool(hv)) {
            opts->follow = nova_as_bool(hv);
        } else if (strcmp(k, "verbose") == 0 && nova_is_bool(hv)) {
            opts->verbose = nova_as_bool(hv);
        } else if (strcmp(k, "headers") == 0 && nova_is_table(hv)) {
            /* Build curl header list from table */
            NovaTable *ht = nova_as_table(hv);
            if (ht == NULL) { continue; }
            uint32_t hiter = 0;
            NovaValue hhk, hhv;
            while (nova_table_next(ht, &hiter, &hhk, &hhv)) {
                if (!nova_is_string(hhk) || !nova_is_string(hhv)) {
                    continue;
                }
                const char *hname = nova_str_data(nova_as_string(hhk));
                const char *hval  = nova_str_data(nova_as_string(hhv));
                char hbuf[1024];
                (void)snprintf(hbuf, sizeof(hbuf), "%s: %s", hname, hval);
                opts->custom_headers =
                    curl_slist_append(opts->custom_headers, hbuf);
            }
        }
    }
}

/* ============================================================
 * INTERNAL: Build response table
 *
 * Pushes a table: {status=N, body="...", headers={...}, url="...", ok=bool}
 * ============================================================ */

static void novai_net_push_response(NovaVM *vm, CURL *curl,
                                    long status, NetBuf *body,
                                    NovaTable *headers) {
    /* Create response table */
    nova_vm_push_table(vm);
    int resp_idx = nova_vm_get_top(vm) - 1;

    /* status */
    nova_vm_push_integer(vm, (nova_int_t)status);
    nova_vm_set_field(vm, resp_idx, "status");

    /* body */
    if (body->data != NULL && body->len > 0) {
        nova_vm_push_string(vm, body->data, body->len);
    } else {
        nova_vm_push_string(vm, "", 0);
    }
    nova_vm_set_field(vm, resp_idx, "body");

    /* headers table */
    nova_vm_push_value(vm, nova_value_table(headers));
    nova_vm_set_field(vm, resp_idx, "headers");

    /* effective url */
    char *effective_url = NULL;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);
    if (effective_url != NULL) {
        nova_vm_push_string(vm, effective_url, strlen(effective_url));
    } else {
        nova_vm_push_string(vm, "", 0);
    }
    nova_vm_set_field(vm, resp_idx, "url");

    /* ok (2xx status) */
    nova_vm_push_bool(vm, (status >= 200 && status < 300) ? 1 : 0);
    nova_vm_set_field(vm, resp_idx, "ok");
}

/* ============================================================
 * INTERNAL: Core request executor
 * ============================================================ */

static int novai_net_perform(NovaVM *vm, const char *url,
                             const char *method,
                             const char *req_body, size_t body_len,
                             NetRequestOpts *opts) {
    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        nova_vm_raise_error(vm, "net: failed to initialize libcurl");
        return -1;
    }

    /* URL */
    curl_easy_setopt(curl, CURLOPT_URL, url);

    /* Method */
    if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        if (req_body != NULL && body_len > 0) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req_body);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
        } else {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
        }
    } else if (strcmp(method, "PUT") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        if (req_body != NULL && body_len > 0) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req_body);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
        }
    } else if (strcmp(method, "PATCH") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
        if (req_body != NULL && body_len > 0) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req_body);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
        }
    } else if (strcmp(method, "DELETE") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    } else if (strcmp(method, "HEAD") == 0) {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    }
    /* GET is the default */

    /* Options */
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, opts->timeout);
    if (opts->follow) {
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    }
    if (opts->verbose) {
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    }

    /* Custom headers */
    if (opts->custom_headers != NULL) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, opts->custom_headers);
    }

    /* SSL: accept system CA bundle */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    /* User-Agent */
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Nova/1.0 (libcurl)");

    /* Response body buffer */
    NetBuf resp_body;
    novai_netbuf_init(&resp_body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, novai_curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp_body);

    /* Response headers — push onto stack to root for GC */
    nova_vm_push_table(vm);
    int hdr_stack_idx = nova_vm_get_top(vm) - 1;
    NovaValue hdr_val = nova_vm_get(vm, hdr_stack_idx);
    NovaTable *resp_headers = nova_as_table(hdr_val);
    if (resp_headers == NULL) {
        novai_netbuf_free(&resp_body);
        curl_easy_cleanup(curl);
        nova_vm_raise_error(vm, "net: out of memory for headers table");
        return -1;
    }
    NetHeaderCtx hdr_ctx;
    hdr_ctx.vm = vm;
    hdr_ctx.headers = resp_headers;
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, novai_curl_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hdr_ctx);

    /* Perform the request */
    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        novai_netbuf_free(&resp_body);
        const char *err = curl_easy_strerror(res);
        curl_easy_cleanup(curl);
        nova_vm_raise_error(vm, "net: %s", err);
        return -1;
    }

    /* Get status code */
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);

    /* Pop the headers table from stack before pushing response */
    nova_vm_pop(vm, 1);

    /* Build response table */
    novai_net_push_response(vm, curl, http_status, &resp_body, resp_headers);

    /* Cleanup */
    novai_netbuf_free(&resp_body);
    curl_easy_cleanup(curl);

    return 1; /* one return value (the response table) */
}

/* ============================================================
 * PUBLIC API: net.get(url [, opts])
 * ============================================================ */

static int nova_net_get(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) { return -1; }
    const char *url = nova_lib_check_string(vm, 0);
    if (url == NULL) { return -1; }

    NetRequestOpts opts;
    novai_net_opts_init(&opts);
    novai_net_read_opts(vm, 1, &opts);

    int rc = novai_net_perform(vm, url, "GET", NULL, 0, &opts);
    novai_net_opts_free(&opts);
    return rc;
}

/* ============================================================
 * PUBLIC API: net.post(url, body [, opts])
 * ============================================================ */

static int nova_net_post(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) { return -1; }
    const char *url = nova_lib_check_string(vm, 0);
    if (url == NULL) { return -1; }

    NovaValue body_val = nova_vm_get(vm, 1);
    const char *body = "";
    size_t body_len = 0;
    if (nova_is_string(body_val)) {
        body = nova_str_data(nova_as_string(body_val));
        body_len = nova_str_len(nova_as_string(body_val));
    }

    NetRequestOpts opts;
    novai_net_opts_init(&opts);
    novai_net_read_opts(vm, 2, &opts);

    int rc = novai_net_perform(vm, url, "POST", body, body_len, &opts);
    novai_net_opts_free(&opts);
    return rc;
}

/* ============================================================
 * PUBLIC API: net.put(url, body [, opts])
 * ============================================================ */

static int nova_net_put(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) { return -1; }
    const char *url = nova_lib_check_string(vm, 0);
    if (url == NULL) { return -1; }

    NovaValue body_val = nova_vm_get(vm, 1);
    const char *body = "";
    size_t body_len = 0;
    if (nova_is_string(body_val)) {
        body = nova_str_data(nova_as_string(body_val));
        body_len = nova_str_len(nova_as_string(body_val));
    }

    NetRequestOpts opts;
    novai_net_opts_init(&opts);
    novai_net_read_opts(vm, 2, &opts);

    int rc = novai_net_perform(vm, url, "PUT", body, body_len, &opts);
    novai_net_opts_free(&opts);
    return rc;
}

/* ============================================================
 * PUBLIC API: net.delete(url [, opts])
 * ============================================================ */

static int nova_net_delete(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) { return -1; }
    const char *url = nova_lib_check_string(vm, 0);
    if (url == NULL) { return -1; }

    NetRequestOpts opts;
    novai_net_opts_init(&opts);
    novai_net_read_opts(vm, 1, &opts);

    int rc = novai_net_perform(vm, url, "DELETE", NULL, 0, &opts);
    novai_net_opts_free(&opts);
    return rc;
}

/* ============================================================
 * PUBLIC API: net.patch(url, body [, opts])
 * ============================================================ */

static int nova_net_patch(NovaVM *vm) {
    if (nova_lib_check_args(vm, 2) != 0) { return -1; }
    const char *url = nova_lib_check_string(vm, 0);
    if (url == NULL) { return -1; }

    NovaValue body_val = nova_vm_get(vm, 1);
    const char *body = "";
    size_t body_len = 0;
    if (nova_is_string(body_val)) {
        body = nova_str_data(nova_as_string(body_val));
        body_len = nova_str_len(nova_as_string(body_val));
    }

    NetRequestOpts opts;
    novai_net_opts_init(&opts);
    novai_net_read_opts(vm, 2, &opts);

    int rc = novai_net_perform(vm, url, "PATCH", body, body_len, &opts);
    novai_net_opts_free(&opts);
    return rc;
}

/* ============================================================
 * PUBLIC API: net.head(url [, opts])
 * ============================================================ */

static int nova_net_head(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) { return -1; }
    const char *url = nova_lib_check_string(vm, 0);
    if (url == NULL) { return -1; }

    NetRequestOpts opts;
    novai_net_opts_init(&opts);
    novai_net_read_opts(vm, 1, &opts);

    int rc = novai_net_perform(vm, url, "HEAD", NULL, 0, &opts);
    novai_net_opts_free(&opts);
    return rc;
}

/* ============================================================
 * PUBLIC API: net.request(opts_table)
 *
 * Generic request where method, url, body are all in the
 * options table:
 *   net.request({
 *     method  = "POST",
 *     url     = "https://api.example.com/data",
 *     body    = json.encode(payload),
 *     headers = {["Content-Type"] = "application/json"},
 *     timeout = 10,
 *   })
 * ============================================================ */

static int nova_net_request(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) { return -1; }
    NovaValue opts_val = nova_vm_get(vm, 0);
    if (!nova_is_table(opts_val) || nova_as_table(opts_val) == NULL) {
        nova_vm_raise_error(vm, "net.request: argument must be a table");
        return -1;
    }

    NovaTable *t = nova_as_table(opts_val);

    /* Extract method */
    const char *method = "GET";
    NovaString *method_key = nova_string_new(vm, "method", 6);
    if (method_key != NULL) {
        NovaValue mv = nova_table_get_str(t, method_key);
        if (nova_is_string(mv)) {
            method = nova_str_data(nova_as_string(mv));
        }
    }

    /* Extract url */
    const char *url = NULL;
    NovaString *url_key = nova_string_new(vm, "url", 3);
    if (url_key != NULL) {
        NovaValue uv = nova_table_get_str(t, url_key);
        if (nova_is_string(uv)) {
            url = nova_str_data(nova_as_string(uv));
        }
    }
    if (url == NULL) {
        nova_vm_raise_error(vm, "net.request: 'url' field is required");
        return -1;
    }

    /* Extract body */
    const char *body = NULL;
    size_t body_len = 0;
    NovaString *body_key = nova_string_new(vm, "body", 4);
    if (body_key != NULL) {
        NovaValue bv = nova_table_get_str(t, body_key);
        if (nova_is_string(bv)) {
            body = nova_str_data(nova_as_string(bv));
            body_len = nova_str_len(nova_as_string(bv));
        }
    }

    /* Read remaining options */
    NetRequestOpts opts;
    novai_net_opts_init(&opts);
    novai_net_read_opts(vm, 0, &opts);

    int rc = novai_net_perform(vm, url, method, body, body_len, &opts);
    novai_net_opts_free(&opts);
    return rc;
}

/* ============================================================
 * PUBLIC API: net.url_encode(str)
 * ============================================================ */

static int nova_net_url_encode(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) { return -1; }
    const char *str = nova_lib_check_string(vm, 0);
    if (str == NULL) { return -1; }

    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        nova_vm_raise_error(vm, "net.url_encode: failed to init curl");
        return -1;
    }

    char *encoded = curl_easy_escape(curl, str, 0);
    if (encoded == NULL) {
        curl_easy_cleanup(curl);
        nova_vm_raise_error(vm, "net.url_encode: encoding failed");
        return -1;
    }

    nova_vm_push_string(vm, encoded, strlen(encoded));
    curl_free(encoded);
    curl_easy_cleanup(curl);
    return 1;
}

/* ============================================================
 * PUBLIC API: net.url_decode(str)
 * ============================================================ */

static int nova_net_url_decode(NovaVM *vm) {
    if (nova_lib_check_args(vm, 1) != 0) { return -1; }
    const char *str = nova_lib_check_string(vm, 0);
    if (str == NULL) { return -1; }

    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        nova_vm_raise_error(vm, "net.url_decode: failed to init curl");
        return -1;
    }

    int out_len = 0;
    char *decoded = curl_easy_unescape(curl, str, 0, &out_len);
    if (decoded == NULL) {
        curl_easy_cleanup(curl);
        nova_vm_raise_error(vm, "net.url_decode: decoding failed");
        return -1;
    }

    nova_vm_push_string(vm, decoded, (size_t)out_len);
    curl_free(decoded);
    curl_easy_cleanup(curl);
    return 1;
}

/* ============================================================
 * MODULE REGISTRATION
 * ============================================================ */

static const NovaLibReg nova_net_lib[] = {
    {"get",        nova_net_get},
    {"post",       nova_net_post},
    {"put",        nova_net_put},
    {"delete",     nova_net_delete},
    {"patch",      nova_net_patch},
    {"head",       nova_net_head},
    {"request",    nova_net_request},
    {"url_encode", nova_net_url_encode},
    {"url_decode", nova_net_url_decode},
    {NULL,         NULL}
};

/* ============================================================
 * MODULE OPENER
 * ============================================================ */

/**
 * @brief Open the net library module.
 *
 * Registers the "net" module table with all HTTP functions.
 * Also initializes libcurl globally if not already done.
 *
 * @param vm  VM instance
 * @return 0 on success, -1 on failure
 */
int nova_open_net(NovaVM *vm) {
    if (vm == NULL) { return -1; }

    /* Global curl init (safe to call multiple times) */
    static int curl_initialized = 0;
    if (!curl_initialized) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            return -1;
        }
        curl_initialized = 1;
    }

    nova_lib_register_module(vm, "net", nova_net_lib);
    return 0;
}

#else /* NOVA_NO_NET */

#include "nova/nova_lib.h"
#include "nova/nova_vm.h"

int nova_open_net(NovaVM *vm) {
    (void)vm;
    return -1;
}

#endif /* NOVA_NO_NET */
