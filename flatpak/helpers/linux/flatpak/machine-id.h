/* Shared by the launcher and its host-side persistence test. */
#ifndef MATON_MACHINE_ID_H
#define MATON_MACHINE_ID_H
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#define MATON_MACHINE_ID "/data/matonos/linux/machine-id"

/* Fail-closed preparation of the Flatpak machine-id. On failure the caller
 * gets a specific reason (ownership, invalid content, or the real errno);
 * logical failures never report errno's stale "Success". */
static inline int prepare_machine_id_reason(const char* root, char* reason, size_t reason_size) {
    if(reason && reason_size)reason[0]=0;
    int dir=open(root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(dir<0) {
        if(reason)snprintf(reason,reason_size,"cannot open %s: %s",root,strerror(errno));
        return -1;
    }
    int lock=openat(dir,"machine-id.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
    struct stat st;
    int rc=-1, fd=-1;
    if(lock<0) {
        if(reason)snprintf(reason,reason_size,"cannot open machine-id.lock: %s",strerror(errno));
        goto done;
    }
    if(fstat(lock,&st)) {
        if(reason)snprintf(reason,reason_size,"cannot stat machine-id.lock: %s",strerror(errno));
        goto done;
    }
    if(!S_ISREG(st.st_mode)) {
        if(reason)snprintf(reason,reason_size,"machine-id.lock is not a regular file");
        goto done;
    }
    if(st.st_uid!=getuid()) {
        if(reason)snprintf(reason,reason_size,"machine-id.lock owned by uid %u, running as uid %u",
                (unsigned)st.st_uid,(unsigned)getuid());
        goto done;
    }
    if(st.st_mode&077) {
        if(reason)snprintf(reason,reason_size,"machine-id.lock has unsafe mode %04o",
                (unsigned)(st.st_mode&0777));
        goto done;
    }
    if(flock(lock,LOCK_EX)) {
        if(reason)snprintf(reason,reason_size,"cannot lock machine-id.lock: %s",strerror(errno));
        goto done;
    }
    fd=openat(dir,"machine-id",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(fd>=0) {
        char text[34]; ssize_t n=read(fd,text,sizeof(text));
        if(n<0) {
            if(reason)snprintf(reason,reason_size,"cannot read machine-id: %s",strerror(errno));
            goto done;
        }
        if(fstat(fd,&st)) {
            if(reason)snprintf(reason,reason_size,"cannot stat machine-id: %s",strerror(errno));
            goto done;
        }
        if(!S_ISREG(st.st_mode)) {
            if(reason)snprintf(reason,reason_size,"machine-id is not a regular file");
            goto done;
        }
        if(st.st_uid!=getuid()) {
            if(reason)snprintf(reason,reason_size,"machine-id owned by uid %u, running as uid %u",
                    (unsigned)st.st_uid,(unsigned)getuid());
            goto done;
        }
        if(n!=32 && n!=33) {
            if(reason)snprintf(reason,reason_size,"invalid machine-id content (length %zd)",n);
            goto done;
        }
        if(n==33 && text[32]!='\n') {
            if(reason)snprintf(reason,reason_size,"invalid machine-id content (missing newline)");
            goto done;
        }
        int nonzero=0;
        for(int i=0;i<32;i++) {
            if(!((text[i]>='0'&&text[i]<='9')||(text[i]>='a'&&text[i]<='f'))) {
                if(reason)snprintf(reason,reason_size,"invalid machine-id content (non-hex character at offset %d)",i);
                goto done;
            }
            nonzero |= text[i]!='0';
        }
        if(nonzero)rc=0;
        else if(reason)snprintf(reason,reason_size,"invalid machine-id content (all zeroes)");
        goto done;
    }
    if(errno!=ENOENT) {
        if(reason)snprintf(reason,reason_size,"cannot open machine-id: %s",strerror(errno));
        goto done;
    }
    unsigned char random[16]; size_t used=0;
    while(used<sizeof(random)) {
        ssize_t n=getrandom(random+used,sizeof(random)-used,0);
        if(n<0&&errno==EINTR)continue;
        if(n<=0) {
            if(n==0) {
                if(reason)snprintf(reason,reason_size,"getrandom returned no entropy");
            } else {
                if(reason)snprintf(reason,reason_size,"getrandom failed: %s",strerror(errno));
            }
            goto done;
        }
        used+=(size_t)n;
    }
    char text[33];static const char hex[]="0123456789abcdef";
    for(int i=0;i<16;i++){text[2*i]=hex[random[i]>>4];text[2*i+1]=hex[random[i]&15];}
    text[32]='\n';
    /* The lock serializes generation; rename publishes only a complete ID.
     * A crash before rename leaves a disposable temporary, never a partial ID. */
    if(unlinkat(dir,"machine-id.tmp",0) && errno!=ENOENT) {
        if(reason)snprintf(reason,reason_size,"cannot remove stale machine-id.tmp: %s",strerror(errno));
        goto done;
    }
    fd=openat(dir,"machine-id.tmp",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0) {
        if(reason)snprintf(reason,reason_size,"cannot create machine-id.tmp: %s",strerror(errno));
        goto done;
    }
    if(fstat(fd,&st)) {
        if(reason)snprintf(reason,reason_size,"cannot stat machine-id.tmp: %s",strerror(errno));
        goto done;
    }
    if(!S_ISREG(st.st_mode)) {
        if(reason)snprintf(reason,reason_size,"machine-id.tmp is not a regular file");
        goto done;
    }
    if(st.st_uid!=getuid()) {
        if(reason)snprintf(reason,reason_size,"machine-id.tmp owned by uid %u, running as uid %u",
                (unsigned)st.st_uid,(unsigned)getuid());
        goto done;
    }
    used=0;
    while(used<sizeof(text)) {
        ssize_t n=write(fd,text+used,sizeof(text)-used);
        if(n<0&&errno==EINTR)continue;
        if(n<=0) {
            if(n==0) {
                if(reason)snprintf(reason,reason_size,"short write to machine-id.tmp");
            } else {
                if(reason)snprintf(reason,reason_size,"cannot write machine-id.tmp: %s",strerror(errno));
            }
            goto done;
        }
        used+=(size_t)n;
    }
    if(fchmod(fd,0444)) {
        if(reason)snprintf(reason,reason_size,"cannot chmod machine-id.tmp: %s",strerror(errno));
        goto done;
    }
    if(fsync(fd)) {
        if(reason)snprintf(reason,reason_size,"cannot sync machine-id.tmp: %s",strerror(errno));
        goto done;
    }
    if(renameat(dir,"machine-id.tmp",dir,"machine-id")) {
        if(reason)snprintf(reason,reason_size,"cannot publish machine-id: %s",strerror(errno));
        goto done;
    }
    if(fsync(dir)) {
        if(reason)snprintf(reason,reason_size,"cannot sync machine-id directory: %s",strerror(errno));
        goto done;
    }
    rc=0;
done:
    if(fd>=0)close(fd);
    if(lock>=0)close(lock);
    close(dir);return rc;
}

static inline int prepare_machine_id(const char* root) {
    return prepare_machine_id_reason(root,NULL,0);
}
#endif
