#ifndef MATON_GTK_SETTINGS_H
#define MATON_GTK_SETTINGS_H
#include "SafePath.h"

static void set_gtk_settings_key(int rootfd, const char* directory) {
    static const char key[] = "gtk-decoration-layout";
    static const char line[] = "gtk-decoration-layout=:\n";
    char old[8192] = {0}, out[8192 + sizeof(line) + 16];
    int dirfd = safe_directory_at(rootfd, directory, 1);
    if (dirfd < 0) return;
    int fd = openat(dirfd, "settings.ini", O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 && errno != ENOENT) { close(dirfd); return; }
    size_t length = 0;
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); close(dirfd); return; }
        ssize_t got = 0;
        while (length < sizeof(old) - 1 && (got = read(fd, old + length, sizeof(old) - 1 - length)) > 0) length += (size_t)got;
        close(fd);
        if (got < 0 || length == sizeof(old) - 1) { close(dirfd); return; }  /* unexpectedly large: leave it alone */
    }
    size_t n = 0; int done = 0, in_settings = 0;
    for (char* cursor = old; *cursor; ) {
        char* end = strchr(cursor, '\n'); size_t len = end ? (size_t)(end - cursor) + 1 : strlen(cursor);
        if (cursor[0] == '[') {
            if (in_settings && !done) { memcpy(out + n, line, sizeof(line) - 1); n += sizeof(line) - 1; done = 1; }
            in_settings = !strncmp(cursor, "[Settings]", 10);
        }
        if (in_settings && !strncmp(cursor, key, sizeof(key) - 1) &&
                strchr(" \t=", cursor[sizeof(key) - 1])) {
            if (!done) { memcpy(out + n, line, sizeof(line) - 1); n += sizeof(line) - 1; done = 1; }
        } else {
            memcpy(out + n, cursor, len); n += len;
            if (!end && len) out[n++] = '\n';
        }
        cursor += len;
    }
    if (!done && in_settings) { memcpy(out + n, line, sizeof(line) - 1); n += sizeof(line) - 1; done = 1; }
    if (!done) n += (size_t)snprintf(out + n, sizeof(out) - n, "%s[Settings]\n%s", n ? "\n" : "", line);
    (void)safe_replace_at(dirfd, "settings.ini", out, n);
    close(dirfd);
}
#endif
