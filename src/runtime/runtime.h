#ifndef CINDER_RUNTIME_H
#define CINDER_RUNTIME_H

#include "quickjs.h"

typedef struct CinderRuntime {
    JSRuntime *rt;
    JSContext *ctx;
} CinderRuntime;

/* Create and initialize a new runtime (registers all built-in modules) */
CinderRuntime *cinder_runtime_new(int argc, char *argv[]);

/* Execute a JS file, returns 0 on success */
int cinder_runtime_exec_file(CinderRuntime *cr, const char *path);

/* Execute a JS string snippet */
int cinder_runtime_exec_str(CinderRuntime *cr, const char *src, const char *filename);

/* Free runtime resources */
void cinder_runtime_free(CinderRuntime *cr);

#endif /* CINDER_RUNTIME_H */
