/* Android VideoView: SurfaceHolder lifecycle and MediaPlayer state, in C.
 * Included after the View, dialog and media implementations. */

/* Resolve file and provider/resource URIs through their public interfaces.
 * MP4 demuxing needs random access: spool a provider's stream to an anonymous
 * file, not a second full copy in the VM heap. The demuxer retains its own fd. */
static bool mplayer_setDataSourceUri(struct dvm *vm,dvm_ref self,
   const union dvm_value *args,int nargs,union dvm_value *out)
{
   (void)nargs;
   if (!ARG(0).l || !ARG(1).l) {
      dvm__throw(vm,"java/lang/NullPointerException","context and uri must not be null");return false;
   }
   union dvm_value scheme={0},path={0};
   if (!am_vcall(vm,ARG(1).l,"getScheme","()Ljava/lang/String;",NULL,0,&scheme)) return false;
   const char *name=dvm_string_utf8(vm,scheme.l);
   if (!name || !strcmp(name,"file")) {
      if (!am_vcall(vm,ARG(1).l,name ? "getPath" : "toString","()Ljava/lang/String;",NULL,0,&path)) return false;
      return mplayer_setDataSourcePath(vm,self,&path,1,out);
   }
   if (strcmp(name,"content") && strcmp(name,"android.resource")) {
      dvm__throw(vm,"java/io/IOException","unsupported media data source scheme: %s",name);return false;
   }
   union dvm_value resolver={0},stream={0};
   if (!am_vcall(vm,ARG(0).l,"getContentResolver","()Landroid/content/ContentResolver;",NULL,0,&resolver) ||
       !am_vcall(vm,resolver.l,"openInputStream","(Landroid/net/Uri;)Ljava/io/InputStream;",&args[1],1,&stream)) return false;
   if (!stream.l) {dvm__throw(vm,"java/io/IOException","no media stream");return false;}
   FILE *file=tmpfile();
   bool good=file!=NULL;
   if (!good) dvm__throw(vm,"java/io/IOException","temporary media file: %s",strerror(errno));
   dvm_ref buffer=good ? dvm_new_array(vm,'B',NULL,65536) : 0;
   if (good && !buffer) good=false;
   union dvm_value array={.l=buffer},read={0},ignored={0};
   while (good) {
      if (!am_vcall(vm,stream.l,"read","([B)I",&array,1,&read)) {good=false;break;}
      if (read.i<0) break;
      if (read.i>65536) {dvm__throw(vm,"java/io/IOException","invalid media stream read count");good=false;break;}
      if (read.i && fwrite(dvm_array_data(vm,buffer),1,(size_t)read.i,file)!=(size_t)read.i) {
         dvm__throw(vm,"java/io/IOException","writing media stream");good=false;
      }
   }
   dvm_ref failure=dvm_exception(vm);
   dvm_clear_exception(vm);
   bool closed=am_vcall(vm,stream.l,"close","()V",NULL,0,&ignored);
   if (failure) vm->exception=failure;
   good &= closed && !failure;
   if (good && fflush(file)) {dvm__throw(vm,"java/io/IOException","flushing media stream");good=false;}
   if (good) good=player_set_source_fd(vm,self,fileno(file),0,0,out);
   if (file) fclose(file);
   return good;
}

enum { VV_ERROR=-1, VV_IDLE, VV_PREPARING, VV_PREPARED, VV_PLAYING,
       VV_PAUSED, VV_COMPLETED };

static void vv_ref(struct dvm *vm,dvm_ref view,const char *field,
                    const char *type,dvm_ref value)
{
   (void)dvm_set_field(vm,view,field,type,(union dvm_value){.l=value});
}

static dvm_ref vv_player(struct dvm *vm,dvm_ref view)
{ return ui_obj(vm,view,"mMediaPlayer","Landroid/media/MediaPlayer;"); }

static bool vv_ready(struct dvm *vm,dvm_ref view)
{
   int state=ui_int(vm,view,"mCurrentState");
   return vv_player(vm,view) && state!=VV_ERROR && state!=VV_IDLE && state!=VV_PREPARING;
}

static bool vv_controller_call(struct dvm *vm,dvm_ref view,const char *name,
                               const char *sig,const union dvm_value *a,int n)
{
   dvm_ref controller=ui_obj(vm,view,"mMediaController","Landroid/widget/MediaController;");
   union dvm_value result={0};
   return !controller || am_vcall(vm,controller,name,sig,a,n,&result);
}

