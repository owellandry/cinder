#include "runtime.h"
#include "modules/mod_fs.h"
#include "modules/mod_path.h"
#include "modules/mod_process.h"
#include "modules/mod_require.h"

#include "quickjs-libc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Module loader ─────────────────────────────────────────────────────────── */

static JSModuleDef *cinder_module_loader(JSContext *ctx,
                                          const char *module_name,
                                          void *opaque,
                                          JSValueConst attributes) {
    (void)opaque;

    /* Built-in native modules */
    if (strcmp(module_name, "fs") == 0)      return js_init_module_fs(ctx, module_name);
    if (strcmp(module_name, "node:fs") == 0) return js_init_module_fs(ctx, module_name);

    if (strcmp(module_name, "path") == 0)      return js_init_module_path(ctx, module_name);
    if (strcmp(module_name, "node:path") == 0) return js_init_module_path(ctx, module_name);

    /* Fall back to QuickJS file/std loader for relative imports */
    return js_module_loader(ctx, module_name, opaque, attributes);
}

/* ── Runtime creation ──────────────────────────────────────────────────────── */

CinderRuntime *cinder_runtime_new(int argc, char *argv[]) {
    CinderRuntime *cr = (CinderRuntime *)calloc(1, sizeof(CinderRuntime));
    if (!cr) return NULL;

    cr->rt = JS_NewRuntime();
    if (!cr->rt) { free(cr); return NULL; }

    /* Memory limit: 512 MB */
    JS_SetMemoryLimit(cr->rt, 512 * 1024 * 1024);
    JS_SetMaxStackSize(cr->rt, 8 * 1024 * 1024);

    cr->ctx = JS_NewContext(cr->rt);
    if (!cr->ctx) {
        JS_FreeRuntime(cr->rt);
        free(cr);
        return NULL;
    }

    /* Standard QuickJS library (setTimeout, setInterval, std, os) */
    js_std_add_helpers(cr->ctx, argc, argv);

    /* Register module loader (new API with import attributes) */
    JS_SetModuleLoaderFunc2(cr->rt, NULL, cinder_module_loader, NULL, NULL);

    /* Register process built-in (global object, not a module) */
    js_init_process(cr->ctx, argc, argv);

    /* require() is registered per-execution in cinder_runtime_exec_file */

    return cr;
}

/* ── Helpers ───────────────────────────────────────────────────────────────── */

static void cinder_dump_error(JSContext *ctx) {
    JSValue exception = JS_GetException(ctx);
    const char *str;
    JSValue stack;
    int is_error = JS_IsError(ctx, exception);

    str = JS_ToCString(ctx, exception);
    if (str) {
        fprintf(stderr, "%s\n", str);
        JS_FreeCString(ctx, str);
    }

    if (is_error) {
        stack = JS_GetPropertyStr(ctx, exception, "stack");
        if (!JS_IsUndefined(stack)) {
            str = JS_ToCString(ctx, stack);
            if (str) {
                fprintf(stderr, "%s\n", str);
                JS_FreeCString(ctx, str);
            }
        }
        JS_FreeValue(ctx, stack);
    }

    JS_FreeValue(ctx, exception);
}

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = (char *)malloc(size + 1);
    if (!buf) { fclose(f); return NULL; }

    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[size] = '\0';
    fclose(f);

    if (out_len) *out_len = (size_t)size;
    return buf;
}

/* ── Execute a file ────────────────────────────────────────────────────────── */

int cinder_runtime_exec_file(CinderRuntime *cr, const char *path) {
    size_t src_len = 0;
    char *src = read_file(path, &src_len);
    if (!src) {
        fprintf(stderr, "cinder: cannot read file '%s'\n", path);
        return 1;
    }

    /* Register require() bound to this file's directory */
    js_init_require(cr->ctx, path);

    int ret = cinder_runtime_exec_str(cr, src, path);
    free(src);
    return ret;
}

/* ── Execute a string ──────────────────────────────────────────────────────── */

int cinder_runtime_exec_str(CinderRuntime *cr, const char *src, const char *filename) {
    JSValue val;

    /* Detect ESM: if file has import/export at top level, use module mode */
    int is_module = (strstr(src, "import ") != NULL ||
                     strstr(src, "export ") != NULL ||
                     strstr(src, "import(") != NULL);

    int eval_flags = is_module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL;
    eval_flags |= JS_EVAL_FLAG_STRICT;

    val = JS_Eval(cr->ctx, src, strlen(src), filename, eval_flags);

    if (JS_IsException(val)) {
        cinder_dump_error(cr->ctx);
        JS_FreeValue(cr->ctx, val);
        return 1;
    }

    JS_FreeValue(cr->ctx, val);

    /* Drain pending Promise/async jobs without blocking on I/O */
    JSContext *loop_ctx;
    int err;
    while ((err = JS_ExecutePendingJob(cr->rt, &loop_ctx)) > 0) { }
    if (err < 0) {
        cinder_dump_error(loop_ctx);
        return 1;
    }

    return 0;
}

/* ── Free ──────────────────────────────────────────────────────────────────── */

void cinder_runtime_free(CinderRuntime *cr) {
    if (!cr) return;
    js_require_cleanup(cr->ctx);   /* free module cache before context teardown */
    /* js_std_free_handlers can crash on Windows MinGW builds;
     * skip it since we're about to exit anyway */
    JS_FreeContext(cr->ctx);
    JS_FreeRuntime(cr->rt);
    free(cr);
}
