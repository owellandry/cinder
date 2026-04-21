/*
 * cinder — CommonJS require() implementation
 * Supports: relative paths, node_modules lookup, module caching
 */
#include "mod_require.h"
#include "mod_fs.h"
#include "mod_path.h"
#include "mod_process.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#  include <windows.h>
#  define PATH_SEP_CHAR '\\'
#else
#  include <unistd.h>
#  define PATH_SEP_CHAR '/'
#endif

/* ── Path utilities ──────────────────────────────────────────────────────────── */

static int path_is_relative(const char *id) {
    return id[0] == '.' && (id[1] == '/' || id[1] == '\\' ||
           (id[1] == '.' && (id[2] == '/' || id[2] == '\\')));
}

static int path_is_absolute(const char *id) {
    return id[0] == '/' || id[0] == '\\' || (id[0] && id[1] == ':');
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG;
}

/* Resolve "id" relative to "base_dir" — tries .js extension, index.js */
static int resolve_file(const char *base_dir, const char *id,
                        char *out, size_t out_size) {
    char candidate[4096];

    /* 1. Try exact path */
    snprintf(candidate, sizeof(candidate), "%s/%s", base_dir, id);
    /* normalize */
    for (char *p = candidate; *p; p++) if (*p == '\\') *p = '/';
    if (file_exists(candidate)) {
        snprintf(out, out_size, "%s", candidate);
        return 0;
    }

    /* 2. Try with .js extension */
    snprintf(candidate, sizeof(candidate), "%s/%s.js", base_dir, id);
    for (char *p = candidate; *p; p++) if (*p == '\\') *p = '/';
    if (file_exists(candidate)) {
        snprintf(out, out_size, "%s", candidate);
        return 0;
    }

    /* 3. Try as directory + /index.js */
    snprintf(candidate, sizeof(candidate), "%s/%s/index.js", base_dir, id);
    for (char *p = candidate; *p; p++) if (*p == '\\') *p = '/';
    if (file_exists(candidate)) {
        snprintf(out, out_size, "%s", candidate);
        return 0;
    }

    /* 4. Try as directory + /package.json main field */
    {
        char pkg_path[4096];
        snprintf(pkg_path, sizeof(pkg_path), "%s/%s/package.json", base_dir, id);
        for (char *p = pkg_path; *p; p++) if (*p == '\\') *p = '/';

        FILE *f = fopen(pkg_path, "r");
        if (f) {
            char buf[8192]; size_t n = fread(buf, 1, sizeof(buf)-1, f);
            buf[n] = '\0'; fclose(f);

            /* Simple "main" field extraction without full JSON parser */
            const char *main_key = strstr(buf, "\"main\"");
            if (main_key) {
                const char *colon = strchr(main_key + 6, ':');
                if (colon) {
                    while (*colon == ':' || *colon == ' ' || *colon == '"') colon++;
                    char main_val[512]; size_t i = 0;
                    while (*colon && *colon != '"' && i < sizeof(main_val)-1)
                        main_val[i++] = *colon++;
                    main_val[i] = '\0';

                    snprintf(candidate, sizeof(candidate), "%s/%s/%s",
                             base_dir, id, main_val);
                    for (char *p = candidate; *p; p++) if (*p == '\\') *p = '/';
                    if (file_exists(candidate)) {
                        snprintf(out, out_size, "%s", candidate);
                        return 0;
                    }
                    /* try adding .js */
                    strncat(candidate, ".js", sizeof(candidate) - strlen(candidate) - 1);
                    if (file_exists(candidate)) {
                        snprintf(out, out_size, "%s", candidate);
                        return 0;
                    }
                }
            }
        }
    }

    return -1;
}

/* Find module in node_modules, walking up directories */
static int resolve_node_modules(const char *start_dir, const char *id,
                                char *out, size_t out_size) {
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s", start_dir);
    for (char *p = dir; *p; p++) if (*p == '\\') *p = '/';

    /* Walk up the directory tree */
    while (1) {
        char nm[4096];
        snprintf(nm, sizeof(nm), "%s/node_modules", dir);

        if (resolve_file(nm, id, out, out_size) == 0) return 0;

        /* Go up one level */
        char *slash = strrchr(dir, '/');
        if (!slash || slash == dir) break;
        *slash = '\0';
    }

    return -1;
}

/* ── Module cache (simple linked list) ──────────────────────────────────────── */

