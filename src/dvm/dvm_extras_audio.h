/* android.media.AudioFormat / AudioTrack, the API 21+ surface.
 *
 * Media3's DefaultAudioSink builds its track as
 *   new AudioTrack(AudioAttributes, new AudioFormat.Builder()...build(), size, mode, session)
 * (or AudioTrack.Builder) and feeds it ByteBuffers; the legacy
 * AudioTrack(int,int,int,int,int,int) constructor and write(byte[]) were the only
 * entry points modelled, so the first video with sound stopped in
 * AudioFormat$Builder.setChannelMask.  Everything here lands on the same
 * luna_audio_track the legacy path drives.
 *
 * Included from dvm_runtime.c; the AudioTrack helpers it calls are defined above. */

#ifndef DVM_EXTRAS_AUDIO_H
#define DVM_EXTRAS_AUDIO_H

static const struct rt_field rt_audio_format_fields[] = {
   { "rate", "I" }, { "mask", "I" }, { "encoding", "I" }, F_END
};

static bool af_builder_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   (void)args; (void)nargs; (void)out;
   rt_set_int(vm, self, "encoding", 1);          /* ENCODING_DEFAULT */
   RETV();
}

static bool af_builder_set(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   (void)nargs;
   const char *n = builtin_name();
   if (!strcmp(n, "setEncoding")) rt_set_int(vm, self, "encoding", args[0].i);
   else if (!strcmp(n, "setSampleRate")) {
      if (args[0].i <= 0) { dvm__throw(vm, "java/lang/IllegalArgumentException", "Invalid sample rate %d", args[0].i); return false; }
      rt_set_int(vm, self, "rate", args[0].i);
   } else if (!strcmp(n, "setChannelMask")) rt_set_int(vm, self, "mask", args[0].i);
   RETL(self);                                   /* setChannelIndexMask and friends are ignored */
}

static bool af_builder_build(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct dvm_class *c = dvm__class_by_desc(vm, "Landroid/media/AudioFormat;");
   dvm_ref f = c ? dvm_new_object(vm, c) : 0;
   if (!f) return false;
   static const char *const names[] = { "rate", "mask", "encoding" };
   for (int i = 0; i < 3; ++i) rt_set_int(vm, f, names[i], rt_get_int(vm, self, names[i]));
   RETL(f);
}

static bool af_get(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                   int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   const char *n = builtin_name();
   if (!strcmp(n, "getSampleRate")) RETI(rt_get_int(vm, self, "rate"));
   if (!strcmp(n, "getEncoding")) RETI(rt_get_int(vm, self, "encoding"));
   if (!strcmp(n, "getChannelCount")) RETI(audiotrack_channel_count(rt_get_int(vm, self, "mask")));
   RETI(rt_get_int(vm, self, "mask"));           /* getChannelMask */
}

static const struct rt_method rt_audio_format_builder[] = {
   M("<init>", "()V", af_builder_init),
   M("setEncoding", "(I)Landroid/media/AudioFormat$Builder;", af_builder_set),
   M("setSampleRate", "(I)Landroid/media/AudioFormat$Builder;", af_builder_set),
   M("setChannelMask", "(I)Landroid/media/AudioFormat$Builder;", af_builder_set),
   M("setChannelIndexMask", "(I)Landroid/media/AudioFormat$Builder;", af_builder_set),
   M("build", "()Landroid/media/AudioFormat;", af_builder_build),
   M_END,
};

static const struct rt_method rt_audio_format[] = {
   M("getSampleRate", "()I", af_get), M("getEncoding", "()I", af_get),
   M("getChannelMask", "()I", af_get), M("getChannelCount", "()I", af_get),
   M_END,
};

