#ifndef CINDER_CLI_H
#define CINDER_CLI_H

/* Entry point for the CLI dispatcher */
int cli_main(int argc, char *argv[]);

/* Print version banner */
void cli_print_version(void);

/* Print help text */
void cli_print_help(void);

#endif /* CINDER_CLI_H */
