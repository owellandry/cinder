#include "cli.h"
#include "runtime/runtime.h"
#include "pm/pm.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define CINDER_VERSION "0.1.0"

void cli_print_version(void) {
    printf("cinder v" CINDER_VERSION "\n");
}

void cli_print_help(void) {
    printf(
        "cinder v" CINDER_VERSION " — JavaScript runtime & package manager\n\n"
        "Usage:\n"
        "  cinder <file.js>          Run a JavaScript file\n"
        "  cinder run <script>       Run a script from package.json\n"
        "  cinder init               Create a new package.json\n"
        "  cinder install            Install all dependencies\n"
        "  cinder add <pkg[@ver]>    Add a package\n"
        "  cinder add -D <pkg[@ver]> Add a dev dependency\n"
        "  cinder remove <pkg>       Remove a package\n"
        "  cinder --version          Print version\n"
        "  cinder --help             Show this help\n\n"
        "Examples:\n"
        "  cinder app.js\n"
        "  cinder add express\n"
        "  cinder add -D typescript@5.0.0\n"
        "  cinder run build\n"
    );
}

int cli_main(int argc, char *argv[]) {
    if (argc < 2) {
        cli_print_help();
        return 0;
    }

    const char *cmd = argv[1];

    /* ── Flags ──────────────────────────────────────────────────────────── */
    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0) {
        cli_print_version();
        return 0;
    }

    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        cli_print_help();
        return 0;
    }

    /* ── Package manager commands ─────────────────────────────────────── */
    if (strcmp(cmd, "init") == 0) {
        return pm_init();
    }

    if (strcmp(cmd, "install") == 0 || strcmp(cmd, "i") == 0) {
        return pm_install();
    }

    if (strcmp(cmd, "add") == 0) {
        if (argc < 3) {
            fprintf(stderr, "cinder add: missing package name\n");
            return 1;
        }
        int is_dev = 0;
        const char *pkg = argv[2];
        if (strcmp(pkg, "-D") == 0 || strcmp(pkg, "--dev") == 0) {
            is_dev = 1;
            if (argc < 4) {
                fprintf(stderr, "cinder add -D: missing package name\n");
                return 1;
            }
            pkg = argv[3];
        }
        return pm_add(pkg, is_dev);
    }

    if (strcmp(cmd, "remove") == 0 || strcmp(cmd, "rm") == 0) {
        if (argc < 3) {
            fprintf(stderr, "cinder remove: missing package name\n");
            return 1;
        }
        return pm_remove(argv[2]);
    }

    /* ── Run a package.json script ─────────────────────────────────────── */
    if (strcmp(cmd, "run") == 0) {
        if (argc < 3) {
            fprintf(stderr, "cinder run: missing script name\n");
            return 1;
        }
        /* Pass remaining args after the script name */
        return pm_run_script(argv[2], argc - 3, argv + 3);
    }

    /* ── Execute a JS file (anything that looks like a path) ───────────── */
    {
        /* Check if it ends in .js or .mjs or .cjs or contains a '/' or '\' */
        size_t len = strlen(cmd);
        int looks_like_file =
            (len > 3 && strcmp(cmd + len - 3, ".js")  == 0) ||
            (len > 4 && strcmp(cmd + len - 4, ".mjs") == 0) ||
            (len > 4 && strcmp(cmd + len - 4, ".cjs") == 0) ||
            strchr(cmd, '/') != NULL ||
            strchr(cmd, '\\') != NULL;

        if (looks_like_file) {
            CinderRuntime *cr = cinder_runtime_new(argc - 1, argv + 1);
            if (!cr) return 1;
            int ret = cinder_runtime_exec_file(cr, cmd);
            cinder_runtime_free(cr);
            return ret;
        }
    }

    fprintf(stderr, "cinder: unknown command '%s'\nRun 'cinder --help' for usage.\n", cmd);
    return 1;
}
