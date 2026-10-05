#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/*
 * MatonOS bwrap shim. flatpak-env-wrapper.c routes the Flatpak
 * CLI through $FLATPAK_BWRAP, and flatpak run execs bwrap with
 * an empty environment: flatpak_bwrap_envp_to_args() turns the
 * whole launcher environment into --setenv arguments and
 * flatpak_bwrap_bundle_args() packs them, NUL-separated, into
 * the --args FD (flatpak-run.c / flatpak-bwrap.c). The per-app
 * X11 relay socket therefore arrives via the inherited
 * MATON_X11_SOCKET (direct bwrap callers keep the environment)
 * or, for flatpak run, via the --setenv MATON_X11_SOCKET
 * triplet inside the bundled FD data. Bind that socket onto the
 * /tmp/.X11-unix tmpfs flatpak mounts and restore DISPLAY,
 * then hand the real bwrap the extended argv.
 */
#include "controller-access.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

/* r24: the Flatpak stack lives in the com.matonos.flatpak APEX. bwrap finds
 * its own libraries through the APEX linker namespace. */
#ifndef MATON_FLATPAK_BIN
#define MATON_FLATPAK_BIN "/apex/com.matonos.flatpak/bin"
#endif
#ifndef MATON_MACHINE_ID_PATH
#define MATON_MACHINE_ID_PATH "/data/matonos/linux/machine-id"
#endif
#ifndef MATON_UDEV_DB_PATH
#define MATON_UDEV_DB_PATH "/data/matonos/linux/udev"
#endif
#define X11_SOCKET_PATH "/tmp/.X11-unix/X0"
#define X11_SOCKET_ENV "MATON_X11_SOCKET"
#define X11_DISPLAY ":0"
#define JOURNAL_SOCKET_ENV "MATON_JOURNAL_SOCKET"
#define JOURNAL_SOCKET_PATH "/run/systemd/journal/socket"
/* SELinux exec chain: matonos_flatpak_run execs the separately labeled
 * matonos-bwrap shim (matonos_bwrap_exec), entering matonos_bwrap. The shim
 * then execs matonos-flatpak (matonos_flatpak_cli_exec) with argv[0]="bwrap",
 * returning to matonos_flatpak_run for bubblewrap's namespace setup. bwrap
 * runs matonos-app-exec as the sandbox command; it dyntransitions to the
 * verified app domain before exec'ing the payload. */
#define APP_EXEC_HOST MATON_FLATPAK_BIN "/matonos-app-exec"
#define APP_EXEC_SANDBOX "/run/matonos/matonos-app-exec"
#define APP_LABEL_ENV "MATON_APP_LABEL"

/* Values each bwrap option consumes, mirroring parse_args_recurse()
 * in bubblewrap's bubblewrap.c. Unknown dashed arguments count as
 * zero: bwrap rejects them before reaching the command anyway. */
static int option_values(const char* arg) {
    static const struct {
        const char* name;
        int values;
    } options[] = {
        /* bubblewrap 0.13.0: three values */
        {"--overlay",3},
        /* two values */
        {"--bind",2},{"--bind-try",2},{"--ro-bind",2},{"--ro-bind-try",2},
        {"--dev-bind",2},{"--dev-bind-try",2},{"--bind-fd",2},{"--ro-bind-fd",2},
        {"--file",2},{"--bind-data",2},{"--ro-bind-data",2},{"--symlink",2},
        {"--chmod",2},{"--setenv",2},
        /* one value */
        {"--args",1},{"--argv0",1},{"--chdir",1},{"--remount-ro",1},
        {"--overlay-src",1},{"--tmp-overlay",1},{"--ro-overlay",1},
        {"--proc",1},{"--exec-label",1},{"--file-label",1},{"--dev",1},
        {"--tmpfs",1},{"--mqueue",1},{"--dir",1},{"--lock-file",1},
        {"--sync-fd",1},{"--block-fd",1},{"--userns-block-fd",1},
        {"--info-fd",1},{"--json-status-fd",1},{"--seccomp",1},
        {"--add-seccomp-fd",1},{"--userns",1},{"--userns2",1},{"--pidns",1},
        {"--unsetenv",1},{"--uid",1},{"--gid",1},{"--hostname",1},
        {"--cap-add",1},{"--cap-drop",1},{"--perms",1},{"--size",1},
        /* every other option takes no values */
    };
    for(size_t i=0;i<sizeof(options)/sizeof(options[0]);i++)
        if(!strcmp(arg,options[i].name))return options[i].values;
    return 0;
}

