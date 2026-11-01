#include <cpu.h>
#include <net.h>
#include <string.h>
#include <task.h>

/*
 * the bottom part of the network stack: ethernet frames, arp, ipv4, icmp and
 * udp, plus the two udp protocols every machine needs, dhcp and dns. tcp
 * lives in tcp.c.
 *
 * everything that arrives gets handled on the card's interrupt (see
 * net_receive). anything that's waiting for an answer sleeps a tick at a time
 * in its own task and then checks again, so a slow network never holds up the
 * rest of the system.
 */

struct net_config net;
volatile bool net_in_irq;

#define ETH_HEADER  14
#define IP_HEADER   20
#define UDP_HEADER  8
#define ETH_IP      0x0800
#define ETH_ARP     0x0806

static const uint8_t broadcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* ---- bytes on the wire are big-endian ---- */

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = v >> 8;
    p[1] = v;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static uint16_t get16(const uint8_t *p)
{
    return p[0] << 8 | p[1];
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* the 16-bit ones' complement sum, folded down at the end. */
uint16_t net_checksum(const void *data, int len, uint32_t start)
{
    const uint8_t *p = data;
    uint32_t sum = start;
    for (; len > 1; len -= 2, p += 2)
        sum += p[0] << 8 | p[1];
    if (len)
        sum += p[0] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return ~sum & 0xFFFF;
}

void net_format_ip(uint32_t ip, char *out)
{
    ksnprintf(out, 16, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 0xFF, ip >> 8 & 0xFF, ip & 0xFF);
}

bool net_parse_ip(const char *s, uint32_t *ip)
{
    uint32_t value = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9')
            return false;
        uint32_t n = 0;
        while (*s >= '0' && *s <= '9')
            n = n * 10 + (uint32_t)(*s++ - '0');
        if (n > 255)
            return false;
        value = value << 8 | n;
        if (part < 3 && *s++ != '.')
            return false;
    }
    if (*s)
        return false;
    *ip = value;
    return true;
}

const char *net_error(int err)
{
    switch (err) {
    case NET_ERR_NO_CARD: return "no network card (start qemu with -nic user,model=rtl8139)";
    case NET_ERR_NO_ADDRESS: return "no network address yet (try dhcp)";
    case NET_ERR_TIMEOUT: return "timed out";
    case NET_ERR_NO_HOST: return "host not found";
    case NET_ERR_REFUSED: return "connection refused";
    case NET_ERR_RESET: return "connection reset";
    case NET_ERR_FULL: return "too many connections";
    case NET_ERR_CLOSED: return "connection closed";
    case NET_ERR_BAD_URL: return "bad address";
    case NET_ERR_NO_ROUTE: return "no route to host";
    }
    return "network error";
}

static void send_frame(const uint8_t *dst_mac, uint16_t type, uint8_t *frame, int payload_len)
{
    memcpy(frame, dst_mac, 6);
    memcpy(frame + 6, net.mac, 6);
    put16(frame + 12, type);
    rtl8139_send(frame, ETH_HEADER + payload_len);
    net.tx_frames++;
    net.tx_bytes += ETH_HEADER + payload_len;
}

/* ---- arp ---- */

/*
 * a small cache of which hardware address goes with which ip address. it
 * learns from arp replies, from arp requests aimed at us, and from whoever
 * sent each ip packet on our own network, so replying to something that just
 * arrived never has to wait.
 */

#define ARP_ENTRIES 16

static struct {
    uint32_t ip;
    uint8_t mac[6];
} arp_cache[ARP_ENTRIES];
static int arp_used, arp_next;

