/* Copyright © 2026 Yuichiro Nakada / Project Lunaria
 * SPDX-License-Identifier: MPL-2.0 */
#include "luna_certs.h"
#include "lunaria_file.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

FILE *luna_certs_open_system_bundle(void)
{
  const char *configured=getenv(X509_get_default_cert_file_env());
  const char *files[]={configured && *configured?configured:X509_get_default_cert_file(),
    "/etc/ssl/certs/ca-certificates.crt","/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
    "/etc/ssl/cert.pem",NULL};
  for(unsigned i=0;files[i];++i) {
    FILE *file=luna_file_fopen(files[i],"rb");
    if(file)return file;
  }
  return NULL;
}
static int write_hash(X509 *cert,unsigned long hash,const char *directory)
{
  for(unsigned suffix=0;;++suffix) {
    char path[4096];
    int n=snprintf(path,sizeof path,"%s/%08lx.%u",directory,hash&0xffffffffUL,suffix);
    if(n<0 || (size_t)n>=sizeof path) {errno=ENAMETOOLONG;return -1;}
    FILE *file=luna_file_fopen(path,"wbx");
    if(!file) {
      if(errno!=EEXIST)return -1;
      if(suffix==UINT_MAX) {errno=EOVERFLOW;return -1;}
      continue;
    }
    int good=PEM_write_X509(file,cert);
    if(fclose(file))good=0;
    if(!good) {luna_file_unlink(path);errno=EIO;return -1;}
    return 0;
  }
}
int luna_certs_make_hash_cache(FILE *source,const char *directory)
{
  if(!source || !directory || !*directory) {errno=EINVAL;return -1;}
  int count=0;
  X509 *cert;
  while((cert=PEM_read_X509(source,NULL,NULL,NULL))!=NULL) {
    unsigned long old=X509_subject_name_hash_old(cert),modern=X509_subject_name_hash(cert);
    int error=write_hash(cert,old,directory);
    if(!error && modern!=old)error=write_hash(cert,modern,directory);
    X509_free(cert);
    if(error)return -1;
    ++count;
  }
  /* PEM's end-of-input marker is not a pending error for the next host user. */
  ERR_clear_error();
  if(ferror(source)) {errno=EIO;return -1;}
  return count;
}
