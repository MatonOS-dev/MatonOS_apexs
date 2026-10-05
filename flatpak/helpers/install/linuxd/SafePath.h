#ifndef MATON_SAFE_PATH_H
#define MATON_SAFE_PATH_H
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

/* Walk from a trusted descriptor. Existing components must be directories;
 * a concurrent creator (EEXIST) is rejected rather than retried. */
static inline int safe_directory_at(int rootfd, const char* path, int create) {
    if (!path || !*path || *path == '/' || strlen(path) >= 4096) { errno = EINVAL; return -1; }
    char copy[4096];
    strcpy(copy, path);
    int fd = fcntl(rootfd, F_DUPFD_CLOEXEC, 3);
    if (fd < 0) return -1;
    char* part = copy;
    for (;;) {
        char* slash = strchr(part, '/');
        if (slash) *slash = 0;
        if (!*part || !strcmp(part, ".") || !strcmp(part, "..")) { close(fd); errno = EINVAL; return -1; }
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && errno == ENOENT && create) {
            if (mkdirat(fd, part, 0700)) { int error = errno; close(fd); errno = error; return -1; }
            next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        int error = errno;
        close(fd);
        if (next < 0) { errno = error; return -1; }
        fd = next;
        if (!slash) return fd;
        part = slash + 1;
    }
}

static inline int safe_replace_at(int dirfd, const char* name, const void* data, size_t length) {
    unsigned char random[16];
    if (getrandom(random, sizeof(random), 0) != (ssize_t)sizeof(random)) return -1;
    char temp[64] = ".matonos-";
    for (size_t i = 0; i < sizeof(random); ++i) snprintf(temp + 9 + i * 2, 3, "%02x", random[i]);
    int fd = openat(dirfd, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1; /* Includes EEXIST/ELOOP: never reuse a temporary. */
    size_t written = 0;
    while (written < length) {
        ssize_t n = write(fd, (const char*)data + written, length - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        written += (size_t)n;
    }
    int ok = written == length && fsync(fd) == 0;
    if (close(fd)) ok = 0;
    /* Reject an existing symlink/non-file. renameat itself never follows the
     * destination, including one swapped in after this check. */
    struct stat st;
    if (ok && fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(st.st_mode)) { errno = ELOOP; ok = 0; }
    } else if (ok && errno != ENOENT) ok = 0;
    if (ok && renameat(dirfd, temp, dirfd, name) == 0) return 0;
    int error = errno;
    unlinkat(dirfd, temp, 0);
    errno = error;
    return -1;
}

static inline FILE* safe_fopen_absolute(const char* path) {
    if (!path || path[0] != '/' || strlen(path) >= 4096) { errno = EINVAL; return NULL; }
    char copy[4096]; strcpy(copy, path + 1);
    char* slash = strrchr(copy, '/');
    if (!slash || !slash[1]) { errno = EINVAL; return NULL; }
    *slash = 0;
    int root = open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root < 0) return NULL;
    int dirfd = safe_directory_at(root, copy, 0);
    close(root);
    if (dirfd < 0) return NULL;
    int fd = openat(dirfd, slash + 1, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    close(dirfd);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return NULL; }
    FILE* file = fdopen(fd, "r");
    if (!file) close(fd);
    return file;
}

static inline DIR* safe_opendir_absolute(const char* path) {
    if (!path || path[0] != '/') { errno = EINVAL; return NULL; }
    int root = open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root < 0) return NULL;
    int fd = safe_directory_at(root, path + 1, 0);
    close(root);
    if (fd < 0) return NULL;
    DIR* dir = fdopendir(fd);
    if (!dir) close(fd);
    return dir;
}
#endif