static void arp_learn(uint32_t ip, const uint8_t *mac)
{
    if (!ip)
        return;
    for (int i = 0; i < arp_used; i++) {
        if (arp_cache[i].ip == ip) {
            memcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    }
    int i = arp_used < ARP_ENTRIES ? arp_used++ : arp_next++ % ARP_ENTRIES;
    arp_cache[i].ip = ip;
    memcpy(arp_cache[i].mac, mac, 6);
}

static bool arp_lookup(uint32_t ip, uint8_t *mac)
{
    for (int i = 0; i < arp_used; i++) {
        if (arp_cache[i].ip == ip) {
            memcpy(mac, arp_cache[i].mac, 6);
            return true;
        }
    }
    return false;
}

static void arp_send(uint16_t op, const uint8_t *target_mac, uint32_t target_ip)
{
    uint8_t frame[ETH_HEADER + 28];
    uint8_t *a = frame + ETH_HEADER;
    put16(a, 1);            /* ethernet */
    put16(a + 2, ETH_IP);
    a[4] = 6;
    a[5] = 4;
    put16(a + 6, op);
    memcpy(a + 8, net.mac, 6);
    put32(a + 14, net.ip);
    memcpy(a + 18, op == 1 ? (const uint8_t *)"\0\0\0\0\0\0" : target_mac, 6);
    put32(a + 24, target_ip);
    send_frame(op == 1 ? broadcast_mac : target_mac, ETH_ARP, frame, 28);
}

static void arp_receive(const uint8_t *a, int len)
{
    if (len < 28 || get16(a) != 1 || get16(a + 2) != ETH_IP)
        return;
    uint16_t op = get16(a + 6);
    uint32_t sender_ip = get32(a + 14), target_ip = get32(a + 24);
    if (target_ip == net.ip || op == 2)
        arp_learn(sender_ip, a + 8);
    if (op == 1 && net.configured && target_ip == net.ip)
        arp_send(2, a + 8, sender_ip);
}

/* ---- ipv4 ---- */

static uint16_t ip_ident;

static bool on_link(uint32_t ip)
{
    return (ip & net.mask) == (net.ip & net.mask);
}

/*
 * works out whose hardware address the frame goes to: the destination itself
 * if it's on our network, or the gateway if it isn't. if we don't know that
 * address yet, ask with arp and wait a little. that only happens outside an
 * interrupt; inside one, the packet just gets dropped and the sender tries
 * again later.
 */
static bool next_hop_mac(uint32_t dst, uint8_t *mac)
{
    if (dst == 0xFFFFFFFF || (net.configured && net.mask && (dst | net.mask) == 0xFFFFFFFF)) {
        memcpy(mac, broadcast_mac, 6);
        return true;
    }
    uint32_t hop = on_link(dst) ? dst : net.gateway;
    if (!hop)
        return false;
    if (arp_lookup(hop, mac))
        return true;
    if (net_in_irq) {
        arp_send(1, 0, hop);
        return false;
    }
    for (int tries = 0; tries < 6; tries++) {
        arp_send(1, 0, hop);
        uint32_t until = timer_ticks() + timer_hz() / 3;
        while (timer_ticks() < until) {
            if (arp_lookup(hop, mac))
                return true;
            task_sleep(1);
        }
    }
    return false;
}

/*
 * sends an ip packet. it finds out where the frame goes next, then wraps the
 * payload in a 20 byte ip header (with its checksum) and an ethernet header,
 * and hands the lot to the card. anything bigger than one frame gets cut
 * short, since we never split packets up.
 */
int ip_send(uint32_t dst, uint8_t proto, const void *payload, int len)
{
    if (!net.card)
        return NET_ERR_NO_CARD;
    if (len > 1500 - IP_HEADER)
        len = 1500 - IP_HEADER;

    uint8_t mac[6];
    if (!next_hop_mac(dst, mac))
        return NET_ERR_NO_ROUTE;

    /*
     * AI Contribution (Bug Fix): a static frame inside the card's interrupt,
     * so answering a packet doesn't take another 1.5 kib from whatever
     * thread's stack the interrupt landed on (see send_segment in tcp.c)
     */
    static uint8_t irq_frame[ETH_HEADER + 1500];
    uint8_t stack_frame[net_in_irq ? 1 : ETH_HEADER + 1500];
    uint8_t *frame = net_in_irq ? irq_frame : stack_frame;
    uint8_t *ip = frame + ETH_HEADER;
    ip[0] = 0x45;                /* version 4, with a 20 byte header */
    ip[1] = 0;
    put16(ip + 2, IP_HEADER + len);
    put16(ip + 4, ip_ident++);
    put16(ip + 6, 0x4000);       /* don't fragment */
    ip[8] = 64;
    ip[9] = proto;
    put16(ip + 10, 0);
    put32(ip + 12, net.ip);
    put32(ip + 16, dst);
    put16(ip + 10, net_checksum(ip, IP_HEADER, 0));
    memcpy(ip + IP_HEADER, payload, len);
    send_frame(mac, ETH_IP, frame, IP_HEADER + len);
    return 0;
}

/* ---- icmp ---- */

static volatile uint16_t ping_seq_seen;
static volatile bool ping_answered;
static const uint16_t ping_ident = 0xC4AF;

static void icmp_receive(uint32_t src, const uint8_t *p, int len)
{
    if (len < 8)
        return;
    if (p[0] == 8) {
        /*
         * an echo request, so send the same thing back as a reply.
         * AI Contribution (Bug Fix): the reply buffer is static, since this
         * only ever runs inside the card's interrupt, and 1.5 kib on top of
         * whatever thread got interrupted was how a ping flood could run a
         * stack over
         */
        static uint8_t reply[1500];
        if (len > (int)sizeof(reply))
            return;
        memcpy(reply, p, len);
        reply[0] = 0;
        put16(reply + 2, 0);
        put16(reply + 2, net_checksum(reply, len, 0));
        ip_send(src, 1, reply, len);
    } else if (p[0] == 0 && get16(p + 4) == ping_ident) {
        ping_seq_seen = get16(p + 6);
        ping_answered = true;
    }
}

/*
 * sends one ping (an icmp echo request with 32 bytes of letters in it) and
 * waits for the reply with the same sequence number, a tick at a time, until
 * the time runs out.
 */
int net_ping(uint32_t ip, uint16_t seq, uint32_t timeout_ticks)
{
    if (!net.card)
        return NET_ERR_NO_CARD;
    uint8_t msg[8 + 32];
    msg[0] = 8;
    msg[1] = 0;
    put16(msg + 2, 0);
    put16(msg + 4, ping_ident);
    put16(msg + 6, seq);
    for (int i = 0; i < 32; i++)
        msg[8 + i] = 'a' + i % 26;
    put16(msg + 2, net_checksum(msg, sizeof(msg), 0));

    ping_answered = false;
    uint32_t start = timer_ticks();
    int err = ip_send(ip, 1, msg, sizeof(msg));
    if (err < 0)
        return err;
    while (timer_ticks() - start < timeout_ticks) {
        if (ping_answered && ping_seq_seen == seq)
            return timer_ticks() - start;
        task_sleep(1);
    }
    return NET_ERR_TIMEOUT;
}

/* ---- udp ---- */

#define UDP_BINDS 8

static struct {
    uint16_t port;
    udp_handler_t handler;
} binds[UDP_BINDS];

bool udp_bind(uint16_t port, udp_handler_t handler)
{
    /*
     * AI Contribution (Bug Fix): port 0 is what marks a slot as empty, so it
     * can't also be a port someone's listening on
     */
    if (!port)
        return false;
    for (int i = 0; i < UDP_BINDS; i++) {
        if (!binds[i].port) {
            binds[i].handler = handler;
            binds[i].port = port;
            return true;
        }
    }
    return false;
}

void udp_unbind(uint16_t port)
{
    for (int i = 0; i < UDP_BINDS; i++)
        if (binds[i].port == port) {
            /*
             * AI Contribution (Bug Fix): the handler goes too. it used to stay
             * behind, so a packet sent to port 0 matched the emptied slot and
             * still reached the old dns or dhcp handler.
             */
            binds[i].port = 0;
            binds[i].handler = 0;
        }
}

/* the checksum is optional in ipv4, and zero means we left it out. */
int udp_send(uint32_t dst, uint16_t sport, uint16_t dport, const void *data, int len)
{
    uint8_t packet[1500];
    if (len > (int)sizeof(packet) - IP_HEADER - UDP_HEADER)
        return NET_ERR_FULL;
    put16(packet, sport);
    put16(packet + 2, dport);
    put16(packet + 4, UDP_HEADER + len);
    put16(packet + 6, 0);
    memcpy(packet + UDP_HEADER, data, len);
    return ip_send(dst, 17, packet, UDP_HEADER + len);
}

static void udp_receive(uint32_t src, const uint8_t *p, int len)
{
    if (len < UDP_HEADER)
        return;
    uint16_t sport = get16(p), dport = get16(p + 2), ulen = get16(p + 4);
    if (ulen < UDP_HEADER || ulen > len)
        return;
    for (int i = 0; i < UDP_BINDS; i++)
        if (binds[i].port == dport && binds[i].handler)
            binds[i].handler(src, sport, p + UDP_HEADER, ulen - UDP_HEADER);
}

/* ---- incoming frames ---- */

static void ip_receive(const uint8_t *eth_src, const uint8_t *ip, int len)
{
    if (len < IP_HEADER || ip[0] >> 4 != 4)
        return;
    int header = (ip[0] & 0xF) * 4;
    int total = get16(ip + 2);
    /*
     * AI Contribution (Bug Fix): an ipv4 header is at least 20 bytes. a packet
     * claiming less put its payload inside its own header, and tcp and udp
     * read their fields from the wrong place.
     */
    if (total > len || total < header || header < IP_HEADER)
        return;
    if (get16(ip + 6) & 0x3FFF)
        return;     /* we don't put fragments back together */

    uint32_t src = get32(ip + 12), dst = get32(ip + 16);
    bool for_us = dst == net.ip || dst == 0xFFFFFFFF || !net.configured ||
                  (net.mask && (dst | net.mask) == 0xFFFFFFFF);
    if (!for_us)
        return;
    if (net.configured && on_link(src))
        arp_learn(src, eth_src);

    const uint8_t *payload = ip + header;
    int plen = total - header;
    switch (ip[9]) {
    case 1: icmp_receive(src, payload, plen); break;
    case 6: tcp_receive(src, payload, plen); break;
    case 17: udp_receive(src, payload, plen); break;
    }
}

void net_receive(const uint8_t *frame, int len)
{
    if (len < ETH_HEADER)
        return;
    net.rx_frames++;
    net.rx_bytes += len;
    uint16_t type = get16(frame + 12);
    if (type == ETH_ARP)
        arp_receive(frame + ETH_HEADER, len - ETH_HEADER);
    else if (type == ETH_IP)
        ip_receive(frame + 6, frame + ETH_HEADER, len - ETH_HEADER);
}

/* ---- dhcp ---- */

/*
 * dhcp is four messages: we broadcast a discover, a server offers us an
 * address, we broadcast a request for it, and the server acknowledges. the
 * options on the end carry the message type, the network mask, the gateway
 * (router), the dns server and how long we're allowed to keep the address.
 */

#define DHCP_CLIENT 68
#define DHCP_SERVER 67

enum { DHCP_DISCOVER = 1, DHCP_OFFER = 2, DHCP_REQUEST = 3, DHCP_ACK = 5, DHCP_NAK = 6 };

static uint32_t dhcp_xid;
static volatile int dhcp_got;     /* the last message type that came back */
static uint32_t offer_ip, offer_server, offer_mask, offer_router, offer_dns, offer_lease;

/*
 * handles a dhcp message from a server. once it's checked the message is a
 * reply to us, it reads through the options for the message type, the mask,
 * the router, the dns server, the server's own address and the lease, and
 * keeps them if it's an offer or an acknowledgement.
 */
static void dhcp_handler(uint32_t src, uint16_t sport, const uint8_t *p, int len)
{
    (void)src, (void)sport;
    if (len < 240 || p[0] != 2 || get32(p + 4) != dhcp_xid || get32(p + 236) != 0x63825363)
        return;
    uint32_t yiaddr = get32(p + 16);
    int type = 0;
    uint32_t mask = 0, router = 0, dns = 0, server = 0, lease = 0;
    for (int i = 240; i < len && p[i] != 255;) {
        if (p[i] == 0) {
            i++;
            continue;
        }
        if (i + 1 >= len)
            break;
        int code = p[i], olen = p[i + 1];
        const uint8_t *v = p + i + 2;
        if (i + 2 + olen > len)
            break;
        if (code == 53 && olen >= 1) type = v[0];
        if (code == 1 && olen >= 4) mask = get32(v);
        if (code == 3 && olen >= 4) router = get32(v);
        if (code == 6 && olen >= 4) dns = get32(v);
        if (code == 54 && olen >= 4) server = get32(v);
        if (code == 51 && olen >= 4) lease = get32(v);
        i += 2 + olen;
    }
    if (type == DHCP_OFFER || type == DHCP_ACK) {
        offer_ip = yiaddr;
        offer_mask = mask;
        offer_router = router;
        offer_dns = dns;
        offer_lease = lease;
        if (server)
            offer_server = server;
    }
    dhcp_got = type;
}

/*
 * sends a dhcp discover or request, as a broadcast since we don't have an
 * address yet. a request names the address we were offered and the server
 * that offered it. it also gives our name ("craftbox") and asks for the mask,
 * router, dns server and lease time.
 */
static void dhcp_send(int type)
{
    uint8_t p[300];
    memset(p, 0, sizeof(p));
    p[0] = 1;          /* a request from a client */
    p[1] = 1;          /* ethernet */
    p[2] = 6;
    put32(p + 4, dhcp_xid);
    put16(p + 10, 0x8000);     /* please broadcast the answer, since we haven't got an address yet */
    memcpy(p + 28, net.mac, 6);
    put32(p + 236, 0x63825363);
    int i = 240;
    p[i++] = 53; p[i++] = 1; p[i++] = type;
    if (type == DHCP_REQUEST) {
        p[i++] = 50; p[i++] = 4; put32(p + i, offer_ip); i += 4;
        p[i++] = 54; p[i++] = 4; put32(p + i, offer_server); i += 4;
    }
    p[i++] = 12; p[i++] = 8; memcpy(p + i, "craftbox", 8); i += 8;
    p[i++] = 55; p[i++] = 4; p[i++] = 1; p[i++] = 3; p[i++] = 6; p[i++] = 51;
    p[i++] = 255;
    udp_send(0xFFFFFFFF, DHCP_CLIENT, DHCP_SERVER, p, i < 300 ? 300 : i);
}

static bool dhcp_wait(int want, uint32_t ticks)
{
    uint32_t until = timer_ticks() + ticks;
    while (timer_ticks() < until) {
        if (dhcp_got == want)
            return true;
        if (dhcp_got == DHCP_NAK)
            return false;
        task_sleep(1);
    }
    return false;
}

/*
 * gets an address with dhcp: discover, wait for an offer, request it, and
 * wait for the acknowledgement, having two goes at the whole thing. if it
 * works, the address, mask, gateway and dns server all go into net.
 */
bool net_dhcp(void)
{
    if (!net.card)
        return false;
    udp_unbind(DHCP_CLIENT);
    udp_bind(DHCP_CLIENT, dhcp_handler);

    bool ok = false;
    net.configured = false;
    net.ip = 0;
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
        dhcp_xid = 0xC4AF0000u ^ timer_ticks() ^ (uint32_t)attempt << 20;
        dhcp_got = 0;
        dhcp_send(DHCP_DISCOVER);
        if (!dhcp_wait(DHCP_OFFER, timer_hz()))
            continue;
        dhcp_got = 0;
        dhcp_send(DHCP_REQUEST);
        ok = dhcp_wait(DHCP_ACK, timer_hz());
    }
    udp_unbind(DHCP_CLIENT);
    if (!ok)
        return false;

    net.ip = offer_ip;
    net.mask = offer_mask ? offer_mask : IP(255, 255, 255, 0);
    net.gateway = offer_router;
    net.dns = offer_dns ? offer_dns : offer_router;
    net.dhcp_server = offer_server;
    net.lease_secs = offer_lease;
    net.configured = true;
    return true;
}

