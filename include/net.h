#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * networking: an rtl8139 ethernet card, with a small ipv4 stack on top of it,
 * which is just enough for a web browser. arp finds the hardware address that
 * goes with an ip address, dhcp gets us an address from the network at boot,
 * dns turns names into addresses, icmp answers (and sends) pings, and tcp
 * carries http.
 *
 * ip addresses are kept as plain numbers with the first byte at the top, so
 * 10.0.2.15 is 0x0a00020f. they only get turned round into network byte order
 * when they actually go out on the wire.
 *
 * frames come in on the card's interrupt and get handled right there, replies
 * and all. anything that has to wait for an answer (dhcp, dns, connecting,
 * ping) has to run in a task and never in an interrupt, and it sleeps while
 * it waits so the rest of the system carries on.
 */

#define IP(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

struct net_config {
    bool card;          /* we found a card we know how to drive */
    bool configured;    /* we've got an ip address */
    uint8_t mac[6];
    uint32_t ip, mask, gateway, dns, dhcp_server;
    uint32_t lease_secs;
    uint32_t rx_frames, tx_frames, rx_bytes, tx_bytes;
};

extern struct net_config net;
extern volatile bool net_in_irq;

/* errors are negative numbers, just like the file system's. */
enum {
    NET_ERR_NO_CARD = -1,
    NET_ERR_NO_ADDRESS = -2,
    NET_ERR_TIMEOUT = -3,
    NET_ERR_NO_HOST = -4,
    NET_ERR_REFUSED = -5,
    NET_ERR_RESET = -6,
    NET_ERR_FULL = -7,
    NET_ERR_CLOSED = -8,
    NET_ERR_BAD_URL = -9,
    NET_ERR_NO_ROUTE = -10,
};

const char *net_error(int err);

/* finds the card and starts it up. false if there isn't one. */
bool net_init(void);

/*
 * runs dhcp, waiting up to about four seconds. true once we've got an
 * address.
 */
bool net_dhcp(void);

/* the driver calls this for every frame that arrives. */
void net_receive(const uint8_t *frame, int len);

/* like "10.0.2.15" */
void net_format_ip(uint32_t ip, char *out);
bool net_parse_ip(const char *s, uint32_t *ip);

/*
 * sends one ping, and gives back how long the round trip took in ticks, or an
 * error.
 */
int net_ping(uint32_t ip, uint16_t seq, uint32_t timeout_ticks);

/*
 * looks up a name (or just reads a dotted address) and puts the address in
 * ip. 0, or an error.
 */
int net_resolve(const char *name, uint32_t *ip);

/* ip and udp, for the protocols that are built on top of them. */
int ip_send(uint32_t dst, uint8_t proto, const void *payload, int len);
int udp_send(uint32_t dst, uint16_t sport, uint16_t dport, const void *data, int len);
typedef void (*udp_handler_t)(uint32_t src, uint16_t sport, const uint8_t *data, int len);
bool udp_bind(uint16_t port, udp_handler_t handler);
void udp_unbind(uint16_t port);

/*
 * the internet checksum, folded down to 16 bits. start is a partial sum to
 * carry on from (0 to start fresh).
 */
uint16_t net_checksum(const void *data, int len, uint32_t start);

/* for /proc/net. */
int net_describe(char *buf, int size);

/*
 * tcp, but only as a client. tcp_connect gives back a socket number, or an
 * error. tcp_write waits until the other side has said it got the data.
 * tcp_read copies out whatever has turned up so far: it gives back how many
 * bytes it copied, 0 if nothing's waiting yet, or net_err_closed once the
 * other side has hung up and everything has been read. tcp_close says goodbye
 * and frees the socket.
 */
int tcp_connect(uint32_t ip, uint16_t port, uint32_t timeout_ticks);
int tcp_write(int s, const void *data, int len);
int tcp_read(int s, void *buf, int max);
void tcp_close(int s);

/*
 * AI Contribution (Bug Fix): drops every connection a task had open, for when
 * the task is killed and can't close them itself. the other end gets a reset.
 */
void tcp_drop_task(int pid);
void tcp_receive(uint32_t src, const uint8_t *seg, int len);

/* the network card's driver. */
bool rtl8139_init(uint8_t mac[6]);
void rtl8139_send(const void *frame, int len);
uint8_t rtl8139_irq(void);
