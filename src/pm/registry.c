/*
 * cinder — npm registry client
 * Fetches package metadata using libcurl + cJSON
 */
#include "registry.h"
#include "resolver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include "cJSON.h"
#include "../util/curl_util.h"

/* ── Dynamic buffer for curl writes ─────────────────────────────────────────── */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} DynBuf;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    DynBuf *buf = (DynBuf *)userdata;
    size_t total = size * nmemb;

    if (buf->len + total + 1 > buf->cap) {
        size_t new_cap = buf->cap == 0 ? 4096 : buf->cap * 2;
        while (new_cap < buf->len + total + 1) new_cap *= 2;
        char *tmp = (char *)realloc(buf->data, new_cap);
        if (!tmp) return 0;
        buf->data = tmp;
        buf->cap  = new_cap;
    }

    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

/* ── HTTP GET helper ─────────────────────────────────────────────────────────── */

static char *http_get(const char *url) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    DynBuf buf = {0};

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "cinder/0.1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    cinder_curl_ssl_setup(curl);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code < 200 || http_code >= 300) {
        free(buf.data);
        return NULL;
    }

    return buf.data; /* caller must free */
}

/* ── registry_fetch ──────────────────────────────────────────────────────────── */

PkgInfo *registry_fetch(const char *name) {
    char url[2048];
    snprintf(url, sizeof(url), "%s/%s", NPM_REGISTRY, name);

    char *json_str = http_get(url);
    if (!json_str) {
        fprintf(stderr, "cinder: failed to fetch package '%s' from registry\n", name);
        return NULL;
    }

    cJSON *root = cJSON_Parse(json_str);
    free(json_str);
    if (!root) {
        fprintf(stderr, "cinder: invalid JSON response for package '%s'\n", name);
        return NULL;
    }

    PkgInfo *info = (PkgInfo *)calloc(1, sizeof(PkgInfo));
    if (!info) { cJSON_Delete(root); return NULL; }

    /* name */
    cJSON *jname = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (cJSON_IsString(jname)) info->name = strdup(jname->valuestring);

    /* description */
    cJSON *jdesc = cJSON_GetObjectItemCaseSensitive(root, "description");
    if (cJSON_IsString(jdesc)) info->description = strdup(jdesc->valuestring);

    /* dist-tags.latest */
    cJSON *jtags   = cJSON_GetObjectItemCaseSensitive(root, "dist-tags");
    cJSON *jlatest = jtags ? cJSON_GetObjectItemCaseSensitive(jtags, "latest") : NULL;
    if (cJSON_IsString(jlatest)) info->latest = strdup(jlatest->valuestring);

    /* versions */
    cJSON *jversions = cJSON_GetObjectItemCaseSensitive(root, "versions");
    if (jversions) {
        int count = cJSON_GetArraySize(jversions);
        info->versions = (PkgVersion *)calloc(count, sizeof(PkgVersion));
        info->version_count = 0;

        cJSON *jver;
        cJSON_ArrayForEach(jver, jversions) {
            const char *ver_str = jver->string;
            cJSON *jdist = cJSON_GetObjectItemCaseSensitive(jver, "dist");
            if (!jdist) continue;

            cJSON *jtarball   = cJSON_GetObjectItemCaseSensitive(jdist, "tarball");
            cJSON *jintegrity = cJSON_GetObjectItemCaseSensitive(jdist, "integrity");

            PkgVersion *pv = &info->versions[info->version_count++];
            pv->version     = strdup(ver_str);
            pv->tarball_url = cJSON_IsString(jtarball)
                                ? strdup(jtarball->valuestring) : strdup("");
            pv->integrity   = cJSON_IsString(jintegrity)
                                ? strdup(jintegrity->valuestring) : strdup("");
        }
    }

    cJSON_Delete(root);
    return info;
}

/* ── registry_resolve_version ────────────────────────────────────────────────── */

int registry_resolve_version(PkgInfo *info, const char *range) {
    if (!info || !range) return -1;

    /* Special case: "latest" or "*" → use dist-tags.latest */
    if (strcmp(range, "latest") == 0 || strcmp(range, "*") == 0) {
        if (info->latest) {
            for (int i = 0; i < info->version_count; i++) {
                if (strcmp(info->versions[i].version, info->latest) == 0)
                    return i;
            }
        }
        return info->version_count > 0 ? info->version_count - 1 : -1;
    }

    /* Find highest version satisfying the range */
    int best = -1;
    Semver best_sv = {0};

    for (int i = 0; i < info->version_count; i++) {
        const char *ver = info->versions[i].version;
        if (!semver_satisfies(ver, range)) continue;

        Semver sv;
        if (semver_parse(ver, &sv) != 0) continue;

        /* Skip pre-releases unless range explicitly references one */
        if (sv.pre[0] != '\0') continue;

        if (best < 0 || semver_cmp(&sv, &best_sv) > 0) {
            best    = i;
            best_sv = sv;
        }
    }

    return best;
}

/* ── registry_pkg_free ────────────────────────────────────────────────────────── */

void registry_pkg_free(PkgInfo *info) {
    if (!info) return;
    free(info->name);
    free(info->description);
    free(info->latest);
    for (int i = 0; i < info->version_count; i++) {
        free(info->versions[i].version);
        free(info->versions[i].tarball_url);
        free(info->versions[i].integrity);
    }
    free(info->versions);
    free(info);
}
