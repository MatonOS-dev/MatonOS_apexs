#ifndef MATON_SOCKET_RELAY_H
#define MATON_SOCKET_RELAY_H
/* Native Wayland/X11 stream relay, including SHM, dma-buf and fence FDs. */
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
typedef struct WaylandRelay { int client, compositor; } WaylandRelay;

/* Wayland transfers SHM buffers and fences with SCM_RIGHTS, not just bytes. */
static int relay_wayland_packet(int source, int destination) {
    char bytes[16384];
    union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(64*sizeof(int))]; } control;
    struct iovec buffer = {.iov_base=bytes,.iov_len=sizeof(bytes)};
    struct msghdr message = {.msg_iov=&buffer,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control.bytes)};
    ssize_t count;
    do { count=recvmsg(source,&message,MSG_CMSG_CLOEXEC); } while (count<0 && errno==EINTR);
    if (count<=0) return -1;
    int fds[64]; size_t fd_count=0;
    for (struct cmsghdr* c=CMSG_FIRSTHDR(&message); c; c=CMSG_NXTHDR(&message,c)) {
        if(c->cmsg_level==SOL_SOCKET && c->cmsg_type==SCM_RIGHTS && c->cmsg_len>=CMSG_LEN(0)) {
            size_t n=(c->cmsg_len-CMSG_LEN(0))/sizeof(int);
            int* values=(int*)CMSG_DATA(c);
            for(size_t i=0;i<n;++i) { if(fd_count<64) fds[fd_count++]=values[i]; else close(values[i]); }
        }
    }
    int failed=(message.msg_flags & MSG_CTRUNC)!=0;
    message.msg_flags=0; buffer.iov_len=(size_t)count;
    ssize_t sent=-1;
    if(!failed) do { sent=sendmsg(destination,&message,MSG_NOSIGNAL); } while(sent<0 && errno==EINTR);
    for(size_t i=0;i<fd_count;++i)close(fds[i]);
    if(sent<=0)return -1;
    while(sent<count) {
        ssize_t n=send(destination,bytes+sent,(size_t)(count-sent),MSG_NOSIGNAL);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)return -1;
        sent+=n;
    }
    return 0;
}
static void* relay_wayland(void* argument) {
    WaylandRelay* relay=argument;
    struct pollfd sockets[2]={{.fd=relay->client,.events=POLLIN},{.fd=relay->compositor,.events=POLLIN}};
    for(;;) {
        int ready=poll(sockets,2,-1);
        if(ready<0 && errno==EINTR)continue;
        if(ready<=0)break;
        int failed=0;
        for(int i=0;i<2;++i) {
            if(sockets[i].revents&POLLIN) { if(relay_wayland_packet(sockets[i].fd,sockets[1-i].fd))failed=1; }
            else if(sockets[i].revents&(POLLHUP|POLLERR|POLLNVAL))failed=1;
        }
        if(failed)break;
    }
    close(relay->client);close(relay->compositor);free(relay);return NULL;
}
#endif