/* Walk argv the way bwrap's parser does and report where option
 * parsing ends: the first non-option argument, or the argument
 * after an explicit "--", starts the command. *args_end is the
 * index just past the last "--args FD" pair, *dashdash the index
 * of the "--" itself; both stay -1 when absent. */
static int scan_argv(int argc, char** argv, int* args_end, int* dashdash) {
    int command=argc;
    *args_end=-1;
    *dashdash=-1;
    for(int i=1;i<argc;) {
        const char* arg=argv[i];
        if(!strcmp(arg,"--")) {
            *dashdash=i;
            command=i+1;
            break;
        }
        if(arg[0]!='-') {
            command=i;
            break;
        }
        if(!strcmp(arg,"--args") && i+1<argc)*args_end=i+2;
        i+=1+option_values(arg);
    }
    return command;
}

/* Start of the NUL-separated string following the one at p, or
 * NULL when p holds the last string in the buffer. */
static char* next_string(char* p, char* end) {
    char* nul=memchr(p,0,(size_t)(end-p));
    if(!nul || nul+1>=end)return NULL;
    return nul+1;
}

/* Recover the relay socket path from the bundled --args FD data:
 * flatpak converted MATON_X11_SOCKET into a --setenv VAR VALUE
 * triplet there. The FD is rewound to its original offset so the
 * real bwrap still reads the arguments from the start (bwrap's
 * load_file_data() reads from the current offset). */
static char* args_fd_x11_socket(int fd, int* x11_tmpfs, char** journal, int* app_sandbox) {
    char* data=NULL;
    size_t capacity=0, length=0;
    char *p,*end,*socket_path=NULL;
    off_t origin=lseek(fd,0,SEEK_CUR);
    if(origin==(off_t)-1)return NULL;
    for(;;) {
        ssize_t got;
        if(length==capacity) {
            size_t grown=capacity ? capacity*2 : 4096;
            char* bigger=realloc(data,grown);
            if(!bigger)break;
            data=bigger;
            capacity=grown;
        }
        do {
            got=read(fd,data+length,capacity-length);
        } while(got<0 && errno==EINTR);
        if(got<=0)break;
        length+=(size_t)got;
    }
    if(!data || !length || data[length-1]!=0) {
        (void)lseek(fd,origin,SEEK_SET);
        free(data);
        return NULL;
    }
    end=data+length;
    p=data;
    while(p && p<end) {
        char* var;
        char* value;
        if(p[0]!='-' || !strcmp(p,"--"))break;
        int values=option_values(p);
        if(!strcmp(p,"--setenv")) {
            var=next_string(p,end);
            value=var ? next_string(var,end) : NULL;
            if(var && !strcmp(var,X11_SOCKET_ENV) && value && *value)
                socket_path=value;
            if(var && !strcmp(var,JOURNAL_SOCKET_ENV) && value && *value)
                *journal=value;
            /* flatpak exports FLATPAK_ID only into the app sandbox. */
            if(var && !strcmp(var,"FLATPAK_ID"))*app_sandbox=1;
        } else if(!strcmp(p,"--tmpfs")) {
            value=next_string(p,end);
            if(value && !strcmp(value,"/tmp/.X11-unix"))*x11_tmpfs=1;
        }
        /* Values and command arguments must not masquerade as options. */
        for(int i=0;i<=values && p;i++)p=next_string(p,end);
    }
    if(lseek(fd,origin,SEEK_SET)==(off_t)-1)socket_path=NULL;
    return socket_path;
}

/* Look up a single --setenv value in the bundled --args FD data. The FD is
 * rewound so the real bwrap still reads it from the start (see above). */
static char* args_fd_lookup(int fd, const char* wanted) {
    char* data=NULL;
    size_t capacity=0, length=0;
    off_t origin=lseek(fd,0,SEEK_CUR);
    if(origin==(off_t)-1)return NULL;
    for(;;) {
        ssize_t got;
        if(length==capacity) {
            size_t grown=capacity ? capacity*2 : 4096;
            char* bigger=realloc(data,grown);
            if(!bigger)break;
            data=bigger;capacity=grown;
        }
        do { got=read(fd,data+length,capacity-length); } while(got<0 && errno==EINTR);
        if(got<=0)break;
        length+=(size_t)got;
    }
    char* found=NULL;
    if(data && length && data[length-1]==0) {
        char* end=data+length;
        char* p=data;
        while(p && p<end) {
            if(p[0]!='-' || !strcmp(p,"--"))break;
            int values=option_values(p);
            if(!strcmp(p,"--setenv")) {
                char* var=next_string(p,end);
                char* value=var ? next_string(var,end) : NULL;
                if(var && value && !strcmp(var,wanted) && *value) { free(found); found=strdup(value); }
            }
            for(int i=0;i<=values && p;i++)p=next_string(p,end);
        }
    }
    (void)lseek(fd,origin,SEEK_SET);
    free(data);
    return found;
}

