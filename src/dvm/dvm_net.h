/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * A blocking HTTP/HTTPS client for the built-in java.net classes.  The VM
 * interprets one thread at a time, so a request runs to completion inside the
 * bytecode call that asked for it — the same shape java.net.HttpURLConnection
 * presents to its caller.
 */

#ifndef LUNARIA_DVM_NET_H
#define LUNARIA_DVM_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Shared by native sockets, Java networking and the emulator menu. */
bool dvm_network_enabled(void);
void dvm_network_set_enabled(bool enabled);
int dvm_sock_close(int fd);

/* Establish TCP/TLS without sending an HTTP request. The connected perform
 * consumes and clears the transport; close also accepts NULL. */
void *dvm_http_connect(const char *url, int timeout_ms, char *error, size_t error_size);
void dvm_http_transport_close(void *transport);

/* A caller-owned, already connected stream. Ownership transfers only when
 * transport_from_io succeeds. close runs exactly once after consumption. */
struct dvm_http_io {
   void *context;
   long (*read)(void *context,void *buffer,size_t length);
   long (*write)(void *context,const void *buffer,size_t length);
   void (*close)(void *context);
};
void *dvm_http_transport_from_io(const struct dvm_http_io *io);
typedef void *(*dvm_http_connector)(void *context,const char *url,int timeout_ms,char *error,size_t error_size);

struct dvm_http_header {
   char *name;
   char *value;
};

struct dvm_http_response {
   char *status_line;               /* exact wire protocol/status/reason */
   int status;                      /* -1 when the exchange never completed */
   struct dvm_http_header *headers;
   int nheaders;
   uint8_t *body;                   /* only once slurped; NULL while streaming */
   size_t body_len;
   char error[512];                 /* empty on success */

   /* The exchange stops at the end of the headers and leaves the connection
    * open, so the body is read as the caller consumes it.  Buffering it whole
    * is not an option for this workload: the game downloads its content as
    * pak files of several gigabytes each, and a response that has to fit in
    * memory first stops the emulator dead at the first one. */
   void *stream;                    /* struct stream *, NULL once finished */
   uint8_t *pre;                    /* body bytes read along with the headers */
   size_t pre_len, pre_pos;
   long long content_length;        /* from the header; -1 when absent */
   long long body_read;             /* handed to the caller so far */
   bool chunked;

   /* Read-ahead.  A guest that copies a file reads it in its own small buffer;
    * one socket read per guest read is one interpreter-lock turn per guest
    * read, and the lock turns over once per rendered frame.  That capped the
    * patch download at the guest's buffer times the frame rate (~0.5 MB/s
    * measured).  Filling a big buffer once and serving the guest's reads out
    * of it takes the socket — and the lock hand-off — out of the inner loop. */
   uint8_t *ra;
   size_t ra_len, ra_pos, ra_cap;
};

bool dvm_http_perform_connected(void **transport, const char *method,
                      const char *url, const struct dvm_http_header *headers,
                      int nheaders, const uint8_t *body, size_t body_len,
                      int timeout_ms, bool follow_redirects,
                      struct dvm_http_response *out);

/* Performs one exchange.  `headers` are sent as given, minus the ones this
 * layer owns (Host, Content-Length, Connection).  Returns false and fills
 * out->error on a transport failure; an HTTP error status is a success here,
 * exactly as it is for HttpURLConnection.getResponseCode(). */
bool dvm_http_perform(const char *method, const char *url,
                      const struct dvm_http_header *headers, int nheaders,
                      const uint8_t *body, size_t body_len,
                      int timeout_ms, bool follow_redirects,
                      struct dvm_http_response *out);

/* The connector is called for every new hop, including redirects. This lets
 * Java socket factories and verification stay in the caller's transport path. */
bool dvm_http_perform_with_connector(void **transport,const char *method,const char *url,
   const struct dvm_http_header *headers,int nheaders,const uint8_t *body,size_t body_len,
   int timeout_ms,bool follow_redirects,struct dvm_http_response *out,
   dvm_http_connector connector,void *context);

void dvm_http_response_free(struct dvm_http_response *r);

/* Reads the next piece of the body.  Returns 0 at end of body, -1 on a
 * transport error.  A chunked response is decoded whole on the first call
 * (server-side chunking is only used here for small API replies); a
 * Content-Length response streams straight off the socket. */