typedef struct ModuleCache {
    char *resolved_path;
    JSValue exports;
    struct ModuleCache *next;
} ModuleCache;

static ModuleCache *g_cache_head = NULL;

static JSValue cache_get(JSContext *ctx, const char *path) {
    for (ModuleCache *e = g_cache_head; e; e = e->next) {
        if (strcmp(e->resolved_path, path) == 0)
            return JS_DupValue(ctx, e->exports);
    }
    return JS_UNDEFINED;
}

static void cache_set(JSContext *ctx, const char *path, JSValue exports) {
    ModuleCache *e = (ModuleCache *)calloc(1, sizeof(ModuleCache));
    if (!e) return;
    e->resolved_path = strdup(path);
    e->exports = JS_DupValue(ctx, exports);
    e->next = g_cache_head;
    g_cache_head = e;
}

/* ── Read a file to string ───────────────────────────────────────────────────── */

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0'; fclose(f);
    return buf;
}

/* ── require() implementation ────────────────────────────────────────────────── */

/* The require function holds the current file's directory as magic data */
typedef struct {
    char current_dir[4096];
} RequireData;

static JSValue js_require_impl(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic,
                               JSValue *func_data) {
    (void)this_val; (void)magic;
    if (argc < 1) return JS_ThrowTypeError(ctx, "require: id required");

    const char *id = JS_ToCString(ctx, argv[0]);
    if (!id) return JS_EXCEPTION;

    /* -- Get current directory from closure data -- */
    const char *cur_dir = JS_ToCString(ctx, func_data[0]);
    if (!cur_dir) { JS_FreeCString(ctx, id); return JS_EXCEPTION; }

    /* -- Resolve absolute path -- */
    char resolved[4096] = {0};
    int found = 0;

    if (path_is_absolute(id)) {
        snprintf(resolved, sizeof(resolved), "%s", id);
        found = file_exists(resolved);
    } else if (path_is_relative(id)) {
        found = (resolve_file(cur_dir, id, resolved, sizeof(resolved)) == 0);
    } else {
        /* node_modules lookup */
        found = (resolve_node_modules(cur_dir, id, resolved, sizeof(resolved)) == 0);
    }

    if (!found) {
        JSValue err = JS_ThrowTypeError(ctx,
            "Cannot find module '%s' (from '%s')", id, cur_dir);
        JS_FreeCString(ctx, id);
        JS_FreeCString(ctx, cur_dir);
        return err;
    }

    JS_FreeCString(ctx, id);
    JS_FreeCString(ctx, cur_dir);

    /* -- Check cache -- */
    JSValue cached = cache_get(ctx, resolved);
    if (!JS_IsUndefined(cached)) return cached;

    /* -- Read source -- */
    char *src = slurp(resolved);
    if (!src) return JS_ThrowTypeError(ctx, "Cannot read module '%s'", resolved);

    /* -- Build dirname and filename strings -- */
    char file_dir[4096];
    snprintf(file_dir, sizeof(file_dir), "%s", resolved);
    char *last_sep = strrchr(file_dir, '/');
    if (!last_sep) last_sep = strrchr(file_dir, '\\');
    if (last_sep) *last_sep = '\0';
    else snprintf(file_dir, sizeof(file_dir), ".");

    /* -- Create module object -- */
    JSValue module_obj  = JS_NewObject(ctx);
    JSValue exports_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, module_obj, "exports", JS_DupValue(ctx, exports_obj));
    JS_SetPropertyStr(ctx, module_obj, "filename", JS_NewString(ctx, resolved));
    JS_SetPropertyStr(ctx, module_obj, "loaded", JS_FALSE);

    /* -- Build a require() bound to the new file's directory -- */
    JSValue dir_str = JS_NewString(ctx, file_dir);
    JSValue child_require = JS_NewCFunctionData(ctx, js_require_impl, 1, 0, 1, &dir_str);
    JS_FreeValue(ctx, dir_str);

    /* -- Wrap source in CommonJS wrapper -- */
    /* (function(exports, require, module, __filename, __dirname) { <src> \n}) */
    size_t src_len = strlen(src);
    size_t wrap_len = src_len + 256;
    char *wrapped = (char *)malloc(wrap_len);
    if (!wrapped) { free(src); return JS_ThrowOutOfMemory(ctx); }
    snprintf(wrapped, wrap_len,
             "(function(exports,require,module,__filename,__dirname){\n%s\n})",
             src);
    free(src);

    /* -- Evaluate wrapper -- */
    JSValue wrapper_fn = JS_Eval(ctx, wrapped, strlen(wrapped), resolved,
                                 JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT);
    free(wrapped);

    if (JS_IsException(wrapper_fn)) {
        JS_FreeValue(ctx, module_obj);
        JS_FreeValue(ctx, exports_obj);
        JS_FreeValue(ctx, child_require);
        return JS_EXCEPTION;
    }

    /* -- Call the wrapper -- */
    JSValue fn_filename = JS_NewString(ctx, resolved);
    JSValue fn_dirname  = JS_NewString(ctx, file_dir);
    JSValue args[5] = {
        JS_DupValue(ctx, exports_obj),  /* exports (dup so loop-free is safe) */
        JS_DupValue(ctx, child_require),/* require  */
        JS_DupValue(ctx, module_obj),   /* module   */
        fn_filename,
        fn_dirname,
    };
    JSValue result = JS_Call(ctx, wrapper_fn, JS_UNDEFINED, 5, args);

    for (int i = 0; i < 5; i++) JS_FreeValue(ctx, args[i]);
    JS_FreeValue(ctx, wrapper_fn);
    JS_FreeValue(ctx, child_require);

    if (JS_IsException(result)) {
        JS_FreeValue(ctx, module_obj);
        JS_FreeValue(ctx, exports_obj);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, result);

    /* -- Get final module.exports (may have been replaced by assignment) -- */
    JSValue final_exports = JS_GetPropertyStr(ctx, module_obj, "exports");
    JS_SetPropertyStr(ctx, module_obj, "loaded", JS_TRUE);
    JS_FreeValue(ctx, module_obj);
    JS_FreeValue(ctx, exports_obj);

    /* -- Cache and return -- */
    cache_set(ctx, resolved, final_exports);
    return final_exports;
}

