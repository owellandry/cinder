/*
 * cinder — package installer
 * Downloads .tgz tarballs from npm and extracts them to node_modules/
 * Supports parallel bulk downloads via curl_multi.
 */
#include "installer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include <curl/curl.h>
#include <zlib.h>
#include "../util/curl_util.h"

#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#  define mkdir_fn(p) _mkdir(p)
#else
#  include <unistd.h>
#  define mkdir_fn(p) mkdir(p, 0755)
#endif

/* ── Cache directory ─────────────────────────────────────────────────────────── */

static char g_cache_dir[4096];
static int  g_cache_ready = 0;

void installer_cache_init(void) {
    if (g_cache_ready) return;
    const char *home = getenv("HOME");
    if (!home) home = getenv("USERPROFILE");
    if (!home) home = ".";
    snprintf(g_cache_dir, sizeof(g_cache_dir), "%s/.cinder/cache", home);
    g_cache_ready = 1;
}

static void cache_path_for(const char *name, const char *version,
                           char *out, size_t out_size) {
    installer_cache_init();
    snprintf(out, out_size, "%s/%s-%s.tgz", g_cache_dir, name, version);
    /* Replace / in scoped package names with -- */
    char *start = out + strlen(g_cache_dir) + 1;
    for (char *p = start; *p; p++) {
        if (*p == '/') *p = '-';
    }
}

/* ── Dynamic buffer ──────────────────────────────────────────────────────────── */

typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} ByteBuf;

static int bytebuf_append(ByteBuf *b, const void *ptr, size_t n) {
    if (b->len + n > b->cap) {
        size_t new_cap = b->cap == 0 ? (1 << 20) : b->cap * 2;
        while (new_cap < b->len + n) new_cap *= 2;
        unsigned char *tmp = (unsigned char *)realloc(b->data, new_cap);
        if (!tmp) return -1;
        b->data = tmp;
        b->cap  = new_cap;
    }
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    return 0;
}

static size_t curl_write_bytes(void *ptr, size_t size, size_t nmemb, void *ud) {
    ByteBuf *b = (ByteBuf *)ud;
    size_t total = size * nmemb;
    return bytebuf_append(b, ptr, total) == 0 ? total : 0;
}

/* ── mkdir -p ───────────────────────────────────────────────────────────────── */

