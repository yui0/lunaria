/* SPDX-License-Identifier: MPL-2.0 */
/* Android's raw-IP VPN descriptor backed by the Windows host interface. */

static bool vpn_prepare(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
  (void)self;
  (void)nargs;
  if (!ARG(0).l) {
    dvm__throw(vm, "java/lang/NullPointerException", "context");
    return false;
  }
  if (luna_vpn_prepared()) RETL(0);
  struct dvm_class *klass = dvm_find_class(vm, "android/content/Intent");
  dvm_ref intent = klass ? dvm_new_object(vm, klass) : 0;
  dvm_ref action = dvm_new_string(vm, LUNA_VPN_CONSENT_ACTION);
  if (!intent || !action) return false;
  dvm_set_field(vm, intent, "action", "Ljava/lang/String;", (union dvm_value){.l=action});
  RETL(intent);
}

static bool vpn_protect(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
  (void)self;
  (void)nargs;
  const struct dvm_method *method = dvm__builtin_method();
  int fd;
  if (method->sig[1] == 'I') {
    fd = ARG(0).i;
  } else {
    if (!ARG(0).l) {
      dvm__throw(vm, "java/lang/NullPointerException", "socket");
      return false;
    }
    if (object_is_a(vm, ARG(0).l, "Ljava/net/DatagramSocket;")) {
      fd = dgs_fd(vm, ARG(0).l);
    } else {
      struct rt_sock *socket = sock_of(vm, ARG(0).l);
      fd = socket && !socket->connected ? socket->fd : -1;
    }
  }
  RETI(luna_vpn_protect(fd));
}

static const struct rt_method rt_vpn_service[] = {
  M("<init>", "()V", empty_void),
  SM("prepare", "(Landroid/content/Context;)Landroid/content/Intent;", vpn_prepare),
  M("protect", "(I)Z", vpn_protect),
  M("protect", "(Ljava/net/Socket;)Z", vpn_protect),
  M("protect", "(Ljava/net/DatagramSocket;)Z", vpn_protect),
  M_END,
};

static struct luna_vpn_config *vpn_builder(struct dvm *vm, dvm_ref self)
{
  struct dvm_object *object = dvm__obj(vm, self);
  struct luna_vpn_config *config = object && object->data_destroy == free ? object->data : NULL;
  if (!config) dvm__throw(vm, "java/lang/IllegalStateException", "VPN builder is not initialized");
  return config;
}

static bool vpn_builder_init(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
  if (!ARG(0).l) {
    dvm__throw(vm, "java/lang/NullPointerException", "VpnService");
    return false;
  }
  struct dvm_object *object = dvm__obj(vm, self);
  struct luna_vpn_config *config = calloc(1, sizeof *config);
  if (!config) {
    dvm__throw(vm, "java/lang/OutOfMemoryError", "VPN builder");
    return false;
  }
  config->mtu = 1500;
  if (object->data_destroy == free) free(object->data);
  object->data = config;
  object->data_destroy = free;
  RETV();
}

static bool vpn_builder_options(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                int nargs, union dvm_value *out)
{
  struct luna_vpn_config *config = vpn_builder(vm, self);
  if (!config) return false;
  const char *method = builtin_name();
  if (!strcmp(method, "setMtu")) {
    int mtu = ARG(0).i;
    if (mtu < 68 || mtu > LUNA_VPN_MAX_PACKET) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "unsupported VPN MTU");
      return false;
    }
    config->mtu = (unsigned)mtu;
  } else if (!strcmp(method, "setBlocking")) {
    config->blocking = ARG(0).i != 0;
  }
  /* The Android session label does not affect routing or the descriptor. */
  RETL(self);
}

static bool vpn_push_prefix(struct dvm *vm, struct luna_vpn_prefix *items,
                             unsigned *count, unsigned max, struct luna_vpn_prefix prefix)
{
  for (unsigned i = 0; i < *count; ++i)
    if (items[i].family == prefix.family && items[i].bits == prefix.bits &&
        !memcmp(items[i].bytes, prefix.bytes, sizeof prefix.bytes)) return true;
  if (*count == max) {
    dvm__throw(vm, "java/lang/IllegalStateException", "VPN address or route limit reached");
    return false;
  }
  items[(*count)++] = prefix;
  return true;
}

