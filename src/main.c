#include "cli/cli.h"
#include <stdio.h>
#ifdef _WIN32
#  include <windows.h>
#endif

int main(int argc, char *argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);   /* UTF-8 output so Unicode prints correctly */
    SetConsoleCP(CP_UTF8);
#endif
    return cli_main(argc, argv);
}
