/*
 * cinder — semver parser and range resolver
 */
#include "resolver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── semver_parse ───────────────────────────────────────────────────────────── */

int semver_parse(const char *str, Semver *out) {
    if (!str || !out) return -1;

    /* Skip leading v */
    if (*str == 'v' || *str == 'V') str++;

    memset(out, 0, sizeof(Semver));

    /* major */
    char *end;
    out->major = (int)strtol(str, &end, 10);
    if (end == str) return -1;
    str = end;
    if (*str != '.') return (*str == '\0') ? 0 : -1;
    str++;

    /* minor */
    out->minor = (int)strtol(str, &end, 10);
    if (end == str) return -1;
    str = end;
    if (*str != '.') return (*str == '\0') ? 0 : -1;
    str++;

    /* patch */
    out->patch = (int)strtol(str, &end, 10);
    str = end;

    /* pre-release */
    if (*str == '-') {
        str++;
        size_t i = 0;
        while (*str && *str != '+' && i < sizeof(out->pre) - 1)
            out->pre[i++] = *str++;
        out->pre[i] = '\0';
    }

    return 0;
}

/* ── semver_cmp ─────────────────────────────────────────────────────────────── */

int semver_cmp(const Semver *a, const Semver *b) {
    if (a->major != b->major) return a->major - b->major;
    if (a->minor != b->minor) return a->minor - b->minor;
    if (a->patch != b->patch) return a->patch - b->patch;
    /* pre-release: no pre > has pre */
    if (a->pre[0] == '\0' && b->pre[0] != '\0') return  1;
    if (a->pre[0] != '\0' && b->pre[0] == '\0') return -1;
    return strcmp(a->pre, b->pre);
}

/* ── Strip range prefix to get base version string ──────────────────────────── */

static const char *strip_prefix(const char *range) {
    while (*range == '^' || *range == '~' || *range == '=' ||
           *range == '>' || *range == '<' || *range == ' ')
        range++;
    if (*range == 'v' || *range == 'V') range++;
    return range;
}

/* ── semver_satisfies ───────────────────────────────────────────────────────── */

int semver_satisfies(const char *version, const char *range) {
    if (!version || !range) return 0;

    /* latest / * / "" → always match */
    if (strcmp(range, "latest") == 0 ||
        strcmp(range, "*")      == 0 ||
        strcmp(range, "")       == 0)
        return 1;

    /* Skip optional spaces */
    while (*range == ' ') range++;

    Semver ver;
    if (semver_parse(version, &ver) != 0) return 0;

    /* ── Caret range ^X.Y.Z ─────────────────────────────────────────────── */
    if (range[0] == '^') {
        const char *base = strip_prefix(range);
        Semver lo;
        if (semver_parse(base, &lo) != 0) return 0;

        /* ^1.2.3 := >=1.2.3 <2.0.0 */
        /* ^0.2.3 := >=0.2.3 <0.3.0 */
        /* ^0.0.3 := >=0.0.3 <0.0.4 */
        if (semver_cmp(&ver, &lo) < 0) return 0;

        if (lo.major > 0) {
            return ver.major == lo.major;
        } else if (lo.minor > 0) {
            return ver.major == 0 && ver.minor == lo.minor;
        } else {
            return ver.major == 0 && ver.minor == 0 && ver.patch == lo.patch;
        }
    }

    /* ── Tilde range ~X.Y.Z ─────────────────────────────────────────────── */
    if (range[0] == '~') {
        const char *base = strip_prefix(range);
        Semver lo;
        if (semver_parse(base, &lo) != 0) return 0;

        /* ~1.2.3 := >=1.2.3 <1.3.0 */
        if (semver_cmp(&ver, &lo) < 0) return 0;
        return ver.major == lo.major && ver.minor == lo.minor;
    }

    /* ── >= range ────────────────────────────────────────────────────────── */
    if (range[0] == '>' && range[1] == '=') {
        Semver lo;
        if (semver_parse(strip_prefix(range + 2), &lo) != 0) return 0;
        return semver_cmp(&ver, &lo) >= 0;
    }

    /* ── > range ─────────────────────────────────────────────────────────── */
    if (range[0] == '>' && range[1] != '=') {
        Semver lo;
        if (semver_parse(strip_prefix(range + 1), &lo) != 0) return 0;
        return semver_cmp(&ver, &lo) > 0;
    }

    /* ── <= range ────────────────────────────────────────────────────────── */
    if (range[0] == '<' && range[1] == '=') {
        Semver hi;
        if (semver_parse(strip_prefix(range + 2), &hi) != 0) return 0;
        return semver_cmp(&ver, &hi) <= 0;
    }

    /* ── < range ─────────────────────────────────────────────────────────── */
    if (range[0] == '<' && range[1] != '=') {
        Semver hi;
        if (semver_parse(strip_prefix(range + 1), &hi) != 0) return 0;
        return semver_cmp(&ver, &hi) < 0;
    }

    /* ── Exact match (= or bare) ─────────────────────────────────────────── */
    {
        const char *base = strip_prefix(range);
        Semver exact;
        if (semver_parse(base, &exact) != 0) return 0;
        return semver_cmp(&ver, &exact) == 0;
    }
}
