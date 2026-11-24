/* SPDX-License-Identifier: MPL-2.0 */
#include "lunaria_os.h"
#include "lunaria_vpn.h"

#include <errno.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* The opaque handles and calling convention are from Wintun 0.14.1. */
struct wintun_api {
  void *(WINAPI *CreateAdapter)(LPCWSTR, LPCWSTR, const GUID *);
  void (WINAPI *CloseAdapter)(void *);
  void (WINAPI *GetAdapterLUID)(void *, NET_LUID *);
  void *(WINAPI *StartSession)(void *, DWORD);
  void (WINAPI *EndSession)(void *);
  HANDLE (WINAPI *GetReadWaitEvent)(void *);
  BYTE *(WINAPI *ReceivePacket)(void *, DWORD *);
  void (WINAPI *ReleaseReceivePacket)(void *, const BYTE *);
  BYTE *(WINAPI *AllocateSendPacket)(void *, DWORD);
  void (WINAPI *SendPacket)(void *, const BYTE *);
};

struct vpn_tunnel {
  HMODULE module;
  struct wintun_api api;
  HANDLE instance, stop, thread;
  WSAEVENT socket_event;
  void *adapter, *session;
  SOCKET socket;
  NET_LUID luid;
  NET_IFINDEX upstream[2];
  MIB_UNICASTIPADDRESS_ROW addresses[LUNA_VPN_MAX_ADDRESSES];
  MIB_IPFORWARD_ROW2 routes[LUNA_VPN_MAX_ROUTES * 2];
  unsigned naddresses, nroutes;
};

static SRWLOCK vpn_lock = SRWLOCK_INIT;
static struct vpn_tunnel *active_tunnel;
static bool initialized, prepared;

static int vpn_error(const char *operation, DWORD code)
{
  fprintf(stderr, "[vpn] %s failed (Windows error %lu)\n", operation, (unsigned long)code);
  switch (code) {
  case ERROR_ACCESS_DENIED: case ERROR_PRIVILEGE_NOT_HELD: errno = EACCES; break;
  case ERROR_INVALID_PARAMETER: errno = EINVAL; break;
  case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: errno = ENOMEM; break;
  case ERROR_ALREADY_EXISTS: case ERROR_OBJECT_ALREADY_EXISTS: case ERROR_BUSY: errno = EBUSY; break;
  case ERROR_NOT_SUPPORTED: case ERROR_PROC_NOT_FOUND: errno = ENOTSUP; break;
  case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_MOD_NOT_FOUND: errno = ENOENT; break;
  default: errno = EIO; break;
  }
  return -1;
}

static bool elevated(void)
{
  TOKEN_ELEVATION token;
  DWORD size;
  HANDLE handle;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &handle)) return false;
  BOOL result = GetTokenInformation(handle, TokenElevation, &token, sizeof token, &size);
  CloseHandle(handle);
  return result && token.TokenIsElevated != 0;
}

static int wintun_open(struct vpn_tunnel *tun)
{
  wchar_t path[32768];
  DWORD len = GetModuleFileNameW(NULL, path, (DWORD)(sizeof path / sizeof *path));
  if (!len || len >= sizeof path / sizeof *path) return vpn_error("locate executable", GetLastError());
  wchar_t *leaf = wcsrchr(path, L'\\');
  const wchar_t suffix[] = L"\\runtime\\wintun.dll";
  if (!leaf || (size_t)(leaf - path) + sizeof suffix / sizeof *suffix > sizeof path / sizeof *path) {
    errno = ENAMETOOLONG;
    return -1;
  }
  memcpy(leaf, suffix, sizeof suffix);
  tun->module = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!tun->module) return vpn_error("load runtime/wintun.dll", GetLastError());
#define LOAD_WINTUN(name) do {                                                   \
    FARPROC address = GetProcAddress(tun->module, "Wintun" #name);                 \
    if (!address) return vpn_error("resolve Wintun" #name, GetLastError());        \
    memcpy(&tun->api.name, &address, sizeof address);                             \
  } while (0)
  LOAD_WINTUN(CreateAdapter);
  LOAD_WINTUN(CloseAdapter);
  LOAD_WINTUN(GetAdapterLUID);
  LOAD_WINTUN(StartSession);
  LOAD_WINTUN(EndSession);
  LOAD_WINTUN(GetReadWaitEvent);
  LOAD_WINTUN(ReceivePacket);
  LOAD_WINTUN(ReleaseReceivePacket);
  LOAD_WINTUN(AllocateSendPacket);
  LOAD_WINTUN(SendPacket);
#undef LOAD_WINTUN
  return 0;
}

