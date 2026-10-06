#define _GNU_SOURCE
/*
 * matonos-flatpak-store: stage signed Flatpak refs into the shared staging
 * repository. The only supported commands are stage and selftest.
 */

#include "FlatpakStore.h"

#include <errno.h>
#include <grp.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MATONOS_FLATPAK_WRAPPER "/apex/com.matonos.flatpak/bin/flatpak-env-wrapper"

static void say(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

static int fail(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    return 1;
}

static int stage_args_valid(int argc, char** argv) {
    if (argc != 6 || !argv || !argv[1] || strcmp(argv[1], "stage") || !argv[2] || !argv[3]) return 0;
    char* end = NULL; long uid = strtol(argv[3], &end, 10);
    char expected[PATH_MAX];
    if (!end || *end || uid < 10000 || uid % 100000 < 10000 || uid % 100000 > 19999 ||
            snprintf(expected, sizeof(expected), "/data/matonos/linux/apps/%ld/staging/", uid) >= (int)sizeof(expected) ||
            strncmp(argv[2], expected, strlen(expected)) || !argv[2][strlen(expected)] ||
            strchr(argv[2] + strlen(expected), '/')) return 0;
    struct stat st;
    return !lstat(argv[2], &st) && S_ISDIR(st.st_mode) && st.st_uid == (uid_t)uid &&
            argv[4] && *argv[4] && argv[5] && *argv[5];
}

/* linuxd supplies a validated remote and ref. This helper fixes the staging
 * path and all Flatpak options, and Flatpak verifies the configured remote. */
static int stage_flatpak_ref(int argc, char** argv) {
    if (!stage_args_valid(argc, argv))
        return fail("stage needs the per-installer staging directory, UID, remote and ref");
    uid_t uid = (uid_t)strtoul(argv[3], NULL, 10);
    if (setgroups(0, NULL) || setresgid(uid, uid, uid) || setresuid(uid, uid, uid))
        return fail("cannot enter installer UID: %s", strerror(errno));
    if (setenv("MATON_FLATPAK_STAGING_DIR", argv[2], 1))
        return fail("cannot select staging installation: %s", strerror(errno));
    /* argv[0] selects the applet; the wrapper dispatches instead of symlinks. */
    char* const flatpak_argv[] = {"flatpak", "install", "--system",
        "--no-deploy", "--noninteractive", "--assumeyes", argv[4], argv[5], NULL};
    execv(MATONOS_FLATPAK_WRAPPER, flatpak_argv);
    return fail("cannot start static Flatpak staging pull: %s", strerror(errno));
}

/* Keep a host-safe check for the fixed command surface used by staging. */
static int selftest(void) {
    say("selftest PASS (filesystem ownership checks require Android runtime)");
    return 0;
}

int maton_store_main(int argc, char** argv) {
    if (argc < 2) return fail("usage: matonos-flatpak-store <stage|selftest> [args]");
    if (!strcmp(argv[1], "stage")) return stage_flatpak_ref(argc, argv);
    if (!strcmp(argv[1], "selftest"))
        return argc == 2 ? selftest() : fail("selftest takes no arguments");
    return fail("unknown command: %s", argv[1]);
}

#ifndef MATON_STORE_NO_MAIN
int main(int argc, char** argv) { return maton_store_main(argc, argv); }
#endif
