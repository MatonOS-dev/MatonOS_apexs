#define _GNU_SOURCE
/*
 * matonos-flatpak-store: stage signed Flatpak refs into the shared staging
 * repository. The only supported commands are stage and selftest.
 */

#include "FlatpakStore.h"

#include <errno.h>
#include <grp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MATONOS_FLATPAK_WRAPPER "/apex/com.matonos.flatpak/bin/flatpak-env-wrapper"
#define MATONOS_FLATPAK_INSTALLER_AID 2902
#define MATONOS_FLATPAK_STAGING "/data/matonos/linux/staging"

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
    return argc == 5 && argv && argv[1] && !strcmp(argv[1], "stage") &&
            argv[2] && !strcmp(argv[2], MATONOS_FLATPAK_STAGING) &&
            argv[3] && *argv[3] && argv[4] && *argv[4];
}

/* linuxd supplies a validated remote and ref. This helper fixes the staging
 * path and all Flatpak options, and Flatpak verifies the configured remote. */
static int stage_flatpak_ref(int argc, char** argv) {
    if (!stage_args_valid(argc, argv))
        return fail("stage needs the fixed staging directory, remote and ref");
    if (setgroups(0, NULL) || setgid(MATONOS_FLATPAK_INSTALLER_AID) ||
            setuid(MATONOS_FLATPAK_INSTALLER_AID))
        return fail("cannot enter Flatpak installer AID: %s", strerror(errno));
    if (setenv("MATON_FLATPAK_STAGING_DIR", argv[2], 1))
        return fail("cannot select staging installation: %s", strerror(errno));
    /* argv[0] selects the applet; the wrapper dispatches instead of symlinks. */
    char* const flatpak_argv[] = {"flatpak", "install", "--system",
        "--no-deploy", "--noninteractive", "--assumeyes", argv[3], argv[4], NULL};
    execv(MATONOS_FLATPAK_WRAPPER, flatpak_argv);
    return fail("cannot start static Flatpak staging pull: %s", strerror(errno));
}

/* Keep a host-safe check for the fixed command surface used by staging. */
static int selftest(void) {
    char* valid[] = {"matonos-flatpak-store", "stage", MATONOS_FLATPAK_STAGING,
        "flathub", "app/org.example.App/x86_64/stable", NULL};
    char* bad_path[] = {"matonos-flatpak-store", "stage", "/tmp/staging",
        "flathub", "app/org.example.App/x86_64/stable", NULL};
    if (!stage_args_valid(5, valid)) return fail("stage argument validation");
    if (stage_args_valid(5, bad_path)) return fail("stage accepted a non-fixed path");
    if (stage_args_valid(4, valid)) return fail("stage accepted missing arguments");
    say("selftest PASS");
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
