#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/*
 * Bionic host launcher for the static Flatpak CLI. linuxd execs
 * /apex/com.matonos.flatpak/bin/flatpak-env-wrapper; this process prepares
 * the Android-side session and environment, then execs the static multicall
 * binary at matonos-flatpak. linuxd passes argv[0]="flatpak" or "ostree"
 * so multicall dispatch does not need APEX symlinks (Soong drops prebuilt
 * symlinks from the payload).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "machine-id.h"
#include "app-session.h"
#include "socket-relay.h"
#include "../dbus-broker/session-control.h"
#include <stddef.h>

#define SESSION_BUS_DIRECTORY "/data/matonos/linux/runtime/session-bus-%d"
/* r24: the whole Flatpak stack ships in the com.matonos.flatpak APEX, mounted
 * at /apex/com.matonos.flatpak, instead of /system_ext. The binaries find
 * their own libraries through the APEX linker namespace, so no
 * LD_LIBRARY_PATH is needed. */
#define MATON_FLATPAK_APEX "/apex/com.matonos.flatpak"
#define MATON_FLATPAK_BIN MATON_FLATPAK_APEX "/bin"
static volatile sig_atomic_t portal_stop;
static void stop_portal(int number) {(void)number;portal_stop=1;}

static void remove_session_directory(int fd,const char* path) {
    if(fd>=0) {
        (void)fchown(fd,1000,1000);(void)fchmod(fd,0700);
        unlinkat(fd,"bus",0);unlinkat(fd,"wayland-0",0);unlinkat(fd,"X0",0);close(fd);
        rmdir(path);
    }
}
static int prepare_session_directory(const char* path) {
    struct stat info;
    if(lstat("/data/matonos/linux/runtime",&info) || !S_ISDIR(info.st_mode) ||
       info.st_uid!=getuid() || (info.st_mode&07777)!=0711)return -1;
    int created=mkdir(path,0700)==0;
    if(!created && errno!=EEXIST)return -1;
    int fd=open(path,O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0 || fstat(fd,&info) || info.st_uid!=getuid()) {
        if(fd>=0)close(fd);
        if(created)rmdir(path);
        return -1;
    }
    /* A live supervisor uniquely owns this PID. Remove only named sockets
     * left by abrupt death, after checking the native directory's owner. */
    const char* names[]={"bus","wayland-0","X0"};
    for(unsigned i=0;i<3;i++) {
        if(fstatat(fd,names[i],&info,AT_SYMLINK_NOFOLLOW)) {
            if(errno==ENOENT)continue;
        } else if(S_ISSOCK(info.st_mode) && !unlinkat(fd,names[i],0))continue;
        close(fd);if(created)rmdir(path);return -1;
    }
    if(fchmod(fd,0710) || fchown(fd,1000,(gid_t)atoi(getenv("MATON_APP_UID")))){remove_session_directory(fd,path);return -1;}
    return fd;
}

