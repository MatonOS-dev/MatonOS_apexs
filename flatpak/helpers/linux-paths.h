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
#ifndef MATON_LINUX_ROOT
#define MATON_LINUX_ROOT "/data/linux-app"
#endif
#define MATON_RUNTIME_DIR MATON_LINUX_ROOT "/runtime"
#define MATON_INSTALL_ROOT MATON_LINUX_ROOT "/install"
#define MATON_STAGING_ROOT MATON_LINUX_ROOT "/staging"
/* AID_VENDOR_MATONOS_FPINSTALL (config.fs): sole owner of install staging. */
#ifndef MATON_FPINSTALL_UID
#define MATON_FPINSTALL_UID 2902
#endif
static inline unsigned maton_user_of(unsigned uid) { return uid / 100000; }
static inline unsigned maton_app_id(unsigned uid) { return uid % 100000; }
static inline int maton_path_uid_valid(unsigned uid) {
    return uid <= INT_MAX && maton_app_id(uid)>=10000 && maton_app_id(uid)<=19999;
}
/* Current pointers are resolved once by the launcher; management can use them
 * while holding its package-operation mutex. */
static inline int maton_install_dir(char* out,size_t size,unsigned uid) {
    int n=snprintf(out,size,MATON_INSTALL_ROOT "/%u",maton_app_id(uid));
    return maton_path_uid_valid(uid) && n>=0 && (size_t)n<size ? 0 : -1;
}
static inline int maton_staging_owner(char* out,size_t size,unsigned uid) {
    int n=snprintf(out,size,MATON_STAGING_ROOT "/%u",uid);
    return uid==MATON_FPINSTALL_UID && n>=0 && (size_t)n<size ? 0 : -1;
}
/* Strict canonical current-pointer or versioned deployment path. */
static inline int maton_parse_install_path(const char* path,unsigned* uid) {
    const char* prefix=MATON_INSTALL_ROOT "/";
    if(!path || strncmp(path,prefix,strlen(prefix)))return 0;
    const char* name=path+strlen(prefix); char* end;
    errno=0;unsigned long value=strtoul(name,&end,10);char canonical[32];
    snprintf(canonical,sizeof(canonical),"%lu",value);
    size_t n=strlen(canonical);
    if(errno || value>19999 || value<10000 || strncmp(name,canonical,n) || (end!=name+n))return 0;
    if(*end) {
        if(*end++!='-' || !*end)return 0;
        for(;*end;end++)if(!((*end>='a'&&*end<='z')||(*end>='A'&&*end<='Z')||(*end>='0'&&*end<='9')))return 0;
    }
    *uid=(unsigned)value;return 1;
}
/* Unpublished app version created by linuxd: install/.pending-<appId>-XXXXXX. */
static inline int maton_parse_pending_path(const char* path) {
    const char* prefix=MATON_INSTALL_ROOT "/.pending-";
    if(!path || strncmp(path,prefix,strlen(prefix)))return 0;
    const char* name=path+strlen(prefix);size_t len=strlen(name);
    if(len<8 || len>230 || name[len-7]!='-')return 0;
    for(size_t i=len-6;i<len;i++){char c=name[i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')))return 0;}
    for(size_t i=0;i<len-7;i++){char c=name[i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'))return 0;}
    return name[0]!='.' && !strstr(name,"..");
}
static inline int maton_parse_staging_owner(const char* path,unsigned* uid) {
    unsigned value;char tail,expected[128];
    if(!path || sscanf(path,MATON_STAGING_ROOT "/%u%c",&value,&tail)!=1 || value!=MATON_FPINSTALL_UID)return 0;
    maton_staging_owner(expected,sizeof(expected),value);
    if(strcmp(path,expected))return 0;
    *uid=value;return 1;
}
typedef struct MatonPathScan { DIR* entries; const char* kind; } MatonPathScan;
static inline int maton_scan_open(MatonPathScan* scan,const char* kind) {
    memset(scan,0,sizeof(*scan));scan->kind=kind;
    const char* path=!strcmp(kind,"staging")?MATON_STAGING_ROOT:MATON_INSTALL_ROOT;
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return errno==ENOENT?0:-1;
    scan->entries=fdopendir(fd);if(!scan->entries){close(fd);return -1;}return 0;
}
static inline int maton_scan_open_user(MatonPathScan* scan,const char* kind,unsigned user) {
    (void)user;return maton_scan_open(scan,kind);
}
static inline int maton_scan_next(MatonPathScan* scan,unsigned* uid) {
    struct dirent* e;
    while(scan->entries && (e=readdir(scan->entries))) {
        char* end;errno=0;unsigned long value=strtoul(e->d_name,&end,10);char canonical[32];
        snprintf(canonical,sizeof(canonical),"%lu",value);
        if(errno || *end || strcmp(canonical,e->d_name) || value>INT_MAX ||
                (!strcmp(scan->kind,"staging")?value!=MATON_FPINSTALL_UID:!maton_path_uid_valid((unsigned)value)))continue;
        struct stat st;
        if(fstatat(dirfd(scan->entries),e->d_name,&st,AT_SYMLINK_NOFOLLOW))continue;
        if(!strcmp(scan->kind,"staging")?!S_ISDIR(st.st_mode):!S_ISLNK(st.st_mode))continue;
        *uid=(unsigned)value;return 1;
    }
    return 0;
}
static inline void maton_scan_close(MatonPathScan* scan) { if(scan->entries)closedir(scan->entries); }
#endif
