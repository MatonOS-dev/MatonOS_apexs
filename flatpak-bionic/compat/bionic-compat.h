#ifndef BIONIC_COMPAT_H
#define BIONIC_COMPAT_H
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sched.h>
#include <string.h>
#include <sys/stat.h>
#include <endian.h>
/* bionic exposes unshare() only when GNU extensions are requested. */
extern int unshare(int flags);
extern int setns(int fd, int nstype);
extern int syncfs(int fd);
#ifndef strdupa
#define strdupa(s) strcpy((char *)__builtin_alloca(strlen(s) + 1), (s))
#endif
#ifndef IFTODT
#define IFTODT(mode) (((mode) & S_IFMT) >> 12)
#endif
static inline int bionic_strverscmp(const char *a, const char *b) {
  while (*a && *b) {
    if (*a >= '0' && *a <= '9' && *b >= '0' && *b <= '9') {
      const char *ae = a, *be = b;
      while (*ae >= '0' && *ae <= '9') ae++;
      while (*be >= '0' && *be <= '9') be++;
      const char *as = a, *bs = b;
      while (as < ae && *as == '0') as++;
      while (bs < be && *bs == '0') bs++;
      size_t al = (size_t)(ae - as), bl = (size_t)(be - bs);
      if (al != bl) return al < bl ? -1 : 1;
      int r = strncmp(as, bs, al);
      if (r) return r;
      if ((ae - a) != (be - b)) return (ae - a) < (be - b) ? 1 : -1;
      a = ae; b = be;
    } else if (*a != *b) {
      return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
    } else { a++; b++; }
  }
  return (unsigned char)*a - (unsigned char)*b;
}
#ifndef strverscmp
#define strverscmp(a, b) bionic_strverscmp((a), (b))
#endif
#ifndef le64toh
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define le64toh(x) ((uint64_t)(x))
#else
#define le64toh(x) __builtin_bswap64((uint64_t)(x))
#endif
#endif
#ifndef explicit_bzero
static inline void bionic_explicit_bzero(void *ptr, size_t len) {
  volatile unsigned char *p = (volatile unsigned char *)ptr;
  while (len--) *p++ = 0;
}
#define explicit_bzero(p, n) bionic_explicit_bzero((p), (n))
#endif
static inline char *bionic_get_current_dir_name(void) {
  size_t size = 128;
  for (;;) {
    char *buf = malloc(size);
    if (buf == NULL) return NULL;
    if (getcwd(buf, size) != NULL) return buf;
    free(buf);
    if (size > ((size_t)-1) / 2) return NULL;
    size *= 2;
  }
}
#define get_current_dir_name bionic_get_current_dir_name
#endif
