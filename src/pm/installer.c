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

/* ── Streaming tar+gzip state machine ─────────────────────────────────────── */

typedef enum { TAR_NEED_HEADER, TAR_WRITE_DATA, TAR_SKIP_DATA } TarStreamState;

typedef struct {
    TarStreamState state;
    unsigned char  hdr_buf[TAR_BLOCK];
    size_t         hdr_pos;
    FILE          *out_file;
    size_t         data_remaining;
    size_t         pad_remaining;
    const char    *dest_dir;   /* points into task struct (stays valid) */
    const char    *pkg_name;   /* points into task struct (stays valid) */
    char           pkg_root[4096];
    size_t         pkg_root_len;
    int            error;
    int            done;
} TarStream;

/* Forward declarations */
static void process_tar_header(TarStream *ts);
static int  tar_stream_feed(TarStream *ts, const unsigned char *data, size_t len);

static void process_tar_header(TarStream *ts) {
    const TarHeader *hdr = (const TarHeader *)ts->hdr_buf;

    if (hdr->name[0] == '\0') { ts->done = 1; return; }

    size_t file_size = (size_t)strtoul(hdr->size, NULL, 8);
    size_t padded    = (file_size + (TAR_BLOCK - 1)) / TAR_BLOCK * TAR_BLOCK;
    size_t pad       = padded - file_size;

    char rel[512];
    if (hdr->prefix[0])
        snprintf(rel, sizeof(rel), "%s/%s", hdr->prefix, hdr->name);
    else
        snprintf(rel, sizeof(rel), "%s", hdr->name);

    /* Strip leading path component ("package/") */
    const char *stripped = strchr(rel, '/');
    if (!stripped || stripped[1] == '\0') {
        ts->data_remaining = 0;
        ts->pad_remaining  = padded;
        ts->state          = TAR_SKIP_DATA;
        return;
    }
    stripped++;

    char full_path[4096];
    snprintf(full_path, sizeof(full_path), "%s/%s/%s",
             ts->dest_dir, ts->pkg_name, stripped);
    for (char *p = full_path; *p; p++) if (*p == '\\') *p = '/';

    /* Security: verify resolved path stays inside pkg_root */
    char canonical[4096];
#ifdef _WIN32
    if (!_fullpath(canonical, full_path, sizeof(canonical))) {
        ts->data_remaining = 0; ts->pad_remaining = padded;
        ts->state = TAR_SKIP_DATA; return;
    }
    for (char *p = canonical; *p; p++) if (*p == '\\') *p = '/';
#else
    snprintf(canonical, sizeof(canonical), "%s", full_path);
#endif
    if (strncmp(canonical, ts->pkg_root, ts->pkg_root_len) != 0 ||
        (canonical[ts->pkg_root_len] != '/' && canonical[ts->pkg_root_len] != '\0')) {
        ts->data_remaining = 0; ts->pad_remaining = padded;
        ts->state = TAR_SKIP_DATA; return;
    }

    if (hdr->typeflag == '5' ||
        (file_size == 0 && rel[strlen(rel)-1] == '/')) {
        /* Directory */
        mkdirp(full_path);
        ts->data_remaining = 0; ts->pad_remaining = padded;
        ts->state = TAR_SKIP_DATA;
    } else if (hdr->typeflag == '0' || hdr->typeflag == '\0') {
        /* Regular file */
        char parent[4096];
        snprintf(parent, sizeof(parent), "%s", full_path);
        char *slash = strrchr(parent, '/');
        if (slash) { *slash = '\0'; mkdirp(parent); }
        ts->out_file       = fopen(full_path, "wb");
        ts->data_remaining = file_size;
        ts->pad_remaining  = pad;
        if (file_size == 0) {
            if (ts->out_file) { fclose(ts->out_file); ts->out_file = NULL; }
            ts->state = TAR_SKIP_DATA;
        } else {
            ts->state = TAR_WRITE_DATA;
        }
    } else {
        /* Symlink, device, etc. — skip */
        ts->data_remaining = 0; ts->pad_remaining = padded;
        ts->state = TAR_SKIP_DATA;
    }
}

