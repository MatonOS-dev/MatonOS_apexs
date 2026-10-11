#ifndef MATON_DOWNLOADS_GRANT_H
#define MATON_DOWNLOADS_GRANT_H
/* Downloads requests come only from Flatpak's effective Context. This header
 * is shared with the launch wrappers so the wire format has one validator. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct MatonFolderRequest {
    char* subpath;
    int readonly;
    int fd;
} MatonFolderRequest;
typedef struct MatonFolderRequests {
    MatonFolderRequest* items;
    size_t count;
} MatonFolderRequests;
typedef struct MatonSharedFolder {
    const char* flatpak;
    const char* android;
    const char* sandbox;
} MatonSharedFolder;
/* Downloads is the first mapping; keep the Android/XDG spellings separate. */
static const MatonSharedFolder maton_shared_folders[] = {
    {"xdg-download","Download","Downloads"},
};

static inline void maton_folders_clear(MatonFolderRequests* folders) {
    for(size_t i=0;i<folders->count;i++) {
        free(folders->items[i].subpath);
        if(folders->items[i].fd>=0)close(folders->items[i].fd);
    }
    free(folders->items);memset(folders,0,sizeof(*folders));
}
static inline int maton_folder_subpath(const char* path) {
    if(!path || *path=='/')return 0;
    if(!*path)return 1;
    const char* component=path;
    for(const char* p=path;;p++) {
        if(*p=='/' || !*p) {
            size_t n=(size_t)(p-component);
            if(!n || (n==1 && *component=='.') ||
                    (n==2 && component[0]=='.' && component[1]=='.'))return 0;
            if(!*p)return 1;
            component=p+1;
        }
    }
}
static inline int maton_folder_add(MatonFolderRequests* folders,const char* path,int readonly) {
    if(!maton_folder_subpath(path)){errno=EINVAL;return -1;}
    for(size_t i=0;i<folders->count;i++)if(!strcmp(folders->items[i].subpath,path)) {
        folders->items[i].readonly &= readonly;return 0;
    }
    if(folders->count>=SIZE_MAX/sizeof(*folders->items)-1){errno=EOVERFLOW;return -1;}
    char* copy=strdup(path);if(!copy)return -1;
    void* items=realloc(folders->items,(folders->count+1)*sizeof(*folders->items));
    if(!items){free(copy);return -1;}
    folders->items=items;
    folders->items[folders->count++]=(MatonFolderRequest){copy,readonly,-1};
    return 0;
}
static inline int maton_folder_compare(const void* a,const void* b) {
    return strcmp(((const MatonFolderRequest*)a)->subpath,((const MatonFolderRequest*)b)->subpath);
}
static inline int maton_folder_token(MatonFolderRequests* folders,char* token) {
    if(!*token || *token=='!')return 0;
    int readonly=0;
    char* suffix=strrchr(token,':');
    if(suffix) {
        if(!strcmp(suffix,":ro"))readonly=1;
        else if(strcmp(suffix,":rw") && strcmp(suffix,":create"))return 0;
        *suffix=0;
    }
    /* Other xdg roots can become table entries here when implemented. */
    const char* name=maton_shared_folders[0].flatpak;
    size_t length=strlen(name);
    if(!strcmp(token,"home") || !strcmp(token,"host"))return maton_folder_add(folders,"",readonly);
    if(strncmp(token,name,length))return 0;
    const char* sub=token+length;
    if(*sub && *sub!='/')return 0;
    if(*sub)sub++;
    return maton_folder_add(folders,sub,readonly);
}
/* Flatpak info --show-permissions serializes an effective GKeyFile. Decode
 * its list escapes before interpreting tokens; never split escaped ';'. */
static inline int maton_folders_metadata(const char* metadata,MatonFolderRequests* folders) {
    char* copy=strdup(metadata?metadata:"");if(!copy)return -1;
    int context=0,rc=0;char* state=NULL;
    for(char* line=strtok_r(copy,"\n",&state);line;line=strtok_r(NULL,"\n",&state)) {
        while(*line==' ' || *line=='\t')line++;
        size_t length=strlen(line);
        while(length && (line[length-1]=='\r' || line[length-1]==' ' || line[length-1]=='\t'))line[--length]=0;
        if(*line=='['){context=!strcmp(line,"[Context]");continue;}
        if(!context || strncmp(line,"filesystems",11))continue;
        char* value=line+11;while(*value==' ' || *value=='\t')value++;
        if(*value!='=')continue;
        value++;while(*value==' ' || *value=='\t')value++;
        char* token=malloc(strlen(value)+1);if(!token){rc=-1;break;}
        size_t used=0;
        for(char* p=value;;p++) {
            if(!*p || *p==';') {
                token[used]=0;
                if(maton_folder_token(folders,token)){rc=-1;break;}
                used=0;if(!*p)break;
            } else if(*p=='\\') {
                p++;
                char c=*p;
                if(c=='s')c=' ';else if(c=='n')c='\n';else if(c=='t')c='\t';else if(c=='r')c='\r';
                else if(c!='\\' && c!=';'){errno=EINVAL;rc=-1;break;}
                token[used++]=c;
            } else token[used++]=*p;
        }
        free(token);if(rc)break;
    }
    free(copy);
    if(rc){maton_folders_clear(folders);return -1;}
    if(folders->count>1)qsort(folders->items,folders->count,sizeof(*folders->items),maton_folder_compare);
    return 0;
}
/* Hex subpaths keep whitespace and delimiters out of the environment wire
 * format. Consent uses fd=-1; a granted launch uses pinned directory fds. */
