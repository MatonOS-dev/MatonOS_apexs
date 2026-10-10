#ifndef MATON_APP_SESSION_H
#define MATON_APP_SESSION_H
/* Trusted launcher boundary. Included only by the exec'd wrapper, never by
 * linuxd's multithreaded Binder service. Nested portal requests must use the
 * portal's immutable environment, not the caller-supplied Spawn environment. */
#include <grp.h>
#include <poll.h>
#include "../../install/linuxd/GtkSettings.h"
#include "../../install/linuxd/MatonMls.h"
#include <stddef.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/audit.h>
#include <linux/capability.h>
#include <sys/syscall.h>
#include <sys/xattr.h>

typedef struct AppSession {
    unsigned uid;
    int pid, controllers, lifeline, group, portal;
    char home[128], data_dir[128], label[256], id[256], app_label[256];
} AppSession;

static int session_owner(AppSession* s,int initial) {
    memset(s,0,sizeof(*s));s->lifeline=-1;s->group=-1;
    /* Only the initial authenticated linuxd launch uses this privileged
     * entry. Stock portal children use the unprivileged applet instead. */
    if(!initial || getuid()!=1000){errno=EPERM;return -1;}
    char owner[384]={0},extra;
    {
        const char* value=getenv("MATON_APP_OWNER");
        if(!value || !getenv("MATON_APP_LIFELINE") || strcmp(getenv("MATON_APP_LIFELINE"),"196")){errno=EPERM;return -1;}
        snprintf(owner,sizeof(owner),"%s",value);s->lifeline=196;
        struct stat st;if(fstat(196,&st))return -1;
        if(!S_ISFIFO(st.st_mode)){errno=EPERM;return -1;}
        if(fcntl(196,F_SETFD,FD_CLOEXEC))return -1;
    }
    if(sscanf(owner,"%u:%d:%d:%255[A-Za-z0-9._-]%c",&s->uid,&s->pid,&s->controllers,s->id,&extra)!=4 ||
       s->uid%100000<10000 || s->uid%100000>=20000 || s->pid<=0 || (s->controllers!=0 && s->controllers!=1)){errno=EPERM;return -1;}
    /* linuxd constructs this environment only after the system bridge
     * authenticates the signed stub's UID and installed Flatpak ref. The
     * lifeline and pinned cgroup descriptors are capabilities. Android may
     * reap the short-lived stub before this wrapper runs, so identity and
     * cgroup setup must not depend on /proc/<pid>. */
    char level[64];
    if(maton_mls_level_from_uid(s->uid,level,sizeof(level))){errno=EPERM;return -1;}
    /* The dyntransition target is the narrow flatpak-run domain; the verified
     * payload is moved to the app domain later by matonos-app-exec. */
    snprintf(s->app_label,sizeof(s->app_label),"u:r:matonos_linux_app:%s",level);
    snprintf(s->label,sizeof(s->label),"u:r:matonos_flatpak_run:%s",level);
    s->group=198;
    struct stat group_info;
    if(fstat(s->group,&group_info))return -1;
    if(!S_ISDIR(group_info.st_mode)){errno=EPERM;return -1;}
    if(fcntl(s->group,F_SETFD,FD_CLOEXEC))return -1;
    /* linuxd opened the kernel-created cgroup named by the authenticated
     * Binder UID/PID before spawning us. This descriptor pins that exact
     * group across stub exit; never reopen a PID-derived path here. */
    if(initial){struct pollfd life={.fd=s->lifeline,.events=POLLIN};if(poll(&life,1,0)!=0){errno=EPERM;return -1;}}
    snprintf(s->data_dir,sizeof(s->data_dir),"/data/matonos/linux/install/%u",s->uid);
    snprintf(s->home,sizeof(s->home),"/data/matonos/linux/home/%u",s->uid);
    return 0;
}
static int session_home(AppSession* s) {
    /* Only the app home is UID-owned; the --user install root (data_dir) is
     * system-owned, so it must not be checked here. */
    int fd=open(s->home,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    struct stat st;
    if(fd<0)return -1;
    int bad=fstat(fd,&st)||st.st_uid!=s->uid||(st.st_mode&0777)!=0700;
    close(fd);return bad?-1:0;
}
#include "session-binder-filter.h"
static int session_enter(AppSession* s) {
    int fd=openat(s->group,"cgroup.procs",O_WRONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return -1;
    char pid[32];int n=snprintf(pid,sizeof(pid),"%d",getpid());
    int ok=write(fd,pid,(size_t)n)==n;close(fd);close(s->group);s->group=-1;
    if(!ok)return -1;
    gid_t groups[2]={3003,2900}; /* Android inet and explicitly granted controllers. */
    if(setgroups(s->controllers?2:1,groups)||setresgid(s->uid,s->uid,s->uid)||setresuid(s->uid,s->uid,s->uid))return -1;
    struct __user_cap_header_struct h={.version=_LINUX_CAPABILITY_VERSION_3};
    struct __user_cap_data_struct caps[2]={{0}};
    if(syscall(SYS_capset,&h,caps))return -1;
    fd=open("/proc/thread-self/attr/current",O_WRONLY|O_CLOEXEC);
    if(fd<0)return -1;
    ok=write(fd,s->label,strlen(s->label))==(ssize_t)strlen(s->label);close(fd);
    if(!ok)return -1;
    return session_filter_binder();
}
static int session_has_payload(AppSession* s) {
    int fd=openat(s->group,"cgroup.procs",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return 0;
    FILE* file=fdopen(fd,"r");if(!file){close(fd);return 0;}
    int pid,alive=0;
    while(fscanf(file,"%d",&pid)==1)if(pid!=s->pid && pid!=s->portal){alive=1;break;}
    fclose(file);return alive;
}
static void session_kill(AppSession* s) {
    int fd=openat(s->group,"cgroup.kill",O_WRONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd>=0){(void)!write(fd,"1",1);close(fd);}
}
#endif
