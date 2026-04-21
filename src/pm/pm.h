#ifndef CINDER_PM_H
#define CINDER_PM_H

/* Initialize a new project (creates package.json) */
int pm_init(void);

/* Install all dependencies listed in package.json */
int pm_install(void);

/* Add a package (name optionally includes @version) */
int pm_add(const char *pkg_spec, int is_dev);

/* Remove a package */
int pm_remove(const char *pkg_name);

/* Run a script defined in package.json */
int pm_run_script(const char *script_name, int argc, char *argv[]);

#endif /* CINDER_PM_H */
