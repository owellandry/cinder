/*
 * cinder — package installer
 * Downloads .tgz tarballs from npm and extracts them to node_modules/
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

/* ── installer_download_and_extract ──────────────────────────────────────────── */

int installer_download_and_extract(const char *pkg_name,
                                   const char *version,
                                   const char *tarball_url,
                                   const char *dest_dir) {
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

    /* ── Decompress .tgz → tar ─── */
    unsigned char *tar_data = NULL;
    size_t tar_len = 0;

    if (gunzip(raw.data, raw.len, &tar_data, &tar_len) != 0) {
        fprintf(stderr, "cinder: failed to decompress package %s\n", pkg_name);
        free(raw.data);
        return -1;
    }
    free(raw.data);

    /* ── Ensure dest_dir exists ─── */
    mkdirp(dest_dir);

    /* ── Extract tar ─── */
    int ret = extract_tar(tar_data, tar_len, dest_dir, pkg_name);
    free(tar_data);

    return ret;
}