static int display_listener(int directory,const char* name) {
    struct sockaddr_un address={.sun_family=AF_UNIX};
    snprintf(address.sun_path,sizeof(address.sun_path),"/proc/self/fd/%d/%s",directory,name);
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
    if(fd>=0 && !bind(fd,(struct sockaddr*)&address,sizeof(address)) &&
       !fchmodat(directory,name,0660,0) && !fchownat(directory,name,1000,(gid_t)atoi(getenv("MATON_APP_UID")),0) && !listen(fd,16))return fd;
    if(fd>=0)close(fd);
    return -1;
}
static void accept_display(int listener,int directory,const char* name,unsigned* connections) {
    int client=accept4(listener,NULL,NULL,SOCK_CLOEXEC);
    if(client<0)return;
    if((*connections)++>=128){close(client);return;}
    int upstream=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
    struct sockaddr_un address={.sun_family=AF_UNIX};
    snprintf(address.sun_path,sizeof(address.sun_path),"/proc/self/fd/%d/%s",directory,name);
    if(upstream<0 || connect(upstream,(struct sockaddr*)&address,sizeof(address))) {
        if(upstream>=0)close(upstream);
        close(client);return;
    }
    struct timeval timeout={.tv_sec=5};
    setsockopt(client,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    setsockopt(upstream,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    WaylandRelay* relay=malloc(sizeof(*relay));pthread_t thread;
    if(relay){relay->client=client;relay->compositor=upstream;}
    pthread_attr_t attributes;pthread_attr_init(&attributes);pthread_attr_setstacksize(&attributes,256*1024);
    int error=relay ? pthread_create(&thread,&attributes,relay_wayland,relay) : ENOMEM;
    pthread_attr_destroy(&attributes);
    if(error){free(relay);close(client);close(upstream);}else pthread_detach(thread);
}

static int valid_dns_server(const char* server) {
    unsigned char address[16];
    if(inet_pton(AF_INET,server,address)==1 || inet_pton(AF_INET6,server,address)==1)return 1;
    const char* zone=strchr(server,'%');
    if(!zone || zone==server || !zone[1] || (size_t)(zone-server)>=INET6_ADDRSTRLEN)return 0;
    char base[INET6_ADDRSTRLEN];
    memcpy(base,server,(size_t)(zone-server));base[zone-server]=0;
    if(inet_pton(AF_INET6,base,address)!=1)return 0;
    for(const char* p=zone+1;*p;p++)
        if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_'||*p=='-'||*p=='.'))return 0;
    return strlen(zone+1)<16;
}

static int install_directory_uid(const char* path, unsigned* result) {
    unsigned uid = 0;
    char trailing;
    char expected[128];
    if (!path || sscanf(path, "/data/matonos/linux/apps/%u%c", &uid, &trailing) != 1 ||
            uid % 100000 < 10000 || uid % 100000 > 19999) return 0;
    snprintf(expected, sizeof(expected), "/data/matonos/linux/apps/%u", uid);
    if (strcmp(path, expected)) return 0;
    *result = uid;
    return 1;
}

static int monitor_file(int directory, const char* name, const char* text) {
    int fd=openat(directory,name,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0644);
    if(fd<0)return -1;
    size_t size=strlen(text);ssize_t written=write(fd,text,size);close(fd);
    return written==(ssize_t)size ? 0 : -1;
}
static int prepare_monitor(const char* display, const char* dns, char* path, size_t size) {
    snprintf(path,size,"%s-monitor",display);
    if(mkdir(path,0700) && errno!=EEXIST)return -1;
    int directory=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    struct stat info;
    if(directory<0)return -1;
    if(fstat(directory,&info)||info.st_uid!=getuid()||(info.st_mode&077)){close(directory);return -1;}
    char resolv[2048]={0};size_t used=0;
    char servers[2048];snprintf(servers,sizeof(servers),"%s",dns ? dns : "");
    char* state=NULL;
    int server_count=0;
    for(char* server=strtok_r(servers,",",&state);server;server=strtok_r(NULL,",",&state)) {
        if(!valid_dns_server(server)) {close(directory);return -1;}
        int added=snprintf(resolv+used,sizeof(resolv)-used,"nameserver %s\n",server);
        if(added<0 || (size_t)added>=sizeof(resolv)-used){close(directory);return -1;}
        used+=(size_t)added;
        server_count++;
    }
    /* Phase 1 stub: linuxd will replace loopback with its per-app forwarder. */
    if(!server_count)snprintf(resolv,sizeof(resolv),"nameserver 127.0.0.1\n");
    int rc=monitor_file(directory,"resolv.conf",resolv) ||
        monitor_file(directory,"hosts","127.0.0.1 localhost\n::1 localhost\n") ||
        monitor_file(directory,"host.conf","multi on\n") || monitor_file(directory,"gai.conf","") ||
        monitor_file(directory,"passwd","linuxd:x:1000:1000:Linux user:/home/linuxd:/sbin/nologin\n") ||
        monitor_file(directory,"group","linuxd:x:1000:\n");
    close(directory);return rc ? -1 : 0;
}
/* A journald socket for the app sandbox (matonos-bwrap binds it at
 * /run/systemd/journal/socket): programs built with journald logging refuse
 * to start without one. Each datagram's MESSAGE field goes to stderr, which
 * linuxd keeps as the app's launch log. */
static int start_journal_sink(const char* display, char* path, size_t size) {
    struct sockaddr_un address={.sun_family=AF_UNIX};
    snprintf(path,size,"%s-journal",display);
    if(strlen(path)>=sizeof(address.sun_path))return -1;
    struct stat previous;
    if(!lstat(path,&previous)) {
        if(!S_ISSOCK(previous.st_mode)||previous.st_uid!=getuid()||unlink(path))return -1;
    } else if(errno!=ENOENT)return -1;
    strcpy(address.sun_path,path);
    int fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0);
    if(fd<0)return -1;
    if(bind(fd,(struct sockaddr*)&address,sizeof(address))){close(fd);return -1;}
    if(chmod(path,0660)||chown(path,1000,(gid_t)atoi(getenv("MATON_APP_UID")?getenv("MATON_APP_UID"):"1000"))){close(fd);unlink(path);return -1;}
    pid_t parent=getpid(), sink=fork();
    if(sink<0){close(fd);unlink(path);return -1;}
    if(sink>0){close(fd);return 0;}
    close(196);
    if(prctl(PR_SET_PDEATHSIG,SIGTERM)||getppid()!=parent)_exit(0);
    const char* directory=getenv("MATON_SESSION_DIRECTORY_FD");
    if(directory)close(atoi(directory));
    const char* x11_directory=getenv("MATON_SESSION_X11_FD");
    if(x11_directory)close(atoi(x11_directory));
    static char message[65536];
    for(;;) {
        ssize_t got=recv(fd,message,sizeof(message)-1,0);
        if(got<0){if(errno==EINTR)continue;break;}
        message[got]=0;
        /* Native protocol: KEY=value lines; binary fields are skipped. */
        for(char* line=message;line<message+got;) {
            char* end=memchr(line,'\n',(size_t)(message+got-line));
            if(!end)end=message+got;
            if(end-line>8&&!memcmp(line,"MESSAGE=",8)) {
                (void)!write(STDERR_FILENO,line+8,(size_t)(end-line-8));
                (void)!write(STDERR_FILENO,"\n",1);
            }
            line=end+1;
        }
    }
    unlink(path);_exit(0);
}
/* Query the actual image CLI, bypassing this wrapper (no recursive portal
 * startup). Bounded output/time; a broken CLI reports unknown and fails closed. */
static void system_flatpak_version(char version[64]) {
    snprintf(version,64,"unknown");
    int output[2];if(pipe2(output,O_CLOEXEC))return;
    pid_t child=fork();
    if(child<0){close(output[0]);close(output[1]);return;}
    if(child==0) {
        close(output[0]);
        if(dup2(output[1],STDOUT_FILENO)<0)_exit(127);
        close(output[1]);
        execl(MATON_FLATPAK_BIN "/matonos-flatpak","flatpak","--version",NULL);
        _exit(127);
    }
    close(output[1]);
    if(fcntl(output[0],F_SETFL,O_NONBLOCK)<0) {
        close(output[0]);kill(child,SIGKILL);while(waitpid(child,NULL,0)<0&&errno==EINTR){};return;
    }
    char text[128];size_t used=0;int status=0,exited=0;
    struct timespec start,now;clock_gettime(CLOCK_MONOTONIC,&start);
    for(;;) {
        ssize_t got=read(output[0],text+used,sizeof(text)-1-used);
        if(got>0)used+=(size_t)got;
        pid_t waited=waitpid(child,&status,WNOHANG);
        if(waited==child){exited=1;break;}
        if(waited<0 && errno!=EINTR)break;
        clock_gettime(CLOCK_MONOTONIC,&now);
        if((now.tv_sec-start.tv_sec)*1000+(now.tv_nsec-start.tv_nsec)/1000000>=2000 ||
           used==sizeof(text)-1)break;
        struct pollfd ready={.fd=got==0 ? -1 : output[0],.events=POLLIN};
        (void)poll(&ready,1,20);
    }
    if(exited && used<sizeof(text)-1) {
        ssize_t got=read(output[0],text+used,sizeof(text)-1-used);
        if(got>0)used+=(size_t)got;
    }
    close(output[0]);
    if(!exited){kill(child,SIGKILL);while(waitpid(child,NULL,0)<0&&errno==EINTR){};return;}
    if(!WIFEXITED(status) || WEXITSTATUS(status)!=0)return;
    text[used]=0;
    while(used && (text[used-1]=='\n'||text[used-1]=='\r'||text[used-1]==' '))text[--used]=0;
    if(strncmp(text,"Flatpak ",8) || !text[8] || strlen(text+8)>=64)return;
    snprintf(version,64,"%s",text+8);
}
/* The supervisor holds the delegated directory for the whole host session.
 * It delegates a native runtime directory back to the compositor broker,
 * which exposes a second socket without relaying D-Bus credentials. Native
 * display relays also survive the initial CLI for nested portal launches.
 * The control capability never reaches application children. */
static int __attribute__((unused)) start_session_portal(int directory, int x11_directory, const char* x11_name, const char* monitor, char* bus, size_t size) {
    int ready[2];if(pipe2(ready,O_CLOEXEC))return -1;
    pid_t supervisor=fork();
    if(supervisor<0){close(ready[0]);close(ready[1]);return -1;}
    if(supervisor==0) {
        close(ready[0]);
        char native_directory[128],bus_path[160];
        snprintf(native_directory,sizeof(native_directory),SESSION_BUS_DIRECTORY,getpid());
        snprintf(bus_path,sizeof(bus_path),"%s/bus",native_directory);
        int native_fd=prepare_session_directory(native_directory);
        if(native_fd<0)_exit(127);
        /* Nested portal launches outlive the initial CLI's linuxd relays.
         * Retain both display capabilities here and expose native sockets. */
        int wayland_listener=display_listener(native_fd,"wayland-0");
        int x11_listener=x11_directory>=0 ? display_listener(native_fd,"X0") : -1;
        if(wayland_listener<0 || (x11_directory>=0 && x11_listener<0)) {
            remove_session_directory(native_fd,native_directory);_exit(127);
        }
        /* The runtime parent is system-owned 0700. Only the broker gets this
         * directory capability; application sandboxes receive one bus socket. */
        struct sigaction stop={.sa_handler=stop_portal};sigemptyset(&stop.sa_mask);
        if(sigaction(SIGTERM,&stop,NULL) || sigaction(SIGINT,&stop,NULL)) {
            remove_session_directory(native_fd,native_directory);_exit(127);
        }
        char flatpak_version[64];system_flatpak_version(flatpak_version);
        struct sockaddr_un address={.sun_family=AF_UNIX};
        snprintf(address.sun_path,sizeof(address.sun_path),"/proc/self/fd/%d/bus-control",directory);
        int control=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);
        if(control<0 || connect(control,(struct sockaddr*)&address,sizeof(address))) {
            remove_session_directory(native_fd,native_directory);_exit(127);
        }
        int gate[2];if(pipe2(gate,O_CLOEXEC)) {
            remove_session_directory(native_fd,native_directory);_exit(127);
        }
        pid_t parent=getpid(),portal=fork();
        if(portal<0){remove_session_directory(native_fd,native_directory);_exit(127);}
        if(portal==0) {
            close(196);close(gate[1]);close(control);close(ready[1]);close(directory);
            close(native_fd);
            close(wayland_listener);if(x11_listener>=0)close(x11_listener);
            signal(SIGTERM,SIG_DFL);signal(SIGINT,SIG_DFL);
            if(x11_directory>=0)close(x11_directory);
            if(prctl(PR_SET_PDEATHSIG,SIGTERM)||getppid()!=parent)_exit(127);
            char value=0;if(read(gate[0],&value,1)!=1 || value!=1)_exit(127);
            close(gate[0]);
            char address[192];snprintf(address,sizeof(address),"unix:path=%s",bus_path);
            if(setenv("DBUS_SESSION_BUS_ADDRESS",address,1))_exit(127);
            snprintf(address,sizeof(address),"%s/wayland-0",native_directory);
            if(setenv("WAYLAND_DISPLAY",address,1))_exit(127);
            if(x11_directory>=0) {
                snprintf(address,sizeof(address),"%s/X0",native_directory);
                if(setenv("MATON_X11_SOCKET",address,1))_exit(127);
            } else unsetenv("MATON_X11_SOCKET");
            if(session_portal_uid())_exit(127);
            execl(MATON_FLATPAK_BIN "/flatpak-portal","flatpak-portal",NULL);_exit(127);
        }
        close(gate[0]);
        struct stat host_directory;
        if(fstat(directory,&host_directory) || host_directory.st_uid<10000 ||
           fchmod(native_fd,0750) ||
           fchown(native_fd,host_directory.st_uid,(gid_t)atoi(getenv("MATON_APP_UID")))) {
            kill(portal,SIGTERM);remove_session_directory(native_fd,native_directory);_exit(127);
        }
        struct MatonSessionRegistration registration={.pid=portal};
        snprintf(registration.flatpak_version,sizeof(registration.flatpak_version),"%s",flatpak_version);
        snprintf(registration.monitor,sizeof(registration.monitor),"%s",monitor);
        struct MatonSessionReply reply={0};
        struct pollfd event={.fd=control,.events=POLLIN};
        union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } ancillary={0};
        struct iovec payload={.iov_base=&registration,.iov_len=sizeof(registration)};
        struct msghdr packet={.msg_iov=&payload,.msg_iovlen=1,.msg_control=ancillary.bytes,.msg_controllen=sizeof(ancillary.bytes)};
        struct cmsghdr* rights=CMSG_FIRSTHDR(&packet);
        rights->cmsg_level=SOL_SOCKET;rights->cmsg_type=SCM_RIGHTS;rights->cmsg_len=CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(rights),&native_fd,sizeof(native_fd));
        int ok=sendmsg(control,&packet,MSG_NOSIGNAL)==sizeof(registration) &&
            poll(&event,1,5000)>0 && recv(control,&reply,sizeof(reply),MSG_TRUNC)==sizeof(reply);
        if(ok && reply.status==MATON_SESSION_FLATPAK_UNSUPPORTED) {
            fprintf(stderr,"matonos-flatpak: %.*s: %.*s\n",
                (int)sizeof(reply.error),reply.error,(int)sizeof(reply.message),reply.message);
            ok=0;
        } else if(!ok) {
            fprintf(stderr,"matonos-flatpak: invalid or missing session portal registration reply\n");
        }
        if(ok && reply.status==0) {
            char value=1;ok=write(gate[1],&value,1)==1;
            if(ok)ok=poll(&event,1,5000)>0 && recv(control,&value,1,0)==1 && value==1;
        } else if(ok && reply.status==2) {
            close(gate[1]);gate[1]=-1;
            kill(portal,SIGTERM);while(waitpid(portal,NULL,0)<0&&errno==EINTR){}
            close(wayland_listener);if(x11_listener>=0)close(x11_listener);
            remove_session_directory(native_fd,native_directory);
            (void)!write(ready[1],&reply.supervisor,sizeof(reply.supervisor));_exit(0);
        } else ok=0;
        if(gate[1]>=0)close(gate[1]);
        if(ok)(void)!write(ready[1],&parent,sizeof(parent));
        close(ready[1]);
        /* Do not reap until control EOF has revoked the registered PID. A
         * zombie reserves the PID against reuse during broker revocation. */
        unsigned connections=0;
        while(ok && !portal_stop) {
            struct pollfd life={.fd=196,.events=POLLIN};
            if(poll(&life,1,0)>0 && life.revents)break;
            siginfo_t info={0};
            if(waitid(P_PID,portal,&info,WEXITED|WNOHANG|WNOWAIT)<0 || info.si_pid)break;
            struct pollfd events[3]={event,{.fd=wayland_listener,.events=POLLIN},{.fd=x11_listener,.events=POLLIN}};
            int rc=poll(events,3,100);
            if(rc<0 && errno==EINTR)continue;
            if(rc<0 || events[0].revents)break;
            if(events[1].revents&POLLIN)accept_display(wayland_listener,directory,"wayland-0",&connections);
            if(events[2].revents&POLLIN)accept_display(x11_listener,x11_directory,x11_name,&connections);
        }
        kill(portal,SIGTERM);
        /* Closing control before reaping reserves the child PID until the
         * broker acknowledges revocation by closing its end. */
        shutdown(control,SHUT_WR);
        char byte;while(recv(control,&byte,1,0)<0&&errno==EINTR){}
        close(control);
        while(waitpid(portal,NULL,0)<0&&errno==EINTR){}
        close(wayland_listener);if(x11_listener>=0)close(x11_listener);
        remove_session_directory(native_fd,native_directory);
        close(directory);if(x11_directory>=0)close(x11_directory);_exit(ok?0:127);
    }
    close(ready[1]);pid_t owner=0;
    struct pollfd event={.fd=ready[0],.events=POLLIN};
    int ok=poll(&event,1,12000)>0 && read(ready[0],&owner,sizeof(owner))==sizeof(owner) && owner>0;
    close(ready[0]);
    if(!ok){kill(supervisor,SIGTERM);while(waitpid(supervisor,NULL,0)<0&&errno==EINTR){}return -1;}
    snprintf(bus,size,"unix:path=" SESSION_BUS_DIRECTORY "/bus",owner);return 0;
}