/* AudioTrack(AudioAttributes, AudioFormat, bufferSizeInBytes, mode, sessionId). */
static bool at_init_format(struct dvm *vm, dvm_ref self, int rate, int mask, int encoding,
                           int buffer_size, int mode)
{
   union dvm_value query[3] = { { .i = rate }, { .i = mask }, { .i = encoding } }, min = {0};
   if (buffer_size <= 0) {
      (void)audiotrack_getMinBufferSize(vm, self, query, 3, &min);
      buffer_size = min.i > 0 ? min.i : 0;
   }
   union dvm_value init[6] = { { .i = 3 }, { .i = rate }, { .i = mask }, { .i = encoding },
                               { .i = buffer_size }, { .i = mode } }, ignored = {0};
   return audiotrack_init(vm, self, init, 6, &ignored);
}

static bool at_init_modern(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   (void)out;
   dvm_ref fmt = args[1].l;
   if (!fmt) { dvm__throw(vm, "java/lang/IllegalArgumentException", "Illegal null AudioFormat"); return false; }
   if (!at_init_format(vm, self, rt_get_int(vm, fmt, "rate"), rt_get_int(vm, fmt, "mask"),
                       rt_get_int(vm, fmt, "encoding"), args[2].i, nargs > 3 ? args[3].i : 1))
      return false;
   if (nargs > 4 && args[4].i > 0) rt_set_int(vm, self, "mSessionId", args[4].i);
   RETV();
}

static const struct rt_field rt_audio_track_builder_fields[] = {
   { "format", "Landroid/media/AudioFormat;" }, { "size", "I" }, { "mode", "I" }, { "session", "I" }, F_END
};

static bool atb_set(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                    int nargs, union dvm_value *out)
{
   (void)nargs;
   const char *n = builtin_name();
   if (!strcmp(n, "setAudioFormat")) (void)dvm_set_field(vm, self, "format", "Landroid/media/AudioFormat;", args[0]);
   else if (!strcmp(n, "setBufferSizeInBytes")) rt_set_int(vm, self, "size", args[0].i);
   else if (!strcmp(n, "setTransferMode")) rt_set_int(vm, self, "mode", args[0].i);
   else if (!strcmp(n, "setSessionId")) rt_set_int(vm, self, "session", args[0].i);
   RETL(self);              /* setAudioAttributes, setPerformanceMode, setOffloadedPlayback ... */
}

static bool atb_build(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                      int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value fmt = {0};
   (void)dvm_get_field(vm, self, "format", "Landroid/media/AudioFormat;", &fmt);
   if (!fmt.l) { dvm__throw(vm, "java/lang/UnsupportedOperationException", "Audio format must be set"); return false; }
   struct dvm_class *c = dvm__class_by_desc(vm, "Landroid/media/AudioTrack;");
   dvm_ref track = c ? dvm_new_object(vm, c) : 0;
   if (!track) return false;
   const int mode = rt_get_int(vm, self, "mode");
   if (!at_init_format(vm, track, rt_get_int(vm, fmt.l, "rate"), rt_get_int(vm, fmt.l, "mask"),
                       rt_get_int(vm, fmt.l, "encoding"), rt_get_int(vm, self, "size"), mode ? mode : 1))
      return false;
   if (rt_get_int(vm, self, "session") > 0) rt_set_int(vm, track, "mSessionId", rt_get_int(vm, self, "session"));
   RETL(track);
}

static const struct rt_method rt_audio_track_builder[] = {
   M("<init>", "()V", nop_void),
   M("setAudioAttributes", "(Landroid/media/AudioAttributes;)Landroid/media/AudioTrack$Builder;", atb_set),
   M("setAudioFormat", "(Landroid/media/AudioFormat;)Landroid/media/AudioTrack$Builder;", atb_set),
   M("setBufferSizeInBytes", "(I)Landroid/media/AudioTrack$Builder;", atb_set),
   M("setTransferMode", "(I)Landroid/media/AudioTrack$Builder;", atb_set),
   M("setSessionId", "(I)Landroid/media/AudioTrack$Builder;", atb_set),
   M("setPerformanceMode", "(I)Landroid/media/AudioTrack$Builder;", atb_set),
   M("setOffloadedPlayback", "(Z)Landroid/media/AudioTrack$Builder;", atb_set),
   M("build", "()Landroid/media/AudioTrack;", atb_build),
   M_END,
};

