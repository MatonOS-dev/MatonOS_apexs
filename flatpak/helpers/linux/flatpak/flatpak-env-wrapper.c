#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/*
 * Static musl host launcher for the static Flatpak CLI. linuxd execs
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
#include "../dbus-broker/session-control.h"
#include <stddef.h>

/* r24: the whole Flatpak stack ships in the com.matonos.flatpak APEX, mounted
 * at /apex/com.matonos.flatpak, instead of /system_ext. The binaries find
 * their own libraries through the APEX linker namespace, so no
 * LD_LIBRARY_PATH is needed. */
#define MATON_FLATPAK_APEX "/apex/com.matonos.flatpak"
#define MATON_FLATPAK_BIN MATON_FLATPAK_APEX "/bin"
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
    if (!path) return 0;
    if (sscanf(path, "/data/matonos/linux/install/%u%c", &uid, &trailing) == 1)
        snprintf(expected, sizeof(expected), "/data/matonos/linux/install/%u", uid);
    else if (sscanf(path, "/data/matonos/linux/runtime/%u%c", &uid, &trailing) == 1)
        snprintf(expected, sizeof(expected), "/data/matonos/linux/runtime/%u", uid);
    else
        return 0;
    if (uid % 100000 < 10000 || uid % 100000 > 19999) return 0;
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
static int grant_monitor_read(const char* path, unsigned uid) {
    int directory=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(directory<0)return -1;
    static const char* files[]={"resolv.conf","hosts","host.conf","gai.conf","passwd","group"};
    int rc=0;
    for(unsigned i=0;i<sizeof(files)/sizeof(files[0]);i++) {
        int fd=openat(directory,files[i],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
        if(fd<0){rc=-1;break;}
        if(fchown(fd,1000,uid)||fchmod(fd,0640))rc=-1;
        close(fd);if(rc)break;
    }
    /* Keep system ownership and grant only the verified app read access. */
    if(!rc && (fchown(directory,1000,uid)||fchmod(directory,0750)))rc=-1;
    close(directory);return rc;
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
    if(/* UID changes make /proc ownership root until dumpability is reset.
                 * The private broker must inspect this same-UID child before exec. */
                prctl(PR_SET_DUMPABLE,1)||prctl(PR_SET_PDEATHSIG,SIGTERM)||getppid()!=parent)_exit(0);
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
static int system_flatpak_version(char* version,size_t size) {
    int output[2];
    if(pipe2(output,O_CLOEXEC))return -1;
    pid_t child=fork();
    if(child<0){close(output[0]);close(output[1]);return -1;}
    if(!child) {
        if(dup2(output[1],STDOUT_FILENO)<0)_exit(127);
        close(output[0]);close(output[1]);
        char* args[]={"flatpak","--version",NULL};
        execv(MATON_FLATPAK_BIN "/matonos-flatpak",args);_exit(127);
    }
    close(output[1]);char text[128]={0};size_t used=0;
    while(used<sizeof(text)-1) {
        ssize_t got=read(output[0],text+used,sizeof(text)-1-used);
        if(got<0&&errno==EINTR)continue;
        if(got<=0)break;used+=(size_t)got;
    }
    close(output[0]);int status=0;
    while(waitpid(child,&status,0)<0&&errno==EINTR){}
    char value[64]={0};
    if(!WIFEXITED(status)||WEXITSTATUS(status)||sscanf(text,"Flatpak %63[0-9.]",value)!=1||strlen(value)>=size)return -1;
    strcpy(version,value);return 0;
}
/* The portal is an ordinary Android ELF at this app's UID and MLS level.
 * The system supervisor pins its identity on the app's private bus before
 * releasing exec, so only this child can claim the Flatpak portal name. */
static int start_app_portal(AppSession* app,const char* monitor,int* control) {
    int ready[2]={-1,-1},gate[2]={-1,-1},socket_fd=-1;
    pid_t portal=-1;
    struct MatonSessionRegistration registration={0};
    if(system_flatpak_version(registration.flatpak_version,sizeof(registration.flatpak_version))) {
        fprintf(stderr,"matonos-flatpak: cannot query system Flatpak version\n");return -1;
    }
    const char* stage="create startup pipes";
    if(pipe2(ready,O_CLOEXEC)||pipe2(gate,O_CLOEXEC))goto fail;
    pid_t parent=getpid();portal=fork();
    if(portal<0)goto fail;
    if(!portal) {
        close(ready[0]);close(gate[1]);close(app->lifeline);
        snprintf(app->label,sizeof(app->label),"%s",app->app_label);
        if(session_enter(app)||session_home(app)||
                /* UID changes make /proc ownership root until dumpability is reset.
                 * The private broker must inspect this same-UID child before exec. */
                prctl(PR_SET_DUMPABLE,1)||prctl(PR_SET_PDEATHSIG,SIGTERM)||getppid()!=parent) {perror("matonos-flatpak: portal identity setup");_exit(127);}
        if(setenv("FLATPAK",MATON_FLATPAK_BIN "/matonos-app-flatpak",1)||
                setenv("FLATPAK_BWRAP",MATON_FLATPAK_BIN "/matonos-app-bwrap",1)||
                setenv("MATON_APP_LABEL",app->app_label,1))_exit(127);
        unsetenv("MATON_APP_OWNER");unsetenv("MATON_APP_UID");
        char byte=1;
        if(write(ready[1],&byte,1)!=1)_exit(127);
        close(ready[1]);
        if(read(gate[0],&byte,1)!=1||byte!=1)_exit(127);
        close(gate[0]);
        char* args[]={"flatpak-portal","--no-idle-exit",NULL};
        execv(MATON_FLATPAK_BIN "/matonos-flatpak-portal",args);
        perror("matonos-flatpak: stock portal exec");_exit(127);
    }
    close(ready[1]);ready[1]=-1;close(gate[0]);gate[0]=-1;
    stage="wait for child identity";
    struct pollfd waiting={.fd=ready[0],.events=POLLIN};char byte=0;
    if(poll(&waiting,1,5000)<=0||read(ready[0],&byte,1)!=1||byte!=1)goto fail;
    close(ready[0]);ready[0]=-1;
    struct sockaddr_un address={.sun_family=AF_UNIX};
    snprintf(address.sun_path,sizeof(address.sun_path),"/data/matonos/linux/tmp/%u/bus-control",app->uid);
    stage="connect private bus control";
    socket_fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);
    if(socket_fd<0||connect(socket_fd,(struct sockaddr*)&address,sizeof(address)))goto fail;
    stage="register portal PID";
    registration.pid=portal;snprintf(registration.monitor,sizeof(registration.monitor),"%s",monitor);
    if(send(socket_fd,&registration,sizeof(registration),MSG_NOSIGNAL)!=sizeof(registration))goto fail;
    struct MatonSessionReply response={0};waiting.fd=socket_fd;
    if(poll(&waiting,1,5000)<=0||recv(socket_fd,&response,sizeof(response),MSG_TRUNC)!=sizeof(response)||response.status)goto fail;
    stage="release portal exec";
    byte=1;if(write(gate[1],&byte,1)!=1)goto fail;
    close(gate[1]);gate[1]=-1;
    stage="wait for portal bus ownership";
    if(poll(&waiting,1,10000)<=0||recv(socket_fd,&byte,1,0)!=1||byte!=1)goto fail;
    app->portal=portal;*control=socket_fd;return 0;
fail:
    fprintf(stderr,"matonos-flatpak: portal startup failed at %s: %s\n",stage,strerror(errno));
    if(portal>0){kill(portal,SIGTERM);while(waitpid(portal,NULL,0)<0&&errno==EINTR){}}
    for(int i=0;i<2;i++){if(ready[i]>=0)close(ready[i]);if(gate[i]>=0)close(gate[i]);}
    if(socket_fd>=0)close(socket_fd);
    return -1;
}
/* Staging runs as the installing app's UID in its own operation directory:
 * /data/matonos/linux/install/<euid>/staging/<operation>, owned by that UID. */
static int installer_staging_valid(const char* path) {
    char prefix[96];
    uid_t uid=geteuid();
    if(uid<10000 || uid%100000<10000 || uid%100000>19999)return 0;
    int n=snprintf(prefix,sizeof(prefix),"/data/matonos/linux/install/%u/staging/",(unsigned)uid);
    if(n<0 || n>=(int)sizeof(prefix) || strncmp(path,prefix,(size_t)n))return 0;
    const char* op=path+n;
    size_t len=strlen(op);
    if(!len || len>64)return 0;
    for(size_t i=0;i<len;i++){char c=op[i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))return 0;}
    struct stat st;
    return !lstat(path,&st) && S_ISDIR(st.st_mode) && st.st_uid==uid;
}