static SOCKADDR_INET socket_address(const struct luna_vpn_prefix *prefix)
{
  SOCKADDR_INET address = {0};
  if (prefix->family == 4) {
    address.Ipv4.sin_family = AF_INET;
    memcpy(&address.Ipv4.sin_addr, prefix->bytes, 4);
  } else {
    address.Ipv6.sin6_family = AF_INET6;
    memcpy(&address.Ipv6.sin6_addr, prefix->bytes, 16);
  }
  return address;
}

static NET_IFINDEX upstream_interface(ADDRESS_FAMILY family)
{
  PMIB_IPFORWARD_TABLE2 table;
  if (GetIpForwardTable2(family, &table) != NO_ERROR) return 0;
  NET_IFINDEX selected = 0;
  uint64_t best = UINT64_MAX;
  for (ULONG i = 0; i < table->NumEntries; ++i) {
    const MIB_IPFORWARD_ROW2 *route = &table->Table[i];
    if (route->Loopback || route->DestinationPrefix.PrefixLength != 0) continue;
    MIB_IPINTERFACE_ROW iface;
    InitializeIpInterfaceEntry(&iface);
    iface.Family = family;
    iface.InterfaceLuid = route->InterfaceLuid;
    if (GetIpInterfaceEntry(&iface) != NO_ERROR || !iface.Connected) continue;
    uint64_t metric = (uint64_t)iface.Metric + route->Metric;
    if (metric < best) {
      best = metric;
      selected = route->InterfaceIndex;
    }
  }
  FreeMibTable(table);
  return selected;
}

static int configure_interface(struct vpn_tunnel *tun, ADDRESS_FAMILY family, unsigned mtu)
{
  MIB_IPINTERFACE_ROW iface;
  InitializeIpInterfaceEntry(&iface);
  iface.Family = family;
  iface.InterfaceLuid = tun->luid;
  DWORD status = GetIpInterfaceEntry(&iface);
  if (status != NO_ERROR) return vpn_error("read Wintun interface", status);
  /* GetIpInterfaceEntry can return 64 for IPv4. SetIpInterfaceEntry requires
   * zero for IPv4 and accepts the existing site prefix length for IPv6. */
  if (family == AF_INET) iface.SitePrefixLength = 0;
  iface.NlMtu = mtu;
  iface.UseAutomaticMetric = FALSE;
  iface.Metric = 0;
  iface.RouterDiscoveryBehavior = RouterDiscoveryDisabled;
  status = SetIpInterfaceEntry(&iface);
  return status == NO_ERROR ? 0 : vpn_error("configure Wintun IP interface", status);
}

static int add_address(struct vpn_tunnel *tun, const struct luna_vpn_prefix *prefix)
{
  MIB_UNICASTIPADDRESS_ROW row;
  InitializeUnicastIpAddressEntry(&row);
  row.InterfaceLuid = tun->luid;
  row.Address = socket_address(prefix);
  row.OnLinkPrefixLength = prefix->bits;
  row.DadState = IpDadStatePreferred;
  DWORD status = CreateUnicastIpAddressEntry(&row);
  if (status != NO_ERROR) return vpn_error("assign Wintun address", status);
  tun->addresses[tun->naddresses++] = row;
  return 0;
}

static int add_route(struct vpn_tunnel *tun, const struct luna_vpn_prefix *prefix)
{
  MIB_IPFORWARD_ROW2 row;
  InitializeIpForwardEntry(&row);
  row.InterfaceLuid = tun->luid;
  row.DestinationPrefix.Prefix = socket_address(prefix);
  row.DestinationPrefix.PrefixLength = prefix->bits;
  row.NextHop.si_family = row.DestinationPrefix.Prefix.si_family;
  row.Metric = 0;
  row.Protocol = MIB_IPPROTO_NETMGMT;
  row.Loopback = FALSE;
  row.AutoconfigureAddress = FALSE;
  row.Publish = FALSE;
  row.Immortal = FALSE;
  DWORD status = CreateIpForwardEntry2(&row);
  if (status != NO_ERROR) return vpn_error("install Wintun route", status);
  tun->routes[tun->nroutes++] = row;
  return 0;
}

/* Windows 10 2004+ exposes this API. Some MinGW headers omit its settings type. */
struct vpn_dns_settings {
  ULONG version;
  ULONG64 flags;
  PWSTR domain, nameserver, search_list;
  ULONG registration_enabled, register_adapter_name, enable_llmnr, query_adapter_name;
  PWSTR profile_nameserver;
};

