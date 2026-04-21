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

/* ── Platform package filter ──────────────────────────────────────────────────  */

static char g_plat_id[64]; /* platform identifier e.g. "win32-x64", filled in pm_install */

/* Returns 1 if `word` appears as a dash-delimited token in `name`. */
static int plat_word_in_name(const char *name, const char *word) {
    size_t wlen = strlen(word);
    const char *p = name;
    while ((p = strstr(p, word)) != NULL) {
        int pre_ok = (p == name) || (*(p - 1) == '-') || (*(p - 1) == '/');
        int post_ok = (*(p + wlen) == '\0') || (*(p + wlen) == '-');
        if (pre_ok && post_ok) return 1;
        p++;
    }
    return 0;
}

/* Returns 1 if the package should be SKIPPED for the current platform.
 * Handles both scoped (@vendor/os-cpu) and non-scoped (pkg-os-cpu) forms. */
static int plat_pkg_skip(const char *pkg_name) {
    static const char *plat_oses[]  = { "darwin", "linux", "freebsd", "android", "win32", NULL };
    static const char *plat_cpus[]  = { "arm64", "arm", "ia32", "x86", NULL }; /* NOT x64 — too common substring */

    /* Extract OS and CPU from g_plat_id (e.g. "win32-x64") */
    char cur_os[32] = {0}, cur_cpu[32] = {0};
    const char *dash = strchr(g_plat_id, '-');
    if (dash) {
        size_t olen = (size_t)(dash - g_plat_id);
        strncpy(cur_os, g_plat_id, olen < 31 ? olen : 31);
        strncpy(cur_cpu, dash + 1, 31);
    }

    /* For scoped packages: @vendor/os-cpu or @vendor/os-cpu-abi */
    const char *slash = strchr(pkg_name + (pkg_name[0] == '@' ? 1 : 0), '/');
    if (slash) {
        const char *suf = slash + 1;
        /* Only filter if suffix looks like a platform string (has dash, no dot) */
        if (strchr(suf, '-') && !strchr(suf, '.')) {
            /* Allow if suffix starts with current platform id */
            if (strncmp(suf, g_plat_id, strlen(g_plat_id)) != 0)
                return 1; /* platform mismatch */
        }
        return 0;
    }

    /* For non-scoped packages: check for foreign OS identifier as a word */
    for (int k = 0; plat_oses[k]; k++) {
        if (strcmp(plat_oses[k], cur_os) == 0) continue; /* current OS — always OK */
        if (plat_word_in_name(pkg_name, plat_oses[k])) return 1;
    }

    /* Check for foreign CPU architecture */
    for (int k = 0; plat_cpus[k]; k++) {
        if (strcmp(plat_cpus[k], cur_cpu) == 0) continue;
        if (plat_word_in_name(pkg_name, plat_cpus[k])) return 1;
    }

    return 0;
}

/* ── Dependency collection helper ─────────────────────────────────────────────  */

typedef struct {
    char  name[512];
    char  range[128];
} DepEntry;

static int collect_deps(cJSON *obj, DepEntry *out, int max) {
    int n = 0;
    if (!obj) return 0;
    cJSON *item;
    cJSON_ArrayForEach(item, obj) {
        if (n >= max) break;
        strncpy(out[n].name, item->string, sizeof(out[n].name) - 1);
        out[n].name[sizeof(out[n].name) - 1] = '\0';
        const char *r = cJSON_IsString(item) ? item->valuestring : "latest";
        strncpy(out[n].range, r, sizeof(out[n].range) - 1);
        out[n].range[sizeof(out[n].range) - 1] = '\0';
        n++;
    }
    return n;
}

/* ── pm_install (parallel) ────────────────────────────────────────────────────  */

