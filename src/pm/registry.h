#ifndef CINDER_REGISTRY_H
#define CINDER_REGISTRY_H

#define NPM_REGISTRY "https://registry.npmjs.org"

typedef struct PkgVersion {
    char *version;     /* exact version string */
    char *tarball_url; /* dist.tarball */
    char *integrity;   /* dist.integrity (sha512) */
} PkgVersion;

typedef struct PkgInfo {
    char *name;
    char *description;
    char *latest;
    PkgVersion *versions;
    int version_count;
} PkgInfo;

/* Fetch package metadata from npm registry.
 * Returns allocated PkgInfo or NULL on error. Caller must call registry_pkg_free(). */
PkgInfo *registry_fetch(const char *name);

/* Fetch metadata for multiple packages in parallel using curl_multi.
 * results[i] is set to PkgInfo* or NULL on failure.
 * Returns number of successful fetches. */
int registry_fetch_multi(const char **names, int count,
                         PkgInfo **results, int max_parallel);

/* Find best matching version for a semver range string.
 * Returns index into info->versions, or -1 if not found. */
int registry_resolve_version(PkgInfo *info, const char *range);

void registry_pkg_free(PkgInfo *info);

#endif /* CINDER_REGISTRY_H */
