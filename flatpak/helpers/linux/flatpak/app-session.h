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
    int pid, controllers, lifeline, group;
    char home[128], data_dir[128], label[256], id[256], app_label[256];
} AppSession;

static int session_text(const char* path,char* text,size_t size) {
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return -1;
    ssize_t got=read(fd,text,size-1);close(fd);
    if(got<=0 || (size_t)got==size-1)return -1;
    text[got]=0;return (int)got;
}
static int session_owner(AppSession* s,int initial) {
    memset(s,0,sizeof(*s));s->lifeline=-1;s->group=-1;
    if(initial && getuid()!=1000){errno=EPERM;return -1;}
    char owner[384]={0},extra;
    if(initial) {
        const char* value=getenv("MATON_APP_OWNER");
        if(!value || !getenv("MATON_APP_LIFELINE") || strcmp(getenv("MATON_APP_LIFELINE"),"196"))return -1;
        snprintf(owner,sizeof(owner),"%s",value);s->lifeline=196;
        struct stat st;if(fstat(196,&st)||!S_ISFIFO(st.st_mode))return -1;
        if(fcntl(196,F_SETFD,FD_CLOEXEC))return -1;
    } else {
        char path[96],exe[256],env[65536];
        snprintf(path,sizeof(path),"/proc/%d/exe",getppid());
        ssize_t n=readlink(path,exe,sizeof(exe)-1);
        if(n<=0)return -1;
        exe[n]=0;
        if(strcmp(exe,"/apex/com.matonos.flatpak/bin/flatpak-portal"))return -1;
        snprintf(path,sizeof(path),"/proc/%d/environ",getppid());
        int got=session_text(path,env,sizeof(env));if(got<0)return -1;
        for(int i=0;i<got;) {
            size_t len=strnlen(env+i,(size_t)(got-i));
            if(len>16 && !strncmp(env+i,"MATON_APP_OWNER=",16))snprintf(owner,sizeof(owner),"%s",env+i+16);
            i+=(int)len+1;
        }
    }
    if(sscanf(owner,"%u:%d:%d:%255[A-Za-z0-9._-]%c",&s->uid,&s->pid,&s->controllers,s->id,&extra)!=4 ||
       s->uid%100000<10000 || s->uid%100000>=20000 || s->pid<=0 || (s->controllers!=0 && s->controllers!=1))return -1;
    if(getuid()!=1000 && getuid()!=s->uid)return -1;
    char path[128],text[4096];struct stat st;
    snprintf(path,sizeof(path),"/proc/%d",s->pid);
    if(stat(path,&st)||st.st_uid!=s->uid)return -1;
    snprintf(path,sizeof(path),"/proc/%d/attr/current",s->pid);
    if(session_text(path,text,sizeof(text))<0)return -1;
    char* range=strstr(text,":s0");if(!range)return -1;
    range[strcspn(range,"\n")]=0;
    /* Derive the MLS level from the verified UID, never from a caller-supplied
     * string: it must equal the level Android itself assigned the live stub
     * (seapp_contexts levelFrom=all). A mismatch fails closed. */
    char level[64];
    if(maton_mls_level_from_uid(s->uid,level,sizeof(level)) || strcmp(range+1,level))return -1;
    /* The dyntransition target is the narrow flatpak-run domain; the verified
     * payload is moved to the app domain later by matonos-app-exec. */
    snprintf(s->app_label,sizeof(s->app_label),"u:r:matonos_linux_app:%s",level);
    snprintf(s->label,sizeof(s->label),"u:r:matonos_flatpak_run:%s",level);
    snprintf(path,sizeof(path),"/sys/fs/cgroup/apps/uid_%u/pid_%d",s->uid,s->pid);
    s->group=open(path,O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(s->group<0)return -1;
    /* Pin the existing Android cgroup inode; never create a replacement.
     * Membership plus proc owner ties this request to the live stub. */
    snprintf(path,sizeof(path),"/proc/%d/cgroup",s->pid);
    if(session_text(path,text,sizeof(text))<0)return -1;
    char expected[128];snprintf(expected,sizeof(expected),"0::/apps/uid_%u/pid_%d\n",s->uid,s->pid);
    if(!strstr(text,expected))return -1;
    if(initial){struct pollfd life={.fd=s->lifeline,.events=POLLIN};if(poll(&life,1,0)!=0)return -1;}
    snprintf(s->data_dir,sizeof(s->data_dir),"/data/matonos/linux/apps/%u",s->uid);
    snprintf(s->home,sizeof(s->home),"%s/home",s->data_dir);
    return 0;
}
static int session_portal_uid(void) {
    uid_t uid=(uid_t)atoi(getenv("MATON_APP_UID"));
    struct __user_cap_header_struct h={.version=_LINUX_CAPABILITY_VERSION_3};
    struct __user_cap_data_struct caps[2]={{0}};
    if(uid%100000<10000 || uid%100000>=20000 || syscall(SYS_capget,&h,caps) || prctl(PR_SET_KEEPCAPS,1) ||
       setresgid(uid,uid,uid) || setresuid(uid,uid,uid))return -1;
    caps[0].effective=caps[0].permitted;
    caps[0].inheritable=caps[0].permitted;
    if(syscall(SYS_capset,&h,caps))return -1;
    for(unsigned i=0;i<32;i++)if(caps[0].permitted&(1U<<i))
        if(prctl(PR_CAP_AMBIENT,PR_CAP_AMBIENT_RAISE,i,0,0))return -1;
    // The wrapper reads this immutable environment when authenticating a
    // nested launch. MAC denies app ptrace of this linuxd-domain process.
    return prctl(PR_SET_DUMPABLE,1);
}
static int session_home(AppSession* s) {
    const char* paths[]={s->data_dir,s->home};
    for(unsigned i=0;i<2;i++) {
        int fd=open(paths[i],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        struct stat st;
        if(fd<0)return -1;
        int bad=fstat(fd,&st)||st.st_uid!=s->uid||(st.st_mode&0777)!=0700;
        close(fd);if(bad)return -1;
    }
    return 0;
}
static int session_filter_binder(void) {
    /* Flatpak/bwrap use no Android Binder. Reject all 'b' ioctls, including
     * compat commands, before any untrusted metadata or payload is parsed.
     * Filters and NNP survive exec/fork and cannot be relaxed by userns root. */
    struct sock_filter code[]={
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,arch)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,AUDIT_ARCH_X86_64,1,0),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
        BPF_JUMP(BPF_JMP|BPF_JSET|BPF_K,0x40000000,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_ioctl,0,4),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,args[1])),
        BPF_STMT(BPF_ALU|BPF_AND|BPF_K,0xff00),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,0x6200,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
    };
    struct sock_fprog prog={.len=sizeof(code)/sizeof(code[0]),.filter=code};
    return prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0) || prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&prog);
}
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
    while(fscanf(file,"%d",&pid)==1)if(pid!=s->pid){alive=1;break;}
    fclose(file);return alive;
}
static void session_kill(AppSession* s) {
    int fd=openat(s->group,"cgroup.kill",O_WRONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd>=0){(void)!write(fd,"1",1);close(fd);}
}
#endif