static int configure_dns(struct vpn_tunnel *tun, const struct luna_vpn_config *config, uint8_t family)
{
  wchar_t servers[LUNA_VPN_MAX_DNS * (INET6_ADDRSTRLEN + 1)] = {0};
  size_t length = 0;
  for (unsigned i = 0; i < config->ndns; ++i) {
    const struct luna_vpn_prefix *dns = &config->dns[i];
    if (dns->family != family) continue;
    if (length) servers[length++] = L',';
    if (!InetNtopW(family == 4 ? AF_INET : AF_INET6, dns->bytes, servers + length,
                   sizeof servers / sizeof *servers - length))
      return vpn_error("format Wintun DNS address", (DWORD)WSAGetLastError());
    length += wcslen(servers + length);
  }
  if (!length) return 0;
  typedef DWORD (WINAPI *set_dns_fn)(GUID, const struct vpn_dns_settings *);
  FARPROC address = GetProcAddress(GetModuleHandleW(L"iphlpapi.dll"), "SetInterfaceDnsSettings");
  if (!address) return vpn_error("resolve SetInterfaceDnsSettings", GetLastError());
  set_dns_fn set_dns;
  memcpy(&set_dns, &address, sizeof address);
  GUID guid;
  DWORD status = ConvertInterfaceLuidToGuid(&tun->luid, &guid);
  if (status != NO_ERROR) return vpn_error("resolve Wintun interface GUID", status);
  const struct vpn_dns_settings settings = {
    .version = 1, .flags = 0x0002 | (family == 6 ? 0x0001 : 0), .nameserver = servers,
  };
  status = set_dns(guid, &settings);
  return status == NO_ERROR ? 0 : vpn_error("set Wintun DNS", status);
}

static void unconfigure(struct vpn_tunnel *tun)
{
  while (tun->nroutes) {
    DWORD status = DeleteIpForwardEntry2(&tun->routes[--tun->nroutes]);
    if (status != NO_ERROR && status != ERROR_NOT_FOUND) vpn_error("remove Wintun route", status);
  }
  while (tun->naddresses) {
    DWORD status = DeleteUnicastIpAddressEntry(&tun->addresses[--tun->naddresses]);
    if (status != NO_ERROR && status != ERROR_NOT_FOUND) vpn_error("remove Wintun address", status);
  }
}

static bool valid_packet(const BYTE *packet, DWORD size)
{
  if (!size || size > LUNA_VPN_MAX_PACKET) return false;
  unsigned version = packet[0] >> 4;
  if (version == 4 && size >= 20) {
    unsigned header = (packet[0] & 15) * 4;
    return header >= 20 && header <= size && ((DWORD)packet[2] * 256 + packet[3]) == size;
  }
  return version == 6 && size >= 40 && 40 + (DWORD)packet[4] * 256 + packet[5] == size;
}

/* Each loopback datagram carries a complete IP packet. The pair uses two
 * exclusive, connected endpoints and never listens on a public iface. */
static unsigned __stdcall packet_bridge(void *opaque)
{
  struct vpn_tunnel *tun = opaque;
  HANDLE events[] = {tun->stop, tun->api.GetReadWaitEvent(tun->session), tun->socket_event};
  BYTE packet[LUNA_VPN_MAX_PACKET];
  DWORD error = 0;
  for (;;) {
    if (WaitForSingleObject(tun->stop, 0) == WAIT_OBJECT_0) break;
    unsigned delivered = 0, injected = 0;
    for (; delivered < 64; ++delivered) {
      DWORD size;
      BYTE *raw = tun->api.ReceivePacket(tun->session, &size);
      if (!raw) {
        error = GetLastError();
        if (error != ERROR_NO_MORE_ITEMS) goto done;
        error = 0;
        break;
      }
      if (valid_packet(raw, size)) {
        if (send(tun->socket, (const char *)raw, (int)size, 0) == SOCKET_ERROR)
          error = (DWORD)WSAGetLastError();

      }
      tun->api.ReleaseReceivePacket(tun->session, raw);
      if (error && error != WSAEWOULDBLOCK && error != WSAENOBUFS) goto done;
      error = 0;
    }
    for (; injected < 64; ++injected) {
      int size = recv(tun->socket, (char *)packet, sizeof packet, 0);
      if (size == SOCKET_ERROR) {
        error = (DWORD)WSAGetLastError();
        if (error == WSAEMSGSIZE) continue;
        if (error != WSAEWOULDBLOCK) goto done;
        error = 0;
        break;
      }
      if (!valid_packet(packet, (DWORD)size)) continue;
      BYTE *raw = tun->api.AllocateSendPacket(tun->session, (DWORD)size);
      if (!raw) {
        error = GetLastError();
        if (error != ERROR_BUFFER_OVERFLOW) goto done;
        error = 0;
        continue;
      }
      memcpy(raw, packet, (size_t)size);
      tun->api.SendPacket(tun->session, raw);
    }
    if (delivered == 64 || injected == 64) continue;
    DWORD ready = WaitForMultipleObjects(3, events, FALSE, INFINITE);
    if (ready == WAIT_OBJECT_0) break;
    if (ready == WAIT_OBJECT_0 + 2) {
      WSANETWORKEVENTS changes;
      if (WSAEnumNetworkEvents(tun->socket, tun->socket_event, &changes)) {
        error = (DWORD)WSAGetLastError();
        break;
      }
      if (changes.lNetworkEvents & FD_CLOSE) {
        error = ERROR_BROKEN_PIPE;
        break;
      }
    } else if (ready != WAIT_OBJECT_0 + 1) {
      error = GetLastError();
      break;
    }
  }
done:
  if (error) {
    vpn_error("packet bridge", error);
    unconfigure(tun);
  }
  return 0;
}

