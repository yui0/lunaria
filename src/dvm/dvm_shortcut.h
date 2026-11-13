/* ShortcutInfo values and builder. Included by dvm_runtime.c. */
static const struct rt_field rt_shortcut_fields[] = {
  {"id","Ljava/lang/String;"}, {"package","Ljava/lang/String;"},
  {"shortLabel","Ljava/lang/CharSequence;"}, {"longLabel","Ljava/lang/CharSequence;"},
  {"disabledMessage","Ljava/lang/CharSequence;"}, {"icon","Landroid/graphics/drawable/Icon;"},
  {"activity","Landroid/content/ComponentName;"}, {"intents","[Landroid/content/Intent;"},
  {"rank","I"}, {"categories","Ljava/util/Set;"},
  {"extras","Landroid/os/PersistableBundle;"}, {"persons","[Landroid/app/Person;"},
  {"locusId","Landroid/content/LocusId;"}, {"longLived","Z"}, F_END
};

static bool shortcut_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                           int nargs,union dvm_value *out)
{
  (void)nargs;
  if (!ARG(0).l || !ARG(1).l) {
    dvm__throw(vm,"java/lang/NullPointerException","context and id are required"); return false;
  }
  const char *id=dvm_string_utf8(vm,ARG(1).l);
  if (!id || !*id) {
    dvm__throw(vm,"java/lang/IllegalArgumentException","id cannot be empty"); return false;
  }
  union dvm_value package={0};
  if (!am_vcall(vm,ARG(0).l,"getPackageName","()Ljava/lang/String;",NULL,0,&package)) return false;
  dvm_set_field(vm,self,"id","Ljava/lang/String;",ARG(1));
  dvm_set_field(vm,self,"package","Ljava/lang/String;",package);
  RETV();
}

/* Neither callers' Intent objects nor returned arrays may mutate the value. */
static dvm_ref shortcut_intents_copy(struct dvm *vm,dvm_ref array,bool validate)
{
  if (!array) return 0;
  struct dvm_object *source=dvm__obj(vm,array);
  if (!source || source->kind!=DVM_OBJ_ARRAY) return 0;
  dvm_ref copy=dvm_new_array(vm,'L',"Landroid/content/Intent;",source->length);
  if (!copy) return 0;
  dvm_ref *items=source->data,*dest=dvm_array_data(vm,copy);
  for (uint32_t i=0;i<source->length;++i) {
    union dvm_value action={0};
    if (!items[i] || (validate && (!intent_getAction(vm,items[i],NULL,0,&action) || !action.l))) {
      dvm__throw(vm,"java/lang/NullPointerException","intent and action are required"); return 0;
    }
    dest[i]=dvm_new_object(vm,dvm_find_class(vm,"android/content/Intent"));
    if (!dest[i] || !intent_copy_into(vm,dest[i],items[i],false)) return 0;
  }
  return copy;
}

static bool shortcut_set_intents(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                                 int nargs,union dvm_value *out)
{
  (void)nargs;
  if (!ARG(0).l) {
    dvm__throw(vm,"java/lang/NullPointerException","intents"); return false;
  }
  dvm_ref source=ARG(0).l;
  if (!strcmp(builtin_name(),"setIntent")) {
    source=dvm_new_array(vm,'L',"Landroid/content/Intent;",1);
    if (!source) return false;
    ((dvm_ref *)dvm_array_data(vm,source))[0]=ARG(0).l;
  }
  dvm_ref copy=shortcut_intents_copy(vm,source,true);
  if (!copy) return false;
  dvm_set_field(vm,self,"intents","[Landroid/content/Intent;",(union dvm_value){.l=copy});
  RETL(self);
}

static bool shortcut_property(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                               int nargs,union dvm_value *out)
{
  (void)nargs;
  const char *name=builtin_name();
  char field[64];
  snprintf(field,sizeof field,"%s",name+3);
  field[0]=(char)tolower((unsigned char)field[0]);
  const struct rt_field *f=rt_shortcut_fields;
  for (;f->name && strcmp(f->name,field);++f) {}
  if (!f->name) return false;
  if (name[0]=='g') return dvm_get_field(vm,self,f->name,f->type,out);
  if (!strcmp(f->type,"Ljava/lang/CharSequence;")) {
    union dvm_value length={0};
    if (!ARG(0).l || !am_vcall(vm,ARG(0).l,"length","()I",NULL,0,&length) || !length.i) {
      if (!dvm_exception(vm)) dvm__throw(vm,"java/lang/IllegalArgumentException","label cannot be empty");
      return false;
    }
  }
  if (!strcmp(field,"rank") && ARG(0).i<0) {
    dvm__throw(vm,"java/lang/IllegalArgumentException","rank must be nonnegative"); return false;
  }
  if (!dvm_set_field(vm,self,f->name,f->type,ARG(0))) return false;
  RETL(self);
}