/* ── Public: register require() global ──────────────────────────────────────── */

/* Free all cached module entries (call before JS_FreeContext) */
void js_require_cleanup(JSContext *ctx) {
    ModuleCache *e = g_cache_head;
    while (e) {
        ModuleCache *next = e->next;
        JS_FreeValue(ctx, e->exports);
        free(e->resolved_path);
        free(e);
        e = next;
    }
    g_cache_head = NULL;
}

void js_init_require(JSContext *ctx, const char *entry_file_path) {
    /* Compute entry directory */
    char entry_dir[4096] = {0};
    if (entry_file_path) {
        snprintf(entry_dir, sizeof(entry_dir), "%s", entry_file_path);
        for (char *p = entry_dir; *p; p++) if (*p == '\\') *p = '/';
        char *last = strrchr(entry_dir, '/');
        if (last) *last = '\0';
        else {
            /* relative path with no directory component — use cwd */
#ifdef _WIN32
            GetCurrentDirectoryA(sizeof(entry_dir), entry_dir);
#else
            getcwd(entry_dir, sizeof(entry_dir));
#endif
        }
    } else {
#ifdef _WIN32
        GetCurrentDirectoryA(sizeof(entry_dir), entry_dir);
#else
        getcwd(entry_dir, sizeof(entry_dir));
#endif
    }

    /* Normalize to forward slashes */
    for (char *p = entry_dir; *p; p++) if (*p == '\\') *p = '/';

    JSValue global = JS_GetGlobalObject(ctx);

    /* Create require() bound to entry directory */
    JSValue dir_str = JS_NewString(ctx, entry_dir);
    JSValue require_fn = JS_NewCFunctionData(ctx, js_require_impl, 1, 0, 1, &dir_str);
    JS_FreeValue(ctx, dir_str);

    JS_SetPropertyStr(ctx, global, "require", require_fn);

    /* Also set __dirname and __filename for top-level scripts */
    JS_SetPropertyStr(ctx, global, "__dirname",
                      JS_NewString(ctx, entry_dir));
    if (entry_file_path) {
        char abs_path[4096];
        snprintf(abs_path, sizeof(abs_path), "%s", entry_file_path);
        for (char *p = abs_path; *p; p++) if (*p == '\\') *p = '/';
        JS_SetPropertyStr(ctx, global, "__filename",
                          JS_NewString(ctx, abs_path));
    }

    JS_FreeValue(ctx, global);
}
