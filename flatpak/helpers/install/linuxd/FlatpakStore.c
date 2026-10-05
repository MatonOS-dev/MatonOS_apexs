#define _GNU_SOURCE
/*
 * matonos-flatpak-store: the r24 installer helper.
 *
 * It is exec'd by linuxd and runs in the dedicated matonos_flatpak_installer
 * domain, the only domain allowed to create, grow, mount and fs-verity the
 * Flatpak code store:
 *
 *   /data/matonos/linux/store/runtime.img   shared runtime/extension store
 *   /data/matonos/linux/store/apps/<id>/code.img  per-app verified code
 *   /data/matonos/linux/store/apps/<id>/vol.img   optional writable volume
 *
 * Images are sparse files on /data loop-mounted with fscontext= matonos_code_fs
 * and context= of the appropriate inode type. The code and runtime images are
 * read-only at run time and carry an fs-verity descriptor when /data supports
 * it, so loop I/O is verified by the kernel. The store helper never builds a
 * substitution mount namespace; it is invoked per operation and exits.
 *
 * Usage:
 *   matonos-flatpak-store ensure-runtime [size-mb]
 *   matonos-flatpak-store seal-runtime
 *   matonos-flatpak-store mount-runtime
 *   matonos-flatpak-store umount-runtime
 *   matonos-flatpak-store app-build <app-id> <deployment-dir>
 *   matonos-flatpak-store app-attach <app-id>
 *   matonos-flatpak-store vol-ensure <app-id> [size-mb]
 *   matonos-flatpak-store vol-attach <app-id>
 *   matonos-flatpak-store loop-detach <loop-device>
 *   matonos-flatpak-store trim <path>
 *   matonos-flatpak-store digest <file>
 *   matonos-flatpak-store selftest
 *
 * The loop-attachment commands are retained for the image-store tooling.
 * They are not part of the current Flatpak launch chain, which uses stock
 * Flatpak deployments and has no privileged mount helper.
 */

#include "FlatpakStore.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/fs.h>
#include <linux/fsverity.h>
#include <linux/loop.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef FS_VERITY_HASH_ALG_SHA256
#define FS_VERITY_HASH_ALG_SHA256 1
#endif

#define DEFAULT_RUNTIME_MB 2048
#define DEFAULT_VOLUME_MB 1024
#define CHUNK (64 * 1024)
#define MATONOS_FLATPAK_WRAPPER "/apex/com.matonos.flatpak/bin/flatpak-env-wrapper"
#define MATONOS_FLATPAK_INSTALLER_AID 2902

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