static inline char* maton_folders_encode(const MatonFolderRequests* folders) {
    size_t size=1;
    for(size_t i=0;i<folders->count;i++) {
        size_t n=strlen(folders->items[i].subpath);
        if(size>SIZE_MAX-32 || n>(SIZE_MAX-size-32)/2){errno=EOVERFLOW;return NULL;}
        size+=32+n*2;
    }
    char* out=malloc(size);if(!out)return NULL;
    char* p=out;
    static const char hex[]="0123456789abcdef";
    for(size_t i=0;i<folders->count;i++) {
        p+=sprintf(p,"%d:%d:",folders->items[i].fd,folders->items[i].readonly);
        const unsigned char* path=(const unsigned char*)folders->items[i].subpath;
        for(;*path;path++){*p++=hex[*path>>4];*p++=hex[*path&15];}
        *p++='\n';
    }
    *p=0;return out;
}
static inline int maton_hex_digit(char c) {
    if(c>='0' && c<='9')return c-'0';
    if(c>='a' && c<='f')return c-'a'+10;
    return -1;
}
/* info --show-permissions prints its GKeyFile through Flatpak's terminal
 * escaping layer: decode that layer before decoding GKeyFile list escapes. */
static inline char* maton_permissions_unescape(const char* printed) {
    if(!printed){errno=EINVAL;return NULL;}
    size_t length=strlen(printed);
    char* out=malloc(length+1);if(!out)return NULL;
    char* write=out;
    for(const char* p=printed;*p;p++) {
        if(*p!='\\'){*write++=*p;continue;}
        p++;
        if(*p=='\\'){*write++='\\';continue;}
        unsigned digits=*p=='x'?2:*p=='u'?4:*p=='U'?8:0;
        if(!digits)goto invalid;
        uint32_t value=0;
        for(unsigned i=0;i<digits;i++) {
            p++;int digit=maton_hex_digit(*p);
            if(digit<0 && *p>='A' && *p<='F')digit=*p-'A'+10;
            if(digit<0)goto invalid;
            value=(value<<4)|(unsigned)digit;
        }
        if(!value || value>0x10ffff || (value>=0xd800 && value<=0xdfff))goto invalid;
        if(digits==2 || value<0x80)*write++=(char)value;
        else if(value<0x800){*write++=(char)(0xc0|(value>>6));*write++=(char)(0x80|(value&63));}
        else if(value<0x10000) {
            *write++=(char)(0xe0|(value>>12));*write++=(char)(0x80|((value>>6)&63));*write++=(char)(0x80|(value&63));
        } else {
            *write++=(char)(0xf0|(value>>18));*write++=(char)(0x80|((value>>12)&63));
            *write++=(char)(0x80|((value>>6)&63));*write++=(char)(0x80|(value&63));
        }
    }
    *write=0;return out;
invalid:
    free(out);errno=EINVAL;return NULL;
}
static inline int maton_folders_decode(const char* encoded,MatonFolderRequests* folders,int descriptors) {
    const char* p=encoded?encoded:"";
    while(*p) {
        char* end=NULL;errno=0;long fd=strtol(p,&end,10);
        if(errno || end==p || *end!=':' || (descriptors?fd<3:fd!=-1) || fd>INT32_MAX)goto invalid;
        p=end+1;
        if((p[0]!='0' && p[0]!='1') || p[1]!=':')goto invalid;
        int ro=p[0]-'0';p+=2;
        if(descriptors)for(size_t i=0;i<folders->count;i++)
            if(folders->items[i].fd==(int)fd)goto invalid;
        const char* newline=strchr(p,'\n');if(!newline || (newline-p)%2)goto invalid;
        size_t n=(size_t)(newline-p)/2;
        char* path=malloc(n+1);if(!path)return -1;
        for(size_t i=0;i<n;i++) {
            int a=maton_hex_digit(p[2*i]),b=maton_hex_digit(p[2*i+1]);
            if(a<0 || b<0 || !(a*16+b)){free(path);goto invalid;}
            path[i]=(char)(a*16+b);
        }
        path[n]=0;
        size_t before=folders->count;
        if(maton_folder_add(folders,path,ro)){free(path);return -1;}
        free(path);
        if(folders->count==before)goto invalid;
        folders->items[before].fd=descriptors?(int)fd:-1;
        p=newline+1;
    }
    return 0;
invalid:
    errno=EINVAL;return -1;
}
#endif
