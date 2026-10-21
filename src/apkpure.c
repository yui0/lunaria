/* MPL-2.0. APK files are saved unchanged; partial/error responses are never published. */
#include "apkpure.h"
#include "dvm/dvm_net.h"
#include "lunaria_os.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static int fail(char *error, size_t size, const char *message)
{ snprintf(error,size,"%s",message); return -1; }

int apkpure_package_valid(const char *s)
{
    size_t len=strlen(s); int dots=0,first=1;
    if (!len || len>=256) return 0;
    for (;*s;++s) {
        unsigned char c=(unsigned char)*s;
        if (c=='.') { if (first) return 0; ++dots; first=1; continue; }
        int alpha=(c>='a' && c<='z') || (c>='A' && c<='Z');
        if (!(alpha || c=='_' || (!first && c>='0' && c<='9'))) return 0;
        first=0;
    }
    return dots && !first;
}

int apkpure_search_url(const char *query,char *out,size_t cap)
{
    static const char prefix[]="https://apkpure.com/search?q=";
    static const char hex[]="0123456789ABCDEF";
    size_t at=sizeof prefix-1;
    if (!query || !*query || cap<=at) return -1;
    memcpy(out,prefix,at);
    for (const unsigned char *p=(const unsigned char *)query;*p;++p) {
        int plain=(*p>='a' && *p<='z') || (*p>='A' && *p<='Z') ||
                  (*p>='0' && *p<='9') || strchr("-._~",*p);
        size_t n=plain ? 1 : 3;
        if (n>=cap-at) return -1;
        if (plain) out[at++]=(char)*p;
        else {out[at++]='%';out[at++]=hex[*p>>4];out[at++]=hex[*p&15];}
    }
    out[at]=0;return 0;
}

/* A small HTML attribute scanner, bounded by the enclosing start tag.
 * Attribute contents and visible labels never become executable markup. */