static bool vpn_builder_address(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                int nargs, union dvm_value *out)
{
  struct luna_vpn_config *config = vpn_builder(vm, self);
  if (!config) return false;
  const char *method = builtin_name();
  bool dns = !strcmp(method, "addDnsServer"), route = !strcmp(method, "addRoute");
  struct luna_vpn_prefix prefix = {0};
  if (object_is_a(vm, ARG(0).l, "Ljava/net/InetAddress;")) {
    int length = inet_bytes(vm, ARG(0).l, prefix.bytes);
    prefix.family = length == 4 ? 4 : length == 16 ? 6 : 0;
  } else {
    const char *literal = ARG(0).l ? dvm_string_utf8(vm, ARG(0).l) : NULL;
    if (literal && inet_pton(AF_INET, literal, prefix.bytes) == 1) prefix.family = 4;
    else if (literal && inet_pton(AF_INET6, literal, prefix.bytes) == 1) prefix.family = 6;
  }
  int maximum_bits = prefix.family == 4 ? 32 : 128;
  int bits = dns ? maximum_bits : ARG(1).i;
  if (!prefix.family || bits < 0 || bits > maximum_bits) {
    dvm__throw(vm, "java/lang/IllegalArgumentException", "invalid VPN IP address or prefix");
    return false;
  }
  prefix.bits = (uint8_t)bits;
  if (route) {
    for (int i = bits; i < maximum_bits; ++i) {
      if (prefix.bytes[i / 8] & (128 >> (i % 8))) {
        dvm__throw(vm, "java/lang/IllegalArgumentException", "VPN route has host bits set");
        return false;
      }
    }
  } else {
    static const uint8_t empty[16];
    bool loopback = prefix.family == 4 ? prefix.bytes[0] == 127 :
      !memcmp(prefix.bytes, empty, 15) && prefix.bytes[15] == 1;
    if (loopback || !memcmp(prefix.bytes, empty, sizeof empty)) {
      dvm__throw(vm, "java/lang/IllegalArgumentException", "invalid VPN interface or DNS address");
      return false;
    }
  }
  bool added = dns
    ? vpn_push_prefix(vm, config->dns, &config->ndns, LUNA_VPN_MAX_DNS, prefix)
    : route
      ? vpn_push_prefix(vm, config->routes, &config->nroutes, LUNA_VPN_MAX_ROUTES, prefix)
      : vpn_push_prefix(vm, config->addresses, &config->naddresses, LUNA_VPN_MAX_ADDRESSES, prefix);
  if (!added) return false;
  RETL(self);
}

static bool vpn_builder_per_app(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                int nargs, union dvm_value *out)
{
  (void)self;
  (void)args;
  (void)nargs;
  (void)out;
  dvm__throw(vm, "java/lang/UnsupportedOperationException",
             "Windows host VPN routing cannot select Android packages");
  return false;
}

static bool vpn_builder_establish(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                                  int nargs, union dvm_value *out)
{
  (void)args;
  (void)nargs;
  if (!luna_vpn_prepared()) RETL(0);
  struct luna_vpn_config *config = vpn_builder(vm, self);
  if (!config) return false;
  if (!config->naddresses) {
    dvm__throw(vm, "java/lang/IllegalArgumentException", "VPN has no interface address");
    return false;
  }
  struct luna_vpn_config request = *config;
  unsigned gil = dvm_gil_unlock_all(vm);
  int fd = luna_vpn_establish(&request);
  int error = errno;
  dvm_gil_relock(vm, gil);
  if (fd < 0) {
    dvm__throw(vm, "java/lang/IllegalStateException", "Windows VPN establish: %s", strerror(error));
    return false;
  }
  dvm_ref descriptor = parcel_fd_adopt(vm, fd);
  if (!descriptor) return false;
  RETL(descriptor);
}

#define VPN_BUILDER "Landroid/net/VpnService$Builder;"
static const struct rt_method rt_vpn_builder[] = {
  M("<init>", "(Landroid/net/VpnService;)V", vpn_builder_init),
  M("setMtu", "(I)" VPN_BUILDER, vpn_builder_options),
  M("setBlocking", "(Z)" VPN_BUILDER, vpn_builder_options),
  M("setSession", "(Ljava/lang/String;)" VPN_BUILDER, vpn_builder_options),
  M("addAddress", "(Ljava/lang/String;I)" VPN_BUILDER, vpn_builder_address),
  M("addAddress", "(Ljava/net/InetAddress;I)" VPN_BUILDER, vpn_builder_address),
  M("addRoute", "(Ljava/lang/String;I)" VPN_BUILDER, vpn_builder_address),
  M("addRoute", "(Ljava/net/InetAddress;I)" VPN_BUILDER, vpn_builder_address),
  M("addDnsServer", "(Ljava/lang/String;)" VPN_BUILDER, vpn_builder_address),
  M("addDnsServer", "(Ljava/net/InetAddress;)" VPN_BUILDER, vpn_builder_address),
  M("addAllowedApplication", "(Ljava/lang/String;)" VPN_BUILDER, vpn_builder_per_app),
  M("addDisallowedApplication", "(Ljava/lang/String;)" VPN_BUILDER, vpn_builder_per_app),
  M("establish", "()Landroid/os/ParcelFileDescriptor;", vpn_builder_establish),
  M_END,
};
#undef VPN_BUILDER
