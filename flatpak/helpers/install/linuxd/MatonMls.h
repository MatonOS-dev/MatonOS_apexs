#ifndef MATONOS_MLS_H
#define MATONOS_MLS_H

/*
 * Per-app MLS level, mirroring Android's app-process labelling.
 *
 * AOSP reference (this tree):
 *   external/selinux/libselinux/src/android/android_seapp.c
 *     set_range_from_level(LEVELFROM_ALL)  -> "s0:c%u,c%u,c%u,c%u"
 *     seapp_context_lookup_internal()      -> userid = uid / AID_USER_OFFSET,
 *                                             appid  = uid % AID_USER_OFFSET
 *                                             (AID_APP_START subtracted for apps)
 *   system/sepolicy/private/seapp_contexts selects levelFrom=all for Android
 *   app processes, so the level is a pure function of the verified UID.
 *
 * The categories are derived here, never taken from a caller-supplied string:
 * linuxd computes them from the stub UID it verified, and matonos-app-exec
 * recomputes them from the UID it actually runs as. The AOSP algorithm is
 * copied verbatim; keep it in sync with the AOSP release this tree tracks.
 */

#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

/* bionic private/android_filesystem_config.h and android_seapp.c constants. */
#define MATON_AID_USER_OFFSET 100000u
#define MATON_AID_APP_START 10000u
#define MATON_AID_APP_END 19999u
#define MATON_CAT_MAPPING_MAX_ID (0x1u << 16)

/* Writes "s0:c<app0>,c<256+app1>,c<512+user0>,c<768+user1>" for an app UID.
 * Returns 0 on success, -1 for a UID outside the Android app range. */
static inline int maton_mls_level_from_uid(uid_t uid, char* out, size_t size) {
    uid_t userid = (uid_t)(uid / MATON_AID_USER_OFFSET);
    uid_t appid = (uid_t)(uid % MATON_AID_USER_OFFSET);
    if (appid < MATON_AID_APP_START) return -1;
    appid -= MATON_AID_APP_START;
    if (appid >= MATON_CAT_MAPPING_MAX_ID || userid >= MATON_CAT_MAPPING_MAX_ID) return -1;
    int written = snprintf(out, size, "s0:c%u,c%u,c%u,c%u",
            (unsigned)(appid & 0xff),
            (unsigned)(256 + ((appid >> 8) & 0xff)),
            (unsigned)(512 + (userid & 0xff)),
            (unsigned)(768 + ((userid >> 8) & 0xff)));
    if (written < 0 || (size_t)written >= size) return -1;
    return 0;
}

#endif