static bool vv_attach_controller(struct dvm *vm,dvm_ref view)
{
   if (!vv_player(vm,view)) return true;
   union dvm_value a={.l=view};
   if (!vv_controller_call(vm,view,"setMediaPlayer",
       "(Landroid/widget/MediaController$MediaPlayerControl;)V",&a,1)) return false;
   dvm_ref parent=ui_obj(vm,view,"parent","Landroid/view/ViewParent;");
   if (parent && ui_is_a(vm,parent,"android/view/View")) a.l=parent;
   if (!vv_controller_call(vm,view,"setAnchorView","(Landroid/view/View;)V",&a,1)) return false;
   a.i=vv_ready(vm,view);
   return vv_controller_call(vm,view,"setEnabled","(Z)V",&a,1);
}

static bool vv_release(struct dvm *vm,dvm_ref view,bool clear_target)
{
   dvm_ref player=vv_player(vm,view);
   if (player) {
      union dvm_value ignored={0};
      if (!mplayer_reset(vm,player,NULL,0,&ignored) ||
          !mplayer_release(vm,player,NULL,0,&ignored)) return false;
      vv_ref(vm,view,"mMediaPlayer","Landroid/media/MediaPlayer;",0);
      rt_set_int(vm,view,"mCurrentState",VV_IDLE);
      if (clear_target) rt_set_int(vm,view,"mTargetState",VV_IDLE);
   }
   return true;
}

static bool vv_start(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                      int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;
   if (vv_ready(vm,self)) {
      if (!mplayer_start(vm,vv_player(vm,self),NULL,0,out)) return false;
      rt_set_int(vm,self,"mCurrentState",VV_PLAYING);
   }
   rt_set_int(vm,self,"mTargetState",VV_PLAYING);
   RETV();
}

static bool vv_pause(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                      int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;
   struct rt_player *p=player_of(vm,vv_player(vm,self));
   if (vv_ready(vm,self) && p && p->playing) {
      if (!mplayer_pause(vm,vv_player(vm,self),NULL,0,out)) return false;
      rt_set_int(vm,self,"mCurrentState",VV_PAUSED);
   }
   rt_set_int(vm,self,"mTargetState",VV_PAUSED);
   RETV();
}

static bool vv_seek(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                     int nargs,union dvm_value *out)
{
   if (vv_ready(vm,self)) {
      if (!mplayer_seekTo(vm,vv_player(vm,self),args,nargs,out)) return false;
      rt_set_int(vm,self,"mSeekWhenPrepared",0);
   } else rt_set_int(vm,self,"mSeekWhenPrepared",ARG(0).i);
   RETV();
}

static bool vv_listener(struct dvm *vm,dvm_ref view,const char *field,
                        const char *type,const char *method,const char *sig,
                        const union dvm_value *args,int nargs,union dvm_value *out)
{
   out->i=0;
   dvm_ref listener=ui_obj(vm,view,field,type);
   return !listener || am_vcall(vm,listener,method,sig,args,nargs,out);
}

/* The default error UI uses Android's framework strings, including locale.
 * Completion is reported when the user acknowledges it, not on media failure. */
static bool vv_error_dialog(struct dvm *vm,dvm_ref view,int error)
{
   if (!ui_int(vm,view,"attachedToWindow")) return true;
   dvm_ref resources=ui_obj(vm,view,"resources","Landroid/content/res/Resources;");
   union dvm_value ids[2]={{0},{0}}, result={0};
   const char *names[]={error==200 ? "VideoView_error_text_invalid_progressive_playback" :
                                    "VideoView_error_text_unknown", "VideoView_error_button"};
   for (int i=0;i<2;++i) {
      union dvm_value a[]={{.l=dvm_new_string(vm,names[i])},
                          {.l=dvm_new_string(vm,"string")},{.l=dvm_new_string(vm,"android")}};
      if (!am_vcall(vm,resources,"getIdentifier",
           "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I",a,3,&ids[i])) return false;
   }
   dvm_ref builder=dvm_new_object(vm,dvm_find_class(vm,"android/app/AlertDialog$Builder"));
   union dvm_value context={.l=ui_obj(vm,view,"context","Landroid/content/Context;")};
   if (!builder || !am_vcall(vm,builder,"<init>","(Landroid/content/Context;)V",&context,1,&result) ||
       !am_vcall(vm,builder,"setMessage","(I)Landroid/app/AlertDialog$Builder;",&ids[0],1,&result)) return false;
   union dvm_value button[]={ids[1],{.l=ui_obj(vm,view,"mCallbacks","Llunaria/media/VideoViewCallbacks;")}};
   if (!am_vcall(vm,builder,"setPositiveButton",
        "(ILandroid/content/DialogInterface$OnClickListener;)Landroid/app/AlertDialog$Builder;",button,2,&result)) return false;
   union dvm_value no={.i=0};
   if (!am_vcall(vm,builder,"setCancelable","(Z)Landroid/app/AlertDialog$Builder;",&no,1,&result)) return false;
   return adb_show(vm,builder,NULL,0,&result);
}