/* write(ByteBuffer, size, mode[, timestamp]): the buffer's remaining bytes go
 * through the byte[] writer and its position advances by what was taken. */
static bool at_write_buffer(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   dvm_ref buffer = args[0].l;
   if (!buffer) { dvm__throw(vm, "java/lang/IllegalArgumentException", "buffer is null"); return false; }
   const int8_t *bytes;
   int32_t remaining, pos = 0;
   if (!bb_window(vm, buffer, &bytes, &remaining)) RETI(AT_ERROR_BAD_VALUE);
   (void)stream_field(vm, buffer, "pos", &pos);
   int32_t size = args[1].i;
   if (size < 0 || size > remaining) RETI(AT_ERROR_BAD_VALUE);
   if (!size) RETI(0);
   dvm_ref copy = dvm_new_array(vm, 'B', "B", (uint32_t)size);
   if (!copy) return false;
   dvm_pin(vm, copy);
   memcpy(dvm_array_data(vm, copy), bytes, (size_t)size);
   union dvm_value w[4] = { { .l = copy }, { .i = 0 }, { .i = size }, { .i = nargs > 2 ? args[2].i : 0 } };
   union dvm_value written = {0};
   const bool ok = audiotrack_write(vm, self, w, 4, &written);
   dvm_unpin(vm, copy);
   if (!ok) return false;
   if (written.i > 0) stream_set_field(vm, buffer, "pos", pos + written.i);
   RETI(written.i);
}

static bool at_false(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{ (void)vm; (void)self; (void)args; (void)nargs; RETI(0); }

static bool at_capacity(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   const int frame = audiotrack_frame_size(rt_get_int(vm, self, "mChannelConfig"),
                                           rt_get_int(vm, self, "mAudioFormat"));
   RETI(frame > 0 ? rt_get_int(vm, self, "mBufferSize") / frame : 0);
}

static bool at_channel_config(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   RETI(rt_get_int(vm, self, "mChannelConfig"));
}

static const struct rt_method rt_x_audio_track[] = {
   M("<init>", "(Landroid/media/AudioAttributes;Landroid/media/AudioFormat;III)V", at_init_modern),
   M("<init>", "(Landroid/media/AudioAttributes;Landroid/media/AudioFormat;II)V", at_init_modern),
   M("write", "(Ljava/nio/ByteBuffer;II)I", at_write_buffer),
   M("write", "(Ljava/nio/ByteBuffer;III)I", at_write_buffer),
   M("write", "(Ljava/nio/ByteBuffer;IIJ)I", at_write_buffer),
   M("getTimestamp", "(Landroid/media/AudioTimestamp;)Z", at_false),
   M("getUnderrunCount", "()I", at_false),
   M("isOffloadedPlayback", "()Z", at_false),
   M("setOffloadDelayPadding", "(II)V", nop_void),
   M("setOffloadEndOfStream", "()V", nop_void),
   M("setPlaybackParams", "(Landroid/media/PlaybackParams;)V", nop_void),
   M("setPreferredDevice", "(Landroid/media/AudioDeviceInfo;)Z", at_false),
   M("attachAuxEffect", "(I)I", at_false),
   M("setAuxEffectSendLevel", "(F)I", at_false),
   M("addOnRoutingChangedListener", "(Landroid/media/AudioRouting$OnRoutingChangedListener;Landroid/os/Handler;)V", nop_void),
   M("removeOnRoutingChangedListener", "(Landroid/media/AudioRouting$OnRoutingChangedListener;)V", nop_void),
   M("getBufferCapacityInFrames", "()I", at_capacity),
   M("setBufferSizeInFrames", "(I)I", at_capacity),
   M("getChannelConfiguration", "()I", at_channel_config),
   M_END,
};

#endif
