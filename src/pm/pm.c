/*
 * cinder — package manager orchestrator
 * Handles: init, install, add, remove, run
 */
#include "pm.h"
#include "registry.h"
#include "resolver.h"
#include "installer.h"
#include "lockfile.h"

#include "dev/devserver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#  define PM_GETCWD(buf, size) GetCurrentDirectoryA((DWORD)(size), (buf))
#  define PATH_SEP  ";"
#  define DIR_SEP   "\\"
#else
#  include <unistd.h>
#  include <sys/wait.h>
#  include <sys/stat.h>
#  define PM_GETCWD(buf, size) getcwd((buf), (size))
#  define PATH_SEP  ":"
#  define DIR_SEP   "/"
#endif

#include "cJSON.h"

#define PKG_JSON     "package.json"
#define NODE_MODULES "node_modules"

/* ── Timer (milliseconds) ───────────────────────────────────────────────────── */

static double pm_now_ms(void) {
#ifdef _WIN32
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (double)cnt.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

static void pm_print_elapsed(double ms) {
    if (ms < 1000.0)
        printf("%.0fms", ms);
    else
        printf("%.2fs", ms / 1000.0);
}

/* ── Pretty package row  "  + name   version" ──────────────────────────────── */

#define NAME_COL 38   /* pad package name to this width */

static void pm_print_pkg(const char *name, const char *version, int cached) {
    int name_len = (int)strlen(name);
    int pad = NAME_COL - name_len;
    if (pad < 1) pad = 1;
    if (cached)
        printf("  ~ %-*s  %s\n", NAME_COL, name, version);
    else
        printf("  + %-*s  %s\n", NAME_COL, name, version);
    (void)pad;
}

/* ── File helpers ────────────────────────────────────────────────────────────── */

static char *read_file_str(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, (size_t)size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

static int write_file_str(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(content, f);
    fclose(f);
    return 0;
}

static cJSON *load_pkg_json(void) {
    char *buf = read_file_str(PKG_JSON);
    if (!buf) return NULL;
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    return root;
}

static int save_pkg_json(cJSON *root) {
    char *str = cJSON_Print(root);
    if (!str) return -1;
    int ret = write_file_str(PKG_JSON, str);
    free(str);
    return ret;
}

/* Parse "name@version" → separate name and version range */
static void parse_pkg_spec(const char *spec, char *name_out, size_t nsize,
                            char *ver_out, size_t vsize) {
    const char *at = NULL;
    const char *search = spec;
    if (spec[0] == '@') search = spec + 1;
    at = strchr(search, '@');

    if (at) {
        size_t nlen = (size_t)(at - spec);
        if (nlen >= nsize) nlen = nsize - 1;
        memcpy(name_out, spec, nlen);
        name_out[nlen] = '\0';
        strncpy(ver_out, at + 1, vsize - 1);
        ver_out[vsize - 1] = '\0';
    } else {
        strncpy(name_out, spec, nsize - 1);
        name_out[nsize - 1] = '\0';
        strncpy(ver_out, "latest", vsize - 1);
        ver_out[vsize - 1] = '\0';
    }
}

/* Install a single package; prints a row and returns installed version or NULL */
static const char *install_one(const char *name, const char *range,
                                CinderLockFile *lf) {
    /* Check lock + node_modules */
    LockEntry *cached = lockfile_find(lf, name);
    if (cached) {
        char dir[2048];
        snprintf(dir, sizeof(dir), "%s/%s/package.json", NODE_MODULES, name);
        FILE *probe = fopen(dir, "r");
        if (probe) {
            fclose(probe);
            pm_print_pkg(name, cached->version, 1);
            return cached->version;
        }
    }

    PkgInfo *info = registry_fetch(name);
    if (!info) {
        fprintf(stderr, "\n  error  Cannot fetch '%s' from registry\n", name);
        return NULL;
    }

    int idx = registry_resolve_version(info, range);
    if (idx < 0) {
        fprintf(stderr, "\n  error  No version of '%s' satisfies '%s'\n", name, range);
        registry_pkg_free(info);
        return NULL;
    }

    PkgVersion *pv = &info->versions[idx];
    const char *resolved_ver = pv->version;

    int ret = installer_download_and_extract(name, resolved_ver,
                                             pv->tarball_url, NODE_MODULES);
    if (ret == 0) {
        lockfile_upsert(lf, name, resolved_ver, pv->tarball_url, pv->integrity);
        pm_print_pkg(name, resolved_ver, 0);
    } else {
        fprintf(stderr, "\n  error  Failed to install '%s'\n", name);
        resolved_ver = NULL;
    }

    registry_pkg_free(info);
    return resolved_ver;
}

/* ── pm_init ──────────────────────────────────────────────────────────────────  */

int pm_init(void) {
    FILE *probe = fopen(PKG_JSON, "r");
    if (probe) {
        fclose(probe);
        fprintf(stderr, "cinder: %s already exists\n", PKG_JSON);
        return 1;
    }

    char cwd[4096] = {0};
    PM_GETCWD(cwd, sizeof(cwd));

    const char *pkg_name = cwd;
    for (const char *p = cwd; *p; p++) {
        if (*p == '/' || *p == '\\') pkg_name = p + 1;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name",        pkg_name);
    cJSON_AddStringToObject(root, "version",     "1.0.0");
    cJSON_AddStringToObject(root, "description", "");
    cJSON_AddStringToObject(root, "main",        "index.js");
    cJSON_AddObjectToObject(root, "scripts");
    cJSON_AddObjectToObject(root, "dependencies");
    cJSON_AddObjectToObject(root, "devDependencies");

    if (save_pkg_json(root) != 0) {
        fprintf(stderr, "cinder: failed to write %s\n", PKG_JSON);
        cJSON_Delete(root);
        return 1;
    }

    cJSON_Delete(root);
    printf("Created %s\n", PKG_JSON);
    return 0;
}

/* ── pm_install ───────────────────────────────────────────────────────────────  */

int pm_install(void) {
    cJSON *root = load_pkg_json();
    if (!root) {
        fprintf(stderr, "error: %s not found. Run 'cinder init' first.\n", PKG_JSON);
        return 1;
    }

    double t0 = pm_now_ms();
    CinderLockFile *lf = lockfile_load();
    int errors = 0, total = 0;

    printf("\n");

    cJSON *deps = cJSON_GetObjectItemCaseSensitive(root, "dependencies");
    if (deps) {
        cJSON *item;
        cJSON_ArrayForEach(item, deps) {
            const char *range = cJSON_IsString(item) ? item->valuestring : "latest";
            if (install_one(item->string, range, lf)) total++;
            else errors++;
        }
    }

    cJSON *dev = cJSON_GetObjectItemCaseSensitive(root, "devDependencies");
    if (dev) {
        cJSON *item;
        cJSON_ArrayForEach(item, dev) {
            const char *range = cJSON_IsString(item) ? item->valuestring : "latest";
            if (install_one(item->string, range, lf)) total++;
            else errors++;
        }
    }

    lockfile_save(lf);
    lockfile_free(lf);
    cJSON_Delete(root);

    double elapsed = pm_now_ms() - t0;
    printf("\n");

    if (errors == 0) {
        printf("  %d package%s installed  [", total, total == 1 ? "" : "s");
        pm_print_elapsed(elapsed);
        printf("]\n\n");
    } else {
        printf("  %d installed, %d failed  [", total, errors);
        pm_print_elapsed(elapsed);
        printf("]\n\n");
    }

    return errors > 0 ? 1 : 0;
}

/* ── pm_add ───────────────────────────────────────────────────────────────────  */

int pm_add(const char *pkg_spec, int is_dev) {
    char name[512], range[128];
    parse_pkg_spec(pkg_spec, name, sizeof(name), range, sizeof(range));

    cJSON *root = load_pkg_json();
    int created = 0;
    if (!root) {
        root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "name", "project");
        cJSON_AddStringToObject(root, "version", "1.0.0");
        cJSON_AddObjectToObject(root, "dependencies");
        cJSON_AddObjectToObject(root, "devDependencies");
        created = 1;
    }

    double t0 = pm_now_ms();
    CinderLockFile *lf = lockfile_load();

    printf("\n");
    const char *resolved = install_one(name, range, lf);
    if (!resolved) {
        lockfile_free(lf);
        cJSON_Delete(root);
        return 1;
    }

    double elapsed = pm_now_ms() - t0;

    /* Update package.json */
    const char *dep_key = is_dev ? "devDependencies" : "dependencies";
    cJSON *deps = cJSON_GetObjectItemCaseSensitive(root, dep_key);
    if (!deps) {
        deps = cJSON_CreateObject();
        cJSON_AddItemToObject(root, dep_key, deps);
    }

    char ver_entry[128];
    snprintf(ver_entry, sizeof(ver_entry), "^%s", resolved);
    cJSON_DeleteItemFromObject(deps, name);
    cJSON_AddStringToObject(deps, name, ver_entry);

    save_pkg_json(root);
    lockfile_save(lf);
    lockfile_free(lf);
    cJSON_Delete(root);

    printf("\n");
    if (created) printf("  Created %s\n", PKG_JSON);
    printf("  1 package installed  [");
    pm_print_elapsed(elapsed);
    printf("]\n\n");
    return 0;
}

/* ── pm_remove ────────────────────────────────────────────────────────────────  */

int pm_remove(const char *pkg_name) {
    cJSON *root = load_pkg_json();
    if (!root) {
        fprintf(stderr, "error: %s not found\n", PKG_JSON);
        return 1;
    }

    cJSON *deps = cJSON_GetObjectItemCaseSensitive(root, "dependencies");
    cJSON *dev  = cJSON_GetObjectItemCaseSensitive(root, "devDependencies");

    int found = 0;
    if (deps && cJSON_GetObjectItemCaseSensitive(deps, pkg_name)) {
        cJSON_DeleteItemFromObject(deps, pkg_name);
        found = 1;
    }
    if (dev && cJSON_GetObjectItemCaseSensitive(dev, pkg_name)) {
        cJSON_DeleteItemFromObject(dev, pkg_name);
        found = 1;
    }

    if (!found) {
        fprintf(stderr, "error: '%s' not in package.json\n", pkg_name);
        cJSON_Delete(root);
        return 1;
    }

    save_pkg_json(root);
    cJSON_Delete(root);

    CinderLockFile *lf = lockfile_load();
    LockEntry **prev = &lf->head;
    for (LockEntry *e = lf->head; e; ) {
        if (strcmp(e->name, pkg_name) == 0) {
            *prev = e->next;
            free(e->name); free(e->version);
            free(e->tarball_url); free(e->integrity);
            free(e);
            break;
        }
        prev = &e->next;
        e = e->next;
    }
    lockfile_save(lf);
    lockfile_free(lf);

    printf("  - %-*s  removed\n", NAME_COL, pkg_name);
    return 0;
}

/* ── Direct process spawner (skips cmd.exe shell overhead) ─────────────────── */

#ifdef _WIN32
static int spawn_wait(const char *cmdline) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);

    /* CreateProcess needs a mutable buffer */
    char buf[4096];
    strncpy(buf, cmdline, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* Try to find the exe directly in node_modules\.bin to avoid cmd.exe */
    char resolved[2048];
    char exe_name[256];
    const char *sp = strchr(cmdline, ' ');
    size_t exe_len = sp ? (size_t)(sp - cmdline) : strlen(cmdline);
    if (exe_len >= sizeof(exe_name)) exe_len = sizeof(exe_name) - 1;
    memcpy(exe_name, cmdline, exe_len);
    exe_name[exe_len] = '\0';

    const char *found_path = NULL;
    const char *exts[] = { ".exe", ".cmd", ".ps1", NULL };
    for (int e = 0; exts[e]; e++) {
        snprintf(resolved, sizeof(resolved),
                 "node_modules\\.bin\\%s%s", exe_name, exts[e]);
        if (GetFileAttributesA(resolved) != INVALID_FILE_ATTRIBUTES) {
            found_path = resolved;
            break;
        }
    }

    if (found_path && (strstr(found_path, ".cmd") || strstr(found_path, ".ps1"))) {
        /* .cmd/.ps1 still needs cmd.exe */
        char wrapped[4096];
        snprintf(wrapped, sizeof(wrapped), "cmd.exe /d /c \"%s\"", found_path);
        if (sp) {
            size_t wl = strlen(wrapped);
            snprintf(wrapped + wl, sizeof(wrapped) - wl, "%s", sp);
        }
        strncpy(buf, wrapped, sizeof(buf) - 1);
        found_path = NULL; /* let CreateProcess find cmd.exe */
    } else if (found_path && strstr(found_path, ".exe")) {
        /* Direct .exe: build full command with args */
        if (sp)
            snprintf(buf, sizeof(buf), "%s%s", found_path, sp);
        else
            strncpy(buf, found_path, sizeof(buf) - 1);
    } else {
        /* Fallback: let cmd.exe handle it */
        snprintf(buf, sizeof(buf), "cmd.exe /d /c %s", cmdline);
    }

    if (!CreateProcessA(NULL, buf, NULL, NULL, TRUE,
                        0, NULL, NULL, &si, &pi)) {
        /* Last resort: use system() */
        return system(cmdline);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}
#else
static int has_shell_meta(const char *s) {
    return strpbrk(s, "|&;()$`\\\"'*?[#~=!{}<>") != NULL;
}

static int spawn_wait(const char *cmdline) {
    if (has_shell_meta(cmdline))
        return system(cmdline);

    /* Tokenize into argv (simple space split — no quoting) */
    char buf[4096];
    strncpy(buf, cmdline, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *toks[128];
    int n = 0;
    char *p = strtok(buf, " \t");
    while (p && n < 127) { toks[n++] = p; p = strtok(NULL, " \t"); }
    toks[n] = NULL;
    if (n == 0) return 0;

    /* Resolve binary in node_modules/.bin if not absolute */
    char resolved[2048];
    if (toks[0][0] != '/') {
        snprintf(resolved, sizeof(resolved), "node_modules/.bin/%s", toks[0]);
        struct stat st;
        if (stat(resolved, &st) == 0 && (st.st_mode & S_IXUSR))
            toks[0] = resolved;
    }

    pid_t pid = fork();
    if (pid < 0) return system(cmdline);
    if (pid == 0) {
        execvp(toks[0], toks);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
#endif

/* ── pm_run_script ────────────────────────────────────────────────────────────  */

int pm_run_script(const char *script_name, int argc, char *argv[]) {
    (void)argc; (void)argv;

    cJSON *root = load_pkg_json();
    if (!root) {
        fprintf(stderr, "error: %s not found\n", PKG_JSON);
        return 1;
    }

    cJSON *scripts = cJSON_GetObjectItemCaseSensitive(root, "scripts");
    if (!scripts) {
        fprintf(stderr, "error: no scripts in %s\n", PKG_JSON);
        cJSON_Delete(root);
        return 1;
    }

    cJSON *script = cJSON_GetObjectItemCaseSensitive(scripts, script_name);
    if (!script || !cJSON_IsString(script)) {
        fprintf(stderr, "error: script '%s' not found\n", script_name);
        cJSON_Delete(root);
        return 1;
    }

    /* Copy command string before freeing the JSON tree */
    char cmd_buf[4096];
    strncpy(cmd_buf, script->valuestring, sizeof(cmd_buf) - 1);
    cmd_buf[sizeof(cmd_buf) - 1] = '\0';
    const char *cmd = cmd_buf;
    cJSON_Delete(root);

    /* ── Fast-path: intercept known dev-server commands ────────────── */
    {
        const char *p = cmd;
        /* skip optional leading "npx " */
        if (strncmp(p, "npx ", 4) == 0) p += 4;

        int is_vite = (strncmp(p, "vite", 4) == 0 &&
                       (p[4] == '\0' || p[4] == ' '));

        if (is_vite) {
            /* Check for --no-native opt-out */
            int native_disabled = (getenv("CINDER_NO_NATIVE") != NULL)
                               || (strstr(cmd, "--no-native") != NULL);
            if (!native_disabled) {
                DevServerConfig cfg;
                if (devserver_discover(&cfg, ".") == 0 && cfg.esbuild_bin) {
                    /* Parse --port from the original command */
                    const char *port_flag = strstr(cmd, "--port");
                    if (port_flag) {
                        port_flag += 6;
                        while (*port_flag == ' ' || *port_flag == '=') port_flag++;
                        int pv = atoi(port_flag);
                        if (pv > 0) cfg.port = pv;
                    }
                    printf("  \033[36m⚡ cinder native dev server\033[0m  "
                           "(bypassing vite for ~10x faster startup)\n");
                    return devserver_run(&cfg);
                }
            }
        }
    }

    /* ── Normal path: spawn the script command ────────────────────── */
    const char *old_path = getenv("PATH");
    if (old_path) {
        char new_path[8192];
        snprintf(new_path, sizeof(new_path),
                 "node_modules" DIR_SEP ".bin" PATH_SEP
                 "." DIR_SEP "node_modules" DIR_SEP ".bin" PATH_SEP
                 "%s", old_path);
#ifdef _WIN32
        SetEnvironmentVariableA("PATH", new_path);
#else
        setenv("PATH", new_path, 1);
#endif
    }

    double t0 = pm_now_ms();
    printf("$ %s\n\n", cmd);
    fflush(stdout);
    int ret = spawn_wait(cmd);

    /* Restore PATH */
    if (old_path) {
#ifdef _WIN32
        SetEnvironmentVariableA("PATH", old_path);
#else
        setenv("PATH", old_path, 1);
#endif
    }

    double elapsed = pm_now_ms() - t0;
    printf("\n  done  [");
    pm_print_elapsed(elapsed);
    printf("]\n");

    return ret;
}

