/*
 * cinder — fs module
 * Provides Node.js-compatible filesystem APIs via QuickJS
 */
#include "mod_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#  include <io.h>
#  define mkdir(p, m) _mkdir(p)
#  define unlink(p)   _unlink(p)
#  define rename(o,n) MoveFileExA(o, n, MOVEFILE_REPLACE_EXISTING)
#else
#  include <unistd.h>
#  include <dirent.h>
#endif

/* ── Helpers ───────────────────────────────────────────────────────────────── */

static JSValue throw_errno(JSContext *ctx, const char *msg) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s: %s", msg, strerror(errno));
    return JS_ThrowTypeError(ctx, "%s", buf);
}

/* ── readFileSync(path[, encoding]) ─────────────────────────────────────────── */

static JSValue js_readFileSync(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "readFileSync: path required");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    const char *encoding = NULL;
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        if (JS_IsString(argv[1])) {
            encoding = JS_ToCString(ctx, argv[1]);
        } else {
            /* options object: { encoding: 'utf8' } */
            JSValue enc = JS_GetPropertyStr(ctx, argv[1], "encoding");
            if (JS_IsString(enc)) encoding = JS_ToCString(ctx, enc);
            JS_FreeValue(ctx, enc);
        }
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        JSValue err = throw_errno(ctx, path);
        JS_FreeCString(ctx, path);
        if (encoding) JS_FreeCString(ctx, encoding);
        return err;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = (char *)malloc(size + 1);
    if (!buf) { fclose(f); JS_FreeCString(ctx, path); return JS_ThrowOutOfMemory(ctx); }
    fread(buf, 1, (size_t)size, f);
    buf[size] = '\0';
    fclose(f);

    JSValue result;
    if (encoding && (strcasecmp(encoding, "utf8") == 0 ||
                     strcasecmp(encoding, "utf-8") == 0)) {
        result = JS_NewString(ctx, buf);
    } else if (!encoding) {
        /* Return Buffer-like Uint8Array */
        result = JS_NewArrayBufferCopy(ctx, (const uint8_t *)buf, (size_t)size);
    } else {
        result = JS_NewString(ctx, buf);
    }

    free(buf);
    JS_FreeCString(ctx, path);
    if (encoding) JS_FreeCString(ctx, encoding);
    return result;
}

/* ── writeFileSync(path, data[, options]) ──────────────────────────────────── */

static JSValue js_writeFileSync(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "writeFileSync: path and data required");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    FILE *f = fopen(path, "wb");
    if (!f) {
        JSValue err = throw_errno(ctx, path);
        JS_FreeCString(ctx, path);
        return err;
    }

    if (JS_IsString(argv[1])) {
        const char *data = JS_ToCString(ctx, argv[1]);
        if (data) {
            fwrite(data, 1, strlen(data), f);
            JS_FreeCString(ctx, data);
        }
    } else {
        /* ArrayBuffer / TypedArray */
        size_t len;
        uint8_t *buf = JS_GetArrayBuffer(ctx, &len, argv[1]);
        if (buf) fwrite(buf, 1, len, f);
    }

    fclose(f);
    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

/* ── appendFileSync(path, data) ────────────────────────────────────────────── */

static JSValue js_appendFileSync(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "appendFileSync: path and data required");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    FILE *f = fopen(path, "ab");
    if (!f) {
        JSValue err = throw_errno(ctx, path);
        JS_FreeCString(ctx, path);
        return err;
    }

    const char *data = JS_ToCString(ctx, argv[1]);
    if (data) {
        fwrite(data, 1, strlen(data), f);
        JS_FreeCString(ctx, data);
    }
    fclose(f);
    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

/* ── existsSync(path) ───────────────────────────────────────────────────────── */

static JSValue js_existsSync(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_FALSE;

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    struct stat st;
    int exists = (stat(path, &st) == 0);
    JS_FreeCString(ctx, path);
    return JS_NewBool(ctx, exists);
}

/* ── mkdirSync(path[, options]) ─────────────────────────────────────────────── */

static int mkdir_recursive(const char *path) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len > 0 && (tmp[len-1] == '/' || tmp[len-1] == '\\'))
        tmp[--len] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '\0';
            struct stat st;
            if (stat(tmp, &st) != 0) {
#ifdef _WIN32
                if (_mkdir(tmp) != 0 && errno != EEXIST) return -1;
#else
                if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
#endif
            }
            *p = '/';
        }
    }
#ifdef _WIN32
    return (_mkdir(tmp) == 0 || errno == EEXIST) ? 0 : -1;
#else
    return (mkdir(tmp, 0755) == 0 || errno == EEXIST) ? 0 : -1;
#endif
}

static JSValue js_mkdirSync(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "mkdirSync: path required");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    int recursive = 0;
    if (argc >= 2) {
        JSValue opt = JS_GetPropertyStr(ctx, argv[1], "recursive");
        if (JS_IsBool(opt)) recursive = JS_ToBool(ctx, opt);
        JS_FreeValue(ctx, opt);
    }

    int ret;
    if (recursive) {
        ret = mkdir_recursive(path);
    } else {
#ifdef _WIN32
        ret = _mkdir(path);
#else
        ret = mkdir(path, 0755);
#endif
    }

    if (ret != 0 && errno != EEXIST) {
        JSValue err = throw_errno(ctx, path);
        JS_FreeCString(ctx, path);
        return err;
    }

    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

