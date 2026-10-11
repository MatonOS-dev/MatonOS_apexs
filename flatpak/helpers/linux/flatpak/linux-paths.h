#ifndef MATON_LINUX_PATHS_H
#define MATON_LINUX_PATHS_H
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <limits.h>
#include <dirent.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#ifndef MATON_MISC_DE
#define MATON_MISC_DE "/data/misc_de"
#endif
#define MATON_MISC_CE "/data/misc_ce"
#define MATON_DE_ROOT_FORMAT MATON_MISC_DE "/%u/matonos-linux"
#define MATON_CE_ROOT_FORMAT MATON_MISC_CE "/%u/matonos-linux"

static inline unsigned maton_user_of(unsigned uid) { return uid / 100000; }
static inline int maton_path_uid_valid(unsigned uid) {
    return uid <= INT_MAX && uid % 100000 >= 10000 && uid % 100000 <= 19999;
}
static inline int maton_de_root(char* out, size_t size, unsigned user) {
    int n=snprintf(out,size,MATON_DE_ROOT_FORMAT,user);
    return n>=0 && (size_t)n<size ? 0 : -1;
}
static inline int maton_install_dir(char* out,size_t size,unsigned uid) {
    int n=snprintf(out,size,MATON_DE_ROOT_FORMAT "/install/%u",maton_user_of(uid),uid);
    return maton_path_uid_valid(uid) && n>=0 && (size_t)n<size ? 0 : -1;
}
static inline int maton_runtime_dir(char* out,size_t size,unsigned user,unsigned uid) {
    int n=snprintf(out,size,MATON_DE_ROOT_FORMAT "/runtime/%u",user,uid);
    return maton_path_uid_valid(uid) && user==maton_user_of(uid) && n>=0 && (size_t)n<size ? 0 : -1;
}
static inline int maton_home_dir(char* out,size_t size,unsigned uid) {
    int n=snprintf(out,size,MATON_CE_ROOT_FORMAT "/home/%u",maton_user_of(uid),uid);
    return maton_path_uid_valid(uid) && n>=0 && (size_t)n<size ? 0 : -1;
}
/* Round-trip rejects signs, leading zeros, overflow and trailing components. */
static inline int maton_parse_path(const char* path,const char* area,const char* kind,unsigned* uid) {
    unsigned user=0,value=0; char tail,format[96],expected[160];
    snprintf(format,sizeof(format),"/data/misc_%s/%%u/matonos-linux/%s/%%u%%c",area,kind);
    if(!path || sscanf(path,format,&user,&value,&tail)!=2 || !maton_path_uid_valid(value) || user!=maton_user_of(value))return 0;
    snprintf(expected,sizeof(expected),"/data/misc_%s/%u/matonos-linux/%s/%u",area,user,kind,value);
    if(strcmp(path,expected))return 0;
    *uid=value;return 1;
}
static inline int maton_parse_install_path(const char* path,unsigned* uid) {
    return maton_parse_path(path,"de","install",uid);
}
static inline int maton_parse_home_path(const char* path,unsigned* uid) {
    return maton_parse_path(path,"ce","home",uid);
}
/* Enumerate only canonical numeric users and UID entries in their DE root.
 * Pin each component without following symlinks; absent state is empty. */
typedef struct MatonPathScan { DIR* users; DIR* entries; unsigned user; const char* kind; } MatonPathScan;
static inline int maton_scan_open(MatonPathScan* scan,const char* kind) {
    memset(scan,0,sizeof(*scan));scan->kind=kind;
    int fd=open(MATON_MISC_DE,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return errno==ENOENT ? 0 : -1;
    scan->users=fdopendir(fd);if(!scan->users){close(fd);return -1;}return 0;
}
/* A user inventory opens only that user's directory. */
static inline int maton_scan_open_user(MatonPathScan* scan,const char* kind,unsigned user) {
    memset(scan,0,sizeof(*scan));scan->kind=kind;scan->user=user;
    char root[160];if(maton_de_root(root,sizeof(root),user))return -1;
    int parent=open(root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(parent<0)return errno==ENOENT ? 0 : -1;
    int fd=openat(parent,kind,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(parent);
    if(fd<0)return errno==ENOENT ? 0 : -1;
    scan->entries=fdopendir(fd);if(!scan->entries){close(fd);return -1;}return 0;
}
static inline int maton_scan_next(MatonPathScan* scan,unsigned* uid) {
    for(;;) {
        struct dirent* e;
        while(scan->entries && (e=readdir(scan->entries))) {
            char* end;unsigned long value=strtoul(e->d_name,&end,10);char canonical[32];
            snprintf(canonical,sizeof(canonical),"%lu",value);
            if(*end || strcmp(canonical,e->d_name) || value>INT_MAX || !maton_path_uid_valid((unsigned)value) || maton_user_of((unsigned)value)!=scan->user)continue;
            struct stat st;
            if(fstatat(dirfd(scan->entries),e->d_name,&st,AT_SYMLINK_NOFOLLOW) || !S_ISDIR(st.st_mode))continue;
            *uid=(unsigned)value;return 1;
        }
        if(scan->entries){closedir(scan->entries);scan->entries=NULL;}
        if(!scan->users || !(e=readdir(scan->users)))return 0;
        char* end;unsigned long user=strtoul(e->d_name,&end,10);char canonical[32];
        snprintf(canonical,sizeof(canonical),"%lu",user);
        if(*end || strcmp(canonical,e->d_name) || user>(unsigned)INT_MAX/100000)continue;
        int parent=openat(dirfd(scan->users),e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(parent<0)continue;
        int root=openat(parent,"matonos-linux",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(parent);
        if(root<0)continue;
        int fd=openat(root,scan->kind,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(root);
        if(fd<0)continue;
        scan->entries=fdopendir(fd);if(!scan->entries){close(fd);return -1;}scan->user=(unsigned)user;
    }
}
static inline void maton_scan_close(MatonPathScan* scan) {
    if(scan->entries)closedir(scan->entries);
    if(scan->users)closedir(scan->users);
}
#endif