static void close_tunnel(struct vpn_tunnel *tun)
{
  if (tun->thread) {
    SetEvent(tun->stop);
    WaitForSingleObject(tun->thread, INFINITE);
    CloseHandle(tun->thread);
    tun->thread = NULL;
  }
  unconfigure(tun);
  if (tun->session) tun->api.EndSession(tun->session);
  if (tun->adapter) tun->api.CloseAdapter(tun->adapter);
  if (tun->socket != INVALID_SOCKET) closesocket(tun->socket);
  if (tun->socket_event != WSA_INVALID_EVENT) WSACloseEvent(tun->socket_event);
  if (tun->stop) CloseHandle(tun->stop);
  if (tun->module) FreeLibrary(tun->module);
  if (tun->instance) CloseHandle(tun->instance);
  if (active_tunnel == tun) active_tunnel = NULL;
}

static void last_close(void *opaque)
{
  struct vpn_tunnel *tun = opaque;
  AcquireSRWLockExclusive(&vpn_lock);
  if (active_tunnel == tun) close_tunnel(tun);
  ReleaseSRWLockExclusive(&vpn_lock);
  free(tun);
}

static void process_exit(void)
{
  AcquireSRWLockExclusive(&vpn_lock);
  if (active_tunnel) close_tunnel(active_tunnel);
  ReleaseSRWLockExclusive(&vpn_lock);
}

void luna_vpn_init(bool enabled)
{
  /* An APK cannot opt itself in by changing its emulated environment. */
  if (initialized) return;
  initialized = true;
  prepared = enabled && atexit(process_exit) == 0;
}

bool luna_vpn_prepared(void)
{
  return prepared;
}

