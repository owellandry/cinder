#ifndef CINDER_MOD_REQUIRE_H
#define CINDER_MOD_REQUIRE_H

#include "quickjs.h"

/* Register the global require() function and module cache on the context */
void js_require_cleanup(JSContext *ctx);
void js_init_require(JSContext *ctx, const char *entry_file_path);

#endif /* CINDER_MOD_REQUIRE_H */
