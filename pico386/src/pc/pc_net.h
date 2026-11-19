// pc_net.h -- the PC's network: the NE2000's other end (pc_net.cpp).
//
// The guest sees a LAN with a router at 10.0.2.2 that hands out 10.0.2.15 by DHCP and answers
// DNS at 10.0.2.3, as QEMU's slirp does. Behind it, the guest's TCP and UDP are carried on the
// host's sockets (lwIP over the P4's Wi-Fi, Winsock on the desktop). With a proxy set, the guest's
// HTTP (port 80) goes to it, rewritten as proxy requests, so a 1990s browser needs no setup.
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- for the core (tiny386/ne2000.c) ----
int  pcnet_enabled(void);                       // build the card at all
void pcnet_guest_mac(uint8_t mac[6]);
void pcnet_guest_frame(const uint8_t *buf, int len);   // the guest sent a frame
// Hands queued frames to the card while can_rx says the ring has room, and services the sockets.
void pcnet_poll(void *nic, int (*can_rx)(void *nic), void (*rx)(void *nic, const uint8_t *buf, int len));

// ---- for the frontend ----
// enabled: the next machine gets the card. proxy_host "" = no proxy. Takes effect at once for
// new connections.
void pcnet_configure(int enabled, const char *proxy_host, int proxy_port);
void pcnet_reset(void);                         // drop every connection (the machine stopped)
// One short line: connections and traffic, for the settings screen.
void pcnet_status(char *buf, size_t n);

#ifdef __cplusplus
}
#endif
