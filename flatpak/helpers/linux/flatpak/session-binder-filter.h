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
#endif