static bool vv_error(struct dvm *vm,dvm_ref view,const union dvm_value *args,
                      int nargs,union dvm_value *out)
{
   rt_set_int(vm,view,"mCurrentState",VV_ERROR);
   rt_set_int(vm,view,"mTargetState",VV_ERROR);
   if (!vv_controller_call(vm,view,"hide","()V",NULL,0)) return false;
   union dvm_value handled={0};
   if (!vv_listener(vm,view,"mOnErrorListener","Landroid/media/MediaPlayer$OnErrorListener;",
       "onError","(Landroid/media/MediaPlayer;II)Z",args,nargs,&handled)) return false;
   if (!handled.i && !vv_error_dialog(vm,view,args[1].i)) return false;
   RETI(1); /* VideoView handles the error or displays its own dialog. */
}

static bool vv_open(struct dvm *vm,dvm_ref view,union dvm_value *out)
{
   dvm_ref uri=ui_obj(vm,view,"mUri","Landroid/net/Uri;");
   dvm_ref holder=ui_obj(vm,view,"mSurfaceHolder","Landroid/view/SurfaceHolder;");
   if (!uri || !holder) return true;
   if (!vv_release(vm,view,false)) return false;
   dvm_ref player=dvm_new_object(vm,dvm_find_class(vm,"android/media/MediaPlayer"));
   if (!player || !mplayer_init(vm,player,NULL,0,out)) return false;
   vv_ref(vm,view,"mMediaPlayer","Landroid/media/MediaPlayer;",player);
   dvm_ref cb=ui_obj(vm,view,"mCallbacks","Llunaria/media/VideoViewCallbacks;");
   const char *listeners[]={"onPrepared","onError","onCompletion","onVideoSizeChanged",
                             "onInfo","onBufferingUpdate"};
   for (unsigned i=0;i<sizeof listeners/sizeof *listeners;++i)
      vv_ref(vm,player,listeners[i],"Ljava/lang/Object;",cb);
   rt_set_int(vm,view,"mCurrentBufferPercentage",0);
   union dvm_value data[]={{.l=ui_obj(vm,view,"context","Landroid/content/Context;")},
                           {.l=uri},{.l=ui_obj(vm,view,"mHeaders","Ljava/util/Map;")}};
   union dvm_value display={.l=holder};
   if (!mplayer_setDataSourceUri(vm,player,data,3,out) ||
       !mplayer_setDisplay(vm,player,&display,1,out) ||
       !mplayer_prepareAsync(vm,player,NULL,0,out)) {
      dvm_ref exception=dvm_exception(vm);
      struct dvm_class *cls=dvm_object_class(vm,exception);
      if (!exception ||
          (!dvm__class_assignable(vm,cls,dvm_find_class(vm,"java/io/IOException")) &&
           !dvm__class_assignable(vm,cls,dvm_find_class(vm,"java/lang/IllegalArgumentException")))) return false;
      dvm_clear_exception(vm);
      union dvm_value error[]={{.l=player},{.i=1},{.i=0}};
      return vv_error(vm,view,error,3,out);
   }
   rt_set_int(vm,view,"mCurrentState",VV_PREPARING);
   return vv_attach_controller(vm,view);
}