static int space(unsigned char c) { return c==' ' || c=='\t' || c=='\r' || c=='\n' || c=='\f'; }
static const char *tag_end(const char *p)
{
    char quote=0;
    for (;*p;++p) {
        if (quote) {if (*p==quote) quote=0;}
        else if (*p=='\'' || *p=='"') quote=*p;
        else if (*p=='>') return p;
    }
    return NULL;
}
static int attribute(const char *p,const char *end,const char *name,char *out,size_t cap)
{
    while (p<end) {
        while (p<end && (space(*p) || *p=='/')) ++p;
        const char *key=p;
        while (p<end && !space(*p) && *p!='=' && *p!='/') ++p;
        size_t len=(size_t)(p-key);
        while (p<end && space(*p)) ++p;
        if (p==end || *p!='=') { if (p==key) ++p; continue; }
        ++p;while (p<end && space(*p)) ++p;
        char quote=p<end && (*p=='\'' || *p=='"') ? *p++ : 0;
        const char *value=p;
        while (p<end && (quote ? *p!=quote : !space(*p))) ++p;
        size_t n=(size_t)(p-value);
        if (len==strlen(name) && !strncasecmp(key,name,len)) {
            if (n>=cap) return 0;
            memcpy(out,value,n);out[n]=0;return 1;
        }
        if (quote && p<end) ++p;
    }
    return 0;
}
static void label(const char *p,const char *end,char *out,size_t cap)
{
    size_t at=0;int blank=0;
    while (p<end && at+1<cap) {
        if (*p=='<') {const char *e=tag_end(p+1);if (!e || e>=end) break;p=e+1;blank=at!=0;continue;}
        char c=*p++;
        if (c=='&') {
            static const struct {const char *name;char value;} entities[]={
                {"amp;",'&'},{"quot;",'"'},{"apos;",'\''},{"lt;",'<'},{"gt;",'>'},{"nbsp;",' '}};
            for (size_t i=0;i<sizeof entities/sizeof entities[0];++i) {
                size_t n=strlen(entities[i].name);
                if ((size_t)(end-p)>=n && !memcmp(p,entities[i].name,n)) {c=entities[i].value;p+=n;break;}
            }
        }
        if (space((unsigned char)c)) {blank=at!=0;continue;}
        if (blank && at+2<cap) out[at++]=' ';
        blank=0;out[at++]=c;
    }
    out[at]=0;
}
int apkpure_parse_results(const char *html,struct apkpure_result *out,size_t cap)
{
    size_t count=0;
    if (!cap) return 0;
    for (const char *p=html;*p;) {
        p=strchr(p,'<');if (!p) break;++p;
        if ((*p!='a' && *p!='A') || !space((unsigned char)p[1])) continue;
        const char *end=tag_end(p+1);if (!end) break;
        char href[2048],title[512];
        int linked=attribute(p+1,end,"href",href,sizeof href);
        int titled=attribute(p+1,end,"title",title,sizeof title);
        p=end+1;if (!linked) continue;
        char *route=href;
        if (!strncmp(route,"https://apkpure.com/",20)) route+=19;
        else if (!strncmp(route,"//apkpure.com/",14)) route+=13;
        if (*route!='/' || route[1]=='/') continue;
        char *cut=strpbrk(route,"?#");if (cut) *cut=0;
        size_t n=strlen(route);while (n>1 && route[n-1]=='/') route[--n]=0;
        char *pkg=strrchr(route,'/');
        /* App links have a slug before the package; footer documents such as
         * /about.html are not Android applications despite containing a dot. */
        if (!pkg || pkg==route || !apkpure_package_valid(++pkg)) continue;
        size_t i=0;for (;i<count;++i) if (!strcmp(out[i].package,pkg)) break;
        if (i<count && strcmp(out[i].title,out[i].package)) continue;
        if (i==count) {
            if (count==cap) continue;
            snprintf(out[i].package,sizeof out[i].package,"%s",pkg);++count;
        }
        /* Search cards may link the icon first and the app name second. */
        if (titled) label(title,title+strlen(title),out[i].title,sizeof out[i].title);
        else {
            const char *close=luna_strcasestr(p,"</a");
            if (close) label(p,close,out[i].title,sizeof out[i].title);
            else out[i].title[0]=0;
        }
        if (!out[i].title[0]) snprintf(out[i].title,sizeof out[i].title,"%s",pkg);
    }
    return (int)count;
}

