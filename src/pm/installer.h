#ifndef CINDER_INSTALLER_H
#define CINDER_INSTALLER_H

/* Download a .tgz tarball from url and extract it to dest_dir/pkg_name/.
 * dest_dir is typically "node_modules".
 * Returns 0 on success. */
int installer_download_and_extract(const char *pkg_name,
                                   const char *version,
                                   const char *tarball_url,
                                   const char *dest_dir);

#endif /* CINDER_INSTALLER_H */