static bool vv_init(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                     int nargs,union dvm_value *out)
{
   if (!view_init_context(vm,self,args,nargs,out) ||
       !surface_view_init(vm,self,args,nargs,out)) return false;
   dvm_ref cb=dvm_new_object(vm,dvm_find_class(vm,"lunaria/media/VideoViewCallbacks"));
   if (!cb) return false;
   vv_ref(vm,cb,"owner","Landroid/widget/VideoView;",self);
   vv_ref(vm,self,"mCallbacks","Llunaria/media/VideoViewCallbacks;",cb);
   union dvm_value a={.l=cb};
   dvm_ref holder=ui_obj(vm,self,"holder","Landroid/view/SurfaceHolder;");
   if (!surface_holder_add_callback(vm,holder,&a,1,out)) return false;
   rt_set_int(vm,self,"mCurrentState",VV_IDLE);
   rt_set_int(vm,self,"mTargetState",VV_IDLE);
   rt_set_int(vm,self,"mAudioFocusType",1);
   a.i=1;
   if (!view_set_focusable(vm,self,&a,1,out)) return false;
   return view_request_focus(vm,self,NULL,0,out);
}

static bool vv_set_uri(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                        int nargs,union dvm_value *out)
{
   union dvm_value uri=ARG(0);
   if (!strcmp(builtin_name(),"setVideoPath") && !uri_parse(vm,0,args,1,&uri)) return false;
   vv_ref(vm,self,"mUri","Landroid/net/Uri;",uri.l);
   vv_ref(vm,self,"mHeaders","Ljava/util/Map;",nargs>1 ? args[1].l : 0);
   rt_set_int(vm,self,"mSeekWhenPrepared",0);
   if (!vv_open(vm,self,out)) return false;
   return view_request_layout(vm,self,NULL,0,out);
}

static bool vv_control(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                        int nargs,union dvm_value *out)
{
   (void)args;(void)nargs;
   const char *name=builtin_name();
   if (!strcmp(name,"resume")) { if (!vv_open(vm,self,out)) return false; }
   else if (!strcmp(name,"suspend")) { if (!vv_release(vm,self,false)) return false; }
   else if (!vv_release(vm,self,true)) return false;
   RETV();
}

static bool vv_query(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                      int nargs,union dvm_value *out)
{
   const char *name=builtin_name();
   if (!strcmp(name,"canPause")) RETI(ui_int(vm,self,"mCanPause"));
   if (!strcmp(name,"canSeekBackward")) RETI(ui_int(vm,self,"mCanSeekBack"));
   if (!strcmp(name,"canSeekForward")) RETI(ui_int(vm,self,"mCanSeekForward"));
   if (!strcmp(name,"getBufferPercentage"))
      RETI(vv_player(vm,self) ? ui_int(vm,self,"mCurrentBufferPercentage") : 0);
   if (!vv_ready(vm,self)) RETI(!strcmp(name,"getDuration") ? -1 : 0);
   dvm_ref player=vv_player(vm,self);
   if (!strcmp(name,"getDuration")) return mplayer_getDuration(vm,player,args,nargs,out);
   if (!strcmp(name,"getCurrentPosition")) return mplayer_getCurrentPosition(vm,player,args,nargs,out);
   return mplayer_isPlaying(vm,player,args,nargs,out);
}

static bool vv_set_listener(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                             int nargs,union dvm_value *out)
{
   (void)nargs;
   const char *name=builtin_name();
   if (!strcmp(name,"setOnPreparedListener"))
      vv_ref(vm,self,"mOnPreparedListener","Landroid/media/MediaPlayer$OnPreparedListener;",ARG(0).l);
   else if (!strcmp(name,"setOnCompletionListener"))
      vv_ref(vm,self,"mOnCompletionListener","Landroid/media/MediaPlayer$OnCompletionListener;",ARG(0).l);
   else if (!strcmp(name,"setOnInfoListener"))
      vv_ref(vm,self,"mOnInfoListener","Landroid/media/MediaPlayer$OnInfoListener;",ARG(0).l);
   else vv_ref(vm,self,"mOnErrorListener","Landroid/media/MediaPlayer$OnErrorListener;",ARG(0).l);
   RETV();
}