static int mkdirp(const char *path) {
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
                if (mkdir_fn(tmp) != 0 && errno != EEXIST) return -1;
            }
            *p = '/';
        }
    }
    if (mkdir_fn(tmp) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* ── Minimal TAR parser ──────────────────────────────────────────────────────── */

/* POSIX tar header (512 bytes) */
typedef struct {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
} TarHeader;

#define TAR_BLOCK 512

static int extract_tar(const unsigned char *data, size_t len,
                       const char *dest_dir, const char *pkg_name) {
    size_t pos = 0;

    /* Precompute the canonical package root to guard against traversal */
    char pkg_root_raw[4096];
    snprintf(pkg_root_raw, sizeof(pkg_root_raw), "%s/%s", dest_dir, pkg_name);
    char pkg_root[4096];
#ifdef _WIN32
    if (!_fullpath(pkg_root, pkg_root_raw, sizeof(pkg_root)))
        return -1;
    /* Normalize to forward slashes for consistent prefix comparison */
    for (char *p = pkg_root; *p; p++) if (*p == '\\') *p = '/';
#else
    if (!realpath(pkg_root_raw, pkg_root))
        return -1;
#endif
    size_t root_len = strlen(pkg_root);

    while (pos + TAR_BLOCK <= len) {
        const TarHeader *hdr = (const TarHeader *)(data + pos);
        pos += TAR_BLOCK;

        /* End-of-archive: two zero blocks */
        if (hdr->name[0] == '\0') continue;

        /* File size from octal string */
        size_t file_size = (size_t)strtoul(hdr->size, NULL, 8);

        /* Build output path: strip first component ("package/") and
         * replace with node_modules/<pkg_name>/ */
        char rel[512];
        char full_path[4096];

        /* Reconstruct path (prefix + name for GNU/POSIX) */
        if (hdr->prefix[0]) {
            snprintf(rel, sizeof(rel), "%s/%s", hdr->prefix, hdr->name);
        } else {
            snprintf(rel, sizeof(rel), "%s", hdr->name);
        }

        /* Strip first path component (e.g., "package/") */
        const char *stripped = strchr(rel, '/');
        if (!stripped) {
            /* Skip top-level entries with no subdir */
            pos += (file_size + TAR_BLOCK - 1) / TAR_BLOCK * TAR_BLOCK;
            continue;
        }
        stripped++; /* skip the '/' */

        snprintf(full_path, sizeof(full_path), "%s/%s/%s",
                 dest_dir, pkg_name, stripped);

        /* Normalize path separators */
        for (char *p = full_path; *p; p++) if (*p == '\\') *p = '/';

        /* Security: verify the resolved path stays inside the package root.
         * This prevents path traversal attacks (e.g., "../../evil" in tarball). */
        char canonical[4096];
#ifdef _WIN32
        if (!_fullpath(canonical, full_path, sizeof(canonical))) {
            pos += (file_size + TAR_BLOCK - 1) / TAR_BLOCK * TAR_BLOCK;
            continue;
        }
        for (char *p = canonical; *p; p++) if (*p == '\\') *p = '/';
#else
        /* On non-Windows realpath requires the path to exist; use the raw path
         * with a prefix-check on the normalized string instead */
        snprintf(canonical, sizeof(canonical), "%s", full_path);
#endif
        if (strncmp(canonical, pkg_root, root_len) != 0 ||
            (canonical[root_len] != '/' && canonical[root_len] != '\0')) {
            /* Path escapes the package root — skip silently */
            pos += (file_size + TAR_BLOCK - 1) / TAR_BLOCK * TAR_BLOCK;
            continue;
        }

        if (hdr->typeflag == '5' || (file_size == 0 && rel[strlen(rel)-1] == '/')) {
            /* Directory */
            mkdirp(full_path);
        } else if (hdr->typeflag == '0' || hdr->typeflag == '\0') {
            /* Regular file — ensure parent dir exists */
            char parent[4096];
            snprintf(parent, sizeof(parent), "%s", full_path);
            char *slash = strrchr(parent, '/');
            if (slash) {
                *slash = '\0';
                mkdirp(parent);
            }

            FILE *f = fopen(full_path, "wb");
            if (f) {
                if (pos + file_size <= len)
                    fwrite(data + pos, 1, file_size, f);
                fclose(f);
            }
        }

        /* Advance past file data (rounded up to 512-byte blocks) */
        pos += (file_size + TAR_BLOCK - 1) / TAR_BLOCK * TAR_BLOCK;
    }

    return 0;
}

/* ── Decompress gzip → raw tar bytes ─────────────────────────────────────────── */

static int gunzip(const unsigned char *in, size_t in_len,
                  unsigned char **out, size_t *out_len) {
    z_stream zs;
    memset(&zs, 0, sizeof(zs));

    /* 16 + MAX_WBITS = decode gzip */
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) return -1;

    size_t cap = in_len * 4;
    unsigned char *buf = (unsigned char *)malloc(cap);
    if (!buf) { inflateEnd(&zs); return -1; }

    zs.next_in  = (Bytef *)in;
    zs.avail_in = (uInt)in_len;

    size_t total = 0;
    int ret;

    do {
        if (total >= cap) {
            cap *= 2;
            unsigned char *tmp = (unsigned char *)realloc(buf, cap);
            if (!tmp) { free(buf); inflateEnd(&zs); return -1; }
            buf = tmp;
        }
        zs.next_out  = buf + total;
        zs.avail_out = (uInt)(cap - total);

        ret = inflate(&zs, Z_NO_FLUSH);
        total = cap - zs.avail_out;

    } while (ret == Z_OK);

    inflateEnd(&zs);

    if (ret != Z_STREAM_END) {
        free(buf);
        return -1;
    }

    *out     = buf;
    *out_len = total;
    return 0;
}

/* ── Decompress + extract a .tgz buffer ──────────────────────────────────────── */

static int decompress_and_extract(const unsigned char *tgz_data, size_t tgz_len,
                                  const char *dest_dir, const char *pkg_name) {
    unsigned char *tar_data = NULL;
    size_t tar_len = 0;

    if (gunzip(tgz_data, tgz_len, &tar_data, &tar_len) != 0)
        return -1;

    mkdirp(dest_dir);
    int ret = extract_tar(tar_data, tar_len, dest_dir, pkg_name);
    free(tar_data);
    return ret;
}

/* ── Cache helpers ───────────────────────────────────────────────────────────── */

static int cache_load(const char *name, const char *version,
                      unsigned char **out, size_t *out_len) {
    char path[4096];
    cache_path_for(name, version, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return -1; }

    unsigned char *buf = (unsigned char *)malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);

    *out = buf;
    *out_len = (size_t)sz;
    return 0;
}

static void cache_store(const char *name, const char *version,
                        const unsigned char *data, size_t len) {
    installer_cache_init();
    mkdirp(g_cache_dir);

    char path[4096];
    cache_path_for(name, version, path, sizeof(path));

    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(data, 1, len, f);
        fclose(f);
    }
}

/* ── installer_download_and_extract (single, original API) ───────────────────── */

int installer_download_and_extract(const char *pkg_name,
                                   const char *version,
                                   const char *tarball_url,
                                   const char *dest_dir) {
    /* Try cache first */
    unsigned char *cached = NULL;
    size_t cached_len = 0;
    if (cache_load(pkg_name, version, &cached, &cached_len) == 0) {
        int ret = decompress_and_extract(cached, cached_len, dest_dir, pkg_name);
        free(cached);
        return ret;
    }

    /* ── Download ─── */
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    ByteBuf raw = {0};
    curl_easy_setopt(curl, CURLOPT_URL, tarball_url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_bytes);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &raw);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "cinder/0.1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    cinder_curl_ssl_setup(curl);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "cinder: download failed for %s: %s\n",
                pkg_name, curl_easy_strerror(res));
        free(raw.data);
        return -1;
    }

    /* Save to cache */
    cache_store(pkg_name, version, raw.data, raw.len);

    int ret = decompress_and_extract(raw.data, raw.len, dest_dir, pkg_name);
    free(raw.data);
    return ret;
}

