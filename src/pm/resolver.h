#ifndef CINDER_RESOLVER_H
#define CINDER_RESOLVER_H

/* Parsed semver: major.minor.patch */
typedef struct Semver {
    int major, minor, patch;
    char pre[64];  /* pre-release tag, empty if none */
} Semver;

/* Parse "1.2.3", "1.2.3-alpha.1" → Semver. Returns 0 on success. */
int semver_parse(const char *str, Semver *out);

/* Compare two semvers: <0, 0, >0 */
int semver_cmp(const Semver *a, const Semver *b);

/* Returns 1 if version satisfies range, 0 otherwise.
 * Supported range types: exact "1.2.3", caret "^1.2.3", tilde "~1.2.3",
 * "latest", "*", ">= x.x.x", "<= x.x.x" */
int semver_satisfies(const char *version, const char *range);

#endif /* CINDER_RESOLVER_H */