static int valid_component(const char* value) {
    if (!value || !*value || strlen(value) > 128 || strstr(value, "..")) return 0;
    for (const char* p = value; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                    (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-'))
            return 0;
    return 1;
}

/* Flatpak ids: dotted, at least two components, no leading/trailing dot. */
static int valid_app_id(const char* value) {
    if (!valid_component(value)) return 0;
    return strchr(value, '.') != NULL && value[0] != '.' && value[strlen(value) - 1] != '.';
}

static int mkdir_p(const char* path, mode_t mode) {
    char buffer[512];
    if (snprintf(buffer, sizeof(buffer), "%s", path) >= (int)sizeof(buffer)) return -1;
    for (char* p = buffer + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(buffer, mode) && errno != EEXIST) return -1;
        *p = '/';
    }
    return mkdir(buffer, mode) && errno != EEXIST ? -1 : 0;
}

static int run_tool(const char* const argv[]) {
    pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        execv(argv[0], (char* const*)argv);
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) { }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

/* Create a sparse image of size_mb, or grow an existing one. */
static int ensure_sparse(const char* path, unsigned long long size_mb) {
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return fail("cannot open %s: %s", path, strerror(errno));
    off_t want = (off_t)(size_mb * 1024ULL * 1024ULL);
    struct stat st;
    if (fstat(fd, &st)) { close(fd); return fail("stat %s: %s", path, strerror(errno)); }
    if (st.st_size >= want) { close(fd); return 0; }
    if (ftruncate(fd, want)) { close(fd); return fail("grow %s: %s", path, strerror(errno)); }
    close(fd);
    say("grew %s to %llu MiB (sparse)", path, size_mb);
    return 0;
}

static int is_blank_image(const char* path) {
    /* A fresh sparse image has no ext4/erofs magic at its head. */
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    unsigned char magic[8] = {0};
    ssize_t got = pread(fd, magic, sizeof(magic), 0);
    close(fd);
    if (got < 4) return 1;
    if (magic[0] == 0xe2 && magic[1] == 0xe1 && magic[2] == 0xf0 && magic[3] == 0xf0) return 0; /* erofs */
    if (magic[0] == 0 && magic[1] == 0) return 1;
    return 0;
}

static int mkfs_ext4(const char* path, unsigned long long size_mb) {
    char size[64];
    snprintf(size, sizeof(size), "%lluM", size_mb);
    const char* argv[] = { MATON_STORE_MKEXT4, "-t", "ext4", "-q", "-F", "-E", "nodiscard",
        "-b", "4096", "-m", "0", path, size, NULL };
    int rc = run_tool(argv);
    if (rc) {
        const char* fallback[] = { "/system/bin/mkfs.ext4", "-q", "-F", "-b", "4096", path, size, NULL };
        rc = run_tool(fallback);
    }
    if (rc) return fail("mke2fs %s failed (%d)", path, rc);
    return 0;
}

static int mkfs_erofs(const char* source, const char* image) {
    const char* argv[] = { MATON_STORE_MKEROFS, "-b", "4096", "-z", "lz4hc",
        "--all-root", "-E", "force-inode-compact", source, image, NULL };
    int rc = run_tool(argv);
    if (rc) return fail("mkfs.erofs %s -> %s failed (%d)", source, image, rc);
    return 0;
}

/* Enable fs-verity on a read-only image so the kernel verifies every read,
 * including loop-device I/O. Degrades explicitly when /data lacks support. */
static int enable_fsverity(const char* path) {
    if (chmod(path, 0400)) return fail("chmod %s read-only: %s", path, strerror(errno));
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return fail("open %s for verity: %s", path, strerror(errno));
    struct fsverity_enable_arg arg;
    memset(&arg, 0, sizeof(arg));
    arg.version = 1;
    arg.hash_algorithm = FS_VERITY_HASH_ALG_SHA256;
    arg.block_size = 4096;
    if (ioctl(fd, FS_IOC_ENABLE_VERITY, &arg) == 0) {
        close(fd);
        say("fs-verity enabled on %s", path);
        return 0;
    }
    int saved = errno;
    close(fd);
    chmod(path, 0400);
    if (saved == ENOTTY || saved == EOPNOTSUPP || saved == ENOSYS) {
        say("fs-verity unsupported on /data (%s); image stays read-only, integrity is mount-time checked",
                strerror(saved));
        return 0;
    }
    if (saved == EEXIST) return 0;
    return fail("fs-verity on %s failed: %s", path, strerror(saved));
}

/* Attach an image to a free loop device; returns the device path. */
static int loop_attach(const char* image, int* out_fd, char* path, size_t path_size) {
    int control = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
    if (control < 0) return fail("open /dev/loop-control: %s", strerror(errno));
    int number = ioctl(control, LOOP_CTL_GET_FREE);
    close(control);
    if (number < 0) return fail("LOOP_CTL_GET_FREE: %s", strerror(errno));
    snprintf(path, path_size, "/dev/block/loop%d", number);
    int lfd = open(path, O_RDWR | O_CLOEXEC);
    if (lfd < 0) return fail("open %s: %s", path, strerror(errno));
    int backing = open(image, O_RDWR | O_CLOEXEC);
    if (backing < 0) { close(lfd); return fail("open backing %s: %s", image, strerror(errno)); }
    struct loop_config config;
    memset(&config, 0, sizeof(config));
    config.fd = backing;
    config.block_size = 4096;
    int rc = ioctl(lfd, LOOP_CONFIGURE, &config);
    if (rc) rc = ioctl(lfd, LOOP_SET_FD, backing);
    close(backing);
    if (rc) { close(lfd); return fail("loop setup for %s: %s", image, strerror(errno)); }
    *out_fd = lfd;
    return 0;
}

static void loop_detach(int lfd) {
    if (lfd >= 0) {
        if (ioctl(lfd, LOOP_CLR_FD, 0)) { /* best effort */ }
        close(lfd);
    }
}

/* data is "fscontext=...,context=...". */
static int mount_image(const char* image, const char* target, const char* fstype,
        int read_only, const char* context) {
    if (mkdir_p(target, 0700)) return fail("mkdir %s: %s", target, strerror(errno));
    int lfd = -1;
    char loop_path[64];
    if (loop_attach(image, &lfd, loop_path, sizeof(loop_path))) return 1;
    char options[512];
    snprintf(options, sizeof(options), "%s", context);
    unsigned long flags = MS_NOSUID | MS_NODEV;
    if (read_only) flags |= MS_RDONLY;
    if (mount(loop_path, target, fstype, flags, options)) {
        int saved = errno;
        loop_detach(lfd);
        return fail("mount %s at %s: %s", loop_path, target, strerror(saved));
    }
    close(lfd);
    return 0;
}

static int umount_image(const char* target) {
    if (umount2(target, MNT_DETACH) && errno != EINVAL && errno != ENOENT)
        return fail("umount %s: %s", target, strerror(errno));
    return 0;
}

static int trim_image(const char* path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return fail("open %s: %s", path, strerror(errno));
    struct fstrim_range range;
    range.start = 0;
    range.len = ~0ULL;
    range.minlen = 4096;
    int rc = ioctl(fd, FITRIM, &range);
    close(fd);
    if (rc) return fail("FITRIM %s: %s", path, strerror(errno));
    say("trimmed %s (%llu bytes discarded)", path, (unsigned long long)range.len);
    return 0;
}

static int sha256_file(const char* path, unsigned char out[32]) {
    /* Prefer the stock digest tool; keeps this helper free of a crypto dep. */
    int pipes[2];
    if (pipe(pipes)) return -1;
    pid_t child = fork();
    if (child < 0) { close(pipes[0]); close(pipes[1]); return -1; }
    if (child == 0) {
        close(pipes[0]);
        dup2(pipes[1], STDOUT_FILENO);
        close(pipes[1]);
        execl("/system/bin/sha256sum", "sha256sum", path, (char*)NULL);
        execl("/system/bin/toybox", "toybox", "sha256sum", path, (char*)NULL);
        char* const argv[] = { (char*)"sha256sum", (char*)path, NULL };
        execvp("sha256sum", argv);
        _exit(127);
    }
    close(pipes[1]);
    char hex[80] = {0};
    ssize_t got = read(pipes[0], hex, sizeof(hex) - 1);
    close(pipes[0]);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) { }
    if (got < 64) return -1;
    for (int i = 0; i < 32; i++) {
        unsigned value = 0;
        for (int j = 0; j < 2; j++) {
            char c = hex[i * 2 + j];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') value |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= (unsigned)(c - 'A' + 10);
            else return -1;
        }
        out[i] = (unsigned char)value;
    }
    return 0;
}

