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

static int session_filter_binder(void) {
    /* Block Binder transactions/control, including compat commands, before
     * untrusted metadata or payload is parsed. Allow exact DMA-BUF sync
     * commands: IMPORT_SYNC_FILE equals the legacy Binder SET_IDLE_TIMEOUT
     * encoding, which carries no transactions.
     * Filters and NNP survive exec/fork and cannot be relaxed by userns root. */
    struct sock_filter code[]={
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,arch)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,AUDIT_ARCH_X86_64,1,0),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
        BPF_JUMP(BPF_JMP|BPF_JSET|BPF_K,0x40000000,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_ioctl,0,7),
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
#endif