static bool shortcut_build(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                            int nargs,union dvm_value *out)
{
  (void)args; (void)nargs;
  dvm_ref value=dvm_new_object(vm,dvm_find_class(vm,"android/content/pm/ShortcutInfo"));
  if (!value) return false;
  for (const struct rt_field *f=rt_shortcut_fields;f->name;++f) {
    union dvm_value v={0};
    if (!dvm_get_field(vm,self,f->name,f->type,&v)) return false;
    if (!strcmp(f->name,"intents") && v.l) {
      v.l=shortcut_intents_copy(vm,v.l,false);
      if (!v.l) return false;
    } else if (!strcmp(f->name,"categories") && v.l) {
      dvm_ref copy=dvm_new_object(vm,dvm_find_class(vm,"java/util/HashSet"));
      union dvm_value ignored={0};
      if (!copy || !am_vcall(vm,copy,"<init>","(Ljava/util/Collection;)V",&v,1,&ignored)) return false;
      v.l=copy;
    } else if (!strcmp(f->name,"persons") && v.l) {
      struct dvm_object *source=dvm__obj(vm,v.l);
      dvm_ref copy=dvm_new_array(vm,'L',"Landroid/app/Person;",source->length);
      if (!copy) return false;
      memcpy(dvm_array_data(vm,copy),source->data,(size_t)source->length*sizeof(dvm_ref));
      v.l=copy;
    }
    if (!dvm_set_field(vm,value,f->name,f->type,v)) return false;
  }
  RETL(value);
}

static bool shortcut_get_intents(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                                  int nargs,union dvm_value *out)
{
  (void)args; (void)nargs;
  union dvm_value v={0};
  dvm_get_field(vm,self,"intents","[Landroid/content/Intent;",&v);
  if (!v.l) RETL(0);
  dvm_ref copy=shortcut_intents_copy(vm,v.l,false);
  if (!copy) return false;
  if (!strcmp(builtin_name(),"getIntent")) {
    struct dvm_object *array=dvm__obj(vm,copy);
    RETL(array->length ? ((dvm_ref *)array->data)[array->length-1] : 0);
  }
  RETL(copy);
}

#define SC_SET(n,s) M(n,"(" s ")Landroid/content/pm/ShortcutInfo$Builder;",shortcut_property)
static const struct rt_method rt_shortcut_builder[] = {
  M("<init>","(Landroid/content/Context;Ljava/lang/String;)V",shortcut_init),
  SC_SET("setShortLabel","Ljava/lang/CharSequence;"),SC_SET("setLongLabel","Ljava/lang/CharSequence;"),
  SC_SET("setDisabledMessage","Ljava/lang/CharSequence;"),SC_SET("setIcon","Landroid/graphics/drawable/Icon;"),
  SC_SET("setActivity","Landroid/content/ComponentName;"),SC_SET("setRank","I"),
  SC_SET("setCategories","Ljava/util/Set;"),SC_SET("setExtras","Landroid/os/PersistableBundle;"),
  SC_SET("setPersons","[Landroid/app/Person;"),SC_SET("setLocusId","Landroid/content/LocusId;"),
  SC_SET("setLongLived","Z"),
  M("setIntents","([Landroid/content/Intent;)Landroid/content/pm/ShortcutInfo$Builder;",shortcut_set_intents),
  M("setIntent","(Landroid/content/Intent;)Landroid/content/pm/ShortcutInfo$Builder;",shortcut_set_intents),
  M("build","()Landroid/content/pm/ShortcutInfo;",shortcut_build), M_END
};
#undef SC_SET
static const struct rt_method rt_shortcut_info[] = {
  M("getId","()Ljava/lang/String;",shortcut_property),M("getPackage","()Ljava/lang/String;",shortcut_property),
  M("getShortLabel","()Ljava/lang/CharSequence;",shortcut_property),M("getLongLabel","()Ljava/lang/CharSequence;",shortcut_property),
  M("getDisabledMessage","()Ljava/lang/CharSequence;",shortcut_property),M("getRank","()I",shortcut_property),
  M("getActivity","()Landroid/content/ComponentName;",shortcut_property),
  M("getCategories","()Ljava/util/Set;",shortcut_property),
  M("getExtras","()Landroid/os/PersistableBundle;",shortcut_property),
  M("getIntent","()Landroid/content/Intent;",shortcut_get_intents),
  M("getIntents","()[Landroid/content/Intent;",shortcut_get_intents), M_END
};