static int write_digest(const char* image) {
    unsigned char digest[32];
    if (sha256_file(image, digest)) return fail("cannot digest %s", image);
    char path[512];
    snprintf(path, sizeof(path), "%s.verity", image);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return fail("write %s: %s", path, strerror(errno));
    ssize_t n = write(fd, digest, sizeof(digest));
    close(fd);
    return n == (ssize_t)sizeof(digest) ? 0 : fail("short digest write for %s", image);
}

static int verify_digest(const char* image) {
    char path[512];
    snprintf(path, sizeof(path), "%s.verity", image);
    unsigned char stored[32], actual[32];
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0; /* no sidecar: fs-verity or unverified, caller logs */
    ssize_t n = read(fd, stored, sizeof(stored));
    close(fd);
    if (n != (ssize_t)sizeof(stored)) return fail("bad digest sidecar %s", path);
    if (sha256_file(image, actual)) return fail("cannot digest %s", image);
    return memcmp(stored, actual, sizeof(stored)) ? fail("integrity check failed for %s", image) : 0;
}

static int app_dir(const char* app_id, char* dir, size_t size) {
    return snprintf(dir, size, MATON_STORE_APPS "/%s", app_id) >= (int)size ? -1 : 0;
}

/* ---- high-level operations ------------------------------------------------ */

