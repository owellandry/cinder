/*
 * cinder — path module
 * Node.js-compatible path utilities
 */
#include "mod_path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#  define PATH_SEP '\\'
#  define PATH_SEP_STR "\\"
#else
#  include <unistd.h>
#  define PATH_SEP '/'
#  define PATH_SEP_STR "/"
#endif

/* ── Helpers ───────────────────────────────────────────────────────────────── */

static int is_sep(char c) { return c == '/' || c == '\\'; }

/* Normalize path separators to '/' (POSIX-style output) */
static void normalize_seps(char *p) {
    for (; *p; p++) if (*p == '\\') *p = '/';
}

/* ── path.join(...args) ─────────────────────────────────────────────────────── */

static JSValue js_path_join(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv) {
    (void)this_val;
    char result[4096] = {0};
    int pos = 0;

    for (int i = 0; i < argc; i++) {
        const char *seg = JS_ToCString(ctx, argv[i]);
        if (!seg) return JS_EXCEPTION;

        if (i > 0 && pos > 0 && !is_sep(result[pos-1])) {
            result[pos++] = '/';
        }

        /* Skip leading slashes for non-first segments if already have content */
        const char *s = seg;
        if (i > 0) while (is_sep(*s)) s++;

        size_t len = strlen(s);
        if (pos + (int)len >= (int)sizeof(result) - 1) {
            JS_FreeCString(ctx, seg);
            return JS_ThrowRangeError(ctx, "path too long");
        }
        memcpy(result + pos, s, len);
        pos += (int)len;
        JS_FreeCString(ctx, seg);
    }
    result[pos] = '\0';

    /* Remove trailing slash unless it's root */
    while (pos > 1 && is_sep(result[pos-1])) result[--pos] = '\0';

    return JS_NewString(ctx, result);
}

/* ── path.dirname(p) ────────────────────────────────────────────────────────── */

static JSValue js_path_dirname(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, ".");

    const char *p = JS_ToCString(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;

    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", p);
    JS_FreeCString(ctx, p);

    /* Remove trailing separators */
    int len = (int)strlen(buf);
    while (len > 1 && is_sep(buf[len-1])) buf[--len] = '\0';

    /* Find last separator */
    int last = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (is_sep(buf[i])) { last = i; break; }
    }

    if (last < 0) return JS_NewString(ctx, ".");
    if (last == 0) return JS_NewString(ctx, "/");
    buf[last] = '\0';
    return JS_NewString(ctx, buf);
}

/* ── path.basename(p[, ext]) ────────────────────────────────────────────────── */

static JSValue js_path_basename(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "");

    const char *p = JS_ToCString(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;

    /* Find last separator */
    const char *base = p;
    for (const char *s = p; *s; s++) {
        if (is_sep(*s)) base = s + 1;
    }

    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", base);
    JS_FreeCString(ctx, p);

    /* Strip extension if provided */
    if (argc >= 2) {
        const char *ext = JS_ToCString(ctx, argv[1]);
        if (ext) {
            size_t blen = strlen(buf);
            size_t elen = strlen(ext);
            if (blen > elen && strcmp(buf + blen - elen, ext) == 0) {
                buf[blen - elen] = '\0';
            }
            JS_FreeCString(ctx, ext);
        }
    }

    return JS_NewString(ctx, buf);
}

/* ── path.extname(p) ────────────────────────────────────────────────────────── */

static JSValue js_path_extname(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewString(ctx, "");

    const char *p = JS_ToCString(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;

    /* Get basename first */
    const char *base = p;
    for (const char *s = p; *s; s++) {
        if (is_sep(*s)) base = s + 1;
    }

    /* Find last dot after base */
    const char *dot = NULL;
    for (const char *s = base; *s; s++) {
        if (*s == '.') dot = s;
    }

    JSValue result = JS_NewString(ctx, dot ? dot : "");
    JS_FreeCString(ctx, p);
    return result;
}

/* ── path.resolve(...args) ──────────────────────────────────────────────────── */

static JSValue js_path_resolve(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    (void)this_val;
    char result[4096];

#ifdef _WIN32
    GetCurrentDirectoryA(sizeof(result), result);
    normalize_seps(result);
#else
    if (!getcwd(result, sizeof(result))) result[0] = '\0';
#endif

    for (int i = 0; i < argc; i++) {
        const char *seg = JS_ToCString(ctx, argv[i]);
        if (!seg) return JS_EXCEPTION;

        if (is_sep(seg[0]) || (seg[1] == ':' && is_sep(seg[2]))) {
            /* Absolute path — replace result */
            snprintf(result, sizeof(result), "%s", seg);
            normalize_seps(result);
        } else {
            /* Relative — append */
            size_t rlen = strlen(result);
            if (rlen > 0 && !is_sep(result[rlen-1])) {
                result[rlen++] = '/';
                result[rlen] = '\0';
            }
            strncat(result, seg, sizeof(result) - strlen(result) - 1);
            normalize_seps(result);
        }
        JS_FreeCString(ctx, seg);
    }

    return JS_NewString(ctx, result);
}

/* ── path.isAbsolute(p) ─────────────────────────────────────────────────────── */

static JSValue js_path_isAbsolute(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_FALSE;

    const char *p = JS_ToCString(ctx, argv[0]);
    if (!p) return JS_EXCEPTION;

    int abs = is_sep(p[0]) || (p[0] && p[1] == ':' && is_sep(p[2]));
    JS_FreeCString(ctx, p);
    return JS_NewBool(ctx, abs);
}

/* ── Module ──────────────────────────────────────────────────────────────────── */

static const JSCFunctionListEntry js_path_funcs[] = {
    JS_CFUNC_DEF("join",       0, js_path_join),
    JS_CFUNC_DEF("dirname",    1, js_path_dirname),
    JS_CFUNC_DEF("basename",   1, js_path_basename),
    JS_CFUNC_DEF("extname",    1, js_path_extname),
    JS_CFUNC_DEF("resolve",    0, js_path_resolve),
    JS_CFUNC_DEF("isAbsolute", 1, js_path_isAbsolute),
    JS_PROP_STRING_DEF("sep",  "/", JS_PROP_CONFIGURABLE),
};

static int js_path_init(JSContext *ctx, JSModuleDef *m) {
    return JS_SetModuleExportList(ctx, m, js_path_funcs,
                                  sizeof(js_path_funcs) / sizeof(js_path_funcs[0]));
}

JSModuleDef *js_init_module_path(JSContext *ctx, const char *module_name) {
    JSModuleDef *m = JS_NewCModule(ctx, module_name, js_path_init);
    if (!m) return NULL;
    JS_AddModuleExportList(ctx, m, js_path_funcs,
                           sizeof(js_path_funcs) / sizeof(js_path_funcs[0]));
    return m;
}