int luna_vpn_establish(const struct luna_vpn_config *config)
{
  if (!prepared || !elevated()) {
    fprintf(stderr, "[vpn] host VPN requires opt-in and an elevated Windows process\n");
    errno = EACCES;
    return -1;
  }
  if (!config || !config->naddresses || config->naddresses > LUNA_VPN_MAX_ADDRESSES ||
      config->nroutes > LUNA_VPN_MAX_ROUTES || config->ndns > LUNA_VPN_MAX_DNS ||
      config->mtu < 68 || config->mtu > LUNA_VPN_MAX_PACKET) {
    errno = EINVAL;
    return -1;
  }

  AcquireSRWLockExclusive(&vpn_lock);
  int guest = -1;
  struct vpn_tunnel *tun = NULL;
  if (active_tunnel) {
    errno = EBUSY;
    goto fail;
  }
  tun = calloc(1, sizeof *tun);
  if (!tun) {
    errno = ENOMEM;
    goto fail;
  }
  tun->socket = INVALID_SOCKET;
  tun->socket_event = WSA_INVALID_EVENT;
  tun->instance = CreateMutexW(NULL, FALSE, L"Global\\LunariaHostVpnSession");
  DWORD mutex_status = GetLastError();
  if (!tun->instance || mutex_status == ERROR_ALREADY_EXISTS) {
    vpn_error("acquire the host VPN session", mutex_status);
    goto fail;
  }
  if (wintun_open(tun)) goto fail;
  tun->upstream[0] = upstream_interface(AF_INET);
  tun->upstream[1] = upstream_interface(AF_INET6);
  tun->adapter = tun->api.CreateAdapter(L"Lunaria VPN", L"Lunaria", NULL);
  if (!tun->adapter) {
    vpn_error("create Wintun adapter", GetLastError());
    goto fail;
  }
  tun->api.GetAdapterLUID(tun->adapter, &tun->luid);
  tun->session = tun->api.StartSession(tun->adapter, 0x400000);
  if (!tun->session) {
    vpn_error("start Wintun session", GetLastError());
    goto fail;
  }

  int fds[2];
  if (luna_socket_pair(AF_UNIX, SOCK_DGRAM, 0, fds)) goto fail;
  guest = fds[0];
  /* Keep the host end outside the guest's integer descriptor table. */
  WSAPROTOCOL_INFOW protocol;
  int duplicated = WSADuplicateSocketW((SOCKET)luna_socket_native(fds[1]),
                                       GetCurrentProcessId(), &protocol);
  if (!duplicated)
    tun->socket = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO,
                             &protocol, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
  DWORD socket_error = (DWORD)WSAGetLastError();
  luna_fd_close(fds[1]);
  if (tun->socket == INVALID_SOCKET) {
    vpn_error("create private packet endpoint", socket_error);
    goto fail;
  }
  tun->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
  tun->socket_event = WSACreateEvent();
  if (!tun->stop || tun->socket_event == WSA_INVALID_EVENT) {
    errno = ENOMEM;
    goto fail;
  }
  if (WSAEventSelect(tun->socket, tun->socket_event, FD_READ | FD_CLOSE)) {
    vpn_error("register Wintun packet notifications", (DWORD)WSAGetLastError());
    goto fail;
  }
  if (luna_fd_nonblock(guest, !config->blocking)) goto fail;

  bool ipv4 = false, ipv6 = false;
  for (unsigned i = 0; i < config->naddresses; ++i) {
    ipv4 |= config->addresses[i].family == 4;
    ipv6 |= config->addresses[i].family == 6;
  }
  if ((ipv4 && configure_interface(tun, AF_INET, config->mtu)) ||
      (ipv6 && configure_interface(tun, AF_INET6, config->mtu))) goto fail;
  for (unsigned i = 0; i < config->naddresses; ++i)
    if (add_address(tun, &config->addresses[i])) goto fail;
  if (configure_dns(tun, config, 4) || configure_dns(tun, config, 6)) goto fail;
  for (unsigned i = 0; i < config->nroutes; ++i) {
    struct luna_vpn_prefix route = config->routes[i];
    if (route.bits) {
      if (add_route(tun, &route)) goto fail;
    } else {
      /* Leave the existing default in place for sockets bound via protect(). */
      route.bits = 1;
      if (add_route(tun, &route)) goto fail;
      route.bytes[0] = 0x80;
      if (add_route(tun, &route)) goto fail;
    }
  }
  tun->thread = (HANDLE)_beginthreadex(NULL, 0, packet_bridge, tun, 0, NULL);
  if (!tun->thread || luna_socket_on_last_close(guest, last_close, tun)) goto fail;
  active_tunnel = tun;
  ReleaseSRWLockExclusive(&vpn_lock);
  return guest;
fail:
  {
    int saved = errno;
    if (guest >= 0) luna_fd_close(guest);
    if (tun) {
      close_tunnel(tun);
      free(tun);
    }
    ReleaseSRWLockExclusive(&vpn_lock);
    errno = saved;
    return -1;
  }
}

bool luna_vpn_protect(int fd)
{
  if (!prepared) return false;
  SOCKET socket = (SOCKET)luna_socket_native(fd);
  if (socket == INVALID_SOCKET) return false;
  WSAPROTOCOL_INFOW info;
  int size = sizeof info;
  if (getsockopt(socket, SOL_SOCKET, SO_PROTOCOL_INFOW, (char *)&info, &size)) return false;
  ADDRESS_FAMILY family = (ADDRESS_FAMILY)info.iAddressFamily;
  if (family != AF_INET && family != AF_INET6) return false;
  DWORD v6_only = 0;
  if (family == AF_INET6) {
    size = sizeof v6_only;
    if (getsockopt(socket, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&v6_only, &size)) return false;
  }
  AcquireSRWLockShared(&vpn_lock);
  DWORD index4 = v6_only ? 0 :
    active_tunnel ? active_tunnel->upstream[0] : upstream_interface(AF_INET);
  DWORD index6 = family != AF_INET6 ? 0 :
    active_tunnel ? active_tunnel->upstream[1] : upstream_interface(AF_INET6);
  ReleaseSRWLockShared(&vpn_lock);
  if (index4) {
    DWORD value = htonl(index4);
    if (setsockopt(socket, IPPROTO_IP, IP_UNICAST_IF, (const char *)&value, sizeof value))
      return false;
  }
  if (index6 && setsockopt(socket, IPPROTO_IPV6, IPV6_UNICAST_IF,
                           (const char *)&index6, sizeof index6))
    return false;
  return index4 || index6;
}