/* Prepend one argument at index at. */
static char** prepend_argument(char** argv, int count, int at, const char* value) {
    char** out=calloc((size_t)count+2,sizeof(char*));
    if(!out)return NULL;
    for(int i=0;i<at;i++)out[i]=argv[i];
    out[at]=(char*)value;
    for(int i=at;i<count;i++)out[i+1]=argv[i];
    out[count+1]=NULL;
    return out;
}

/* Build the argv passed to the real bwrap: the extra arguments go at
 * index at, everything else keeps its position. */
static char** insert_args(int argc, char** argv, int at, char** extra, int count) {    char** extended=calloc((size_t)argc+(size_t)count+1,sizeof(char*));
    int i;
    int n=0;
    if(!extended)return NULL;
    for(i=0;i<at;i++)extended[n++]=argv[i];
    for(i=0;i<count;i++)extended[n++]=extra[i];
    for(i=at;i<argc;i++)extended[n++]=argv[i];
    return extended;
}
static int is_socket(const char* path) {
    struct stat info;
    return path && *path && lstat(path,&info)==0 && S_ISSOCK(info.st_mode);
}

/* The shim and multicall executable are staged beside each other in the
 * APEX. Resolve the multicall path from this ELF's actual location so host
 * launch probes can exercise the staged musl shim from a temporary directory
 * without changing the production executable path or trusting an env var. */
static int multicall_path(char* path, size_t size) {
    ssize_t length = readlink("/proc/self/exe", path, size - 1);
    if (length <= 0 || (size_t)length >= size - 1) return -1;
    path[length] = '\0';
    char* slash = strrchr(path, '/');
    if (!slash) return -1;
    static const char name[] = "/matonos-flatpak";
    size_t directory = (size_t)(slash - path);
    if (directory + sizeof(name) > size) return -1;
    memcpy(path + directory, name, sizeof(name));
    return 0;
}

