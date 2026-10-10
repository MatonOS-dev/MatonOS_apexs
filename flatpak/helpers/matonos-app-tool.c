#define _GNU_SOURCE
/* Unprivileged applet entry for the per-app stock portal. Every process
 * remains at the app UID, in matonos_linux_app, with its inherited seccomp
 * filter. This entry never uses linuxd's privileged launch wrapper. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv) {
    (void)argc;
    unsigned uid=getuid();
    if(uid%100000<10000 || uid%100000>=20000 || geteuid()!=uid) {
        fprintf(stderr,"matonos-app-tool: app UID required\n");return 127;
    }
    const char* name=strrchr(argv[0],'/');name=name?name+1:argv[0];
    int bwrap=!strcmp(name,"matonos-app-bwrap");
    /* The stock portal builds a minimal CLI environment and does not retain
     * FLATPAK_BWRAP. Pin the unprivileged applet for its nested run here. */
    if(!bwrap && setenv("FLATPAK_BWRAP",
            "/apex/com.matonos.flatpak/bin/matonos-app-bwrap",1))return 127;
    argv[0]=bwrap ? "bwrap" : "flatpak";
    execv("/apex/com.matonos.flatpak/bin/matonos-flatpak",argv);
    perror("matonos-app-tool: exec");return 127;
}