/* ---- dns ---- */

/*
 * a query for one name's address (type a), sent to the dns server dhcp gave
 * us. answers are remembered, so a page with lots of links to the same site
 * only has to ask once.
 */

#define DNS_PORT  49153
#define DNS_CACHE 8

static struct {
    char name[64];
    uint32_t ip;
} dns_cache[DNS_CACHE];
static int dns_next;

static volatile uint16_t dns_id;
static volatile uint32_t dns_answer;
static volatile int dns_state;     /* 0 while waiting, 1 once answered, -1 if there's no such name */

static int skip_name(const uint8_t *p, int i, int len)
{
    while (i < len) {
        if (p[i] == 0)
            return i + 1;
        if ((p[i] & 0xC0) == 0xC0)
            return i + 2;
        i += p[i] + 1;
    }
    return len;
}

/*
 * handles a dns reply. if it's the answer to our question, it skips past the
 * questions and looks through the answers for the first address (type a). a
 * name that doesn't exist is marked as such.
 */
static void dns_handler(uint32_t src, uint16_t sport, const uint8_t *p, int len)
{
    (void)src, (void)sport;
    if (len < 12 || get16(p) != dns_id || !(p[2] & 0x80))
        return;
    if ((p[3] & 0xF) != 0) {
        dns_state = -1;
        return;
    }
    int questions = get16(p + 4), answers = get16(p + 6);
    int i = 12;
    for (int q = 0; q < questions; q++)
        i = skip_name(p, i, len) + 4;
    for (int a = 0; a < answers && i + 10 <= len; a++) {
        i = skip_name(p, i, len);
        if (i + 10 > len)
            break;
        int type = get16(p + i), rdlen = get16(p + i + 8);
        i += 10;
        if (type == 1 && rdlen == 4 && i + 4 <= len) {
            dns_answer = get32(p + i);
            dns_state = 1;
            return;
        }
        i += rdlen;
    }
    dns_state = -1;
}