static bool vv_set_audio(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                          int nargs,union dvm_value *out)
{
   (void)nargs;
   if (!strcmp(builtin_name(),"setAudioFocusRequest")) {
      if (ARG(0).i<0 || ARG(0).i>4) {
         dvm__throw(vm,"java/lang/IllegalArgumentException","Illegal audio focus type");return false;
      }
      rt_set_int(vm,self,"mAudioFocusType",ARG(0).i);
   } else {
      if (!ARG(0).l) {dvm__throw(vm,"java/lang/IllegalArgumentException","Illegal null AudioAttributes");return false;}
      vv_ref(vm,self,"mAudioAttributes","Landroid/media/AudioAttributes;",ARG(0).l);
   }
   RETV();
}

static bool vv_set_controller(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                               int nargs,union dvm_value *out)
{
   (void)nargs;
   if (!vv_controller_call(vm,self,"hide","()V",NULL,0)) return false;
   vv_ref(vm,self,"mMediaController","Landroid/widget/MediaController;",ARG(0).l);
   if (!vv_attach_controller(vm,self)) return false;
   RETV();
}

/* MeasureSpec arithmetic uses int64_t so large legal dimensions cannot
 * overflow C signed multiplication while preserving the Android aspect rule. */
static bool vv_measure(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                        int nargs,union dvm_value *out)
{
   int64_t vw=ui_int(vm,self,"mVideoWidth"),vh=ui_int(vm,self,"mVideoHeight");
   uint32_t wm=(uint32_t)ARG(0).i>>30,hm=(uint32_t)ARG(1).i>>30;
   int64_t ws=(uint32_t)ARG(0).i&0x3fffffffu,hs=(uint32_t)ARG(1).i&0x3fffffffu;
   int64_t w=wm ? ws : vw,h=hm ? hs : vh;
   if (vw>0 && vh>0) {
      if (wm==1 && hm==1) {
         if (vw*h<w*vh) w=h*vw/vh;
         else if (vw*h>w*vh) h=w*vh/vw;
      } else if (wm==1) {
         h=w*vh/vw;
         if (hm==2 && h>hs) h=hs;
      } else if (hm==1) {
         w=h*vw/vh;
         if (wm==2 && w>ws) w=ws;
      } else {
         w=vw;h=vh;
         if (hm==2 && h>hs) {h=hs;w=h*vw/vh;}
         if (wm==2 && w>ws) {w=ws;h=w*vh/vw;}
      }
   }
   union dvm_value size[]={{.i=(int32_t)w},{.i=(int32_t)h}};
   return view_set_measured(vm,self,size,nargs,out);
}

