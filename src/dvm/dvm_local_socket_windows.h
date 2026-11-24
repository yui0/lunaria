/* SPDX-License-Identifier: MPL-2.0 */
/* Android filesystem LocalSocket client and its first-byte descriptor handoff. */
struct rt_local_socket { int fd, right; bool closed, wrote; };
static void local_socket_free(void *p)
{
  struct rt_local_socket *s = p;
  if (s->fd >= 0) luna_fd_close(s->fd);
  if (s->right >= 0) luna_fd_close(s->right);
  free(s);
}
static bool local_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                       int nargs, union dvm_value *out)
{
  (void)args; (void)nargs;
  struct dvm_object *o = dvm__obj(vm,self);
  struct rt_local_socket *s = calloc(1,sizeof *s);
  if (!s) { dvm__throw(vm,"java/lang/OutOfMemoryError","LocalSocket"); return false; }
  s->fd = s->right = -1;
  o->data = s;
  o->data_destroy = local_socket_free;
  RETV();
}
static bool local_addr_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
  dvm_set_field(vm,self,"name","Ljava/lang/String;",ARG(0));
  RETV();
}
static bool local_io(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                     int nargs, union dvm_value *out)
{
  const char *name = builtin_name();
  if (!strcmp(name,"write")) {
    union dvm_value socket = {0};
    dvm_get_field(vm,self,"socket","Landroid/net/LocalSocket;",&socket);
    self = socket.l;
  }
  struct dvm_object *o = dvm__obj(vm,self);
  struct rt_local_socket *s = o && o->data_destroy == local_socket_free ? o->data : NULL;
  if (!s) { dvm__throw(vm,"java/io/IOException","Uninitialized LocalSocket"); return false; }
  if (!strcmp(name,"close")) {
    if (s->fd >= 0) luna_fd_close(s->fd);
    if (s->right >= 0) luna_fd_close(s->right);
    s->fd = s->right = -1;
    s->closed = true;
    RETV();
  }
  if (s->closed) { dvm__throw(vm,"java/io/IOException","Closed LocalSocket"); return false; }
  if (!strcmp(name,"connect")) {
    union dvm_value path = {0};
    dvm_get_field(vm,ARG(0).l,"name","Ljava/lang/String;",&path);
    const char *value = path.l ? dvm_string_utf8(vm,path.l) : NULL;
    if (s->fd >= 0 || !value || value[0] != '/') {
      dvm__throw(vm,"java/io/IOException","Expected an unconnected filesystem LocalSocket"); return false;
    }
    char *copy = strdup(value);
    if (!copy) { dvm__throw(vm,"java/lang/OutOfMemoryError","LocalSocket address"); return false; }
    unsigned depth = dvm_gil_unlock_all(vm);
    int fd = arm_exec_local_connect(copy);
    int error = errno;
    free(copy);
    dvm_gil_relock(vm,depth);
    if (fd < 0 || s->closed || s->fd >= 0) {
      if (fd >= 0) luna_fd_close(fd);
      dvm__throw(vm,"java/io/IOException","LocalSocket connect: %s",strerror(fd < 0 ? error : EBUSY));
      return false;
    }
    s->fd = fd;
    RETV();
  }
  if (s->fd < 0) { dvm__throw(vm,"java/io/IOException","LocalSocket is not connected"); return false; }
  if (!strcmp(name,"setFileDescriptorsForSend")) {
    struct dvm_object *array = dvm__obj(vm,ARG(0).l);
    int right = -1;
    if (array && array->length) {
      if (array->kind != DVM_OBJ_ARRAY || array->length != 1 || s->wrote) {
        dvm__throw(vm,"java/io/IOException","Only one descriptor on the first write is supported"); return false;
      }
      right = luna_fd_dup(player_fd_of(vm,((dvm_ref *)array->data)[0]),0,0);
      if (right < 0) { dvm__throw(vm,"java/io/IOException","Invalid file descriptor"); return false; }
    }
    if (s->right >= 0) luna_fd_close(s->right);
    s->right = right;
    RETV();
  }
  if (!strcmp(name,"getOutputStream")) {
    dvm_ref stream = dvm_new_object(vm,dvm__class_by_desc(vm,"Landroid/net/LocalSocketImpl$SocketOutputStream;"));
    if (!stream) return false;
    dvm_set_field(vm,stream,"socket","Landroid/net/LocalSocket;",(union dvm_value){.l=self});
    RETL(stream);
  }
  int fd = luna_fd_dup(s->fd,0,0), right = s->right;
  unsigned char byte = (unsigned char)ARG(0).i;
  s->right = -1;
  s->wrote = true;
  unsigned depth = dvm_gil_unlock_all(vm);
  int rc = fd < 0 ? -1 : right >= 0 ? luna_local_send_right(fd,right,byte)
                                    : luna_socket_send(fd,&byte,1,0) == 1 ? 0 : -1;
  int error = errno;
  if (right >= 0) luna_fd_close(right);
  if (fd >= 0) luna_fd_close(fd);
  dvm_gil_relock(vm,depth);
  if (rc) { dvm__throw(vm,"java/io/IOException","LocalSocket write: %s",strerror(error)); return false; }
  RETV();
}
static const struct rt_method rt_local_socket[] = {
  M("<init>","()V",local_init), M("close","()V",local_io),
  M("connect","(Landroid/net/LocalSocketAddress;)V",local_io),
  M("setFileDescriptorsForSend","([Ljava/io/FileDescriptor;)V",local_io),
  M("getOutputStream","()Ljava/io/OutputStream;",local_io), M_END
};
static const struct rt_method rt_local_address[] = {
  M("<init>","(Ljava/lang/String;Landroid/net/LocalSocketAddress$Namespace;)V",local_addr_init), M_END
};
static const struct rt_method rt_local_out[] = { M("write","(I)V",local_io), M_END };
static const struct rt_field rt_local_addr_fields[] = { {"name","Ljava/lang/String;"}, F_END };
static const struct rt_field rt_local_out_fields[] = { {"socket","Landroid/net/LocalSocket;"}, F_END };
