/*
 * cinder — process module
 * Registers `process` as a global object on the JS context
 */
#include "mod_process.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#  define getcwd _getcwd
#  define platform_str "win32"
#else
#  include <unistd.h>
#  if defined(__APPLE__)
#    define platform_str "darwin"
#  else
#    define platform_str "linux"
#  endif
#endif

#define CINDER_VERSION "0.1.0"

/* ── stdout/stderr write helpers ─────────────────────────────────────────────── */

static JSValue js_stdout_write(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, argv[0]);
    if (s) { fputs(s, stdout); JS_FreeCString(ctx, s); }
    return JS_UNDEFINED;
}

static JSValue js_stderr_write(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, argv[0]);
    if (s) { fputs(s, stderr); JS_FreeCString(ctx, s); }
    return JS_UNDEFINED;
}

/* ── process.exit([code]) ──────────────────────────────────────────────────── */

static JSValue js_process_exit(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    (void)this_val;
    int code = 0;
    if (argc >= 1) JS_ToInt32(ctx, &code, argv[0]);
    exit(code);
    return JS_UNDEFINED;
}

/* ── process.cwd() ──────────────────────────────────────────────────────────── */

static JSValue js_process_cwd(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    char buf[4096];
    if (!getcwd(buf, sizeof(buf))) return JS_NewString(ctx, ".");
    return JS_NewString(ctx, buf);
}

/* ── process.hrtime.bigint() ────────────────────────────────────────────────── */

static JSValue js_process_hrtime_bigint(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
#ifdef _WIN32
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    uint64_t ns = (uint64_t)(cnt.QuadPart * 1000000000LL / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
    return JS_NewBigUint64(ctx, ns);
}

/* ── Build and register the `process` global ────────────────────────────────── */

void js_init_process(JSContext *ctx, int argc, char *argv[]) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue process = JS_NewObject(ctx);

    /* process.argv */
    JSValue args = JS_NewArray(ctx);
    for (int i = 0; i < argc; i++) {
        JS_SetPropertyUint32(ctx, args, (uint32_t)i, JS_NewString(ctx, argv[i]));
    }
    JS_SetPropertyStr(ctx, process, "argv", args);

    /* process.env */
    JSValue env = JS_NewObject(ctx);
#ifdef _WIN32
    /* Windows: GetEnvironmentStrings */
    LPCH envblock = GetEnvironmentStringsA();
    if (envblock) {
        for (LPCH e = envblock; *e; e += strlen(e) + 1) {
            char *eq = strchr(e, '=');
            if (eq && eq != e) {
                char key[512];
                size_t klen = (size_t)(eq - e);
                if (klen >= sizeof(key)) klen = sizeof(key) - 1;
                memcpy(key, e, klen);
                key[klen] = '\0';
                JS_SetPropertyStr(ctx, env, key, JS_NewString(ctx, eq + 1));
            }
        }
        FreeEnvironmentStringsA(envblock);
    }
#else
    extern char **environ;
    for (char **e = environ; *e; e++) {
        char *eq = strchr(*e, '=');
        if (eq && eq != *e) {
            char key[512];
            size_t klen = (size_t)(eq - *e);
            if (klen >= sizeof(key)) klen = sizeof(key) - 1;
            memcpy(key, *e, klen);
            key[klen] = '\0';
            JS_SetPropertyStr(ctx, env, key, JS_NewString(ctx, eq + 1));
        }
    }
#endif
    JS_SetPropertyStr(ctx, process, "env", env);

    /* process.version */
    JS_SetPropertyStr(ctx, process, "version",
                      JS_NewString(ctx, "v" CINDER_VERSION));
    JS_SetPropertyStr(ctx, process, "versions",
                      JS_NewObject(ctx));

    /* process.platform */
    JS_SetPropertyStr(ctx, process, "platform",
                      JS_NewString(ctx, platform_str));

    /* process.exit */
    JS_SetPropertyStr(ctx, process, "exit",
                      JS_NewCFunction(ctx, js_process_exit, "exit", 1));

    /* process.cwd */
    JS_SetPropertyStr(ctx, process, "cwd",
                      JS_NewCFunction(ctx, js_process_cwd, "cwd", 0));

    /* process.hrtime.bigint */
    JSValue hrtime = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, hrtime, "bigint",
                      JS_NewCFunction(ctx, js_process_hrtime_bigint, "bigint", 0));
    JS_SetPropertyStr(ctx, process, "hrtime", hrtime);

    /* process.pid */
#ifdef _WIN32
    JS_SetPropertyStr(ctx, process, "pid", JS_NewInt32(ctx, (int)GetCurrentProcessId()));
#else
    JS_SetPropertyStr(ctx, process, "pid", JS_NewInt32(ctx, (int)getpid()));
#endif

    /* process.stdout / process.stderr stubs */
    JSValue stdout_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, stdout_obj, "write",
        JS_NewCFunction(ctx, js_stdout_write, "write", 1));
    JS_SetPropertyStr(ctx, process, "stdout", stdout_obj);

    JSValue stderr_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, stderr_obj, "write",
        JS_NewCFunction(ctx, js_stderr_write, "write", 1));
    JS_SetPropertyStr(ctx, process, "stderr", stderr_obj);

    JS_SetPropertyStr(ctx, global, "process", process);
    JS_FreeValue(ctx, global);
}