int main(int argc, char** argv) {
    (void)argc;
    /* linuxd supplies the multicall applet name in argv[0]. */
    if(strcmp(argv[0],"flatpak") && strcmp(argv[0],"ostree")) {
        fprintf(stderr,"matonos-flatpak: unsupported applet name: %s\n",argv[0]);
        return 127;
    }
    const char* staging_request=getenv("MATON_FLATPAK_STAGING_DIR");
    int installer_mode=staging_request && installer_staging_valid(staging_request);
    if(staging_request && !installer_mode) {
        fprintf(stderr,"matonos-flatpak: invalid staging installer context\n");return 127;
    }
    int is_run=0;
    for(int i=1;i<argc;i++)if(!strcmp(argv[i],"run")){is_run=1;break;}
    AppSession app={.lifeline=-1,.group=-1};
    int owned=is_run;
    int initial=owned;
    if(owned && session_owner(&app,initial)) {
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
    const char* bwrap = MATON_FLATPAK_BIN "/matonos-bwrap";
    if(!installer_mode) {
        char machine_reason[256];
        if(prepare_machine_id_reason("/data/matonos/linux",machine_reason,sizeof(machine_reason))) {
            fprintf(stderr,"matonos-flatpak: machine-id: %s\n",
                    machine_reason[0] ? machine_reason : "unknown failure");
            return 127;
        }
    }
    char display_copy[128] = {0};
    char static_base[192],static_config[192]={0};
    snprintf(static_base,sizeof(static_base),"/data/matonos/linux/run/flatpak-config-%d",getpid());
    const char* supplied_dns=getenv("MATON_FLATPAK_DNS");
    if(!installer_mode && prepare_monitor(static_base,supplied_dns,static_config,sizeof(static_config))) {
        fprintf(stderr,"matonos-flatpak: cannot prepare static namespace configuration\n");return 127;
    }
    if(owned && grant_monitor_read(static_config,app.uid)) {
        perror("matonos-flatpak: grant app configuration access");return 127;
    }
    char x11_socket[160] = {0};
    /* The app session is fixed: /data/matonos/linux/tmp/<uid>, Wayland socket
     * "wayland-0" and the session bus at "bus". Nothing is handed in. */
    int graphical = owned;
    if (graphical) snprintf(display_copy, sizeof(display_copy), "%s", "wayland-0");
    if (graphical) {
        struct stat socket_info;
        char alias[160], target[64]={0}, canonical[64];
        snprintf(alias,sizeof(alias),"/data/matonos/linux/tmp/%u/wayland-0-x11",app.uid);
        ssize_t length=readlink(alias,target,sizeof(target)-1);
        unsigned number=0;char trailing;
        if(length>0) {
            target[length]=0;
            if(sscanf(target,"x11/X%u%c",&number,&trailing)==1) {
                snprintf(canonical,sizeof(canonical),"x11/X%u",number);
                if(!strcmp(target,canonical))
                    snprintf(x11_socket,sizeof(x11_socket),"/data/matonos/linux/tmp/%u/%s",app.uid,target);
            }
        }
        // Accept only the socket owned by this verified app, never a host path.
        if(!x11_socket[0] || lstat(x11_socket,&socket_info) ||
                !S_ISSOCK(socket_info.st_mode) || socket_info.st_uid!=app.uid)
            x11_socket[0]=0;
    }

    char journal[160] = {0};
    if (graphical) {
        char journal_display[128];
        snprintf(journal_display,sizeof(journal_display),"/data/matonos/linux/tmp/%u/wayland-journal",app.uid);
        if (start_journal_sink(journal_display, journal, sizeof(journal)) == 0)
            bwrap = MATON_FLATPAK_BIN "/matonos-bwrap";
        else journal[0] = '\0';
    }
    char pads[320];
    const char *pad_list=getenv("MATON_SESSION_PAD_NODES");
    if(pad_list && strlen(pad_list)>=sizeof(pads))return 127;
    snprintf(pads,sizeof(pads),"%s",owned && pad_list ? pad_list : "");
    char dns[2048], bus[192];
    snprintf(dns,sizeof(dns),"%s",getenv("MATON_FLATPAK_DNS") ? getenv("MATON_FLATPAK_DNS") : "");
    if(owned) snprintf(bus,sizeof(bus),"unix:path=/data/matonos/linux/tmp/%u/bus",app.uid);
    else snprintf(bus,sizeof(bus),"%s",getenv("DBUS_SESSION_BUS_ADDRESS") ? getenv("DBUS_SESSION_BUS_ADDRESS") : "");
    /* The installer UID cannot use linuxd's shared home/cache/runtime dirs;
     * linuxd creates installer-owned ones inside the operation directory. */
    char staging[160], home[192], data_home[224], config_home[224], cache[192], tmp[192], runtime[192];
    if(installer_mode) {
        snprintf(staging,sizeof(staging),"%s",staging_request);
        snprintf(home,sizeof(home),"%s/home",staging);
        snprintf(data_home,sizeof(data_home),"%s/home/.local/share",staging);
        snprintf(cache,sizeof(cache),"%s/cache",staging);
        snprintf(tmp,sizeof(tmp),"%s/tmp",staging);
        snprintf(runtime,sizeof(runtime),"%s/runtime",staging);
    } else {
        snprintf(home,sizeof(home),"/data/matonos/linux/flatpak-data");
        snprintf(data_home,sizeof(data_home),"/data/matonos/linux/flatpak-data/.local/share");
        snprintf(cache,sizeof(cache),"/data/matonos/linux/cache");
        snprintf(tmp,sizeof(tmp),"/tmp");
        snprintf(runtime,sizeof(runtime),"/data/matonos/linux/run");
    }
    snprintf(config_home,sizeof(config_home),"%s/.config",home);
    if (clearenv() != 0 ||
        setenv("PATH", MATON_FLATPAK_BIN ":/system/bin:/system/xbin", 1) != 0 ||
        setenv("XDG_RUNTIME_DIR", runtime, 1) != 0 ||
        setenv("HOME", home, 1) != 0 ||
        setenv("TMPDIR", tmp, 1) != 0 ||
        setenv("XDG_DATA_HOME", data_home, 1) != 0 ||
        setenv("XDG_CONFIG_HOME", config_home, 1) != 0 ||
        setenv("XDG_CACHE_HOME", cache, 1) != 0 ||
        setenv("FLATPAK_SYSTEM_DIR", installer_mode ? staging : flatpak_system_dir, 1) != 0 ||
        setenv("FLATPAK_SYSTEM_CACHE_DIR", cache, 1) != 0 ||
        /* Flatpak checks this directory for extra-data even on --system
         * installs. Keep that lookup inside the verified operation tree:
         * the downloading app UID cannot traverse the legacy shared root. */
        setenv("FLATPAK_USER_DIR", installer_mode ? staging : flatpak_user_dir, 1) != 0 ||
        setenv("FLATPAK_DOWNLOAD_TMPDIR", tmp, 1) != 0 ||
        setenv("SSL_CERT_DIR", "/apex/com.android.conscrypt/cacerts", 1) != 0 ||
        setenv("CURL_CA_BUNDLE", "/apex/com.android.conscrypt/cacerts", 1) != 0 ||
        setenv("G_TLS_CA_PATH", "/apex/com.android.conscrypt/cacerts", 1) != 0 ||
        setenv("MATON_FLATPAK_CONFIG_DIR", static_config, 1) != 0) {
        perror("matonos-flatpak: setting runtime environment failed");
        return 127;
    }
    if(setenv("MATON_SESSION_PAD_NODES",pads,1))return 127;
    /* Host-side CLI work (remote-add's temporary GnuPG home, downloads) uses
     * the temp dir of the app whose installation it operates on:
     * /data/matonos/linux/install/<uid>/tmp, preferring the stub's --user
     * installation over the runtime app's --system one. Android's /tmp is
     * shell-owned and not writable for us. Staging keeps its own tmp. */
    if(!installer_mode && !owned) {
        unsigned tmp_uid=0;
        const char* root=install_directory_uid(flatpak_user_dir,&tmp_uid) ? flatpak_user_dir :
                install_directory_uid(flatpak_system_dir,&tmp_uid) ? flatpak_system_dir : NULL;
        struct stat root_info;
        /* linuxd creates (and labels) installation roots. Before the first
         * operation on one, use linuxd's own temp dir instead. */
        if(root && stat(root,&root_info)==0 && S_ISDIR(root_info.st_mode)) {
            char app_tmp[160];
            snprintf(app_tmp,sizeof(app_tmp),"%s/tmp",root);
            if(mkdir(app_tmp,0700) && errno!=EEXIST) {
                perror("matonos-flatpak: app temp dir");return 127;
            }
            if(setenv("TMPDIR",app_tmp,1)||setenv("FLATPAK_DOWNLOAD_TMPDIR",app_tmp,1))return 127;
        } else if(setenv("TMPDIR","/data/matonos/linux/cache",1)||
                setenv("FLATPAK_DOWNLOAD_TMPDIR","/data/matonos/linux/cache",1)) return 127;
    }
    if(owned) {
        if(setenv("MATON_APP_DATA_DIR",app.data_dir,1)||setenv("MATON_APP_HOME_DIR",app.home,1))return 127;
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
    if(owned) {
        if(fsetxattr(app.group,"user.app_id",app.id,strlen(app.id),0)) {
            if(errno!=EOPNOTSUPP && errno!=ENOTSUP){perror("cgroup app_id");return 127;}
            fprintf(stderr,"matonos-flatpak: cgroup user.app_id unsupported\n");
        }
        char app_runtime[64];snprintf(app_runtime,sizeof(app_runtime),"/data/matonos/linux/tmp/%u",app.uid);
        if(setenv("HOME",app.home,1)||
           setenv("XDG_RUNTIME_DIR",app_runtime,1)||setenv("FLATPAK_SYSTEM_CACHE_DIR",app.home,1)||
           setenv("TMPDIR",app_runtime,1)||setenv("FLATPAK_DOWNLOAD_TMPDIR",app_runtime,1)||
           setenv("FLATPAK_USER_DIR",app.data_dir,1))return 127;
        // GLib's Android defaults include /data/cache, outside this app's
        // home. Explicit XDG roots keep filesystem=host from binding that
        // protected Android directory while building the sandbox (Kate/VLC).
        char data[160], config[160], app_cache[160];
        snprintf(data,sizeof(data),"%s/.local/share",app.home);
        snprintf(config,sizeof(config),"%s/.config",app.home);
        snprintf(app_cache,sizeof(app_cache),"%s/.cache",app.home);
        if(setenv("XDG_DATA_HOME",data,1)||setenv("XDG_CONFIG_HOME",config,1)||
                setenv("XDG_CACHE_HOME",app_cache,1))return 127;
        int portal_control=-1;
        if(start_app_portal(&app,static_config,&portal_control)) {
            fprintf(stderr,"matonos-flatpak: stock portal failed to become ready\n");return 127;
        }
        pid_t child=initial?fork():0;
        if(child<0)return 127;
        if(child==0) {
            if(app.lifeline>=0)close(app.lifeline);
            if(session_enter(&app)){perror("matonos-flatpak: enter app sandbox");_exit(127);}
            /* The home is mode 0700 and owned by the app UID. Validate it
             * after switching to that identity instead of requiring linuxd's
             * system UID to bypass the directory's DAC permissions. */
            if(session_home(&app)){perror("matonos-flatpak: unverified app home");_exit(127);}
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
            // Android's host root includes protected /data directories and
            // is not a Linux desktop filesystem. Keep the app's own home
            // (bound by our bwrap shim) and its explicit grants; suppress
            // only the blanket host export that makes Kate/VLC fail setup.
            // Flatpak clears bwrap's environment, so carry the verified X11
            // socket through its bundled --setenv arguments as before.
            char option[224];
            snprintf(option,sizeof(option),"--env=MATON_X11_SOCKET=%s",x11_socket);
            char** args=calloc((size_t)argc+3,sizeof(char*));
            if(!args)_exit(127);
            int count=0;
            for(int i=0;i<argc;i++) {
                args[count++]=argv[i];
                if(!strcmp(argv[i],"run")) {
                    args[count++]="--nofilesystem=host";
                    if(x11_socket[0])args[count++]=option;
                }
            }
            execv(MATON_FLATPAK_BIN "/matonos-flatpak",args);
            perror("matonos-flatpak: exec app CLI");_exit(127);
        }
        int status=0,exited=0;
        for(;;) {
            if(!exited && waitpid(child,&status,WNOHANG)==child)exited=1;
            struct pollfd life={.fd=app.lifeline,.events=POLLIN};
            int rc=poll(&life,1,100);
            if(rc>0 && life.revents){session_kill(&app);break;}
            if(exited && !session_has_payload(&app))break;
        }
        if(!exited)while(waitpid(child,&status,0)<0 && errno==EINTR){}
        kill(app.portal,SIGTERM);
        while(waitpid(app.portal,NULL,0)<0&&errno==EINTR){}
        close(portal_control);
        close(app.group);close(app.lifeline);
        return WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);
    }
    execv(MATON_FLATPAK_BIN "/matonos-flatpak", argv);
    perror("matonos-flatpak: exec failed");
    return 127;
}
#endif