int pm_install(void) {
    cJSON *root = load_pkg_json();
    if (!root) {
        fprintf(stderr, "error: %s not found. Run 'cinder init' first.\n", PKG_JSON);
        return 1;
    }

    double t0 = pm_now_ms();
    CinderLockFile *lf = lockfile_load();

    printf("\n");

    /* Platform identifier for optional dep filtering — used in Phase 1 and Phase 5 */
#ifdef _WIN32
    {
        SYSTEM_INFO _si = {0};
        GetNativeSystemInfo(&_si);
        snprintf(g_plat_id, sizeof(g_plat_id), "win32-%s",
                 (_si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64)
                 ? "arm64" : "x64");
    }
#elif defined(__APPLE__)
#  if defined(__aarch64__)
    snprintf(g_plat_id, sizeof(g_plat_id), "darwin-arm64");
#  else
    snprintf(g_plat_id, sizeof(g_plat_id), "darwin-x64");
#  endif
#else
#  if defined(__aarch64__)
    snprintf(g_plat_id, sizeof(g_plat_id), "linux-arm64");
#  elif defined(__arm__)
    snprintf(g_plat_id, sizeof(g_plat_id), "linux-arm");
#  else
    snprintf(g_plat_id, sizeof(g_plat_id), "linux-x64");
#  endif
#endif

/* all_deps tracks every package seen (root + transitive) to detect duplicates.
 * resolved_* arrays are only for the initial root deps (Phase 2-4). */
#define MAX_ALL_DEPS  4096
#define MAX_ROOT_DEPS 1024

    DepEntry *all_deps        = (DepEntry *)calloc(MAX_ALL_DEPS, sizeof(DepEntry));
    int      *need_registry   = (int *)calloc(MAX_ROOT_DEPS, sizeof(int));
    int      *need_download_lf= (int *)calloc(MAX_ROOT_DEPS, sizeof(int));

    /* Resolved metadata buffers for initial root deps */
    char (*resolved_versions)[128]  = (char(*)[128])calloc(MAX_ROOT_DEPS, 128);
    char (*resolved_urls)[2048]     = (char(*)[2048])calloc(MAX_ROOT_DEPS, 2048);
    char (*resolved_integrity)[256] = (char(*)[256])calloc(MAX_ROOT_DEPS, 256);

    if (!all_deps || !need_registry || !need_download_lf ||
        !resolved_versions || !resolved_urls || !resolved_integrity) {
        fprintf(stderr, "error: out of memory\n");
        free(all_deps); free(need_registry); free(need_download_lf);
        free(resolved_versions); free(resolved_urls); free(resolved_integrity);
        lockfile_free(lf); cJSON_Delete(root);
        return 1;
    }

    int dep_count = 0;
    cJSON *deps = cJSON_GetObjectItemCaseSensitive(root, "dependencies");
    dep_count += collect_deps(deps, all_deps + dep_count, MAX_ROOT_DEPS - dep_count);

    cJSON *dev = cJSON_GetObjectItemCaseSensitive(root, "devDependencies");
    dep_count += collect_deps(dev, all_deps + dep_count, MAX_ROOT_DEPS - dep_count);

    /* Root optional deps — platform filtered */
    {
        cJSON *opt = cJSON_GetObjectItemCaseSensitive(root, "optionalDependencies");
        if (opt) {
            cJSON *item;
            cJSON_ArrayForEach(item, opt) {
                if (dep_count >= MAX_ROOT_DEPS) break;
                const char *name = item->string;
                if (plat_pkg_skip(name)) continue;
                strncpy(all_deps[dep_count].name, name,
                        sizeof(all_deps[dep_count].name) - 1);
                const char *r = cJSON_IsString(item) ? item->valuestring : "latest";
                strncpy(all_deps[dep_count].range, r,
                        sizeof(all_deps[dep_count].range) - 1);
                dep_count++;
            }
        }
    }

    int root_dep_count = dep_count; /* Phase 2-4 only touches [0..root_dep_count) */
    (void)root_dep_count;

    if (dep_count == 0) {
        printf("  No dependencies to install\n\n");
        goto cleanup;
    }

    /* Phase 2: classify — cached vs needs-lockfile-url vs needs-registry */
    int cached_count       = 0;
    int need_registry_count   = 0;
    int need_download_count   = 0;

    for (int i = 0; i < dep_count; i++) {
        LockEntry *le = lockfile_find(lf, all_deps[i].name);
        if (le) {
            char probe[2048];
            snprintf(probe, sizeof(probe), "%s/%s/package.json",
                     NODE_MODULES, all_deps[i].name);
            FILE *f = fopen(probe, "r");
            if (f) {
                fclose(f);
                pm_print_pkg(all_deps[i].name, le->version, 1);
                cached_count++;
                continue;
            }
            need_download_lf[need_download_count++] = i;
        } else {
            need_registry[need_registry_count++] = i;
        }
    }

    int errors    = 0;
    int installed = cached_count;

    /* Phase 3: parallel registry fetch for unlocked packages */
    for (int j = 0; j < need_download_count; j++) {
        int i = need_download_lf[j];
        LockEntry *le = lockfile_find(lf, all_deps[i].name);
        if (le) {
            strncpy(resolved_versions[i], le->version, 127);
            strncpy(resolved_urls[i], le->tarball_url, 2047);
            strncpy(resolved_integrity[i], le->integrity, 255);
        }
    }

    if (need_registry_count > 0) {
        const char **fetch_names   = (const char **)calloc(MAX_ROOT_DEPS, sizeof(char *));
        PkgInfo    **fetch_results = (PkgInfo **)calloc(MAX_ROOT_DEPS, sizeof(PkgInfo *));

        if (fetch_names && fetch_results) {
            for (int j = 0; j < need_registry_count; j++)
                fetch_names[j] = all_deps[need_registry[j]].name;

            registry_fetch_multi(fetch_names, need_registry_count,
                                 fetch_results, INSTALL_MAX_PARALLEL);

            for (int j = 0; j < need_registry_count; j++) {
                int i = need_registry[j];
                PkgInfo *info = fetch_results[j];
                if (!info) {
                    fprintf(stderr, "\n  error  Cannot fetch '%s' from registry\n",
                            all_deps[i].name);
                    errors++;
                    continue;
                }
                int idx = registry_resolve_version(info, all_deps[i].range);
                if (idx < 0) {
                    fprintf(stderr, "\n  error  No version of '%s' satisfies '%s'\n",
                            all_deps[i].name, all_deps[i].range);
                    registry_pkg_free(info);
                    errors++;
                    continue;
                }
                PkgVersion *pv = &info->versions[idx];
                strncpy(resolved_versions[i], pv->version, 127);
                strncpy(resolved_urls[i], pv->tarball_url, 2047);
                strncpy(resolved_integrity[i], pv->integrity, 255);
                registry_pkg_free(info);
            }
        }
        free(fetch_names);
        free(fetch_results);
    }

    /* Phase 4: parallel download + extract */
    int total_to_download = need_download_count + need_registry_count;
    if (total_to_download > 0) {
        InstallTask *tasks = (InstallTask *)calloc((size_t)total_to_download,
                                                    sizeof(InstallTask));
        int *task_dep_idx = (int *)calloc((size_t)total_to_download, sizeof(int));
        int task_count = 0;

        /* Add lockfile-url downloads */
        for (int j = 0; j < need_download_count; j++) {
            int i = need_download_lf[j];
            if (resolved_urls[i][0] == '\0') continue;
            tasks[task_count].pkg_name    = all_deps[i].name;
            tasks[task_count].version     = resolved_versions[i];
            tasks[task_count].tarball_url = resolved_urls[i];
            tasks[task_count].dest_dir    = NODE_MODULES;
            tasks[task_count].result      = -1;
            task_dep_idx[task_count]       = i;
            task_count++;
        }

        /* Add registry-resolved downloads */
        for (int j = 0; j < need_registry_count; j++) {
            int i = need_registry[j];
            if (resolved_urls[i][0] == '\0') continue;
            tasks[task_count].pkg_name    = all_deps[i].name;
            tasks[task_count].version     = resolved_versions[i];
            tasks[task_count].tarball_url = resolved_urls[i];
            tasks[task_count].dest_dir    = NODE_MODULES;
            tasks[task_count].result      = -1;
            task_dep_idx[task_count]       = i;
            task_count++;
        }

        if (task_count > 0) {
            installer_download_multi(tasks, task_count, INSTALL_MAX_PARALLEL);

            for (int t = 0; t < task_count; t++) {
                int i = task_dep_idx[t];
                if (tasks[t].result == 0) {
                    pm_print_pkg(all_deps[i].name, resolved_versions[i], 0);
                    lockfile_upsert(lf, all_deps[i].name,
                                    resolved_versions[i],
                                    resolved_urls[i],
                                    resolved_integrity[i]);
                    installed++;
                } else {
                    fprintf(stderr, "\n  error  Failed to install '%s'\n",
                            all_deps[i].name);
                    errors++;
                }
            }
        }

        free(tasks);
        free(task_dep_idx);
    }

    /* Phase 5: BFS transitive + optional dependency installation.
     *
     * Walks every installed package's package.json, collects their
     * `dependencies` and `optionalDependencies` (platform-filtered),
     * installs any that are missing, then repeats until no new packages.
     * This correctly handles deep dep trees (e.g. react-router-dom →
     * react-router → @remix-run/router). */
    {
#define MAX_BATCH 512
        int wave_cursor = 0; /* next index in all_deps to scan */

        while (1) {
            int scan_end = dep_count;

            /* ── collect missing transitive deps from current wave ── */
            DepEntry    *batch      = (DepEntry    *)calloc(MAX_BATCH, sizeof(DepEntry));
            char       (*bver)[128] = (char(*)[128])calloc(MAX_BATCH, 128);
            char       (*burl)[2048]= (char(*)[2048])calloc(MAX_BATCH, 2048);
            char       (*binteg)[256]=(char(*)[256])calloc(MAX_BATCH, 256);
            if (!batch || !bver || !burl || !binteg) {
                free(batch); free(bver); free(burl); free(binteg); break;
            }
            int batch_count = 0;

            for (int i = wave_cursor; i < scan_end; i++) {
                char pj_path[2048];
                snprintf(pj_path, sizeof(pj_path), "%s/%s/package.json",
                         NODE_MODULES, all_deps[i].name);
                FILE *pf = fopen(pj_path, "r");
                if (!pf) continue;
                fseek(pf, 0, SEEK_END); long sz = ftell(pf); fseek(pf, 0, SEEK_SET);
                char *json = (char *)malloc((size_t)sz + 1);
                if (!json) { fclose(pf); continue; }
                size_t rd = fread(json, 1, (size_t)sz, pf);
                json[rd] = '\0'; fclose(pf);

                cJSON *pkg = cJSON_Parse(json);
                free(json);
                if (!pkg) continue;

                /* Scan both `dependencies` and `optionalDependencies` */
                const char *dep_types[] = { "dependencies", "optionalDependencies", NULL };
                for (int dt = 0; dep_types[dt] && batch_count < MAX_BATCH; dt++) {
                    cJSON *pkgdeps = cJSON_GetObjectItemCaseSensitive(pkg, dep_types[dt]);
                    if (!pkgdeps) continue;

                    cJSON *dep_item;
                    cJSON_ArrayForEach(dep_item, pkgdeps) {
                        if (batch_count >= MAX_BATCH) break;
                        const char *dname = dep_item->string;

                        /* Platform filter for optional deps */
                        if (dt == 1 && plat_pkg_skip(dname)) continue;

                        /* Skip if already tracked in all_deps (prevents cycles) */
                        int seen = 0;
                        for (int j = 0; j < dep_count && !seen; j++)
                            if (strcmp(all_deps[j].name, dname) == 0) seen = 1;
                        for (int j = 0; j < batch_count && !seen; j++)
                            if (strcmp(batch[j].name, dname) == 0) seen = 1;
                        if (seen) continue;

                        /* Skip if already physically present in node_modules */
                        char probe[2048];
                        snprintf(probe, sizeof(probe), "%s/%s/package.json",
                                 NODE_MODULES, dname);
                        FILE *pf2 = fopen(probe, "r");
                        if (pf2) {
                            fclose(pf2);
                            /* Mark as seen without installing */
                            if (dep_count < MAX_ALL_DEPS) {
                                strncpy(all_deps[dep_count].name, dname, 511);
                                all_deps[dep_count].range[0] = '\0';
                                dep_count++;
                            }
                            continue;
                        }

                        /* Queue for installation */
                        strncpy(batch[batch_count].name, dname, 511);
                        const char *vr = cJSON_IsString(dep_item)
                                         ? dep_item->valuestring : "latest";
                        strncpy(batch[batch_count].range, vr, 127);
                        batch_count++;
                    }
                }
                cJSON_Delete(pkg);
            }

            wave_cursor = scan_end;

            if (batch_count == 0) {
                free(batch); free(bver); free(burl); free(binteg);
                break; /* BFS complete */
            }

            /* ── parallel registry fetch for this batch ── */
            const char **bnames = (const char **)calloc((size_t)batch_count,
                                                         sizeof(char *));
            PkgInfo    **binfo  = (PkgInfo    **)calloc((size_t)batch_count,
                                                         sizeof(PkgInfo *));
            if (bnames && binfo) {
                for (int j = 0; j < batch_count; j++) bnames[j] = batch[j].name;
                registry_fetch_multi(bnames, batch_count, binfo, INSTALL_MAX_PARALLEL);

                for (int j = 0; j < batch_count; j++) {
                    /* Check lockfile first (may already be resolved) */
                    LockEntry *le = lockfile_find(lf, batch[j].name);
                    if (le) {
                        strncpy(bver[j], le->version, 127);
                        strncpy(burl[j], le->tarball_url, 2047);
                        strncpy(binteg[j], le->integrity, 255);
                        if (binfo[j]) { registry_pkg_free(binfo[j]); binfo[j] = NULL; }
                        continue;
                    }
                    if (!binfo[j]) { burl[j][0] = '\0'; continue; }
                    int idx = registry_resolve_version(binfo[j], batch[j].range);
                    if (idx < 0) { burl[j][0] = '\0'; registry_pkg_free(binfo[j]); binfo[j] = NULL; continue; }
                    PkgVersion *pv = &binfo[j]->versions[idx];
                    strncpy(bver[j], pv->version, 127);
                    strncpy(burl[j], pv->tarball_url, 2047);
                    strncpy(binteg[j], pv->integrity, 255);
                    registry_pkg_free(binfo[j]); binfo[j] = NULL;
                }
            }
            free(bnames); free(binfo);

            /* ── parallel install for this batch ── */
            InstallTask *btasks    = (InstallTask *)calloc((size_t)batch_count,
                                                            sizeof(InstallTask));
            int         *bidx_map  = (int *)calloc((size_t)batch_count, sizeof(int));
            int btask_count = 0;

            if (btasks && bidx_map) {
                for (int j = 0; j < batch_count; j++) {
                    if (burl[j][0] == '\0') continue;
                    btasks[btask_count].pkg_name    = batch[j].name;
                    btasks[btask_count].version     = bver[j];
                    btasks[btask_count].tarball_url = burl[j];
                    btasks[btask_count].dest_dir    = NODE_MODULES;
                    btasks[btask_count].result      = -1;
                    bidx_map[btask_count]            = j;
                    btask_count++;
                }

                if (btask_count > 0) {
                    installer_download_multi(btasks, btask_count, INSTALL_MAX_PARALLEL);

                    for (int t = 0; t < btask_count; t++) {
                        int j = bidx_map[t];
                        if (btasks[t].result == 0) {
                            pm_print_pkg(batch[j].name, bver[j], 0);
                            lockfile_upsert(lf, batch[j].name, bver[j],
                                            burl[j], binteg[j]);
                            installed++;
                            if (dep_count < MAX_ALL_DEPS) {
                                strncpy(all_deps[dep_count].name, batch[j].name, 511);
                                strncpy(all_deps[dep_count].range, batch[j].range, 127);
                                dep_count++;
                            }
                        }
                        /* transitive failures are non-fatal */
                    }
                }
            }
            free(btasks); free(bidx_map);
            free(batch); free(bver); free(burl); free(binteg);
        }
    }

    /* Phase 6: save lockfile */
    lockfile_save(lf);

    double elapsed = pm_now_ms() - t0;
    printf("\n");

    if (errors == 0) {
        printf("  %d package%s installed  [", installed, installed == 1 ? "" : "s");
        pm_print_elapsed(elapsed);
        printf("]\n\n");
    } else {
        printf("  %d installed, %d failed  [", installed, errors);
        pm_print_elapsed(elapsed);
        printf("]\n\n");
    }

cleanup:
    lockfile_free(lf);
    cJSON_Delete(root);
    free(all_deps);
    free(need_registry);
    free(need_download_lf);
    free(resolved_versions);
    free(resolved_urls);
    free(resolved_integrity);
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
                    printf("  \033[36m>> cinder native dev server\033[0m  "
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