static bool vv_callback(struct dvm *vm,dvm_ref self,const union dvm_value *args,
                         int nargs,union dvm_value *out)
{
   dvm_ref view=ui_obj(vm,self,"owner","Landroid/widget/VideoView;");
   const char *name=builtin_name();
   if (!view) RETV();
   if (!strcmp(name,"surfaceCreated")) {
      vv_ref(vm,view,"mSurfaceHolder","Landroid/view/SurfaceHolder;",ARG(0).l);
      if (!vv_open(vm,view,out)) return false;
   } else if (!strcmp(name,"surfaceDestroyed")) {
      vv_ref(vm,view,"mSurfaceHolder","Landroid/view/SurfaceHolder;",0);
      if (!vv_controller_call(vm,view,"hide","()V",NULL,0) || !vv_release(vm,view,true)) return false;
   } else if (!strcmp(name,"surfaceChanged")) {
      rt_set_int(vm,view,"mSurfaceWidth",args[2].i);rt_set_int(vm,view,"mSurfaceHeight",args[3].i);
      if (vv_ready(vm,view) && ui_int(vm,view,"mTargetState")==VV_PLAYING &&
          ui_int(vm,view,"mVideoWidth")==args[2].i && ui_int(vm,view,"mVideoHeight")==args[3].i) {
         union dvm_value seek={.i=ui_int(vm,view,"mSeekWhenPrepared")};
         if (seek.i && !vv_seek(vm,view,&seek,1,out)) return false;
         return vv_start(vm,view,NULL,0,out);
      }
   } else if (!strcmp(name,"onClick")) {
      union dvm_value player={.l=vv_player(vm,view)};
      if (!vv_listener(vm,view,"mOnCompletionListener","Landroid/media/MediaPlayer$OnCompletionListener;",
          "onCompletion","(Landroid/media/MediaPlayer;)V",&player,1,out)) return false;
   } else {
      /* Ignore notifications from a released/replaced playback session. */
      if (ARG(0).l!=vv_player(vm,view)) RETV();
      if (!strcmp(name,"onError")) return vv_error(vm,view,args,nargs,out);
      if (!strcmp(name,"onCompletion")) {
         rt_set_int(vm,view,"mCurrentState",VV_COMPLETED);rt_set_int(vm,view,"mTargetState",VV_COMPLETED);
         if (!vv_controller_call(vm,view,"hide","()V",NULL,0)) return false;
         return vv_listener(vm,view,"mOnCompletionListener","Landroid/media/MediaPlayer$OnCompletionListener;",
                            name,"(Landroid/media/MediaPlayer;)V",args,nargs,out);
      }
      if (!strcmp(name,"onInfo")) {
         if (!vv_listener(vm,view,"mOnInfoListener","Landroid/media/MediaPlayer$OnInfoListener;",
             name,"(Landroid/media/MediaPlayer;II)Z",args,nargs,out)) return false;
         RETI(1);
      }
      if (!strcmp(name,"onBufferingUpdate")) {rt_set_int(vm,view,"mCurrentBufferPercentage",args[1].i);RETV();}
      if (!strcmp(name,"onPrepared")) {
         rt_set_int(vm,view,"mCurrentState",VV_PREPARED);
         (void)dvm_set_field(vm,view,"mCanPause","Z",(union dvm_value){.i=1});
         (void)dvm_set_field(vm,view,"mCanSeekBack","Z",(union dvm_value){.i=1});
         (void)dvm_set_field(vm,view,"mCanSeekForward","Z",(union dvm_value){.i=1});
         if (!vv_listener(vm,view,"mOnPreparedListener","Landroid/media/MediaPlayer$OnPreparedListener;",
             name,"(Landroid/media/MediaPlayer;)V",args,nargs,out)) return false;
         if (vv_player(vm,view)!=ARG(0).l || !vv_ready(vm,view)) RETV();
         union dvm_value enabled={.i=1};
         if (!vv_controller_call(vm,view,"setEnabled","(Z)V",&enabled,1)) return false;
      }
      struct rt_player *p=player_of(vm,ARG(0).l);
      if (!p) RETV();
      rt_set_int(vm,view,"mVideoWidth",p->width);rt_set_int(vm,view,"mVideoHeight",p->height);
      if (!strcmp(name,"onPrepared")) {
         union dvm_value seek={.i=ui_int(vm,view,"mSeekWhenPrepared")};
         if (seek.i && !vv_seek(vm,view,&seek,1,out)) return false;
      }
      if (p->width>0 && p->height>0) {
         union dvm_value size[]={{.i=p->width},{.i=p->height}};
         dvm_ref holder=ui_obj(vm,view,"holder","Landroid/view/SurfaceHolder;");
         if (!surface_holder_set_fixed_size(vm,holder,size,2,out) ||
             !view_request_layout(vm,view,NULL,0,out)) return false;
      }
      if (!strcmp(name,"onPrepared") && ui_int(vm,view,"mTargetState")==VV_PLAYING &&
          ((!p->width || !p->height) ||
           (p->width==ui_int(vm,view,"mSurfaceWidth") && p->height==ui_int(vm,view,"mSurfaceHeight"))))
         return vv_start(vm,view,NULL,0,out);
   }
   RETV();
}

