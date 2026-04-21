/*
 * cinder — lock file (cinder.lock)
 * Simple JSON format: { "packages": { "<name>": { "version", "tarball", "integrity" } } }
 */
#include "lockfile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

/* ── lockfile_load ───────────────────────────────────────────────────────────── */

CinderLockFile *lockfile_load(void) {
    CinderLockFile *lf = (CinderLockFile *)calloc(1, sizeof(CinderLockFile));
    if (!lf) return NULL;

    FILE *f = fopen(LOCKFILE_NAME, "r");
    if (!f) return lf; /* fresh empty lock */

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return lf; }
    fread(buf, 1, (size_t)size, f);
    buf[size] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return lf;

    cJSON *pkgs = cJSON_GetObjectItemCaseSensitive(root, "packages");
    if (pkgs) {
        cJSON *item;
        cJSON_ArrayForEach(item, pkgs) {
            cJSON *jver  = cJSON_GetObjectItemCaseSensitive(item, "version");
            cJSON *jurl  = cJSON_GetObjectItemCaseSensitive(item, "tarball");
            cJSON *jint  = cJSON_GetObjectItemCaseSensitive(item, "integrity");

            LockEntry *e = (LockEntry *)calloc(1, sizeof(LockEntry));
            if (!e) break;
            e->name        = strdup(item->string);
            e->version     = cJSON_IsString(jver) ? strdup(jver->valuestring) : strdup("");
            e->tarball_url = cJSON_IsString(jurl) ? strdup(jurl->valuestring) : strdup("");
            e->integrity   = cJSON_IsString(jint) ? strdup(jint->valuestring) : strdup("");
            e->next = lf->head;
            lf->head = e;
        }
    }

    cJSON_Delete(root);
    return lf;
}

/* ── lockfile_find ───────────────────────────────────────────────────────────── */

LockEntry *lockfile_find(CinderLockFile *lf, const char *name) {
    for (LockEntry *e = lf->head; e; e = e->next) {
        if (strcmp(e->name, name) == 0) return e;
    }
    return NULL;
}

/* ── lockfile_upsert ─────────────────────────────────────────────────────────── */

void lockfile_upsert(CinderLockFile *lf, const char *name, const char *version,
                     const char *tarball_url, const char *integrity) {
    LockEntry *e = lockfile_find(lf, name);
    if (e) {
        free(e->version);     e->version     = strdup(version);
        free(e->tarball_url); e->tarball_url = strdup(tarball_url);
        free(e->integrity);   e->integrity   = strdup(integrity);
        return;
    }
    e = (LockEntry *)calloc(1, sizeof(LockEntry));
    if (!e) return;
    e->name        = strdup(name);
    e->version     = strdup(version);
    e->tarball_url = strdup(tarball_url);
    e->integrity   = strdup(integrity);
    e->next = lf->head;
    lf->head = e;
}

/* ── lockfile_save ───────────────────────────────────────────────────────────── */

int lockfile_save(CinderLockFile *lf) {
    cJSON *root = cJSON_CreateObject();
    cJSON *pkgs = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "packages", pkgs);

    for (LockEntry *e = lf->head; e; e = e->next) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "version",   e->version     ? e->version     : "");
        cJSON_AddStringToObject(entry, "tarball",   e->tarball_url ? e->tarball_url : "");
        cJSON_AddStringToObject(entry, "integrity", e->integrity   ? e->integrity   : "");
        cJSON_AddItemToObject(pkgs, e->name, entry);
    }

    char *str = cJSON_Print(root);
    cJSON_Delete(root);
    if (!str) return -1;

    FILE *f = fopen(LOCKFILE_NAME, "w");
    if (!f) { free(str); return -1; }
    fputs(str, f);
    fputs("\n", f);
    fclose(f);
    free(str);
    return 0;
}

/* ── lockfile_free ───────────────────────────────────────────────────────────── */

void lockfile_free(CinderLockFile *lf) {
    if (!lf) return;
    LockEntry *e = lf->head;
    while (e) {
        LockEntry *next = e->next;
        free(e->name);
        free(e->version);
        free(e->tarball_url);
        free(e->integrity);
        free(e);
        e = next;
    }
    free(lf);
}