/*
 * turns a name into an address. dotted addresses don't need looking up, and
 * names we've asked about before come straight from the cache. otherwise it
 * builds a question, sends it to the dns server, and waits for an answer,
 * asking again a couple of times if nothing comes back.
 */
int net_resolve(const char *name, uint32_t *ip)
{
    if (net_parse_ip(name, ip))
        return 0;
    if (!net.card)
        return NET_ERR_NO_CARD;
    if (!net.configured)
        return NET_ERR_NO_ADDRESS;
    for (int i = 0; i < DNS_CACHE; i++) {
        if (dns_cache[i].ip && !strcmp(dns_cache[i].name, name)) {
            *ip = dns_cache[i].ip;
            return 0;
        }
    }

    /*
     * the question: the name as labels, each with its length in front, then
     * type a, class in
     */
    uint8_t q[300];
    int n = 12;
    memset(q, 0, 12);
    uint16_t id = (uint16_t)(timer_ticks() * 31 + 7);
    put16(q, id);
    put16(q + 2, 0x0100);     /* recursion desired */
    put16(q + 4, 1);
    for (const char *s = name; *s;) {
        const char *dot = strchr(s, '.');
        int l = dot ? dot - s : (int)strlen(s);
        if (l == 0 || l > 63 || n + l + 6 > (int)sizeof(q))
            return NET_ERR_NO_HOST;
        q[n++] = l;
        memcpy(q + n, s, l);
        n += l;
        s += l;
        if (*s == '.')
            s++;
    }
    q[n++] = 0;
    put16(q + n, 1);
    put16(q + n + 2, 1);
    n += 4;

    udp_unbind(DNS_PORT);
    udp_bind(DNS_PORT, dns_handler);
    int result = NET_ERR_TIMEOUT;
    for (int attempt = 0; attempt < 3 && result == NET_ERR_TIMEOUT; attempt++) {
        dns_id = id;
        dns_state = 0;
        int err = udp_send(net.dns, DNS_PORT, 53, q, n);
        if (err < 0) {
            result = err;
            break;
        }
        uint32_t until = timer_ticks() + timer_hz() * 2;
        while (timer_ticks() < until && dns_state == 0)
            task_sleep(1);
        if (dns_state == 1) {
            *ip = dns_answer;
            result = 0;
        } else if (dns_state == -1) {
            result = NET_ERR_NO_HOST;
        }
    }
    udp_unbind(DNS_PORT);

    if (result == 0) {
        int slot = dns_next++ % DNS_CACHE;
        strlcpy(dns_cache[slot].name, name, sizeof(dns_cache[slot].name));
        dns_cache[slot].ip = *ip;
    }
    return result;
}