int main(int argc, char** argv) {
    const char* socket_path;
    const char* journal_path;
    int args_end;
    int dashdash;
    int command;
    int at;
    char** extended;
    char multicall[PATH_MAX];
    const char* machine_id_path = MATON_MACHINE_ID_PATH;
    const char* udev_db_path = MATON_UDEV_DB_PATH;
#ifdef MATONOS_HOST_LAUNCH_PROBE
    char probe_machine_id[PATH_MAX];
    char probe_udev[PATH_MAX];
    char* bundled_probe_root = NULL;
#endif
    char* pad_nodes=NULL;
    char** extra;
    int count=0;

    int x11_tmpfs=0, app_sandbox=0;
    char* bundled=NULL;
    char* bundled_journal=NULL;
    char* app_label=NULL;
    command=scan_argv(argc,argv,&args_end,&dashdash);
    for(int i=1;i<command && strcmp(argv[i],"--");) {
        if(!strcmp(argv[i],"--setenv") && i+2<command && !strcmp(argv[i+1],"FLATPAK_ID"))app_sandbox=1;
        i+=1+option_values(argv[i]);
    }
    /* Only the app sandbox gets the binds: flatpak's own --tmpfs
     * /tmp/.X11-unix (from --socket=x11) marks X11 access, FLATPAK_ID
     * the app itself. Helper sandboxes such as xdg-dbus-proxy inherit
     * the variables but have neither and must stay untouched. */
    if(args_end>=0)bundled=args_fd_x11_socket(atoi(argv[args_end-1]),&x11_tmpfs,&bundled_journal,&app_sandbox);
    if(args_end>=0)app_label=args_fd_lookup(atoi(argv[args_end-1]),"MATON_APP_LABEL");
#ifdef MATONOS_HOST_LAUNCH_PROBE
    if(args_end>=0)bundled_probe_root=args_fd_lookup(atoi(argv[args_end-1]),"MATONOS_BWRAP_PROBE_ROOT");
    const char* probe_root=getenv("MATONOS_BWRAP_PROBE_ROOT");
    if((!probe_root || !*probe_root) && bundled_probe_root)probe_root=bundled_probe_root;
    if(probe_root && *probe_root) {
        int machine_len=snprintf(probe_machine_id,sizeof(probe_machine_id),"%s/machine-id",probe_root);
        int udev_len=snprintf(probe_udev,sizeof(probe_udev),"%s/udev",probe_root);
        if(machine_len>0 && (size_t)machine_len<sizeof(probe_machine_id) &&
           udev_len>0 && (size_t)udev_len<sizeof(probe_udev)) {
            machine_id_path=probe_machine_id;
            udev_db_path=probe_udev;
        }
    }
    free(bundled_probe_root);
#endif
    if(!app_label && getenv("MATON_APP_LABEL"))app_label=strdup(getenv("MATON_APP_LABEL"));
    /* Only enter the app domain for the real app sandbox; helpers keep their
     * own flow. The label must be a matonos app context or it is ignored. */
    if(!app_sandbox || !app_label || strncmp(app_label,"u:r:matonos_linux_app:",sizeof("u:r:matonos_linux_app:")-1)) {
        free(app_label);app_label=NULL;
    }
    socket_path=getenv(X11_SOCKET_ENV);
    if(!socket_path || !*socket_path)socket_path=bundled;
    journal_path=getenv(JOURNAL_SOCKET_ENV);
    if(!journal_path || !*journal_path)journal_path=bundled_journal;
    if(args_end>=0)pad_nodes=args_fd_lookup(atoi(argv[args_end-1]),"MATON_SESSION_PAD_NODES");
    if(!pad_nodes && getenv("MATON_SESSION_PAD_NODES"))pad_nodes=strdup(getenv("MATON_SESSION_PAD_NODES"));
    extra = calloc(96 + 3 * 4, sizeof(char*));
    if (!extra) return 127;
    if(app_sandbox && pad_nodes && *pad_nodes) {
        char* state;unsigned pads=0;
        for(char* node=strtok_r(pad_nodes,",",&state);node;node=strtok_r(NULL,",",&state)) {
            struct stat st;
            if(++pads>4 || strncmp(node,"/dev/input/event",16) || !node[16] ||
                    strspn(node+16,"0123456789")!=strlen(node+16) ||
                    lstat(node,&st) || !S_ISCHR(st.st_mode)) {
                fprintf(stderr,"matonos-bwrap: invalid session pad node\n");return 127;
            }
            extra[count++]="--dev-bind";extra[count++]=node;extra[count++]=node;
        }
    }
    if(x11_tmpfs && is_socket(socket_path)) {
        extra[count++]="--bind";extra[count++]=(char*)socket_path;extra[count++]=X11_SOCKET_PATH;
        extra[count++]="--setenv";extra[count++]="DISPLAY";extra[count++]=X11_DISPLAY;
    }
    /* Programs logging straight to journald (tracing-journald, sd_journal)
     * fail to start without its socket; the launcher drains this one into
     * the app's launch log. */
    if(app_sandbox && is_socket(journal_path)) {
        extra[count++]="--bind";extra[count++]=(char*)journal_path;extra[count++]=JOURNAL_SOCKET_PATH;
    }
    /* The launcher generated these four host-config files for this process.
     * The app root sees only this minimal identity/network configuration and
     * the Conscrypt trust store, never Android's general /etc tree. */
    const char* config=getenv("MATON_FLATPAK_CONFIG_DIR");
    char* config_end=NULL;
    long config_pid=config && !strncmp(config,"/data/matonos/linux/runtime/flatpak-config-",43) ?
        strtol(config+43,&config_end,10) : 0;
    if(app_sandbox && config_pid>0 && config_end && !strcmp(config_end,"-monitor")) {
        static char passwd[256],group[256],resolv[256];
        snprintf(passwd,sizeof(passwd),"%s/passwd",config);
        snprintf(group,sizeof(group),"%s/group",config);
        snprintf(resolv,sizeof(resolv),"%s/resolv.conf",config);
        extra[count++]="--ro-bind";extra[count++]=passwd;extra[count++]="/etc/passwd";
        extra[count++]="--ro-bind";extra[count++]=group;extra[count++]="/etc/group";
        extra[count++]="--ro-bind";extra[count++]=resolv;extra[count++]="/etc/resolv.conf";
        extra[count++]="--symlink";extra[count++]="/apex/com.android.conscrypt/cacerts";extra[count++]="/etc/ssl/certs";
        extra[count++]="--symlink";extra[count++]="/tmp";extra[count++]="/var/tmp";
    }
    /* flatpak-run.c only uses the host ID if /etc or /var has one.
     * Android has neither. Override both paths after Flatpak mounts /var. */
    if(app_sandbox) {
        extra[count++]="--ro-bind";extra[count++]=(char*)machine_id_path;extra[count++]="/etc/machine-id";
        extra[count++]="--ro-bind";extra[count++]=(char*)machine_id_path;extra[count++]="/var/lib/dbus/machine-id";
        /* Flatpak already exposes /sys/class, /sys/dev and /sys/devices.
         * Bind the directory itself, so atomic database replacements and
         * future devices are visible in existing sandboxes. Metadata does
         * not grant access to any device node. */
        extra[count++]="--ro-bind";extra[count++]=(char*)udev_db_path;extra[count++]="/run/udev";
        /* Host udev multicast cannot reliably cross Flatpak's net namespace,
         * and linuxd's system UID is not a trusted root udev sender. SDL2/3
         * support this generic hint and watch /dev/input with inotify (or
         * poll when inotify is unavailable), including permission changes. */
        extra[count++]="--setenv";extra[count++]="SDL_JOYSTICK_DISABLE_UDEV";extra[count++]="1";
    }
    /* r24: expose the launcher and its label inside the app sandbox. bwrap
     * calls it as the command; it setcon()s to the app domain and execs the
     * verified payload. */
    if(app_sandbox && app_label) {
        const char* data=getenv("MATON_APP_DATA_DIR");
        unsigned uid=0;char trailing;
        if(!data || sscanf(data,"/data/matonos/linux/apps/%u%c",&uid,&trailing)!=1 ||
           uid%100000<10000 || uid%100000>19999) return 125;
        static char home[192];
        snprintf(home,sizeof(home),"%s/home",data);
        extra[count++]="--bind";extra[count++]=home;extra[count++]=home;
        extra[count++]="--setenv";extra[count++]="HOME";extra[count++]=home;
        extra[count++]="--ro-bind";extra[count++]=APP_EXEC_HOST;extra[count++]=APP_EXEC_SANDBOX;
        extra[count++]="--setenv";extra[count++]=APP_LABEL_ENV;extra[count++]=app_label;
    }
    if(count) {
        /* bwrap applies the bundled arguments at the --args pair,
         * so inserting right after it puts the bind on top of
         * flatpak's --tmpfs /tmp/.X11-unix and after its sorted
         * --setenv/--unsetenv environment arguments (our DISPLAY
         * therefore wins), while staying ahead of "--" and the
         * command. Without a bundled pair, insert where option
         * parsing ends: before an explicit "--", else directly
         * before the command. */
        at=args_end>=0 ? args_end : (dashdash>=0 ? dashdash : command);
        extended=insert_args(argc,argv,at,extra,count);
        if(extended) {
            unsetenv(X11_SOCKET_ENV);
            unsetenv(JOURNAL_SOCKET_ENV);
            char** final_argv=extended;
            if(app_label && command<argc) {
                /* The command was shifted right by the inserted arguments;
                 * put the launcher immediately in front of it. */
                int shifted=command + (at<=command ? count : 0);
                char** with_launcher=prepend_argument(extended,argc+count,shifted,APP_EXEC_SANDBOX);
                if(with_launcher) final_argv=with_launcher;
            }
            /* The APEX multicall ELF selects the bwrap applet from argv[0].
             * The shim itself was launched as matonos-bwrap, so preserve the
             * target applet name explicitly across this exec. */
            final_argv[0]="bwrap";
            if (multicall_path(multicall, sizeof(multicall)) != 0) {
                fprintf(stderr,"matonos-bwrap: cannot resolve adjacent multicall binary\n");
                return 127;
            }
            execv(multicall,final_argv);
            perror("matonos-bwrap: exec failed");
            return 127;
        }
    }
    /* Helper invocations (for example Flatpak's ldconfig setup) have no
     * injected arguments, but still need multicall applet dispatch. */
    argv[0]="bwrap";
    if (multicall_path(multicall, sizeof(multicall)) != 0) {
        fprintf(stderr,"matonos-bwrap: cannot resolve adjacent multicall binary\n");
        return 127;
    }
    execv(multicall,argv);
    perror("matonos-bwrap: exec failed");
    return 127;
}
