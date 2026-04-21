#ifndef CINDER_LOCKFILE_H
#define CINDER_LOCKFILE_H

#define LOCKFILE_NAME "cinder.lock"

typedef struct LockEntry {
    char *name;
    char *version;
    char *tarball_url;
    char *integrity;
    struct LockEntry *next;
} LockEntry;

typedef struct CinderLockFile {
    LockEntry *head;
} CinderLockFile;

/* Load cinder.lock from cwd, or return empty lock if not found. */
CinderLockFile *lockfile_load(void);

/* Add or update an entry in the lock */
void lockfile_upsert(CinderLockFile *lf, const char *name, const char *version,
                     const char *tarball_url, const char *integrity);

/* Check if a package is already locked */
LockEntry *lockfile_find(CinderLockFile *lf, const char *name);

/* Write cinder.lock to cwd */
int lockfile_save(CinderLockFile *lf);

void lockfile_free(CinderLockFile *lf);

#endif /* CINDER_LOCKFILE_H */
