#ifndef MATON_CONTROLLER_ACCESS_H
#define MATON_CONTROLLER_ACCESS_H
#include <grp.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <linux/capability.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

/* config.fs: AID_VENDOR_GAME_CONTROLLERS. Never put this in linuxd's groups. */
#define MATON_CONTROLLER_GID 2900
static inline int controller_group_present(void) {
    gid_t groups[256];
    int count = getgroups(256, groups);
    for (int i = 0; i < count; ++i)
        if (groups[i] == MATON_CONTROLLER_GID) return 1;
    return 0;
}

/* Run only in the exec'd launcher child, never in the multi-threaded daemon.
 * Missing decision preserves groups for Flatpak's nested portal launches.
 * Strip SETGID before any helper or sandbox can run. */
static inline int configure_controller_group(void) {
    const char* decision = getenv("MATON_GAME_CONTROLLERS");
    if (decision) {
        if (strcmp(decision, "0") && strcmp(decision, "1")) { errno = EINVAL; return -1; }
        gid_t groups[257];
        int count = getgroups(256, groups);
        if (count < 0) return -1;
        int wanted = !strcmp(decision, "1"), used = 0;
        for (int i = 0; i < count; ++i)
            if (groups[i] != MATON_CONTROLLER_GID) groups[used++] = groups[i];
        if (wanted) groups[used++] = MATON_CONTROLLER_GID;
        if ((wanted || used != count) && setgroups(used, groups)) return -1;
        unsetenv("MATON_GAME_CONTROLLERS");
    }
    struct __user_cap_header_struct header = { .version = _LINUX_CAPABILITY_VERSION_3, .pid = 0 };
    struct __user_cap_data_struct caps[2] = {{0}};
    if (syscall(SYS_capget, &header, caps)) return -1;
    unsigned mask = 1U << CAP_SETGID;
    caps[0].effective &= ~mask;
    caps[0].permitted &= ~mask;
    caps[0].inheritable &= ~mask;
    if (syscall(SYS_capset, &header, caps)) return -1;
    return 0;
}
#endif
