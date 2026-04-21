#pragma once
/*
 * cinder dev — native HTTP + WebSocket dev server
 * Orchestrates esbuild --watch for JS/TS/TSX bundling.
 * Starts in < 5ms; rebuilds in 10-50ms via esbuild.
 */

typedef struct DevServerConfig {
    int         port;         /* default 5173 */
    const char *root;         /* project root dir */
    const char *entry;        /* e.g. "src/main.tsx" */
    const char *public_dir;   /* e.g. "public" */
    const char *css_entry;    /* e.g. "src/index.css" (optional) */
    const char *esbuild_bin;  /* path to esbuild executable */
    const char *tw_bin;       /* path to tailwindcss executable (may be NULL) */
} DevServerConfig;

/* Discover project settings and fill config. Returns 0 on success. */
int devserver_discover(DevServerConfig *cfg, const char *root);

/* Run the dev server (blocks until Ctrl+C / error). */
int devserver_run(const DevServerConfig *cfg);