static const struct rt_field rt_videoview_fields[]={
   {"mUri","Landroid/net/Uri;"},{"mHeaders","Ljava/util/Map;"},
   {"mMediaPlayer","Landroid/media/MediaPlayer;"},{"mSurfaceHolder","Landroid/view/SurfaceHolder;"},
   {"mCallbacks","Llunaria/media/VideoViewCallbacks;"},{"mMediaController","Landroid/widget/MediaController;"},
   {"mCurrentState","I"},{"mTargetState","I"},{"mVideoWidth","I"},{"mVideoHeight","I"},
   {"mSurfaceWidth","I"},{"mSurfaceHeight","I"},{"mSeekWhenPrepared","I"},
   {"mCurrentBufferPercentage","I"},{"mAudioFocusType","I"},
   {"mAudioAttributes","Landroid/media/AudioAttributes;"},
   {"mCanPause","Z"},{"mCanSeekBack","Z"},{"mCanSeekForward","Z"},
   {"mOnPreparedListener","Landroid/media/MediaPlayer$OnPreparedListener;"},
   {"mOnCompletionListener","Landroid/media/MediaPlayer$OnCompletionListener;"},
   {"mOnErrorListener","Landroid/media/MediaPlayer$OnErrorListener;"},
   {"mOnInfoListener","Landroid/media/MediaPlayer$OnInfoListener;"},F_END
};
static const struct rt_method rt_videoview[]={
   M("<init>","(Landroid/content/Context;)V",vv_init),
   M("<init>","(Landroid/content/Context;Landroid/util/AttributeSet;)V",vv_init),
   M("<init>","(Landroid/content/Context;Landroid/util/AttributeSet;I)V",vv_init),
   M("<init>","(Landroid/content/Context;Landroid/util/AttributeSet;II)V",vv_init),
   M("setVideoPath","(Ljava/lang/String;)V",vv_set_uri),
   M("setVideoURI","(Landroid/net/Uri;)V",vv_set_uri),
   M("setVideoURI","(Landroid/net/Uri;Ljava/util/Map;)V",vv_set_uri),
   M("start","()V",vv_start),M("pause","()V",vv_pause),M("seekTo","(I)V",vv_seek),
   M("stopPlayback","()V",vv_control),M("suspend","()V",vv_control),M("resume","()V",vv_control),
   M("isPlaying","()Z",vv_query),M("getDuration","()I",vv_query),
   M("getCurrentPosition","()I",vv_query),M("getBufferPercentage","()I",vv_query),
   M("canPause","()Z",vv_query),M("canSeekBackward","()Z",vv_query),M("canSeekForward","()Z",vv_query),
   M("onMeasure","(II)V",vv_measure),M("resolveAdjustedSize","(II)I",view_getDefaultSize),
   M("setMediaController","(Landroid/widget/MediaController;)V",vv_set_controller),
   M("setAudioFocusRequest","(I)V",vv_set_audio),
   M("setAudioAttributes","(Landroid/media/AudioAttributes;)V",vv_set_audio),
   M("setOnPreparedListener","(Landroid/media/MediaPlayer$OnPreparedListener;)V",vv_set_listener),
   M("setOnCompletionListener","(Landroid/media/MediaPlayer$OnCompletionListener;)V",vv_set_listener),
   M("setOnErrorListener","(Landroid/media/MediaPlayer$OnErrorListener;)V",vv_set_listener),
   M("setOnInfoListener","(Landroid/media/MediaPlayer$OnInfoListener;)V",vv_set_listener),M_END
};
static const struct rt_field rt_vv_callback_fields[]={{"owner","Landroid/widget/VideoView;"},F_END};
static const struct rt_method rt_vv_callbacks[]={
   M("surfaceCreated","(Landroid/view/SurfaceHolder;)V",vv_callback),
   M("surfaceChanged","(Landroid/view/SurfaceHolder;III)V",vv_callback),
   M("surfaceDestroyed","(Landroid/view/SurfaceHolder;)V",vv_callback),
   M("onPrepared","(Landroid/media/MediaPlayer;)V",vv_callback),
   M("onCompletion","(Landroid/media/MediaPlayer;)V",vv_callback),
   M("onError","(Landroid/media/MediaPlayer;II)Z",vv_callback),
   M("onInfo","(Landroid/media/MediaPlayer;II)Z",vv_callback),
   M("onVideoSizeChanged","(Landroid/media/MediaPlayer;II)V",vv_callback),
   M("onBufferingUpdate","(Landroid/media/MediaPlayer;I)V",vv_callback),
   M("onClick","(Landroid/content/DialogInterface;I)V",vv_callback),M_END
};
static const char *const rt_vv_interfaces[]={"Landroid/widget/MediaController$MediaPlayerControl;",NULL};
static const char *const rt_vv_callback_interfaces[]={
   "Landroid/view/SurfaceHolder$Callback;","Landroid/media/MediaPlayer$OnPreparedListener;",
   "Landroid/media/MediaPlayer$OnCompletionListener;","Landroid/media/MediaPlayer$OnErrorListener;",
   "Landroid/media/MediaPlayer$OnInfoListener;","Landroid/media/MediaPlayer$OnVideoSizeChangedListener;",
   "Landroid/media/MediaPlayer$OnBufferingUpdateListener;","Landroid/content/DialogInterface$OnClickListener;",NULL
};