static int ensure_runtime(unsigned long long size_mb) {
    if (mkdir_p(MATON_STORE_ROOT, 0700) || mkdir_p(MATON_STORE_RUNTIME_MNT, 0700) ||
            mkdir_p(MATON_STORE_APPS, 0700))
        return fail("cannot create store directories: %s", strerror(errno));
    if (ensure_sparse(MATON_STORE_RUNTIME_IMG, size_mb)) return 1;
    if (access(MATON_STORE_RUNTIME_IMG, R_OK) == 0 && !is_blank_image(MATON_STORE_RUNTIME_IMG)) return 0;
    return mkfs_ext4(MATON_STORE_RUNTIME_IMG, size_mb);
}

static int mount_runtime(void) {
    if (verify_digest(MATON_STORE_RUNTIME_IMG)) return 1;
    char options[256];
    snprintf(options, sizeof(options), "fscontext=%s,context=%s",
            MATON_STORE_FSCONTEXT, MATON_STORE_RUNTIME_CONTEXT);
    return mount_image(MATON_STORE_RUNTIME_IMG, MATON_STORE_RUNTIME_MNT, "ext4", 1, options);
}

static int mount_runtime_writable(void) {
    char options[256];
    snprintf(options, sizeof(options), "fscontext=%s,context=%s",
            MATON_STORE_FSCONTEXT, MATON_STORE_VOLUME_CONTEXT);
    return mount_image(MATON_STORE_RUNTIME_IMG, MATON_STORE_RUNTIME_MNT, "ext4", 0, options);
}

static int app_build(const char* app_id, const char* source) {
    if (!valid_app_id(app_id)) return fail("invalid app id");
    if (!source || source[0] != '/') return fail("deployment directory must be absolute");
    char dir[512], image[600], target[600];
    if (app_dir(app_id, dir, sizeof(dir))) return fail("app path too long");
    snprintf(image, sizeof(image), "%s/code.img", dir);
    snprintf(target, sizeof(target), "%s/code.mnt", dir);
    if (mkdir_p(dir, 0700) || mkdir_p(target, 0700)) return fail("cannot create %s: %s", dir, strerror(errno));
    /* Never reuse a live image: build beside it and swap atomically. */
    char pending[640];
    snprintf(pending, sizeof(pending), "%s.pending", image);
    unlink(pending);
    if (ensure_sparse(pending, 64)) return 1;
    if (mkfs_erofs(source, pending)) return 1;
    if (enable_fsverity(pending)) return 1;
    if (write_digest(pending)) return 1;
    char pending_digest[700], final_digest[700];
    snprintf(pending_digest, sizeof(pending_digest), "%s.verity", pending);
    snprintf(final_digest, sizeof(final_digest), "%s.verity", image);
    unlink(final_digest);
    if (rename(pending, image)) return fail("atomic swap of %s: %s", image, strerror(errno));
    if (rename(pending_digest, final_digest)) return fail("atomic swap of %s: %s", final_digest, strerror(errno));
    say("built %s", image);
    return 0;
}

