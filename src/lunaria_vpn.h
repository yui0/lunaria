/* SPDX-License-Identifier: MPL-2.0 */
#ifndef LUNARIA_VPN_H
#define LUNARIA_VPN_H

#include <stdbool.h>
#include <stdint.h>

#define LUNA_VPN_CONSENT_ACTION "lunaria.intent.action.HOST_VPN_CONSENT"
#define LUNA_VPN_MAX_ADDRESSES 16
#define LUNA_VPN_MAX_ROUTES 128
#define LUNA_VPN_MAX_DNS 8
/* An IP packet occupies one datagram on the private IPv4 loopback pair. */
#define LUNA_VPN_MAX_PACKET 65507

struct luna_vpn_prefix {
  uint8_t family; /* 4 or 6 */
  uint8_t bits;
  uint8_t bytes[16];
};

struct luna_vpn_config {
  unsigned mtu;
  bool blocking;
  unsigned naddresses, nroutes, ndns;
  struct luna_vpn_prefix addresses[LUNA_VPN_MAX_ADDRESSES];
  struct luna_vpn_prefix routes[LUNA_VPN_MAX_ROUTES];
  struct luna_vpn_prefix dns[LUNA_VPN_MAX_DNS];
};

void luna_vpn_init(bool enabled);
bool luna_vpn_prepared(void);
/* Returns an owning guest descriptor, or -1 with errno set. */
int luna_vpn_establish(const struct luna_vpn_config *config);
bool luna_vpn_protect(int fd);

#endif
