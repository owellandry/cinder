#ifndef CINDER_INSTALLER_H
#define CINDER_INSTALLER_H

/* Download a .tgz tarball from url and extract it to dest_dir/pkg_name/.
 * dest_dir is typically "node_modules".
 * Returns 0 on success. */
int installer_download_and_extract(const char *pkg_name,
                                   const char *version,
                                   const char *tarball_url,
                                   const char *dest_dir);

/* ── Parallel bulk installer ─────────────────────────────────────────────────── */

#define INSTALL_MAX_PARALLEL 8

typedef struct {
    const char *pkg_name;
    const char *version;
    const char *tarball_url;
    const char *dest_dir;
    int         result;      /* 0 = success, -1 = failure (set by callee) */
} InstallTask;

/* Download and extract multiple packages in parallel using curl_multi.
 * Returns number of successfully installed packages. */
int installer_download_multi(InstallTask *tasks, int count, int max_parallel);

/* Ensure the global tarball cache directory exists. */
void installer_cache_init(void);

#endif /* CINDER_INSTALLER_H */