/* Attach a per-app image to a free loop device and print its path. */
static int attach_image(const char* app_id, const char* name, const char* record_name) {
    if (!valid_app_id(app_id)) return fail("invalid app id");
    char dir[512], image[600];
    if (app_dir(app_id, dir, sizeof(dir))) return fail("app path too long");
    snprintf(image, sizeof(image), "%s/%s", dir, name);
    if (access(image, R_OK)) return fail("no image %s", image);
    if (verify_digest(image)) return 1;
    int lfd = -1;
    char loop_path[64];
    if (loop_attach(image, &lfd, loop_path, sizeof(loop_path))) return 1;
    close(lfd); /* the configured loop stays attached until loop-detach */
    char record[640];
    snprintf(record, sizeof(record), "%s/%s.loop", dir, record_name);
    int rfd = open(record, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (rfd < 0) return fail("write attach record %s: %s", record, strerror(errno));
    size_t length = strlen(loop_path);
    ssize_t written = write(rfd, loop_path, length);
    close(rfd);
    if (written != (ssize_t)length) return fail("short attach record %s", record);
    printf("%s\n", loop_path);
    return 0;
}

static int app_attach(const char* app_id) { return attach_image(app_id, "code.img", "code"); }
static int vol_attach(const char* app_id) { return attach_image(app_id, "vol.img", "vol"); }

static int valid_loop_device(const char* path) {
    static const char prefix[] = "/dev/block/loop";
    if (!path || strncmp(path, prefix, sizeof(prefix) - 1)) return 0;
    const char* digits = path + sizeof(prefix) - 1;
    if (!*digits) return 0;
    for (const char* p = digits; *p; p++)
        if (*p < '0' || *p > '9') return 0;
    return 1;
}

/* Drop stale attachment records before a loop device can be reused. */
static void forget_attach_records(const char* loop_path) {
    DIR* dir = opendir(MATON_STORE_APPS);
    if (!dir) return;
    struct dirent* entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;
        static const char* const names[] = { "code.loop", "vol.loop" };
        for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            char path[640], stored[128];
            if (snprintf(path, sizeof(path), MATON_STORE_APPS "/%s/%s", entry->d_name, names[i]) >= (int)sizeof(path))
                continue;
            int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (fd < 0) continue;
            ssize_t n = read(fd, stored, sizeof(stored) - 1);
            close(fd);
            if (n <= 0) continue;
            stored[n] = 0;
            char* newline = strchr(stored, '\n');
            if (newline) *newline = 0;
            if (!strcmp(stored, loop_path)) unlink(path);
        }
    }
    closedir(dir);
}

/* Detach a loop device previously returned by app-attach/vol-attach. */
static int loop_detach_device(const char* path) {
    if (!valid_loop_device(path)) return fail("not a loop device: %s", path ? path : "(null)");
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return fail("open %s: %s", path, strerror(errno));
    int rc = ioctl(fd, LOOP_CLR_FD, 0);
    close(fd);
    if (rc) return fail("detach %s: %s", path, strerror(errno));
    forget_attach_records(path);
    return 0;
}

static int vol_ensure(const char* app_id, unsigned long long size_mb) {
    if (!valid_app_id(app_id)) return fail("invalid app id");
    char dir[512], image[600];
    if (app_dir(app_id, dir, sizeof(dir))) return fail("app path too long");
    snprintf(image, sizeof(image), "%s/vol.img", dir);
    if (mkdir_p(dir, 0700)) return fail("cannot create %s: %s", dir, strerror(errno));
    if (ensure_sparse(image, size_mb)) return 1;
    if (!is_blank_image(image)) return 0;
    return mkfs_ext4(image, size_mb);
}

/* Self-test of the host-side primitives; safe without root or device nodes. */
static int selftest(void) {
    char dir[] = "/tmp/matonos-store-selftest-XXXXXX";
    if (!mkdtemp(dir)) return fail("mkdtemp: %s", strerror(errno));
    char image[600];
    snprintf(image, sizeof(image), "%s/runtime.img", dir);
    if (ensure_sparse(image, 8)) return 1;
    struct stat st;
    if (stat(image, &st) || st.st_size != 8LL * 1024 * 1024) return fail("sparse size");
    if (ensure_sparse(image, 16)) return 1;
    if (stat(image, &st) || st.st_size != 16LL * 1024 * 1024) return fail("grow size");
    if (st.st_blocks * 512 > 1024 * 1024) return fail("image is not sparse");
    if (!is_blank_image(image)) return fail("blank detection");
    if (write_digest(image)) return 1;
    if (verify_digest(image)) return 1;
    /* Corrupt and confirm the digest check refuses it. */
    int fd = open(image, O_WRONLY | O_CLOEXEC);
    if (fd < 0 || pwrite(fd, "x", 1, 0) != 1) return fail("corrupt");
    close(fd);
    if (verify_digest(image) == 0) return fail("integrity check did not detect corruption");
    say("selftest PASS");
    return 0;
}