#ifdef MATONOS_HOST_LAUNCH_PROBE
int main(int argc, char** argv) {
    if (argc < 3 || strcmp(argv[1], "--host-probe")) {
        fprintf(stderr, "usage: flatpak-env-wrapper-host-probe --host-probe STATIC_FLATPAK [args...]\n");
        return 2;
    }
    char** child = calloc((size_t)argc, sizeof(*child));
    if (!child) return 127;
    child[0] = (char*)"flatpak";
    for (int i = 3; i < argc; i++) child[i - 2] = argv[i];
    execv(argv[2], child);
    perror("flatpak-env-wrapper-host-probe: exec static Flatpak");
    return 127;
}
#else
int main(int argc, char** argv) {
    (void)argc;
    /* linuxd supplies the multicall applet name in argv[0]. */
    if(strcmp(argv[0],"flatpak") && strcmp(argv[0],"ostree")) {
        fprintf(stderr,"matonos-flatpak: unsupported applet name: %s\n",argv[0]);
        return 127;
    }
    const char* staging_request=getenv("MATON_FLATPAK_STAGING_DIR");
    int installer_mode=staging_request && !strcmp(staging_request,"/data/matonos/linux/staging") && geteuid()==2902;
    if(staging_request && !installer_mode) {
        fprintf(stderr,"matonos-flatpak: invalid staging installer context\n");return 127;
    }
    int is_run=0;
    for(int i=1;i<argc;i++)if(!strcmp(argv[i],"run")){is_run=1;break;}
    AppSession app={.lifeline=-1,.group=-1};
    int owned=is_run;
    int initial=getenv("MATON_SESSION_DIRECTORY_FD")!=NULL;
    if(owned && (session_owner(&app,initial) || session_home(&app))) {
        perror("matonos-flatpak: unverified app owner");return 127;
    }
    if(owned){char uid[32];snprintf(uid,sizeof(uid),"%u",app.uid);if(setenv("MATON_APP_UID",uid,1))return 127;}
    char flatpak_system_dir[128], flatpak_user_dir[128];
    const char* supplied_system_dir=getenv("FLATPAK_SYSTEM_DIR");
    const char* supplied_user_dir=getenv("FLATPAK_USER_DIR");
    if(owned) {
        unsigned runtime_install_uid=0;
        if(!install_directory_uid(supplied_system_dir,&runtime_install_uid) ||
                runtime_install_uid==(unsigned)app.uid ||
                runtime_install_uid/100000!=(unsigned)app.uid/100000 || !supplied_user_dir ||
                strcmp(supplied_user_dir,app.data_dir)) {
            fprintf(stderr,"matonos-flatpak: invalid installation roots for verified app\n");return 127;
        }
        snprintf(flatpak_system_dir,sizeof(flatpak_system_dir),"%s",supplied_system_dir);
        snprintf(flatpak_user_dir,sizeof(flatpak_user_dir),"%s",supplied_user_dir);
    } else {
        snprintf(flatpak_system_dir,sizeof(flatpak_system_dir),"%s",
                supplied_system_dir ? supplied_system_dir : "/data/matonos/linux/flatpak");
        snprintf(flatpak_user_dir,sizeof(flatpak_user_dir),"%s",
                supplied_user_dir ? supplied_user_dir : "/data/matonos/linux/flatpak-user");
    }
    const char* display = getenv("WAYLAND_DISPLAY");
    const char* bwrap = MATON_FLATPAK_BIN "/matonos-bwrap";
    if(!installer_mode) {
        char machine_reason[256];
        if(prepare_machine_id_reason("/data/matonos/linux",machine_reason,sizeof(machine_reason))) {
            fprintf(stderr,"matonos-flatpak: machine-id: %s\n",
                    machine_reason[0] ? machine_reason : "unknown failure");
            return 127;
        }
    }
    int display_fd = -1; char trailing; char display_copy[128] = {0};
    char static_base[192],static_config[192]={0};
    snprintf(static_base,sizeof(static_base),"/data/matonos/linux/runtime/flatpak-config-%d",getpid());
    const char* supplied_dns=getenv("MATON_FLATPAK_DNS");
    if(!installer_mode && prepare_monitor(static_base,supplied_dns,static_config,sizeof(static_config))) {
        fprintf(stderr,"matonos-flatpak: cannot prepare static namespace configuration\n");return 127;
    }
    char x11_socket[160] = {0};
    int graphical = display && sscanf(display, "/data/matonos/linux/runtime/wayland-%d%c", &display_fd, &trailing) == 1 && display_fd >= 0;
    int nested=0, owner=0;
    if(!graphical && display && sscanf(display,SESSION_BUS_DIRECTORY "/wayland-0%c",&owner,&trailing)==1 && owner>0) {
        char expected[192];snprintf(expected,sizeof(expected),"unix:path=" SESSION_BUS_DIRECTORY "/bus",owner);
        nested=getenv("DBUS_SESSION_BUS_ADDRESS") && !strcmp(getenv("DBUS_SESSION_BUS_ADDRESS"),expected);
        snprintf(expected,sizeof(expected),SESSION_BUS_DIRECTORY "/wayland-0",owner);
        nested=nested && !strcmp(display,expected);
        graphical=nested;
    }
    if (graphical) snprintf(display_copy, sizeof(display_copy), "%s", display);
    if (graphical) {
        // linuxd relays X11 on a sibling socket of the Wayland
        // one; only then does the bwrap shim have a socket to bind.
        struct stat socket_info;
        if(nested) {
            const char* inherited=getenv("MATON_X11_SOCKET");
            int pid;char extra;
            if(inherited && sscanf(inherited,SESSION_BUS_DIRECTORY "/X0%c",&pid,&extra)==1 && pid==owner)
                snprintf(x11_socket,sizeof(x11_socket),"%s",inherited);
        } else snprintf(x11_socket, sizeof(x11_socket), "%s-x11", display_copy);
        if (lstat(x11_socket, &socket_info) != 0 || !S_ISSOCK(socket_info.st_mode))
            x11_socket[0] = '\0';
        else
            bwrap = MATON_FLATPAK_BIN "/matonos-bwrap";
    }
    char journal[160] = {0};
    if (graphical) {
        char journal_display[128];
        snprintf(journal_display,sizeof(journal_display),"/data/matonos/linux/runtime/wayland-%d",getpid());
        if (start_journal_sink(nested?journal_display:display_copy, journal, sizeof(journal)) == 0)
            bwrap = MATON_FLATPAK_BIN "/matonos-bwrap";
        else journal[0] = '\0';
    }
    int session_directory=-1;
    const char* directory=getenv("MATON_SESSION_DIRECTORY_FD");
    if(directory) {
        if(strcmp(directory,"198"))return 127;
        session_directory=198;
        if(fcntl(session_directory,F_SETFD,FD_CLOEXEC))return 127;
    }
    int x11_directory=-1;char x11_name[64]={0};
    const char* x11_fd=getenv("MATON_SESSION_X11_FD");
    const char* x11_file=getenv("MATON_SESSION_X11_NAME");
    if(x11_fd) {
        int number;char extra;
        if(session_directory<0 || strcmp(x11_fd,"199") || !x11_file ||
           sscanf(x11_file,"X%d%c",&number,&extra)!=1 || number<0)return 127;
        x11_directory=199;
        if(fcntl(x11_directory,F_SETFD,FD_CLOEXEC))return 127;
        snprintf(x11_name,sizeof(x11_name),"%s",x11_file);
    }
    char pads[320];
    const char *pad_list=getenv("MATON_SESSION_PAD_NODES");
    if(pad_list && strlen(pad_list)>=sizeof(pads))return 127;
    snprintf(pads,sizeof(pads),"%s",owned && pad_list ? pad_list : "");
    char dns[2048], bus[192];
    snprintf(dns,sizeof(dns),"%s",getenv("MATON_FLATPAK_DNS") ? getenv("MATON_FLATPAK_DNS") : "");
    snprintf(bus,sizeof(bus),"%s",getenv("DBUS_SESSION_BUS_ADDRESS") ? getenv("DBUS_SESSION_BUS_ADDRESS") : "");
    if (clearenv() != 0 ||
        setenv("PATH", MATON_FLATPAK_BIN ":/system/bin:/system/xbin", 1) != 0 ||
        setenv("XDG_RUNTIME_DIR", "/data/matonos/linux/runtime", 1) != 0 ||
        setenv("TMPDIR", "/tmp", 1) != 0 ||
        setenv("HOME", "/data/matonos/linux/flatpak-data", 1) != 0 ||
        setenv("TMPDIR", "/tmp", 1) != 0 ||
        setenv("XDG_DATA_HOME", "/data/matonos/linux/flatpak-data/.local/share", 1) != 0 ||
        setenv("FLATPAK_SYSTEM_DIR", installer_mode ? "/data/matonos/linux/staging" : flatpak_system_dir, 1) != 0 ||
        setenv("FLATPAK_SYSTEM_CACHE_DIR", "/data/matonos/linux/cache", 1) != 0 ||
        setenv("FLATPAK_USER_DIR", flatpak_user_dir, 1) != 0 ||
        setenv("FLATPAK_DOWNLOAD_TMPDIR", "/tmp", 1) != 0 ||
        setenv("SSL_CERT_DIR", "/apex/com.android.conscrypt/cacerts", 1) != 0 ||
        setenv("CURL_CA_BUNDLE", "/apex/com.android.conscrypt/cacerts", 1) != 0 ||
        setenv("G_TLS_CA_PATH", "/apex/com.android.conscrypt/cacerts", 1) != 0 ||
        setenv("MATON_FLATPAK_CONFIG_DIR", static_config, 1) != 0) {
        perror("matonos-flatpak: setting runtime environment failed");
        return 127;
    }
    if(setenv("MATON_SESSION_PAD_NODES",pads,1))return 127;
    if(owned) {
        if(setenv("MATON_APP_DATA_DIR",app.data_dir,1))return 127;
        char owner[384],uid[32];
        snprintf(owner,sizeof(owner),"%u:%d:%d:%s",app.uid,app.pid,app.controllers,app.id);
        snprintf(uid,sizeof(uid),"%u",app.uid);
        if(setenv("MATON_APP_OWNER",owner,1)||setenv("MATON_APP_UID",uid,1)||setenv("FLATPAK",MATON_FLATPAK_BIN "/flatpak",1))return 127;
    }
    // clearenv() above dropped everything, so the shim path
    // and its socket are exported here, after the reset.
    if (setenv("FLATPAK_BWRAP", bwrap, 1) != 0 ||
        (x11_socket[0] != '\0' && setenv("MATON_X11_SOCKET", x11_socket, 1) != 0) ||
        (journal[0] != '\0' && setenv("MATON_JOURNAL_SOCKET", journal, 1) != 0)) {
        perror("matonos-flatpak: setting bwrap environment failed");
        return 127;
    }
    if (graphical && setenv("WAYLAND_DISPLAY", display_copy, 1) != 0) return 127;
    if (graphical) {
        if(setenv("MATON_FLATPAK_DNS",dns,1))return 127;
        if(!bus[0]) {
            fprintf(stderr,"matonos-flatpak: missing compositor session bus\n");return 127;
        }
        if(setenv("DBUS_SESSION_BUS_ADDRESS",bus,1))return 127;
    }
    if(session_directory>=0)close(session_directory);
    if(x11_directory>=0)close(x11_directory);
    if(owned) {
        if(fsetxattr(app.group,"user.app_id",app.id,strlen(app.id),0)) {
            if(errno!=EOPNOTSUPP && errno!=ENOTSUP){perror("cgroup app_id");return 127;}
            fprintf(stderr,"matonos-flatpak: cgroup user.app_id unsupported\n");
        }
        if(setenv("HOME",app.home,1)||
           setenv("XDG_RUNTIME_DIR",app.home,1)||setenv("FLATPAK_SYSTEM_CACHE_DIR",app.home,1)||
           setenv("FLATPAK_USER_DIR",app.data_dir,1))return 127;
        char data[160];snprintf(data,sizeof(data),"%s/.local/share",app.home);
        if(setenv("XDG_DATA_HOME",data,1))return 127;
        pid_t child=initial?fork():0;
        if(child<0)return 127;
        if(child==0) {
            if(app.lifeline>=0)close(app.lifeline);
            if(session_enter(&app)){perror("matonos-flatpak: enter app sandbox");_exit(127);}
            int home=open(app.home,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(home>=0) {
                char gtk[384];const char* versions[]={"gtk-3.0","gtk-4.0"};
                for(unsigned i=0;i<2;i++) {
                    snprintf(gtk,sizeof(gtk),".var/app/%s/config/%s",app.id,versions[i]);
                    set_gtk_settings_key(home,gtk);
                }
                close(home);
            }
            /* Hand the app-domain label (with the stub's MLS categories) to
             * the sandbox's static-NDK transition launcher. */
            if(setenv("MATON_APP_LABEL",app.app_label,1))_exit(127);
            unsetenv("MATON_APP_OWNER");unsetenv("MATON_APP_UID");
            execv(MATON_FLATPAK_BIN "/matonos-flatpak",argv);_exit(127);
        }
        int status=0,exited=0;
        for(;;) {
            if(!exited && waitpid(child,&status,WNOHANG)==child)exited=1;
            struct pollfd life={.fd=app.lifeline,.events=POLLIN};
            int rc=poll(&life,1,100);
            if(rc>0 && life.revents){session_kill(&app);break;}
            if(exited && !session_has_payload(&app))break;
        }
        int supervisor=0;
        if(sscanf(bus,"unix:path=" SESSION_BUS_DIRECTORY "/bus",&supervisor)==1 && supervisor>0)kill(supervisor,SIGTERM);
        if(!exited)while(waitpid(child,&status,0)<0 && errno==EINTR){}
        close(app.group);close(app.lifeline);
        return WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);
    }
    execv(MATON_FLATPAK_BIN "/matonos-flatpak", argv);
    perror("matonos-flatpak: exec failed");
    return 127;
}
#endif
