#ifndef CINDER_MOD_PROCESS_H
#define CINDER_MOD_PROCESS_H

#include "quickjs.h"

/* Register `process` as a global variable on ctx */
void js_init_process(JSContext *ctx, int argc, char *argv[]);

#endif /* CINDER_MOD_PROCESS_H */
