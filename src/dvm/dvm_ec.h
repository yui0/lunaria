/* MPL-2.0. EC parameters and key components backed by OpenSSL's real groups. */
static bool ec_gen_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;if(!ARG(0).l){dvm__throw(vm,"java/lang/NullPointerException","name");return false;}
   dvm_set_field(vm,self,"name","Ljava/lang/String;",ARG(0));RETV();
}
static bool ec_name(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)args;(void)nargs;return dvm_get_field(vm,self,"name","Ljava/lang/String;",out);}
static int ec_curve_nid(const char *name)
{
   if(!name)return NID_undef;
   if(!strcmp(name,"secp256r1"))return NID_X9_62_prime256v1;
   int nid=OBJ_txt2nid(name);return nid;
}
static int ec_size_nid(int bits)
{
   switch(bits){case 224:return NID_secp224r1;case 256:return NID_X9_62_prime256v1;
      case 384:return NID_secp384r1;case 521:return NID_secp521r1;default:return NID_undef;}
}
static bool ec_point_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;if(!ARG(0).l || !ARG(1).l){dvm__throw(vm,"java/lang/NullPointerException","coordinate");return false;}
   dvm_set_field(vm,self,"x","Ljava/math/BigInteger;",ARG(0));dvm_set_field(vm,self,"y","Ljava/math/BigInteger;",ARG(1));RETV();
}
static bool ec_value(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;const char *method=builtin_name(),*field=NULL,*type="Ljava/math/BigInteger;";
   if(!strcmp(method,"getAffineX"))field="x";else if(!strcmp(method,"getAffineY"))field="y";
   else if(!strcmp(method,"getP"))field="p";else if(!strcmp(method,"getA"))field="a";
   else if(!strcmp(method,"getB"))field="b";else if(!strcmp(method,"getOrder"))field="order";
   else if(!strcmp(method,"getCofactor")){field="cofactor";type="I";}
   else if(!strcmp(method,"getField")){field="field";type="Ljava/security/spec/ECField;";}
   else if(!strcmp(method,"getCurve")){field="curve";type="Ljava/security/spec/EllipticCurve;";}
   else if(!strcmp(method,"getGenerator")){field="generator";type="Ljava/security/spec/ECPoint;";}
   else if(!strcmp(method,"getSeed")){
      union dvm_value seed={0};dvm_get_field(vm,self,"seed","[B",&seed);
      if(!seed.l)RETL(0);RETL(make_bytes(vm,dvm_array_data(vm,seed.l),dvm_array_length(vm,seed.l)));
   }
   return field && dvm_get_field(vm,self,field,type,out);
}
static bool ec_field_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;if(!ARG(0).l){dvm__throw(vm,"java/lang/NullPointerException","p");return false;}
   BIGNUM *p=bigint_bn(vm,ARG(0).l);bool valid=p && !BN_is_negative(p) && !BN_is_zero(p);BN_free(p);
   if(!valid){dvm__throw(vm,"java/lang/IllegalArgumentException","p must be positive");return false;}
   dvm_set_field(vm,self,"p","Ljava/math/BigInteger;",ARG(0));RETV();
}
static bool ec_field_size(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;union dvm_value p={0};dvm_get_field(vm,self,"p","Ljava/math/BigInteger;",&p);
   BIGNUM *bn=bigint_bn(vm,p.l);if(!bn)return false;int bits=BN_num_bits(bn);BN_free(bn);RETI(bits);
}
static bool ec_curve_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   if(!ARG(0).l || !ARG(1).l || !ARG(2).l){dvm__throw(vm,"java/lang/NullPointerException","curve");return false;}
   if(object_is_a(vm,ARG(0).l,"Ljava/security/spec/ECFieldFp;")){
      union dvm_value p={0};dvm_get_field(vm,ARG(0).l,"p","Ljava/math/BigInteger;",&p);
      BIGNUM *prime=bigint_bn(vm,p.l),*a=bigint_bn(vm,ARG(1).l),*b=bigint_bn(vm,ARG(2).l);
      bool valid=prime && a && b && !BN_is_negative(a) && !BN_is_negative(b) && BN_cmp(a,prime)<0 && BN_cmp(b,prime)<0;
      BN_free(prime);BN_free(a);BN_free(b);
      if(!valid){dvm__throw(vm,"java/lang/IllegalArgumentException","curve coefficient out of range");return false;}
   }
   dvm_set_field(vm,self,"field","Ljava/security/spec/ECField;",ARG(0));
   dvm_set_field(vm,self,"a","Ljava/math/BigInteger;",ARG(1));dvm_set_field(vm,self,"b","Ljava/math/BigInteger;",ARG(2));
   union dvm_value seed={0};if(nargs>3 && ARG(3).l)seed.l=make_bytes(vm,dvm_array_data(vm,ARG(3).l),dvm_array_length(vm,ARG(3).l));
   dvm_set_field(vm,self,"seed","[B",seed);RETV();
}
static bool ec_params_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;if(!ARG(0).l || !ARG(1).l || !ARG(2).l){dvm__throw(vm,"java/lang/NullPointerException","EC parameters");return false;}
   BIGNUM *order=bigint_bn(vm,ARG(2).l);bool valid=order && !BN_is_negative(order) && !BN_is_zero(order) && ARG(3).i>0;BN_free(order);
   if(!valid){dvm__throw(vm,"java/lang/IllegalArgumentException","order and cofactor must be positive");return false;}
   dvm_set_field(vm,self,"curve","Ljava/security/spec/EllipticCurve;",ARG(0));dvm_set_field(vm,self,"generator","Ljava/security/spec/ECPoint;",ARG(1));
   dvm_set_field(vm,self,"order","Ljava/math/BigInteger;",ARG(2));dvm_set_field(vm,self,"cofactor","I",ARG(3));RETV();
}
static BIGNUM *ec_bn_field(struct dvm *vm,dvm_ref object,const char *field)
{union dvm_value value={0};dvm_get_field(vm,object,field,"Ljava/math/BigInteger;",&value);return value.l ? bigint_bn(vm,value.l) : NULL;}
static EC_GROUP *ec_group_from_spec(struct dvm *vm,dvm_ref spec)
{
   if(object_is_a(vm,spec,"Ljava/security/spec/ECGenParameterSpec;")){
      union dvm_value name={0};dvm_get_field(vm,spec,"name","Ljava/lang/String;",&name);
      int nid=ec_curve_nid(dvm_string_utf8(vm,name.l));return nid==NID_undef ? NULL : EC_GROUP_new_by_curve_name(nid);
   }
   if(!object_is_a(vm,spec,"Ljava/security/spec/ECParameterSpec;"))return NULL;
   union dvm_value curve={0},field={0},generator={0},cofactor={0};
   dvm_get_field(vm,spec,"curve","Ljava/security/spec/EllipticCurve;",&curve);
   dvm_get_field(vm,curve.l,"field","Ljava/security/spec/ECField;",&field);
   if(!object_is_a(vm,field.l,"Ljava/security/spec/ECFieldFp;"))return NULL;
   dvm_get_field(vm,spec,"generator","Ljava/security/spec/ECPoint;",&generator);dvm_get_field(vm,spec,"cofactor","I",&cofactor);
   BIGNUM *p=ec_bn_field(vm,field.l,"p"),*a=ec_bn_field(vm,curve.l,"a"),*b=ec_bn_field(vm,curve.l,"b"),
      *x=ec_bn_field(vm,generator.l,"x"),*y=ec_bn_field(vm,generator.l,"y"),*order=ec_bn_field(vm,spec,"order"),*h=BN_new();
   EC_GROUP *group=p && a && b ? EC_GROUP_new_curve_GFp(p,a,b,NULL) : NULL;EC_POINT *point=group ? EC_POINT_new(group) : NULL;
   bool valid=point && x && y && order && h && BN_set_word(h,(unsigned)cofactor.i) &&
      EC_POINT_set_affine_coordinates(group,point,x,y,NULL) && EC_GROUP_set_generator(group,point,order,h) && EC_GROUP_check(group,NULL);
   EC_POINT_free(point);BN_free(p);BN_free(a);BN_free(b);BN_free(x);BN_free(y);BN_free(order);BN_free(h);
   if(!valid){EC_GROUP_free(group);return NULL;}return group;
}
static bool ec_store_group(struct dvm *vm,dvm_ref generator,EC_GROUP *group)
{
   unsigned char *der=NULL;int n=i2d_ECPKParameters(group,&der);
   dvm_ref bytes=n>0 ? make_bytes(vm,der,(uint32_t)n) : 0;OPENSSL_free(der);
   if(!bytes)return false;
   return dvm_set_field(vm,generator,"parameters","[B",(union dvm_value){.l=bytes});
}
static bool ec_key_number(struct dvm *vm,dvm_ref self,const char *name,union dvm_value *out)
{
   uint32_t n=0;const unsigned char *bytes=key_bytes(vm,self,&n),*cursor=bytes;union dvm_value kind={0};dvm_get_field(vm,self,"kind","I",&kind);
   EVP_PKEY *key=!bytes ? NULL : kind.i==2 ? d2i_PUBKEY(NULL,&cursor,n) : d2i_AutoPrivateKey(NULL,&cursor,n);
   BIGNUM *bn=NULL;bool good=key && EVP_PKEY_is_a(key,"EC") && EVP_PKEY_get_bn_param(key,name,&bn)>0;
   if(good)good=bigint_bn_result(vm,bn,out);BN_clear_free(bn);EVP_PKEY_free(key);
   if(!good && !dvm_exception(vm))dvm__throw(vm,"java/security/ProviderException","Cannot read EC key component");return good;
}
static bool ec_public_point(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;dvm_ref point=dvm_new_object(vm,dvm_find_class(vm,"java/security/spec/ECPoint"));if(!point)return false;
   dvm_pin(vm,point);union dvm_value value={0};bool good=ec_key_number(vm,self,OSSL_PKEY_PARAM_EC_PUB_X,&value);
   if(good)dvm_set_field(vm,point,"x","Ljava/math/BigInteger;",value);
   if(good)good=ec_key_number(vm,self,OSSL_PKEY_PARAM_EC_PUB_Y,&value);
   if(good)dvm_set_field(vm,point,"y","Ljava/math/BigInteger;",value);dvm_unpin(vm,point);
   if(!good)return false;RETL(point);
}
static bool ec_private_scalar(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{(void)args;(void)nargs;return ec_key_number(vm,self,OSSL_PKEY_PARAM_PRIV_KEY,out);}
static bool ec_group_bn(struct dvm *vm,dvm_ref object,const char *name,const BIGNUM *bn)
{union dvm_value v={0};return bigint_bn_result(vm,bn,&v) && dvm_set_field(vm,object,name,"Ljava/math/BigInteger;",v);}
static bool ec_key_params(struct dvm *vm,dvm_ref self,const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;uint32_t n=0;const unsigned char *bytes=key_bytes(vm,self,&n),*cursor=bytes;
   union dvm_value kind={0};dvm_get_field(vm,self,"kind","I",&kind);
   EVP_PKEY *key=!bytes ? NULL : kind.i==2 ? d2i_PUBKEY(NULL,&cursor,n) : d2i_AutoPrivateKey(NULL,&cursor,n);
   unsigned char *der=NULL;int size=key ? i2d_KeyParams(key,&der) : 0;cursor=der;
   EC_GROUP *group=size>0 ? d2i_ECPKParameters(NULL,&cursor,size) : NULL;OPENSSL_free(der);EVP_PKEY_free(key);
   if(!group){dvm__throw(vm,"java/security/ProviderException","Cannot read EC parameters");return false;}
   BIGNUM *p=BN_new(),*a=BN_new(),*b=BN_new(),*order=BN_new(),*h=BN_new(),*x=BN_new(),*y=BN_new();
   bool good=p && a && b && order && h && x && y && EC_GROUP_get_curve(group,p,a,b,NULL) &&
      EC_GROUP_get_order(group,order,NULL) && EC_GROUP_get_cofactor(group,h,NULL) &&
      EC_POINT_get_affine_coordinates(group,EC_GROUP_get0_generator(group),x,y,NULL);
   dvm_ref result=good ? dvm_new_object(vm,dvm_find_class(vm,"java/security/spec/ECParameterSpec")) : 0;
   dvm_pin(vm,result);
   dvm_ref curve=result ? dvm_new_object(vm,dvm_find_class(vm,"java/security/spec/EllipticCurve")) : 0;
   if(curve)dvm_set_field(vm,result,"curve","Ljava/security/spec/EllipticCurve;",(union dvm_value){.l=curve});
   dvm_ref field=curve ? dvm_new_object(vm,dvm_find_class(vm,"java/security/spec/ECFieldFp")) : 0;
   if(field)dvm_set_field(vm,curve,"field","Ljava/security/spec/ECField;",(union dvm_value){.l=field});
   good=good && field && ec_group_bn(vm,field,"p",p) && ec_group_bn(vm,curve,"a",a) && ec_group_bn(vm,curve,"b",b) && ec_group_bn(vm,result,"order",order);
   dvm_ref point=good ? dvm_new_object(vm,dvm_find_class(vm,"java/security/spec/ECPoint")) : 0;
   if(point)dvm_set_field(vm,result,"generator","Ljava/security/spec/ECPoint;",(union dvm_value){.l=point});
   good=good && point && ec_group_bn(vm,point,"x",x) && ec_group_bn(vm,point,"y",y);
   if(good)dvm_set_field(vm,result,"cofactor","I",(union dvm_value){.i=(int32_t)BN_get_word(h)});
   const unsigned char *seed=EC_GROUP_get0_seed(group);size_t seed_size=EC_GROUP_get_seed_len(group);
   if(good && seed_size)dvm_set_field(vm,curve,"seed","[B",(union dvm_value){.l=make_bytes(vm,seed,(uint32_t)seed_size)});
   dvm_unpin(vm,result);BN_free(p);BN_free(a);BN_free(b);BN_free(order);BN_free(h);BN_free(x);BN_free(y);EC_GROUP_free(group);
   if(!good)return false;RETL(result);
}
static const struct rt_field rt_ec_gen_fields[]={{"name","Ljava/lang/String;"},F_END};
static const struct rt_method rt_ec_gen[]={M("<init>","(Ljava/lang/String;)V",ec_gen_init),M("getName","()Ljava/lang/String;",ec_name),M_END};
static const struct rt_field rt_ec_point_fields[]={{"x","Ljava/math/BigInteger;"},{"y","Ljava/math/BigInteger;"},F_END};
static const struct rt_method rt_ec_point[]={M("<init>","(Ljava/math/BigInteger;Ljava/math/BigInteger;)V",ec_point_init),M("getAffineX","()Ljava/math/BigInteger;",ec_value),M("getAffineY","()Ljava/math/BigInteger;",ec_value),M_END};
static const struct rt_field rt_ec_field_fields[]={{"p","Ljava/math/BigInteger;"},F_END};
static const struct rt_method rt_ec_field[]={M("<init>","(Ljava/math/BigInteger;)V",ec_field_init),M("getP","()Ljava/math/BigInteger;",ec_value),M("getFieldSize","()I",ec_field_size),M_END};
static const struct rt_field rt_ec_curve_fields[]={{"field","Ljava/security/spec/ECField;"},{"a","Ljava/math/BigInteger;"},{"b","Ljava/math/BigInteger;"},{"seed","[B"},F_END};
static const struct rt_method rt_ec_curve[]={M("<init>","(Ljava/security/spec/ECField;Ljava/math/BigInteger;Ljava/math/BigInteger;)V",ec_curve_init),M("<init>","(Ljava/security/spec/ECField;Ljava/math/BigInteger;Ljava/math/BigInteger;[B)V",ec_curve_init),M("getField","()Ljava/security/spec/ECField;",ec_value),M("getA","()Ljava/math/BigInteger;",ec_value),M("getB","()Ljava/math/BigInteger;",ec_value),M("getSeed","()[B",ec_value),M_END};
static const struct rt_field rt_ec_params_fields[]={{"curve","Ljava/security/spec/EllipticCurve;"},{"generator","Ljava/security/spec/ECPoint;"},{"order","Ljava/math/BigInteger;"},{"cofactor","I"},F_END};
static const struct rt_method rt_ec_params[]={M("<init>","(Ljava/security/spec/EllipticCurve;Ljava/security/spec/ECPoint;Ljava/math/BigInteger;I)V",ec_params_init),M("getCurve","()Ljava/security/spec/EllipticCurve;",ec_value),M("getGenerator","()Ljava/security/spec/ECPoint;",ec_value),M("getOrder","()Ljava/math/BigInteger;",ec_value),M("getCofactor","()I",ec_value),M_END};
static const struct rt_method rt_ec_public_key[]={M("getW","()Ljava/security/spec/ECPoint;",ec_public_point),M("getParams","()Ljava/security/spec/ECParameterSpec;",ec_key_params),M_END};
static const struct rt_method rt_ec_private_key[]={M("getS","()Ljava/math/BigInteger;",ec_private_scalar),M("getParams","()Ljava/security/spec/ECParameterSpec;",ec_key_params),M_END};
static const char *const rt_ec_public_ifaces[]={"Ljava/security/interfaces/ECPublicKey;","Ljava/security/interfaces/ECKey;",NULL};
static const char *const rt_ec_private_ifaces[]={"Ljava/security/interfaces/ECPrivateKey;","Ljava/security/interfaces/ECKey;",NULL};
static const char *const rt_ec_field_ifaces[]={"Ljava/security/spec/ECField;",NULL};
static const char *const rt_ec_param_ifaces[]={"Ljava/security/spec/AlgorithmParameterSpec;",NULL};