/* ── readdirSync(path) ──────────────────────────────────────────────────────── */

static JSValue js_readdirSync(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "readdirSync: path required");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    JSValue arr = JS_NewArray(ctx);
    int idx = 0;

#ifdef _WIN32
    char pattern[4096];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    WIN32_FIND_DATAA ffd;
    HANDLE hFind = FindFirstFileA(pattern, &ffd);
    if (hFind == INVALID_HANDLE_VALUE) {
        JS_FreeCString(ctx, path);
        return throw_errno(ctx, path);
    }
    do {
        if (strcmp(ffd.cFileName, ".") == 0 || strcmp(ffd.cFileName, "..") == 0) continue;
        JS_SetPropertyUint32(ctx, arr, idx++, JS_NewString(ctx, ffd.cFileName));
    } while (FindNextFileA(hFind, &ffd));
    FindClose(hFind);
#else
    DIR *d = opendir(path);
    if (!d) {
        JSValue err = throw_errno(ctx, path);
        JS_FreeCString(ctx, path);
        JS_FreeValue(ctx, arr);
        return err;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        JS_SetPropertyUint32(ctx, arr, idx++, JS_NewString(ctx, ent->d_name));
    }
    closedir(d);
#endif

    JS_FreeCString(ctx, path);
    return arr;
}

/* ── statSync(path) ─────────────────────────────────────────────────────────── */

static JSValue js_statSync(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "statSync: path required");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    struct stat st;
    if (stat(path, &st) != 0) {
        JSValue err = throw_errno(ctx, path);
        JS_FreeCString(ctx, path);
        return err;
    }

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "size",  JS_NewInt64(ctx, st.st_size));
    JS_SetPropertyStr(ctx, obj, "isFile",
        JS_NewCFunction(ctx, NULL, "isFile", 0)); /* placeholder */

    /* isFile / isDirectory as functions */
    int is_file = (st.st_mode & S_IFMT) == S_IFREG;
    int is_dir  = (st.st_mode & S_IFMT) == S_IFDIR;

    JS_SetPropertyStr(ctx, obj, "isFile",
        JS_NewBool(ctx, is_file)); /* simplified: return bool directly */
    JS_SetPropertyStr(ctx, obj, "isDirectory",
        JS_NewBool(ctx, is_dir));
    JS_SetPropertyStr(ctx, obj, "mode",   JS_NewInt32(ctx, st.st_mode));
    JS_SetPropertyStr(ctx, obj, "mtimeMs",JS_NewFloat64(ctx, (double)st.st_mtime * 1000.0));

    JS_FreeCString(ctx, path);
    return obj;
}

/* ── unlinkSync(path) ───────────────────────────────────────────────────────── */

static JSValue js_unlinkSync(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "unlinkSync: path required");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    if (unlink(path) != 0) {
        JSValue err = throw_errno(ctx, path);
        JS_FreeCString(ctx, path);
        return err;
    }

    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

/* ── renameSync(oldPath, newPath) ───────────────────────────────────────────── */

static JSValue js_renameSync(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "renameSync: oldPath and newPath required");

    const char *old_path = JS_ToCString(ctx, argv[0]);
    const char *new_path = JS_ToCString(ctx, argv[1]);
    if (!old_path || !new_path) {
        JS_FreeCString(ctx, old_path);
        JS_FreeCString(ctx, new_path);
        return JS_EXCEPTION;
    }

    int ret;
#ifdef _WIN32
    ret = MoveFileExA(old_path, new_path, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
#else
    ret = rename(old_path, new_path);
#endif

    if (ret != 0) {
        JSValue err = throw_errno(ctx, old_path);
        JS_FreeCString(ctx, old_path);
        JS_FreeCString(ctx, new_path);
        return err;
    }

    JS_FreeCString(ctx, old_path);
    JS_FreeCString(ctx, new_path);
    return JS_UNDEFINED;
}

/* ── Module init ──────────────────────────────────────────────────────────────  */

static const JSCFunctionListEntry js_fs_funcs[] = {
    JS_CFUNC_DEF("readFileSync",   2, js_readFileSync),
    JS_CFUNC_DEF("writeFileSync",  2, js_writeFileSync),
    JS_CFUNC_DEF("appendFileSync", 2, js_appendFileSync),
    JS_CFUNC_DEF("existsSync",     1, js_existsSync),
    JS_CFUNC_DEF("mkdirSync",      1, js_mkdirSync),
    JS_CFUNC_DEF("readdirSync",    1, js_readdirSync),
    JS_CFUNC_DEF("statSync",       1, js_statSync),
    JS_CFUNC_DEF("unlinkSync",     1, js_unlinkSync),
    JS_CFUNC_DEF("renameSync",     2, js_renameSync),
};

static int js_fs_init(JSContext *ctx, JSModuleDef *m) {
    return JS_SetModuleExportList(ctx, m, js_fs_funcs,
                                  sizeof(js_fs_funcs) / sizeof(js_fs_funcs[0]));
}

JSModuleDef *js_init_module_fs(JSContext *ctx, const char *module_name) {
    JSModuleDef *m = JS_NewCModule(ctx, module_name, js_fs_init);
    if (!m) return NULL;
    JS_AddModuleExportList(ctx, m, js_fs_funcs,
                           sizeof(js_fs_funcs) / sizeof(js_fs_funcs[0]));
    return m;
}