long dvm_http_read(struct dvm_http_response *r, void *buf, size_t n);

/* Bytes dvm_http_read() can return without touching the socket.  A caller that
 * drops a lock around the read uses this to skip doing so when it would not
 * block. */
size_t dvm_http_avail(const struct dvm_http_response *r);

/* Reads whatever is left of the body into r->body / r->body_len, for the
 * callers that want it as one array (error bodies, small API replies).
 * Returns false only on a transport error. */
bool dvm_http_slurp(struct dvm_http_response *r);

/* ------------------------------------------------------------------------ *
 * Stream sockets for java.net.Socket / javax.net.ssl.SSLSocket
 *
 * The HTTP client above owns its connection from open to close.  A library
 * that speaks HTTP itself (OkHttp, and anything built on Okio) needs the layer
 * underneath instead: a connected TCP socket it reads and writes, and TLS
 * layered on that socket after the fact.  These are that layer.  All of them
 * block; the callers drop the interpreter and execution locks around them.
 * ------------------------------------------------------------------------ */

/* Connects to host:port (a name or a numeric address), IPv4 first, bound
 * first to local_addr:local_port when either is given (NULL / 0 = any).
 * Returns the fd, or -1 with errno set and err filled.  timeout_ms <= 0 waits
 * for as long as the kernel does. */
int dvm_sock_connect(const char *host, int port, const char *local_addr,
                     int local_port, int timeout_ms, char *err, size_t errsz);

/* SO_RCVTIMEO for a Socket's soTimeout; 0 means block forever. */
void dvm_sock_set_timeout(int fd, int timeout_ms);

struct dvm_tls;

#define DVM_TLS_VERIFY_CHAIN 1   /* against the host trust store */
#define DVM_TLS_VERIFY_HOST  2   /* the certificate names `host` */

/* TLS client handshake over a connected fd.  `host` is the server name sent
 * in SNI.  `verify` says what the handshake itself checks: the chain against
 * the host's trust store (what Android's default TrustManagerImpl does against
 * its own) and, only when asked, the name — on a device an SSLSocket leaves
 * the name to the HostnameVerifier unless the endpoint identification
 * algorithm is set.  `alpn` is the ALPN protocol list in wire format (length-
 * prefixed), or NULL.  `min_version`/`max_version` are TLS1_x_VERSION values,
 * 0 for the library default.  `ciphers` are IANA/Java cipher-suite names, or
 * NULL for the default set.  Returns NULL with err filled on failure;
 * *verify_failed says whether it was the certificate that was rejected. */
struct dvm_tls *dvm_tls_connect(int fd, const char *host, int verify,
                                const uint8_t *alpn, size_t alpn_len,
                                int min_version, int max_version,
                                const char *const *ciphers, int nciphers,
                                bool *verify_failed, char *err, size_t errsz);

/* 0 at end of stream, -1 on error, -2 when timeout_ms (> 0) passed with
 * nothing to read.  Safe against a concurrent dvm_tls_write(). */
long dvm_tls_read(struct dvm_tls *t, void *buf, size_t n, int timeout_ms);
/* Bytes written, or -1. */
long dvm_tls_write(struct dvm_tls *t, const void *buf, size_t n);
/* Decrypted bytes available without touching the socket. */
size_t dvm_tls_pending(struct dvm_tls *t);
/* Sends close_notify and frees the session.  Does not close the fd.  No other
 * call may be in progress on it. */
void dvm_tls_free(struct dvm_tls *t);

/* "TLSv1.3", "TLSv1.2", ... — the names SSLSession.getProtocol() reports. */
const char *dvm_tls_protocol(const struct dvm_tls *t);
/* IANA name, e.g. "TLS_AES_128_GCM_SHA256" — SSLSession.getCipherSuite(). */
const char *dvm_tls_cipher(const struct dvm_tls *t);
/* Negotiated ALPN protocol, or "" when none was. */
void dvm_tls_alpn(const struct dvm_tls *t, char *out, size_t outsz);
/* The peer's chain, leaf first, as DER.  Returns the count; the caller frees
 * each (*der)[i] and *der, *len. */