/* ---- starting up, and reporting ---- */

bool net_init(void)
{
    net.card = rtl8139_init(net.mac);
    return net.card;
}

/*
 * writes out /proc/net: the card, its hardware address, our address, mask,
 * gateway and dns server, and how many frames and bytes have gone each way.
 */
int net_describe(char *buf, int size)
{
    char ip[16], mask[16], gw[16], dns[16];
    net_format_ip(net.ip, ip);
    net_format_ip(net.mask, mask);
    net_format_ip(net.gateway, gw);
    net_format_ip(net.dns, dns);
    if (!net.card)
        return ksnprintf(buf, size, "eth0: no network card found\n");
    return ksnprintf(buf, size,
                     "eth0: rtl8139, irq %u\n"
                     "  hardware  %02x:%02x:%02x:%02x:%02x:%02x\n"
                     "  address   %s%s\n"
                     "  netmask   %s\n"
                     "  gateway   %s\n"
                     "  dns       %s\n"
                     "  received  %u frames, %u bytes\n"
                     "  sent      %u frames, %u bytes\n",
                     rtl8139_irq(), net.mac[0], net.mac[1], net.mac[2], net.mac[3], net.mac[4], net.mac[5],
                     net.configured ? ip : "none", net.configured ? " (dhcp)" : "",
                     mask, gw, dns, net.rx_frames, net.rx_bytes, net.tx_frames, net.tx_bytes);
}
