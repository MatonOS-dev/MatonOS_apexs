#ifndef MATON_SESSION_BINDER_FILTER_H
#define MATON_SESSION_BINDER_FILTER_H
#include <errno.h>
#include <stddef.h>
#include <linux/audit.h>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Per host architecture: the native ABI, its 32-bit compat ABI (Flatpak
 * multiarch, e.g. Steam/Wine on x86), and the compat ioctl number, which the
 * native headers do not define. Binder is checked identically on both ABIs.
 * Translated code (FEX, box64, Berberis) enters the kernel natively. */
#if defined(__x86_64__)
#define MATON_NATIVE_ARCH AUDIT_ARCH_X86_64
#define MATON_COMPAT_ARCH AUDIT_ARCH_I386
#define MATON_COMPAT_NR_IOCTL 54   /* arch/x86/entry/syscalls/syscall_32.tbl */
/* x32 syscalls set this bit on x86_64; they are not supported. */
#define MATON_NATIVE_NR_FORBIDDEN 0x40000000
#elif defined(__aarch64__)
#define MATON_NATIVE_ARCH AUDIT_ARCH_AARCH64
#define MATON_COMPAT_ARCH AUDIT_ARCH_ARM
#define MATON_COMPAT_NR_IOCTL 54   /* arch/arm/tools/syscall.tbl (EABI) */
#define MATON_NATIVE_NR_FORBIDDEN 0
#elif defined(__riscv) && __riscv_xlen == 64
#define MATON_NATIVE_ARCH AUDIT_ARCH_RISCV64
#define MATON_COMPAT_ARCH AUDIT_ARCH_RISCV32
#define MATON_COMPAT_NR_IOCTL 29   /* generic table (asm-generic/unistd.h) */
#define MATON_NATIVE_NR_FORBIDDEN 0
#else
#error "session-binder-filter.h: add this architecture's native/compat ABIs"
#endif

static int session_filter_binder(void) {
    /* Block Binder transactions/control, including compat commands, before
     * untrusted metadata or payload is parsed. Allow exact DMA-BUF sync
     * commands: IMPORT_SYNC_FILE equals the legacy Binder SET_IDLE_TIMEOUT
     * encoding, which carries no transactions.
     * Filters and NNP survive exec/fork and cannot be relaxed by userns root. */
    struct sock_filter code[]={
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,arch)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,MATON_NATIVE_ARCH,5,0),
        /* The compat ABI gets the same Binder check; any other arch dies. */
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,MATON_COMPAT_ARCH,0,3),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,MATON_COMPAT_NR_IOCTL,6,0),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
        /* A zero mask never matches, keeping jump offsets fixed. */
        BPF_JUMP(BPF_JMP|BPF_JSET|BPF_K,MATON_NATIVE_NR_FORBIDDEN,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_ioctl,0,7),
        /* Shared by both ABIs; a compat argument is the low 32 bits. */
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,args[1])),
        /* DMA-BUF shares Binder's type byte. Allow only the exact fence
         * commands; the legacy idle-timeout collision is documented above. */
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,DMA_BUF_IOCTL_SYNC,5,0),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,DMA_BUF_IOCTL_EXPORT_SYNC_FILE,4,0),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,DMA_BUF_IOCTL_IMPORT_SYNC_FILE,3,0),
        BPF_STMT(BPF_ALU|BPF_AND|BPF_K,0xff00),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,0x6200,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
    };
    struct sock_fprog prog={.len=sizeof(code)/sizeof(code[0]),.filter=code};
    return prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0) || prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&prog);
}

/* SysV IPC (sem*, shm*, msg*): MatonOS kernels keep Android's
 * CONFIG_SYSVIPC off, so these calls go to the app's per-app supervisor in
 * its stub (linux/sysvipc) through a seccomp user-notification listener.
 * Returns the listener fd (O_CLOEXEC), or -1 when this architecture has no
 * emulation yet; the calls then fail with ENOSYS as before. Install after
 * session_filter_binder (which sets no_new_privs and kills x32). */
#if defined(__x86_64__)
static const unsigned maton_sysvipc_native_nr[]={29,30,31,64,65,66,67,68,69,70,71,220};
/* ipc(2) multiplexer plus the direct i386 calls; no direct semop exists. */
static const unsigned maton_sysvipc_compat_nr[]={117,393,394,395,396,397,398,399,400,401,402,420};
#define MATON_SYSVIPC_EMULATED 1
#else
/* arm64/riscv64 use the generic table; linux/sysvipc decodes x86 only. */
#define MATON_SYSVIPC_EMULATED 0
#endif

static int session_filter_sysvipc(void) {
#if MATON_SYSVIPC_EMULATED
    enum { NATIVE_N=sizeof(maton_sysvipc_native_nr)/sizeof(maton_sysvipc_native_nr[0]),
           COMPAT_N=sizeof(maton_sysvipc_compat_nr)/sizeof(maton_sysvipc_compat_nr[0]),
           LEN=3+1+NATIVE_N+1+3+COMPAT_N+1+2 };
    struct sock_filter code[LEN];
    unsigned n=0,to_compat,allow_jumps[2],notify_jumps[NATIVE_N+COMPAT_N],notifies=0;
    code[n++]=(struct sock_filter)BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,arch));
    code[n++]=(struct sock_filter)BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,MATON_NATIVE_ARCH,1,0);
    to_compat=n;code[n++]=(struct sock_filter)BPF_STMT(BPF_JMP|BPF_JA,0);
    code[n++]=(struct sock_filter)BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr));
    for(unsigned i=0;i<NATIVE_N;i++){notify_jumps[notifies++]=n;code[n++]=(struct sock_filter)BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,maton_sysvipc_native_nr[i],0,0);}
    allow_jumps[0]=n;code[n++]=(struct sock_filter)BPF_STMT(BPF_JMP|BPF_JA,0);
    code[to_compat].k=n-to_compat-1;
    code[n++]=(struct sock_filter)BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,arch));
    code[n++]=(struct sock_filter)BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,MATON_COMPAT_ARCH,1,0);
    allow_jumps[1]=n;code[n++]=(struct sock_filter)BPF_STMT(BPF_JMP|BPF_JA,0);
    code[n++]=(struct sock_filter)BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr));
    for(unsigned i=0;i<COMPAT_N;i++){notify_jumps[notifies++]=n;code[n++]=(struct sock_filter)BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,maton_sysvipc_compat_nr[i],0,0);}
    unsigned allow=n;code[n++]=(struct sock_filter)BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW);
    unsigned notify=n;code[n++]=(struct sock_filter)BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_USER_NOTIF);
    /* The compat block's last JEQ falls through to ALLOW, which follows it. */
    for(unsigned i=0;i<2;i++)code[allow_jumps[i]].k=allow-allow_jumps[i]-1;
    for(unsigned i=0;i<notifies;i++)code[notify_jumps[i]].jt=(unsigned char)(notify-notify_jumps[i]-1);
    struct sock_fprog prog={.len=(unsigned short)n,.filter=code};
    return (int)syscall(SYS_seccomp,SECCOMP_SET_MODE_FILTER,SECCOMP_FILTER_FLAG_NEW_LISTENER,&prog);
#else
    return -1;
#endif
}
#endif