static int tar_stream_feed(TarStream *ts, const unsigned char *data, size_t len) {
    size_t pos = 0;
    while (pos < len && !ts->done && !ts->error) {
        switch (ts->state) {
            case TAR_NEED_HEADER: {
                size_t need  = TAR_BLOCK - ts->hdr_pos;
                size_t avail = len - pos;
                size_t copy  = need < avail ? need : avail;
                memcpy(ts->hdr_buf + ts->hdr_pos, data + pos, copy);
                ts->hdr_pos += copy;
                pos += copy;
                if (ts->hdr_pos == TAR_BLOCK) {
                    ts->hdr_pos = 0;
                    process_tar_header(ts);
                }
                break;
            }
            case TAR_WRITE_DATA: {
                size_t avail    = len - pos;
                size_t to_write = ts->data_remaining < avail ? ts->data_remaining : avail;
                if (ts->out_file) {
                    if (fwrite(data + pos, 1, to_write, ts->out_file) != to_write)
                        ts->error = 1;
                }
                ts->data_remaining -= to_write;
                pos += to_write;
                if (ts->data_remaining == 0) {
                    if (ts->out_file) { fclose(ts->out_file); ts->out_file = NULL; }
                    ts->state = ts->pad_remaining > 0 ? TAR_SKIP_DATA : TAR_NEED_HEADER;
                }
                break;
            }
            case TAR_SKIP_DATA: {
                size_t avail   = len - pos;
                size_t to_skip = ts->pad_remaining < avail ? ts->pad_remaining : avail;
                ts->pad_remaining -= to_skip;
                pos += to_skip;
                if (ts->pad_remaining == 0) ts->state = TAR_NEED_HEADER;
                break;
            }
        }
    }
    return ts->error;
}

typedef struct {
    z_stream      zs;
    TarStream     ts;
    unsigned char inflate_buf[65536];
    FILE         *cache_file;  /* simultaneously write raw bytes to cache */
    char          cache_path[4096]; /* for cleanup on error */
    int           error;
} StreamCtx;

static int stream_ctx_init(StreamCtx *sc, const char *dest_dir,
                           const char *pkg_name, const char *cache_path) {
    memset(sc, 0, sizeof(*sc));
    if (inflateInit2(&sc->zs, 16 + MAX_WBITS) != Z_OK) return -1;
    sc->ts.state    = TAR_NEED_HEADER;
    sc->ts.dest_dir = dest_dir;
    sc->ts.pkg_name = pkg_name;
    /* Precompute canonical package root */
    char raw_root[4096];
    snprintf(raw_root, sizeof(raw_root), "%s/%s", dest_dir, pkg_name);
#ifdef _WIN32
    if (!_fullpath(sc->ts.pkg_root, raw_root, sizeof(sc->ts.pkg_root))) {
        inflateEnd(&sc->zs); return -1;
    }
    for (char *p = sc->ts.pkg_root; *p; p++) if (*p == '\\') *p = '/';
#else
    snprintf(sc->ts.pkg_root, sizeof(sc->ts.pkg_root), "%s", raw_root);
#endif
    sc->ts.pkg_root_len = strlen(sc->ts.pkg_root);
    if (cache_path) {
        installer_cache_init();
        mkdirp(g_cache_dir);
        strncpy(sc->cache_path, cache_path, sizeof(sc->cache_path) - 1);
        sc->cache_file = fopen(cache_path, "wb"); /* NULL is OK; just won't cache */
    }
    return 0;
}

static void stream_ctx_cleanup(StreamCtx *sc, int success) {
    if (sc->ts.out_file) { fclose(sc->ts.out_file); sc->ts.out_file = NULL; }
    inflateEnd(&sc->zs);
    if (sc->cache_file) {
        fclose(sc->cache_file);
        sc->cache_file = NULL;
        /* Remove partial cache file on failure */
        if (!success && sc->cache_path[0]) remove(sc->cache_path);
    }
}

static size_t streaming_write_cb(void *ptr, size_t size, size_t nmemb, void *ud) {
    StreamCtx *sc = (StreamCtx *)ud;
    size_t total = size * nmemb;
    if (sc->error) return 0;

    /* Mirror raw compressed bytes to cache file */
    if (sc->cache_file) fwrite(ptr, 1, total, sc->cache_file);

    /* Inflate + feed to tar state machine */
    sc->zs.next_in  = (Bytef *)ptr;
    sc->zs.avail_in = (uInt)total;
    while (sc->zs.avail_in > 0) {
        sc->zs.next_out  = sc->inflate_buf;
        sc->zs.avail_out = (uInt)sizeof(sc->inflate_buf);
        int ret  = inflate(&sc->zs, Z_NO_FLUSH);
        size_t have = sizeof(sc->inflate_buf) - sc->zs.avail_out;
        if (have > 0 && tar_stream_feed(&sc->ts, sc->inflate_buf, have) != 0) {
            sc->error = 1; return 0;
        }
        if (ret == Z_STREAM_END) break;
        if (ret != Z_OK) { sc->error = 1; return 0; }
    }
    return total;
}

