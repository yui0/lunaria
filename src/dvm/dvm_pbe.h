/* Password based keys use the encoding and derivation selected by the
 * algorithm, never a replacement AES key. PKCS#12's diversifiers produce
 * independent key and IV streams (RFC 7292, appendix B). */
#include <openssl/pkcs12.h>

struct pbe_algorithm { const EVP_MD *md; int bits; const char *canonical; };
static bool pbe_algorithm(const char *name,struct pbe_algorithm *out)
{
   if(!name)return false;
   const char *hashes[]={"SHA","SHA1","SHA-1","SHA256","SHA-256"};
   const int sizes[]={128,192,256};
   for(unsigned h=0;h<5;++h)for(unsigned b=0;b<3;++b){
      char candidate[96];snprintf(candidate,sizeof candidate,"PBEWITH%sAND%dBITAES-CBC-BC",hashes[h],sizes[b]);
      if(strcasecmp(name,candidate))continue;
      static const char *names[]={"PBEWithSHA1And128BitAES-CBC-BC","PBEWithSHA1And192BitAES-CBC-BC","PBEWithSHA1And256BitAES-CBC-BC",
         "PBEWithSHA256And128BitAES-CBC-BC","PBEWithSHA256And192BitAES-CBC-BC","PBEWithSHA256And256BitAES-CBC-BC"};
      if(out)*out=(struct pbe_algorithm){h<3 ? EVP_sha1() : EVP_sha256(),sizes[b],names[(h<3 ? 0 : 3)+b]};
      return true;
   }
   return false;
}
static const EVP_MD *pbe_pbkdf2_digest(const char *name)
{
   if(!name)return NULL;
   if(!strcasecmp(name,"PBKDF2WithHmacSHA1"))return EVP_sha1();
   if(!strcasecmp(name,"PBKDF2WithHmacSHA224"))return EVP_sha224();
   if(!strcasecmp(name,"PBKDF2WithHmacSHA256"))return EVP_sha256();
   if(!strcasecmp(name,"PBKDF2WithHmacSHA384"))return EVP_sha384();
   if(!strcasecmp(name,"PBKDF2WithHmacSHA512"))return EVP_sha512();
   return NULL;
}
static dvm_ref pbe_array_copy(struct dvm *vm,dvm_ref array,char type)
{
   if(!array)return 0;
   uint32_t n=dvm_array_length(vm,array);
   dvm_ref copy=dvm_new_array(vm,type,NULL,n);
   if(copy && n)memcpy(dvm_array_data(vm,copy),dvm_array_data(vm,array),(size_t)n*(type=='C' ? 2 : 1));
   return copy;
}
static bool pbe_object_is(struct dvm *vm,dvm_ref object,const char *name)
{
   struct dvm_class *cls=object ? dvm_object_class(vm,object) : NULL;
   return cls && dvm__class_assignable(vm,cls,dvm_find_class(vm,name));
}
static bool pbe_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   if(nargs>1){
      if(!ARG(1).l){dvm__throw(vm,"java/lang/NullPointerException","salt");return false;}
      if(!dvm_array_length(vm,ARG(1).l) || ARG(2).i<=0 || (nargs>3 && ARG(3).i<=0)){
         dvm__throw(vm,"java/lang/IllegalArgumentException","Empty salt or nonpositive iteration count/key length");return false;
      }
   }
   union dvm_value v={.l=ARG(0).l ? pbe_array_copy(vm,ARG(0).l,'C') : dvm_new_array(vm,'C',NULL,0)};
   if(!v.l)return false;dvm_set_field(vm,self,"password","[C",v);
   v.l=nargs>1 ? pbe_array_copy(vm,ARG(1).l,'B') : 0;if(nargs>1 && !v.l)return false;dvm_set_field(vm,self,"salt","[B",v);
   rt_set_int(vm,self,"iterations",nargs>2 ? ARG(2).i : 0);
   rt_set_int(vm,self,"keyLength",nargs>3 ? ARG(3).i : 0);RETV();
}
static bool pbe_value(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;const char *name=builtin_name();union dvm_value value={0};
   if(!strcmp(name,"getIterationCount"))RETI(rt_get_int(vm,self,"iterations"));
   if(!strcmp(name,"getKeyLength"))RETI(rt_get_int(vm,self,"keyLength"));
   if(!strcmp(name,"getEncoded")){dvm_get_field(vm,self,"encoded","[B",&value);RETL(pbe_array_copy(vm,value.l,'B'));}
   bool password=!strcmp(name,"getPassword");
   dvm_get_field(vm,self,password ? "password" : "salt",password ? "[C" : "[B",&value);
   if(password && !value.l){dvm__throw(vm,"java/lang/IllegalStateException","Password has been cleared");return false;}
   RETL(pbe_array_copy(vm,value.l,password ? 'C' : 'B'));
}
static bool pbe_clear(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;union dvm_value pw={0};dvm_get_field(vm,self,"password","[C",&pw);
   if(pw.l){OPENSSL_cleanse(dvm_array_data(vm,pw.l),(size_t)dvm_array_length(vm,pw.l)*2);pw.l=0;dvm_set_field(vm,self,"password","[C",pw);}RETV();
}
static bool pbe_parameter_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   if(!ARG(0).l){dvm__throw(vm,"java/lang/NullPointerException","salt");return false;}
   union dvm_value v={.l=pbe_array_copy(vm,ARG(0).l,'B')};if(!v.l)return false;
   dvm_set_field(vm,self,"salt","[B",v);rt_set_int(vm,self,"iterations",ARG(1).i);
   v.l=nargs>2 ? ARG(2).l : 0;dvm_set_field(vm,self,"parameterSpec","Ljava/security/spec/AlgorithmParameterSpec;",v);RETV();
}
static bool pbe_parameter_spec(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;union dvm_value v={0};dvm_get_field(vm,self,"parameterSpec","Ljava/security/spec/AlgorithmParameterSpec;",&v);RETL(v.l);
}
static dvm_ref pbe_password_encoded(struct dvm *vm,dvm_ref password)
{
   uint32_t n=dvm_array_length(vm,password);
   if(n>(INT_MAX/2)-1){dvm__throw(vm,"java/lang/OutOfMemoryError","PBE password is too long");return 0;}
   dvm_ref result=dvm_new_array(vm,'B',NULL,n ? (n+1)*2 : 0);
   if(!result)return 0;
   const uint16_t *chars=dvm_array_data(vm,password);uint8_t *bytes=dvm_array_data(vm,result);
   for(uint32_t i=0;i<n;++i){bytes[2*i]=(uint8_t)(chars[i]>>8);bytes[2*i+1]=(uint8_t)chars[i];}
   return result;
}
static bool pbe_derive_encoded(struct dvm *vm,const struct pbe_algorithm *algorithm,dvm_ref encoded,dvm_ref salt,int iterations,dvm_ref *key,dvm_ref *iv)
{
   uint32_t sn=dvm_array_length(vm,salt),pn=dvm_array_length(vm,encoded);
   uint8_t kb[32],vb[16];
   /* The PKCS12 generator hashes once even when an externally supplied
    * PBEParameterSpec has a nonpositive iteration count, as BC does. */
   if(iterations<1)iterations=1;
   bool good=encoded && sn<=INT_MAX && pn<=INT_MAX &&
      PKCS12_key_gen_uni(dvm_array_data(vm,encoded),(int)pn,dvm_array_data(vm,salt),(int)sn,1,iterations,algorithm->bits/8,kb,algorithm->md)==1 &&
      PKCS12_key_gen_uni(dvm_array_data(vm,encoded),(int)pn,dvm_array_data(vm,salt),(int)sn,2,iterations,16,vb,algorithm->md)==1;
   if(good){*key=make_bytes(vm,kb,(uint32_t)algorithm->bits/8);*iv=make_bytes(vm,vb,16);good=*key && *iv;}
   OPENSSL_cleanse(kb,sizeof kb);OPENSSL_cleanse(vb,sizeof vb);
   if(!good && !dvm_exception(vm))dvm__throw(vm,"java/security/InvalidAlgorithmParameterException","PKCS12 derivation failed");
   return good;
}
static bool pbe_derive(struct dvm *vm,const struct pbe_algorithm *algorithm,dvm_ref password,dvm_ref salt,int iterations,dvm_ref *key,dvm_ref *iv)
{
   dvm_ref encoded=pbe_password_encoded(vm,password);if(!encoded)return false;
   bool good=pbe_derive_encoded(vm,algorithm,encoded,salt,iterations,key,iv);
   OPENSSL_cleanse(dvm_array_data(vm,encoded),dvm_array_length(vm,encoded));return good;
}
static bool skf_getInstance(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)self;
   if(!ARG(0).l){dvm__throw(vm,"java/lang/NullPointerException","algorithm");return false;}
   if(nargs>1){
      const char *provider=ARG(1).l ? dvm_string_utf8(vm,ARG(1).l) : NULL;
      if(!provider || !*provider){dvm__throw(vm,"java/lang/IllegalArgumentException","Missing provider");return false;}
      if(strcmp(provider,"BC") && strcmp(provider,"AndroidOpenSSL")){dvm__throw(vm,"java/security/NoSuchProviderException","%s",provider);return false;}
   }
   const char *name=dvm_string_utf8(vm,ARG(0).l);
   if(!pbe_algorithm(name,NULL) && !pbe_pbkdf2_digest(name)){
      dvm__throw(vm,"java/security/NoSuchAlgorithmException","%s",name);return false;
   }
   dvm_ref factory=dvm_new_object(vm,dvm_find_class(vm,"javax/crypto/SecretKeyFactory"));
   if(factory)dvm_set_field(vm,factory,"algorithm","Ljava/lang/String;",ARG(0));RETL(factory);
}
static bool skf_generateSecret(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;union dvm_value av={0},pw={0},salt={0};
   dvm_get_field(vm,self,"algorithm","Ljava/lang/String;",&av);const char *name=dvm_string_utf8(vm,av.l);
   dvm_ref spec=ARG(0).l;
   if(!pbe_object_is(vm,spec,"javax/crypto/spec/PBEKeySpec")){
      dvm__throw(vm,"java/security/spec/InvalidKeySpecException","Expected PBEKeySpec");return false;
   }
   dvm_get_field(vm,spec,"password","[C",&pw);dvm_get_field(vm,spec,"salt","[B",&salt);
   if(!pw.l){dvm__throw(vm,"java/lang/IllegalStateException","Password has been cleared");return false;}
   int iterations=rt_get_int(vm,spec,"iterations"),bits=rt_get_int(vm,spec,"keyLength");
   struct pbe_algorithm algorithm;
   if(pbe_algorithm(name,&algorithm)){
      dvm_ref encoded=0,iv=0;
      if(salt.l){if(!pbe_derive(vm,&algorithm,pw.l,salt.l,iterations,&encoded,&iv))return false;}
      else encoded=pbe_password_encoded(vm,pw.l);
      if(!encoded)return false;
      dvm_ref key=key_make(vm,"Lcom/android/org/bouncycastle/jcajce/provider/symmetric/util/BCPBEKey;",encoded,algorithm.canonical,0);
      if(!key)return false;
      union dvm_value v={.l=pbe_array_copy(vm,pw.l,'C')};dvm_set_field(vm,key,"password","[C",v);
      v.l=pbe_array_copy(vm,salt.l,'B');dvm_set_field(vm,key,"salt","[B",v);
      rt_set_int(vm,key,"iterations",iterations);rt_set_int(vm,key,"derived",salt.l!=0);
      v.l=iv;dvm_set_field(vm,key,"iv","[B",v);RETL(key);
   }
   const EVP_MD *md=pbe_pbkdf2_digest(name);
   if(!md || !salt.l || iterations<=0 || bits<8){dvm__throw(vm,"java/security/spec/InvalidKeySpecException","Invalid PBKDF2 parameters");return false;}
   uint32_t n=dvm_array_length(vm,pw.l);const uint16_t *chars=dvm_array_data(vm,pw.l);
   if(n>(INT_MAX-1)/3){dvm__throw(vm,"java/lang/OutOfMemoryError","PBKDF2 password is too long");return false;}
   uint8_t *password=malloc((size_t)n*3+1);if(!password){dvm__throw(vm,"java/lang/OutOfMemoryError","PBKDF2 password");return false;}
   size_t length=0;
   for(uint32_t i=0;i<n;++i){
      uint32_t cp=chars[i];
      if(cp>=0xd800 && cp<=0xdbff && i+1<n && chars[i+1]>=0xdc00 && chars[i+1]<=0xdfff){cp=0x10000+((cp-0xd800)<<10)+(chars[++i]-0xdc00);}
      else if(cp>=0xd800 && cp<=0xdfff)cp='?';
      if(cp<0x80)password[length++]=(uint8_t)cp;
      else if(cp<0x800){password[length++]=0xc0|(cp>>6);password[length++]=0x80|(cp&63);}
      else if(cp<0x10000){password[length++]=0xe0|(cp>>12);password[length++]=0x80|((cp>>6)&63);password[length++]=0x80|(cp&63);}
      else{password[length++]=0xf0|(cp>>18);password[length++]=0x80|((cp>>12)&63);password[length++]=0x80|((cp>>6)&63);password[length++]=0x80|(cp&63);}
   }
   dvm_ref encoded=dvm_new_array(vm,'B',NULL,(uint32_t)bits/8);uint32_t sn=dvm_array_length(vm,salt.l);
   bool good=encoded && sn<=INT_MAX && PKCS5_PBKDF2_HMAC((char *)password,(int)length,dvm_array_data(vm,salt.l),(int)sn,iterations,md,bits/8,dvm_array_data(vm,encoded))==1;
   OPENSSL_cleanse(password,length);free(password);
   if(!good){if(!dvm_exception(vm))dvm__throw(vm,"java/security/spec/InvalidKeySpecException","PBKDF2 derivation failed");return false;}
   RETL(key_make(vm,"Ljavax/crypto/spec/SecretKeySpec;",encoded,name,0));
}
/* Prepare a separate operational key; initializing a Cipher never changes the
 * password-only key supplied by its caller. */