int dvm_tls_peer_chain(const struct dvm_tls *t, uint8_t ***der, int **len);
/* The key-exchange/authentication name a TrustManager is handed as
 * authType ("ECDHE_RSA", "RSA", ..., "GENERIC" for TLS 1.3). */
const char *dvm_tls_auth_type(const struct dvm_tls *t);
/* Session id bytes (may be empty). */
size_t dvm_tls_session_id(const struct dvm_tls *t, uint8_t *out, size_t outsz);

/* IANA names of the cipher suites this TLS library can offer, NULL-terminated
 * and owned by the library. */
const char *const *dvm_tls_supported_ciphers(void);

/* DNS-SD host adapter. */
/* Caller serializes operations. Events own TXT data until event_clear().
 * Callbacks are collected during poll; guest code never runs inside Avahi. */
struct nsd_host;
enum nsd_event_kind { NSD_FOUND, NSD_LOST, NSD_RESOLVED, NSD_FAILED };
struct nsd_event {
   uint64_t id;
   enum nsd_event_kind kind;
   int error, interface_index, protocol;
   char name[256], type[256], domain[256], host[256], address[64];
   uint16_t port;
   unsigned char *txt;
   size_t txt_size;
};
struct nsd_host *nsd_host_open(int *error);
void nsd_host_close(struct nsd_host *host);
int nsd_host_browse(struct nsd_host *, uint64_t id, const char *type);
/* Keep the resolver open for service-info change notifications. */
int nsd_host_resolve(struct nsd_host *, uint64_t id, const struct nsd_event *service);
int nsd_host_cancel(struct nsd_host *, uint64_t id);
int nsd_host_poll(struct nsd_host *);
int nsd_host_next(struct nsd_host *, struct nsd_event *out);
void nsd_event_clear(struct nsd_event *);

/* Readiness wait with a process cancellation descriptor.  Cancellation is
 * persistent (the descriptor is never drained); -2 means timeout. */
int dvm_socket_wait(int fd, short events, int timeout_ms, int cancel_fd);
long dvm_tls_read_cancel(struct dvm_tls *t, void *buf, size_t n,
                         int timeout_ms, int cancel_fd);
long dvm_tls_write_cancel(struct dvm_tls *t, const void *buf, size_t n,
                          int cancel_fd);

/* Per-call error text: safe when a reader and writer share one session.
 * error_size == 0 permits a NULL error buffer. */
long dvm_tls_read_error_cancel(struct dvm_tls *t, void *buf, size_t n,
                         int timeout_ms, int cancel_fd, char *error, size_t error_size);
long dvm_tls_write_error_cancel(struct dvm_tls *t, const void *buf, size_t n,
                         int cancel_fd, char *error, size_t error_size);

struct dvm_tls *dvm_tls_connect_cancel(int fd, const char *host, int verify,
                                const uint8_t *alpn, size_t alpn_len,
                                int min_version, int max_version,
                                const char *const *ciphers, int nciphers,
                                bool *verify_failed, char *err, size_t errsz,
                                int timeout_ms, int cancel_fd);

#endif /* LUNARIA_DVM_NET_H */

/* Included again by dvm_runtime.c after its XML and Java runtime helpers. */
#if defined(LUNARIA_DVM_NETWORK_POLICY_IMPLEMENTATION) && !defined(LUNARIA_DVM_NETWORK_POLICY_DEFINED)
#define LUNARIA_DVM_NETWORK_POLICY_DEFINED

/* MPL-2.0. Application cleartext policy. Uses the existing compiled XML reader.
 * The process owns one policy; queries allocate no Java objects or network I/O. */
