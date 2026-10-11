#ifndef MATON_DOWNLOADS_USER_DIRS_H
#define MATON_DOWNLOADS_USER_DIRS_H
#include "SafePath.h"
#include <stdlib.h>

/* Called as the app UID before Flatpak runs. XDG_CONFIG_HOME in the final
 * sandbox is .var/app/<id>/config, not the wrapper's own .config. */
static inline int maton_download_user_dirs(int home,const char* id) {
    int download=safe_directory_at(home,"Downloads",1);
    if(download<0)return -1;
    close(download);
    char* app_path=NULL;
    if(asprintf(&app_path,".var/app/%s",id)<0)return -1;
    int app=safe_directory_at(home,app_path,1);free(app_path);
    if(app<0)return -1;
    /* Flatpak may make config a relative link (config -> .config) for apps
     * that persist their whole per-app dir. Follow one plain sibling name. */
    char target[256];
    ssize_t length=readlinkat(app,"config",target,sizeof(target)-1);
    const char* name="config";
    if(length>0) {
        target[length]=0;
        if(strchr(target,'/') || !strcmp(target,".") || !strcmp(target,"..")){close(app);errno=ENOTDIR;return -1;}
        name=target;
    }
    int directory=safe_directory_at(app,name,1);close(app);
    if(directory<0)return -1;
    char* contents=NULL;size_t size=0;
    FILE* output=open_memstream(&contents,&size);
    if(!output){close(directory);return -1;}
    int old=openat(directory,"user-dirs.dirs",O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    int rc=0;
    if(old>=0) {
        struct stat st;
        if(fstat(old,&st) || !S_ISREG(st.st_mode)){close(old);rc=-1;}
        else {
            FILE* input=fdopen(old,"r");
            if(!input){close(old);rc=-1;}
            else {
                char* line=NULL;size_t capacity=0;ssize_t n;
                while((n=getline(&line,&capacity,input))>=0) {
                    const char* key=line;while(*key==' ' || *key=='\t')key++;
                    if(strncmp(key,"XDG_DOWNLOAD_DIR=",17) && fwrite(line,1,(size_t)n,output)!=(size_t)n){rc=-1;break;}
                }
                if(ferror(input))rc=-1;
                free(line);fclose(input);
            }
        }
    } else if(errno!=ENOENT)rc=-1;
    if(!rc) {
        if(fflush(output))rc=-1;
        else if(size && contents[size-1]!='\n' && fputc('\n',output)==EOF)rc=-1;
        if(fputs("XDG_DOWNLOAD_DIR=\"$HOME/Downloads\"\n",output)==EOF)rc=-1;
    }
    if(fclose(output))rc=-1;
    if(!rc)rc=safe_replace_at(directory,"user-dirs.dirs",contents,size);
    free(contents);close(directory);return rc;
}
#endif
