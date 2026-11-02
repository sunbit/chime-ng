#include "core.h"
#include <string.h>
static bool equal_ci(const char *a,const char *b) {
    for(;*a&&*b;a++,b++){char x=*a,y=*b;if(x>='A'&&x<='Z')x+=32;if(y>='A'&&y<='Z')y+=32;if(x!=y)return false;}
    return *a==*b;
}
static bool copy(char *dst,size_t size,const char *src) { size_t n=strlen(src);if(n>=size)return false;memcpy(dst,src,n+1);return true; }
bool http_parse_headers(char *buffer,HttpRequest *r) {
    memset(r,0,sizeof *r);
    char *end=strstr(buffer,"\r\n");if(!end)return false;*end=0;
    char *path=strchr(buffer,' ');if(!path)return false;*path++=0;
    char *version=strchr(path,' ');if(!version)return false;*version++=0;
    if(strcmp(version,"HTTP/1.1")||path[0]!='/'||!copy(r->method,sizeof r->method,buffer)||!copy(r->path,sizeof r->path,path))return false;
    bool auth=false,type=false,host=false;
    for(char *line=end+2;*line;) {
        end=strstr(line,"\r\n");if(!end)return false;*end=0;
        if(!*line)return host && end[2]==0;
        if(*line==' '||*line=='\t')return false;
        char *colon=strchr(line,':');if(!colon)return false;*colon++=0;
        for(const char *k=line;*k;k++)if(!((*k>='a'&&*k<='z')||(*k>='A'&&*k<='Z')||*k=='-'))return false;
        while(*colon==' '||*colon=='\t')colon++;
        char *tail=colon+strlen(colon);while(tail>colon&&(tail[-1]==' '||tail[-1]=='\t'))*--tail=0;
        for(const char *v=colon;*v;v++)if((unsigned char)*v<32||*v==127)return false;
        if(equal_ci(line,"Content-Length")){if(r->length_present||!parse_uint(colon,SOUND_MAX_BYTES,&r->length))return false;r->length_present=true;}
        else if(equal_ci(line,"Transfer-Encoding")||equal_ci(line,"Expect"))return false;
        else if(equal_ci(line,"Authorization")){if(auth||!copy(r->authorization,sizeof r->authorization,colon))return false;auth=true;}
        else if(equal_ci(line,"Content-Type")){if(type||!copy(r->content_type,sizeof r->content_type,colon))return false;type=true;}
        else if(equal_ci(line,"X-Chime-Request")){if(r->custom_header||strcmp(colon,"1"))return false;r->custom_header=true;}
        else if(equal_ci(line,"Host")){if(host||!*colon)return false;host=true;}
        line=end+2;
    }
    return host;
}