static int request(const char *url,struct dvm_http_response *r,char *error,size_t size)
{
    struct dvm_http_header headers[]={ {"User-Agent","Lunaria APK launcher"},{"Accept-Encoding","identity"} };
    if (!dvm_http_perform("GET",url,headers,2,NULL,0,10000,true,r)) {
        snprintf(error,size,"%s",r->error);dvm_http_response_free(r);return -1;
    }
    if (r->status!=200) {
        snprintf(error,size,"APKPure returned HTTP %d. Please try again later.",r->status);
        dvm_http_response_free(r);return -1;
    }
    return 0;
}
int apkpure_search(const char *query,struct apkpure_result *out,size_t cap,
                   char *error,size_t error_size,apkpure_progress progress,void *context)
{
    char url[4096];struct dvm_http_response r={0};
    if (apkpure_search_url(query,url,sizeof url)) return fail(error,error_size,"Enter a shorter application name.");
    if (request(url,&r,error,error_size)) return -1;
    char *html=NULL;size_t used=0,capacity=0;int result=-1;
    unsigned char buffer[16384];
    for (;;) {
        if (progress && progress(context,used,r.content_length>0 ? (uint64_t)r.content_length : 0)) {fail(error,error_size,"Cancelled.");break;}
        long n=dvm_http_read(&r,buffer,sizeof buffer);
        if (n<0) {fail(error,error_size,r.error);break;}
        if (!n) {
            if (r.content_length>=0 && (uint64_t)used!=(uint64_t)r.content_length) {
                fail(error,error_size,"The search response was incomplete. Please try again.");break;
            }
            if(html)html[used]=0;
            result=apkpure_parse_results(html ? html : "",out,cap);
            if (!result) fail(error,error_size,"No applications found on APKPure.");
            break;
        }
        /* The HTML includes metadata unrelated to the result cards. Its size
         * is not fixed: grow on demand rather than rejecting normal pages at
         * an arbitrary document limit or allocating that limit up front. */
        if ((size_t)n>SIZE_MAX-used-1) {fail(error,error_size,"Not enough memory for search results.");break;}
        size_t need=used+(size_t)n+1;
        if(need>capacity) {
            size_t next=capacity ? capacity : sizeof buffer;
            while(next<need) {
                if(next>SIZE_MAX/2){next=need;break;}
                next*=2;
            }
            char *grown=realloc(html,next);
            if(!grown){fail(error,error_size,"Not enough memory for search results.");break;}
            html=grown;capacity=next;
        }
        memcpy(html+used,buffer,(size_t)n);
        used+=(size_t)n;
    }
    free(html);dvm_http_response_free(&r);return result;
}
int apkpure_download(const char *package,const char *directory,char *path,size_t cap,
                     char *error,size_t error_size,apkpure_progress progress,void *context)
{
    char url[512],temp[4096],final[4096];struct dvm_http_response r={0};
    if (!apkpure_package_valid(package)) return fail(error,error_size,"Invalid Android package name.");
    snprintf(url,sizeof url,"https://d.apkpure.com/b/XAPK/%s?version=latest",package);
    int len=snprintf(temp,sizeof temp,"%s/%s-XXXXXX.xapk.part",directory,package);
    if (len<0 || (size_t)len>=sizeof temp || (size_t)len>=cap) return fail(error,error_size,"Download path is too long.");
    if (request(url,&r,error,error_size)) return -1;
    int fd=luna_file_mkstemps(temp,10);
    if (fd<0) {dvm_http_response_free(&r);return fail(error,error_size,"Cannot create the download file.");}
    unsigned char buf[65536],magic[4];size_t magic_len=0;uint64_t bytes=0;int result=-1;
    for (;;) {
        if (progress && progress(context,bytes,r.content_length>0 ? (uint64_t)r.content_length : 0)) {fail(error,error_size,"Cancelled.");break;}
        long n=dvm_http_read(&r,buf,sizeof buf);
        if (n<0) {fail(error,error_size,r.error);break;}
        if (!n) {
            if (magic_len<4 || memcmp(magic,"PK\003\004",4)) {fail(error,error_size,"APKPure did not return an APK or XAPK archive.");break;}
            if (r.content_length>=0 && bytes!=(uint64_t)r.content_length) {fail(error,error_size,"The download was incomplete. Please try again.");break;}
            result=0;break;
        }
        if (magic_len<4) {size_t take=(size_t)n<4-magic_len ? (size_t)n : 4-magic_len;memcpy(magic+magic_len,buf,take);magic_len+=take;}
        size_t pos=0;
        while (pos<(size_t)n) {
            ptrdiff_t wrote=luna_fd_write(fd,buf+pos,(size_t)n-pos);
            if (wrote<0 && errno==EINTR) continue;
            if (wrote<=0) {fail(error,error_size,"Cannot write the download (check free disk space).");goto done;}
            pos+=(size_t)wrote;
        }
        bytes+=(uint64_t)n;
    }
done:
    if (luna_fd_close(fd) && !result) result=fail(error,error_size,"Cannot finish writing the download.");
    dvm_http_response_free(&r);
    if (!result) {
        snprintf(final,sizeof final,"%s",temp);final[strlen(final)-5]=0;
        if (luna_file_rename(temp,final)) result=fail(error,error_size,"Cannot save the downloaded archive.");
        else snprintf(path,cap,"%s",final);
    }
    if (result) remove(temp);
    return result;
}