static bool pbe_cipher_key(struct dvm *vm,const char *transform,dvm_ref original,dvm_ref params,dvm_ref *key,dvm_ref *iv)
{
   union dvm_value av={0},encoded={0},stored_iv={0},password={0};
   dvm_get_field(vm,original,"algorithm","Ljava/lang/String;",&av);struct pbe_algorithm algorithm;
   bool bc_key=pbe_object_is(vm,original,"com/android/org/bouncycastle/jcajce/provider/symmetric/util/BCPBEKey");
   if(!bc_key){
      if(!pbe_object_is(vm,original,"javax/crypto/SecretKey")){dvm__throw(vm,"java/security/InvalidKeyException","PBE requires a SecretKey");return false;}
      if(!pbe_algorithm(transform,&algorithm) || !pbe_object_is(vm,params,"javax/crypto/spec/PBEParameterSpec")){
         dvm__throw(vm,"java/security/InvalidKeyException","Cipher requires a PBE key or PBE parameters");return false;
      }
      struct dvm_method *get_encoded=dvm_find_method(vm,dvm_object_class(vm,original),"getEncoded","()[B");
      if(!get_encoded || !dvm_call(vm,get_encoded,original,NULL,0,&encoded))return false;
      union dvm_value salt={0};dvm_get_field(vm,params,"salt","[B",&salt);
      if(!encoded.l || !pbe_derive_encoded(vm,&algorithm,encoded.l,salt.l,rt_get_int(vm,params,"iterations"),&encoded.l,iv))return false;
      *key=key_make(vm,"Ljavax/crypto/spec/SecretKeySpec;",encoded.l,"AES",0);return *key!=0;
   }
   if(!av.l || !pbe_algorithm(dvm_string_utf8(vm,av.l),&algorithm)){
      dvm__throw(vm,"java/security/InvalidKeyException","Unsupported PBE key");return false;
   }
   dvm_get_field(vm,original,"password","[C",&password);
   if(!password.l){dvm__throw(vm,"java/security/InvalidKeyException","Missing PBE password");return false;}
   if(rt_get_int(vm,original,"derived")){
      dvm_get_field(vm,original,"encoded","[B",&encoded);dvm_get_field(vm,original,"iv","[B",&stored_iv);
      *iv=stored_iv.l;
      if(params && !pbe_object_is(vm,params,"javax/crypto/spec/PBEParameterSpec")){
         union dvm_value override={0};dvm_get_field(vm,params,"iv","[B",&override);
         if(!override.l){dvm__throw(vm,"java/security/InvalidAlgorithmParameterException","Unsupported PBE parameters");return false;}
         *iv=override.l;
      }
   }else{
      if(!pbe_object_is(vm,params,"javax/crypto/spec/PBEParameterSpec")){
         dvm__throw(vm,"java/security/InvalidAlgorithmParameterException","PBE requires salt and iteration count");return false;
      }
      union dvm_value salt={0};dvm_get_field(vm,params,"salt","[B",&salt);
      if(!pbe_derive(vm,&algorithm,password.l,salt.l,rt_get_int(vm,params,"iterations"),&encoded.l,iv))return false;
   }
   *key=key_make(vm,"Ljavax/crypto/spec/SecretKeySpec;",encoded.l,"AES",0);return *key!=0;
}
static const struct rt_field rt_pbe_key_spec_fields[]={
   {"password","[C"},{"salt","[B"},{"iterations","I"},{"keyLength","I"},F_END
};
static const struct rt_method rt_pbe_key_spec[]={
   M("<init>","([C)V",pbe_init),M("<init>","([C[BI)V",pbe_init),M("<init>","([C[BII)V",pbe_init),
   M("getPassword","()[C",pbe_value),M("getSalt","()[B",pbe_value),M("getIterationCount","()I",pbe_value),
   M("getKeyLength","()I",pbe_value),M("clearPassword","()V",pbe_clear),M_END
};
static const struct rt_field rt_pbe_parameter_fields[]={
   {"salt","[B"},{"iterations","I"},{"parameterSpec","Ljava/security/spec/AlgorithmParameterSpec;"},F_END
};
static const struct rt_method rt_pbe_parameter[]={
   M("<init>","([BI)V",pbe_parameter_init),M("<init>","([BILjava/security/spec/AlgorithmParameterSpec;)V",pbe_parameter_init),
   M("getSalt","()[B",pbe_value),M("getIterationCount","()I",pbe_value),
   M("getParameterSpec","()Ljava/security/spec/AlgorithmParameterSpec;",pbe_parameter_spec),M_END
};
static const struct rt_field rt_pbe_key_fields[]={
   {"password","[C"},{"salt","[B"},{"iterations","I"},{"iv","[B"},{"derived","I"},F_END
};
static const char *const rt_pbe_key_ifaces[]={"Ljavax/crypto/interfaces/PBEKey;",NULL};
static const struct rt_method rt_pbe_key[]={
   M("getEncoded","()[B",pbe_value),
   M("getPassword","()[C",pbe_value),M("getSalt","()[B",pbe_value),M("getIterationCount","()I",pbe_value),M_END
};
static const struct rt_method rt_secret_key_factory[]={
   SM("getInstance","(Ljava/lang/String;)Ljavax/crypto/SecretKeyFactory;",skf_getInstance),
   SM("getInstance","(Ljava/lang/String;Ljava/lang/String;)Ljavax/crypto/SecretKeyFactory;",skf_getInstance),
   M("getAlgorithm","()Ljava/lang/String;",key_getAlgorithm),
   M("generateSecret","(Ljava/security/spec/KeySpec;)Ljavax/crypto/SecretKey;",skf_generateSecret),M_END
};

static const char *const rt_secret_key_ifaces[]={"Ljavax/crypto/SecretKey;",NULL};