static int print_status(void) {
    struct stat st;
    printf("store=%s runtime=%s\n", MATON_STORE_ROOT,
            stat(MATON_STORE_RUNTIME_IMG, &st) == 0 ? "present" : "absent");
    DIR* dir = opendir(MATON_STORE_APPS);
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)))
            if (entry->d_name[0] != '.') printf("app=%s\n", entry->d_name);
        closedir(dir);
    }
    return 0;
}

/* Stage network refs only in the dedicated installer domain and AID. linuxd
 * supplies a validated remote and ref; this helper fixes every CLI option. */
static int stage_flatpak_ref(int argc, char** argv) {
    if (argc != 5) return fail("stage needs staging directory, remote and ref");
    if (setgroups(0, NULL) || setgid(MATONOS_FLATPAK_INSTALLER_AID) ||
            setuid(MATONOS_FLATPAK_INSTALLER_AID))
        return fail("cannot enter Flatpak installer AID: %s", strerror(errno));
    if (strcmp(argv[2], "/data/matonos/linux/staging"))
        return fail("invalid staging installation path");
    if (setenv("MATON_FLATPAK_STAGING_DIR", argv[2], 1))
        return fail("cannot select staging installation");
    char* const flatpak_argv[] = {MATONOS_FLATPAK_WRAPPER, "install", "--system",
        "--no-deploy", "--noninteractive", "--assumeyes", argv[3], argv[4], NULL};
    execv(MATONOS_FLATPAK_WRAPPER, flatpak_argv);
    return fail("cannot start static Flatpak staging pull: %s", strerror(errno));
}

int maton_store_main(int argc, char** argv) {
    if (argc < 2) return fail("usage: matonos-flatpak-store <command> [args]");
    const char* command = argv[1];
    unsigned long long default_size = DEFAULT_RUNTIME_MB;
    if (!strcmp(command, "ensure-runtime"))
        return ensure_runtime(argc > 2 ? strtoull(argv[2], NULL, 10) : default_size);
    if (!strcmp(command, "seal-runtime")) return mount_runtime();
    if (!strcmp(command, "mount-runtime")) return mount_runtime();
    if (!strcmp(command, "mount-runtime-rw")) return mount_runtime_writable();
    if (!strcmp(command, "umount-runtime")) return umount_image(MATON_STORE_RUNTIME_MNT);
    if (!strcmp(command, "app-build"))
        return argc > 3 ? app_build(argv[2], argv[3]) : fail("app-build needs id and source");
    if (!strcmp(command, "app-attach")) return argc > 2 ? app_attach(argv[2]) : fail("app-attach needs id");
    if (!strcmp(command, "vol-ensure"))
        return argc > 2 ? vol_ensure(argv[2], argc > 3 ? strtoull(argv[3], NULL, 10) : DEFAULT_VOLUME_MB)
                        : fail("vol-ensure needs id");
    if (!strcmp(command, "vol-attach")) return argc > 2 ? vol_attach(argv[2]) : fail("vol-attach needs id");
    if (!strcmp(command, "loop-detach"))
        return argc > 2 ? loop_detach_device(argv[2]) : fail("loop-detach needs a loop device");
    if (!strcmp(command, "trim")) return argc > 2 ? trim_image(argv[2]) : fail("trim needs a path");
    if (!strcmp(command, "status")) return print_status();
    if (!strcmp(command, "stage")) return stage_flatpak_ref(argc, argv);
    if (!strcmp(command, "selftest")) return selftest();
    return fail("unknown command: %s", command);
}

#ifndef MATON_STORE_NO_MAIN
int main(int argc, char** argv) { return maton_store_main(argc, argv); }
#endif