/* ── Parallel bulk installer (curl_multi) ────────────────────────────────────── */

typedef struct {
    InstallTask *task;
    ByteBuf      buf;
    int          from_cache;
} MultiCtx;

int installer_download_multi(InstallTask *tasks, int count, int max_parallel) {
    if (count == 0) return 0;
    if (max_parallel <= 0) max_parallel = INSTALL_MAX_PARALLEL;

    installer_cache_init();

    /* First pass: serve from cache (no network needed) */
    int ok_count = 0;
    int need_download = 0;
    int *need_dl = (int *)calloc((size_t)count, sizeof(int));
    if (!need_dl) return 0;

    for (int i = 0; i < count; i++) {
        unsigned char *cached = NULL;
        size_t cached_len = 0;
        if (cache_load(tasks[i].pkg_name, tasks[i].version,
                       &cached, &cached_len) == 0) {
            if (decompress_and_extract(cached, cached_len,
                                       tasks[i].dest_dir,
                                       tasks[i].pkg_name) == 0) {
                tasks[i].result = 0;
                ok_count++;
            } else {
                tasks[i].result = -1;
                need_dl[i] = 1;
                need_download++;
            }
            free(cached);
        } else {
            need_dl[i] = 1;
            need_download++;
        }
    }

    if (need_download == 0) {
        free(need_dl);
        return ok_count;
    }

    /* Second pass: parallel download via curl_multi */
    CURLM *multi = curl_multi_init();
    if (!multi) { free(need_dl); return ok_count; }

    curl_multi_setopt(multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, (long)max_parallel);
    curl_multi_setopt(multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);

    MultiCtx *ctxs = (MultiCtx *)calloc((size_t)count, sizeof(MultiCtx));
    if (!ctxs) { curl_multi_cleanup(multi); free(need_dl); return ok_count; }

    int active_count = 0;
    for (int i = 0; i < count; i++) {
        if (!need_dl[i]) continue;

        CURL *easy = curl_easy_init();
        if (!easy) { tasks[i].result = -1; continue; }

        ctxs[i].task = &tasks[i];
        ctxs[i].buf  = (ByteBuf){0};
        ctxs[i].from_cache = 0;

        curl_easy_setopt(easy, CURLOPT_URL, tasks[i].tarball_url);
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, curl_write_bytes);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, &ctxs[i].buf);
        curl_easy_setopt(easy, CURLOPT_USERAGENT, "cinder/0.1.0");
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(easy, CURLOPT_TIMEOUT, 60L);
        curl_easy_setopt(easy, CURLOPT_PRIVATE, &ctxs[i]);
        /* Enable connection reuse and HTTP/2 */
        curl_easy_setopt(easy, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
        cinder_curl_ssl_setup(easy);

        curl_multi_add_handle(multi, easy);
        active_count++;
    }

    /* Event loop: process transfers and extract as they complete */
    int still_running = 0;
    do {
        CURLMcode mc = curl_multi_perform(multi, &still_running);
        if (mc != CURLM_OK) break;

        /* Process completed transfers */
        CURLMsg *msg;
        int msgs_left;
        while ((msg = curl_multi_info_read(multi, &msgs_left))) {
            if (msg->msg != CURLMSG_DONE) continue;

            CURL *easy = msg->easy_handle;
            MultiCtx *ctx = NULL;
            curl_easy_getinfo(easy, CURLINFO_PRIVATE, &ctx);

            if (msg->data.result == CURLE_OK && ctx) {
                /* Cache the downloaded tarball */
                cache_store(ctx->task->pkg_name, ctx->task->version,
                            ctx->buf.data, ctx->buf.len);

                if (decompress_and_extract(ctx->buf.data, ctx->buf.len,
                                           ctx->task->dest_dir,
                                           ctx->task->pkg_name) == 0) {
                    ctx->task->result = 0;
                    ok_count++;
                } else {
                    ctx->task->result = -1;
                    fprintf(stderr, "  error  Failed to extract '%s'\n",
                            ctx->task->pkg_name);
                }
            } else {
                if (ctx) {
                    ctx->task->result = -1;
                    fprintf(stderr, "  error  Download failed for '%s'\n",
                            ctx->task->pkg_name);
                }
            }

            if (ctx) free(ctx->buf.data);
            curl_multi_remove_handle(multi, easy);
            curl_easy_cleanup(easy);
        }

        if (still_running)
            curl_multi_wait(multi, NULL, 0, 200, NULL);

    } while (still_running);

    free(ctxs);
    free(need_dl);
    curl_multi_cleanup(multi);
    return ok_count;
}