struct nsp_config { int parent,cleartext; };
struct nsp_domain { char *name; int config; bool subdomains; };
struct nsp_policy {
   struct nsp_config *configs;size_t count,capacity;
   struct nsp_domain *domains;size_t domain_count,domain_capacity;
   const char *error;bool oom;
};
static void nsp_destroy(void *data)
{
   struct nsp_policy *p=data;if(!p)return;
   for(size_t i=0;i<p->domain_count;++i) free(p->domains[i].name);
   free(p->domains);free(p->configs);free(p);
}
static bool nsp_error(struct nsp_policy *p,const char *error)
{ p->error=error;return false; }
static void *nsp_grow(struct nsp_policy *p,void *array,size_t *capacity,size_t count,size_t width)
{
   if(count<*capacity)return array;
   size_t n=*capacity ? *capacity*2 : 8;
   if(n<=count || n>SIZE_MAX/width){p->oom=true;return NULL;}
   void *next=realloc(array,n*width);
   if(!next){p->oom=true;return NULL;}
   *capacity=n;return next;
}
static char *nsp_lower(const char *text)
{
   size_t n=strlen(text);if(n>(SIZE_MAX-1)/4)return NULL;
   char *out=malloc(n*4+1);if(!out)return NULL;size_t at=0,pos=0;
   while(pos<n) {
      uint32_t cp=utf8_next(text,n,&pos);
      /* Locale.US special casing for capital dotted I expands to two codepoints. */
      if(cp==0x130){out[at++]='i';at+=utf8_encode_char(0x307,out+at);}
      else at+=utf8_encode_char((uint32_t)luna_uc_lower((int32_t)cp),out+at);
   }
   out[at]=0;return out;
}
static int nsp_bool(const unsigned char *attribute,char **strings,uint32_t count)
{
   uint8_t type=attribute[15];uint32_t value=ax_u32(attribute+16);
   if(type==0x12 || type==0x10 || type==0x11)return value!=0;
   if(type==3 && value<count) {
      if(!strcmp(strings[value],"true"))return 1;
      if(!strcmp(strings[value],"false"))return 0;
   }
   if(type==1) {
      int32_t number=0;const char *text=NULL;
      int kind=arm_exec_apk_resource_value(value,&number,&text);
      if(kind=='Z' || kind=='I')return number!=0;
   }
   return -2;
}
/* Parse once, then use parent indices to resolve inheritance after base-config
 * is known. A base-config may legally follow domain-configs in the document. */