/* Stream-extract from an already-cached .tgz file (zero extra heap alloc) */
static int extract_from_cache(const char *cache_path, const char *dest_dir,
                               const char *pkg_name) {
    FILE *f = fopen(cache_path, "rb");
    if (!f) return -1;
    StreamCtx sc;
    if (stream_ctx_init(&sc, dest_dir, pkg_name, NULL) != 0) { fclose(f); return -1; }
    unsigned char buf[65536];
    size_t n;
    while (!sc.error && (n = fread(buf, 1, sizeof(buf), f)) > 0)
        streaming_write_cb(buf, 1, n, &sc);
    fclose(f);
    int ok = !sc.error;
    stream_ctx_cleanup(&sc, ok);
    return ok ? 0 : -1;
}

/* ── installer_download_and_extract (single, original API) ───────────────────── */

int installer_download_and_extract(const char *pkg_name, const char *version,
                                   const char *tarball_url, const char *dest_dir) {
    installer_cache_init();

    /* Try cache first */
    char cache_path[4096];
    cache_path_for(pkg_name, version, cache_path, sizeof(cache_path));
    {
        FILE *probe = fopen(cache_path, "rb");
        if (probe) {
            fclose(probe);
            return extract_from_cache(cache_path, dest_dir, pkg_name);
        }
    }

    /* Not cached — stream download + decompress + extract */
    StreamCtx sc;
    if (stream_ctx_init(&sc, dest_dir, pkg_name, cache_path) != 0) return -1;

    CURL *curl = curl_easy_init();
    if (!curl) { stream_ctx_cleanup(&sc, 0); return -1; }

    mkdirp(dest_dir);
    curl_easy_setopt(curl, CURLOPT_URL, tarball_url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, streaming_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sc);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "cinder/0.1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    cinder_curl_ssl_setup(curl);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    int ok = (res == CURLE_OK && !sc.error);
    if (!ok && res != CURLE_OK)
        fprintf(stderr, "cinder: download failed for %s: %s\n",
                pkg_name, curl_easy_strerror(res));
    stream_ctx_cleanup(&sc, ok);
    return ok ? 0 : -1;
}

/* ── Parallel bulk installer (curl_multi) ────────────────────────────────────── */

typedef struct {
    InstallTask *task;
    StreamCtx    sc;
    char         cache_path[4096];
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
        char cp[4096];
        cache_path_for(tasks[i].pkg_name, tasks[i].version, cp, sizeof(cp));
        FILE *probe = fopen(cp, "rb");
        if (probe) {
            fclose(probe);
            if (extract_from_cache(cp, tasks[i].dest_dir, tasks[i].pkg_name) == 0) {
                tasks[i].result = 0;
                ok_count++;
            } else {
                tasks[i].result = -1;
                need_dl[i] = 1;
                need_download++;
            }
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

        ctxs[i].task       = &tasks[i];
        ctxs[i].from_cache = 0;
        cache_path_for(tasks[i].pkg_name, tasks[i].version,
                       ctxs[i].cache_path, sizeof(ctxs[i].cache_path));

        mkdirp(tasks[i].dest_dir);
        if (stream_ctx_init(&ctxs[i].sc, tasks[i].dest_dir, tasks[i].pkg_name,
                            ctxs[i].cache_path) != 0) {
            curl_easy_cleanup(easy);
            tasks[i].result = -1;
            continue;
        }

        curl_easy_setopt(easy, CURLOPT_URL, tasks[i].tarball_url);
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, streaming_write_cb);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, &ctxs[i].sc);
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
    (void)active_count;

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

            if (msg->data.result == CURLE_OK && ctx && !ctx->sc.error) {
                ctx->task->result = 0;
                ok_count++;
                stream_ctx_cleanup(&ctx->sc, 1);
            } else {
                if (ctx) {
                    ctx->task->result = -1;
                    fprintf(stderr, "  error  Download/extract failed for '%s'\n",
                            ctx->task->pkg_name);
                    stream_ctx_cleanup(&ctx->sc, 0);
                }
            }

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
