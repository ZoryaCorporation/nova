/**
 * @file    npwd.c
 * @brief   npwd — Print working directory
 * @author  Anthony Taliento
 * @date    2026-03-16
 * @copyright (c) 2026 Zorya Corporation. MIT License.
 *
 * Standalone tool binary. Links against libnova_toolutil.a only.
 * Usage: npwd
 */

#include "shared/ntool_common.h"

#include <stdio.h>
#include <string.h>
#include <zorya/pal.h>

/* ---- Core entry point ---- */

int ntool_pwd(const NToolFlags *flags) {
    (void)flags;
    char buf[NTOOL_PATH_MAX];
    if (zorya_getcwd(buf, sizeof(buf)) != 0) {
        fprintf(stderr, "npwd: unable to determine working directory\n");
        return -1;
    }
    puts(buf);
    return 0;
}

/* ---- Standalone entry point ---- */

int main(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "--help") == 0 ||
                     strcmp(argv[1], "-h") == 0)) {
        fprintf(stderr, "Usage: npwd\n");
        fprintf(stderr, "\nPrint the current working directory.\n");
        return 0;
    }

    NToolFlags flags;
    memset(&flags, 0, sizeof(flags));
    return ntool_pwd(&flags) != 0 ? 1 : 0;
}