static bool nsp_parse(struct nsp_policy *p,const unsigned char *bytes,size_t length)
{
   if(length<8 || ax_u16(bytes)!=3 || ax_u16(bytes+2)!=8 || ax_u32(bytes+4)>length || ax_u32(bytes+4)<8)
      return nsp_error(p,"Invalid network security XML");
   length=ax_u32(bytes+4);uint32_t string_count=0;
   /* Validate progress and chunk boundaries before using the shared string
    * pool reader. A zero-sized unknown chunk must never keep it spinning. */
   for(size_t pos=8;pos<length;) {
      if(length-pos<8)return nsp_error(p,"Truncated network security chunk");
      uint16_t kind=ax_u16(bytes+pos),header=ax_u16(bytes+pos+2);
      uint32_t size=ax_u32(bytes+pos+4);
      if(header<8 || size<header || size>length-pos || (kind==1 && header<28))
         return nsp_error(p,"Invalid network security chunk");
      pos+=size;
   }
   char **strings=ax_string_pool(bytes,length,&string_count);
   if(!strings)return nsp_error(p,"Invalid network security string pool");
   for(uint32_t i=0;i<string_count;++i)if(!strings[i]) {
      for(uint32_t j=0;j<string_count;++j)free(strings[j]);
      free(strings);return nsp_error(p,"Invalid network security string");
   }
   struct nsp_scope { int kind,config,domain;uint32_t name; } *stack=NULL;
   size_t depth=0,stack_capacity=0;bool root=false,base=false,ok=true;
   for(size_t pos=8;ok && pos+8<=length;) {
      uint16_t type=ax_u16(bytes+pos);uint32_t size=ax_u32(bytes+pos+4);
      if(size<8 || size>length-pos){ok=nsp_error(p,"Truncated network security XML");break;}
      if(type==0x102) {
         if(size<36){ok=nsp_error(p,"Invalid network security tag");break;}
         uint32_t ni=ax_u32(bytes+pos+20);
         if(ni>=string_count){ok=nsp_error(p,"Invalid network security tag name");break;}
         const char *name=strings[ni];
         unsigned start=ax_u16(bytes+pos+24),width=ax_u16(bytes+pos+26),count=ax_u16(bytes+pos+28);
         if(width<20 || start<20 || (size_t)start+16>size || count>(size-16-start)/width) {
            ok=nsp_error(p,"Invalid network security attributes");break;
         }
         int allow=-1,subdomains=0;
         for(unsigned i=0;ok && i<count;++i) {
            const unsigned char *a=bytes+pos+16+start+(size_t)i*width;
            uint32_t ai=ax_u32(a+4);if(ai>=string_count){ok=nsp_error(p,"Invalid attribute name");break;}
            if(!strcmp(strings[ai],"cleartextTrafficPermitted"))allow=nsp_bool(a,strings,string_count);
            if(!strcmp(strings[ai],"includeSubdomains"))subdomains=nsp_bool(a,strings,string_count);
         }
         if(!ok)break;
         if(allow==-2 || subdomains==-2){ok=nsp_error(p,"Invalid policy boolean");break;}
         struct nsp_scope scope={0,0,-1,ni};
         int parent=depth ? stack[depth-1].kind : -1;
         if(!depth) {
            if(root || strcmp(name,"network-security-config")){ok=nsp_error(p,"Expected network-security-config root");break;}
            root=true;scope.kind=1;
         } else if(parent==1 && !strcmp(name,"base-config")) {
            if(base){ok=nsp_error(p,"Multiple base-config elements");break;}
            base=true;scope.kind=2;if(allow>=0)p->configs[0].cleartext=allow;
         } else if((parent==1 || parent==3) && !strcmp(name,"domain-config")) {
            void *grown=nsp_grow(p,p->configs,&p->capacity,p->count,sizeof *p->configs);
            if(!grown){ok=false;break;}p->configs=grown;
            scope.kind=3;scope.config=(int)p->count;
            p->configs[p->count++]=(struct nsp_config){parent==3 ? stack[depth-1].config : 0,allow};
         } else if(parent==3 && !strcmp(name,"domain")) {
            void *grown=nsp_grow(p,p->domains,&p->domain_capacity,p->domain_count,sizeof *p->domains);
            if(!grown){ok=false;break;}p->domains=grown;
            scope.kind=4;scope.config=stack[depth-1].config;scope.domain=(int)p->domain_count;
            p->domains[p->domain_count++]=(struct nsp_domain){NULL,scope.config,subdomains!=0};
         } else if(parent==4) {ok=nsp_error(p,"Nested tag in domain name");break;}
         void *grown=nsp_grow(p,stack,&stack_capacity,depth,sizeof *stack);
         if(!grown){ok=false;break;}stack=grown;
         stack[depth++]=scope;
      } else if(type==0x104 && depth && stack[depth-1].kind==4) {
         if(size<28){ok=nsp_error(p,"Invalid domain text");break;}
         uint32_t si=ax_u32(bytes+pos+16);if(si>=string_count){ok=nsp_error(p,"Invalid domain text index");break;}
         struct nsp_domain *d=&p->domains[stack[depth-1].domain];
         if(d->name){ok=nsp_error(p,"Multiple domain text events");break;}
         const char *first=strings[si],*last=first+strlen(first);
         while(first<last && (unsigned char)*first<=32)++first;
         while(last>first && (unsigned char)last[-1]<=32)--last;
         char *trim=luna_strndup(first,(size_t)(last-first));
         if(!trim){p->oom=true;ok=false;break;}
         d->name=nsp_lower(trim);free(trim);
         if(!d->name){p->oom=true;ok=false;break;}
         if(!d->name[0] || d->name[0]=='.'){ok=nsp_error(p,"Invalid domain name");break;}
         for(size_t i=0;i+1<p->domain_count;++i)
            if(p->domains[i].name && !strcmp(p->domains[i].name,d->name)){ok=nsp_error(p,"Duplicate domain name");break;}
      } else if(type==0x103) {
         if(!depth || size<24){ok=nsp_error(p,"Unexpected end tag");break;}
         uint32_t ni=ax_u32(bytes+pos+20);
         if(ni>=string_count || strcmp(strings[ni],strings[stack[depth-1].name])) {
            ok=nsp_error(p,"Mismatched network security end tag");break;
         }
         struct nsp_scope scope=stack[--depth];
         if(scope.kind==4 && !p->domains[scope.domain].name){ok=nsp_error(p,"Empty domain name");break;}
      }
      pos+=size;
   }
   if(ok && (!root || depth))ok=nsp_error(p,"Incomplete network security XML");
   for(uint32_t i=0;i<string_count;++i)free(strings[i]);
   free(strings);free(stack);return ok;
}
static struct nsp_policy *nsp_get(struct dvm *vm,dvm_ref self)
{
   struct dvm_object *o=dvm__obj(vm,self);if(!o)return NULL;
   struct nsp_policy *p=o->data;
   if(!p) {
      p=calloc(1,sizeof *p);
      if(!p){dvm__throw(vm,"java/lang/OutOfMemoryError","NetworkSecurityPolicy");return NULL;}
      o->data=p;o->data_destroy=nsp_destroy;
      p->configs=nsp_grow(p,p->configs,&p->capacity,0,sizeof *p->configs);
      if(!p->configs)goto invalid;
      uint32_t id=0;const char *configured=getenv("ANDROID_NETWORK_SECURITY_CONFIG");
      if(configured && *configured)id=(uint32_t)strtoul(configured,NULL,10);
      int allow=lunaria_app_target_sdk()<28;
      const char *flag=getenv("ANDROID_USES_CLEARTEXT_TRAFFIC");
      if(!id && flag && *flag)allow=!strcmp(flag,"1") || !strcmp(flag,"true");
      p->configs[p->count++]=(struct nsp_config){-1,allow};
      if(id) {
         int32_t value=0;const char *path=NULL;size_t length=0;
         unsigned char *xml=NULL;
         if(arm_exec_apk_resource_value(id,&value,&path)=='L' && path)xml=arm_exec_asset_read(path,&length);
         if(!xml)nsp_error(p,"Network security resource not found");
         else {nsp_parse(p,xml,length);free(xml);}
      }
   }
invalid:
   if(p->oom){dvm__throw(vm,"java/lang/OutOfMemoryError","Network security config");return NULL;}
   if(p->error){dvm__throw(vm,"java/lang/RuntimeException","Failed to parse network security config: %s",p->error);return NULL;}
   return p;
}
static bool nsp_allowed(const struct nsp_policy *p,int config)
{
   while(p->configs[config].cleartext<0)config=p->configs[config].parent;
   return p->configs[config].cleartext!=0;
}
static bool nsp_instance(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;(void)args;(void)nargs;static dvm_ref instance;
   RETL(runtime_singleton(vm,"Landroid/security/NetworkSecurityPolicy;",&instance));
}
static bool nsp_global(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;struct nsp_policy *p=nsp_get(vm,self);if(!p)return false;
   if(!nsp_allowed(p,0))RETI(0);
   for(size_t i=0;i<p->domain_count;++i)if(!nsp_allowed(p,p->domains[i].config))RETI(0);
   RETI(1);
}
static bool nsp_host(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;struct nsp_policy *p=nsp_get(vm,self);if(!p)return false;
   const char *text=dvm_string_utf8(vm,ARG(0).l);
   if(!text || !*text || !p->domain_count)RETI(nsp_allowed(p,0));
   if(*text=='.'){dvm__throw(vm,"java/lang/IllegalArgumentException","hostname must not begin with a .");return false;}
   char *host=nsp_lower(text);if(!host){dvm__throw(vm,"java/lang/OutOfMemoryError","hostname");return false;}
   size_t length=strlen(host);if(length && host[length-1]=='.')host[--length]=0;
   int config=0;size_t best=0;
   for(size_t i=0;i<p->domain_count;++i) {
      const struct nsp_domain *d=&p->domains[i];size_t n=strlen(d->name);
      if(!strcmp(host,d->name)){config=d->config;break;}
      if(d->subdomains && n>best && length>n && host[length-n-1]=='.' && !strcmp(host+length-n,d->name)) {config=d->config;best=n;}
   }
   free(host);RETI(nsp_allowed(p,config));
}
static const struct rt_method rt_network_policy[]={
   SM("getInstance","()Landroid/security/NetworkSecurityPolicy;",nsp_instance),
   M("isCleartextTrafficPermitted","()Z",nsp_global),
   M("isCleartextTrafficPermitted","(Ljava/lang/String;)Z",nsp_host),M_END
};

#endif /* LUNARIA_DVM_NETWORK_POLICY_IMPLEMENTATION */
