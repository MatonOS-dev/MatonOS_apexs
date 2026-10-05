#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/*
 * matonos-app-exec: enter the verified app sandbox domain and exec the
 * payload. bubblewrap runs this as the final command of a sandbox.
 *
 * Under full Treble a coredomain may only take file:entrypoint from
 * system_file_type (domain.te), so the verified app image (an exec_type that
 * must never be mislabelled as system or vendor code) cannot be entered by an
 * exec transition. This launcher therefore:
 *
 *   1. verifies that it really runs in the trusted matonos_app_launch domain
 *      at the per-app MLS level derived from the UID it actually runs as (the
 *      verified stub UID), using the same algorithm Android uses for app
 *      processes (external/selinux android_seapp.c set_range_from_level
 *      LEVELFROM_ALL via seapp_contexts levelFrom=all); a caller-supplied
 *      MATON_APP_LABEL is only accepted if it matches exactly;
 *   2. changes to the app domain at that level (dyntransition; the launcher is
 *      mlstrustedsubject for MLS only, exactly like zygote);
 *   3. execs the payload with execute_no_trans.
 *
 * It mounts nothing and holds no capabilities. Flatpak and bubblewrap provide
 * the deployment paths directly; no privileged mount helper participates in
 * the launch chain. No capability enters the app sandbox.
 */
#include "MatonMls.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "matonos-app-exec: missing command\n");
        return 127;
    }
    uid_t uid = getuid();
    char level[64];
    if (maton_mls_level_from_uid(uid, level, sizeof(level))) {
        fprintf(stderr, "matonos-app-exec: uid %u is not an Android app UID\n", (unsigned)uid);
        return 127;
    }
    /* The trusted launcher's own label is fixed by the exec chain: bwrap
     * transitions matonos_bwrap -> matonos_app_launch at the same per-app
     * level. Refuse to act if that is not what we actually are. */
    char own[160];
    int fd = open("/proc/thread-self/attr/current", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        perror("matonos-app-exec: attr/current");
        return 127;
    }
    ssize_t got = read(fd, own, sizeof(own) - 1);
    close(fd);
    if (got <= 0) {
        perror("matonos-app-exec: read attr/current");
        return 127;
    }
    own[got] = 0;
    own[strcspn(own, "\n")] = 0;
    char expected_own[160];
    int written = snprintf(expected_own, sizeof(expected_own), "u:r:matonos_app_launch:%s", level);
    if (written < 0 || (size_t)written >= sizeof(expected_own)) return 127;
    if (strcmp(own, expected_own)) {
        fprintf(stderr, "matonos-app-exec: not running in the trusted launcher domain\n");
        return 127;
    }
    char label[128];
    written = snprintf(label, sizeof(label), "u:r:matonos_linux_app:%s", level);
    if (written < 0 || (size_t)written >= sizeof(label)) return 127;
    const char* supplied = getenv("MATON_APP_LABEL");
    if (supplied && strcmp(supplied, label)) {
        fprintf(stderr, "matonos-app-exec: supplied label does not match verified UID\n");
        return 127;
    }
    fd = open("/proc/thread-self/attr/current", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        perror("matonos-app-exec: attr/current");
        return 127;
    }
    if (write(fd, label, strlen(label)) != (ssize_t)strlen(label)) {
        perror("matonos-app-exec: setcon");
        close(fd);
        return 127;
    }
    close(fd);
    /* The payload is never accepted as an absolute host path; it is resolved
     * inside the sandbox root that bwrap has already pivoted into. */
    execvp(argv[1], &argv[1]);
    perror("matonos-app-exec: exec payload");
    return 127;
}
