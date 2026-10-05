#ifndef MATONOS_FLATPAK_STORE_H
#define MATONOS_FLATPAK_STORE_H

/*
 * Paths for the r24 Flatpak code store. The runtime/extension store and each
 * app's verified code image live below /data/matonos/linux/store as sparse
 * files. The installer only attaches/detaches loop devices; the per-app code
 * image and volume are mounted by matonos-app-exec inside that app's sandbox
 * mount namespace with:
 *     fscontext=u:object_r:matonos_code_fs:s0
 *     context=u:object_r:<matonos_(app_code)_exec>:<stub MLS level>
 * The MLS level is computed from the verified stub UID (MatonMls.h), never
 * taken from an argument. The optional per-app volume is mounted with
 * context=matonos_app_volume_file and is never executable. The shared runtime
 * store is mounted in the host namespace with context=matonos_runtime_exec:s0
 * and is readable/executable by every sandbox at any level (s0 is dominated
 * by every app level).
 */
#define MATON_STORE_ROOT "/data/matonos/linux/store"
#define MATON_STORE_RUNTIME_IMG MATON_STORE_ROOT "/runtime.img"
#define MATON_STORE_RUNTIME_MNT MATON_STORE_ROOT "/runtime"
#define MATON_STORE_APPS MATON_STORE_ROOT "/apps"

/* Filesystem-object label shared by every store image. */
#define MATON_STORE_FSCONTEXT "u:object_r:matonos_code_fs:s0"
/* Inode labels used with context= at mount time. The per-app labels get the
 * app's MLS level appended by matonos-app-exec; only the shared runtime is
 * fixed here. */
#define MATON_STORE_RUNTIME_CONTEXT "u:object_r:matonos_runtime_exec:s0"
#define MATON_STORE_VOLUME_CONTEXT "u:object_r:matonos_app_volume_file:s0"

/* Tools the installer is allowed to execute (system_ext prebuilts). */
#define MATON_STORE_MKEROFS "/system_ext/bin/mkfs.erofs"
#define MATON_STORE_MKEXT4 "/system/bin/mke2fs"

int maton_store_main(int argc, char** argv);

#endif
