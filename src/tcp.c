#include <cpu.h>
#include <heap.h>
#include <io.h>
#include <net.h>
#include <string.h>
#include <task.h>

/*
 * tcp, but only the client side, since that's all a browser needs: connect,
 * send a request, and read the answer until the server hangs up.
 *
 * it's kept simple on purpose. sending is stop and wait: each segment gets
 * sent again every second until the other side says it got it. receiving only
 * takes segments in order. anything that turns up early gets dropped, and our
 * acknowledgement for what we do have makes the server send the rest again.
 * whatever arrives in order goes on the end of the socket's buffer, which
 * grows when it needs to, so the window we advertise can stay the same size
 * the whole time.
 *
 * segments get handled on the card's interrupt. the fields the interrupt
 * changes are volatile, since the task waiting on them keeps reading them in
 * a loop.
 */

#define SOCKETS   8
#define MSS       1460
/*
 * AI Contribution (Bug Fix): the receive buffer used to start at 16 kib and
 * double inside the card's interrupt, up to 8 mib, copying everything over
 * with interrupts off each time. now it's one fixed buffer, set up when the
 * connection opens, and the window we advertise is however much of it is
 * free, so the other end never sends more than fits. reading makes room again.
 */
#define RX_BUF    65535

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

enum { CLOSED, SYN_SENT, ESTABLISHED, FIN_WAIT_1, FIN_WAIT_2, CLOSE_WAIT, LAST_ACK };

struct sock {
    bool used;
    int owner;      /* AI Contribution (Bug Fix): the pid that opened it */
    volatile int state;
    uint32_t remote_ip;
    uint16_t remote_port, local_port;
    uint32_t snd_nxt;
    volatile uint32_t snd_una, rcv_nxt;
    uint8_t *rx;
    volatile int rx_len;
    int rx_read;
    volatile bool peer_fin, reset;
};

static struct sock socks[SOCKETS];
static uint16_t next_port = 49200;

static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint16_t get16(const uint8_t *p) { return p[0] << 8 | p[1]; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/*
 * sequence numbers wrap round, so "after" means the difference between them
 * is positive when you treat it as a signed number.
 */
static bool seq_after(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

/*
 * the checksum covers a pretend header (both addresses, the protocol and the
 * length) as well as the segment itself.
 */
static void send_segment(struct sock *s, uint32_t seq, uint8_t flags, const void *data, int len)
{
    /*
     * AI Contribution (Bug Fix): inside the card's interrupt, the segment is
     * built in a static buffer instead of on the stack. the interrupt borrows
     * whichever thread's stack was running, and 1.5 kib more of it here (plus
     * the frame in ip_send) was a lot to take from someone else. interrupts
     * don't nest, so the one static buffer is never in use twice.
     */
    static uint8_t irq_seg[20 + 4 + MSS];
    uint8_t stack_seg[net_in_irq ? 1 : 20 + 4 + MSS];
    uint8_t *seg = net_in_irq ? irq_seg : stack_seg;
    int header = flags & F_SYN ? 24 : 20;
    put16(seg, s->local_port);
    put16(seg + 2, s->remote_port);
    put32(seg + 4, seq);
    put32(seg + 8, flags & F_ACK ? s->rcv_nxt : 0);
    seg[12] = header / 4 << 4;
    seg[13] = flags;
    put16(seg + 14, s->rx ? RX_BUF - s->rx_len : 0);    /* AI Contribution (Bug Fix): the real free space */
    put16(seg + 16, 0);
    put16(seg + 18, 0);
    if (flags & F_SYN) {
        seg[20] = 2;       /* the maximum segment size option */
        seg[21] = 4;
        put16(seg + 22, MSS);
    }
    if (len)
        memcpy(seg + header, data, len);

    uint8_t pseudo[12];
    put32(pseudo, net.ip);
    put32(pseudo + 4, s->remote_ip);
    pseudo[8] = 0;
    pseudo[9] = 6;
    put16(pseudo + 10, header + len);
    uint16_t partial = ~net_checksum(pseudo, 12, 0);
    put16(seg + 16, net_checksum(seg, header + len, partial));
    ip_send(s->remote_ip, 6, seg, header + len);
}

static void send_ack(struct sock *s)
{
    send_segment(s, s->snd_nxt, F_ACK, 0, 0);
}

/*
 * AI Contribution (Bug Fix): says whether the data fit. it used to return
 * nothing, and the caller acknowledged the bytes either way, so anything that
 * didn't fit was lost for good: the other end thought it had arrived and
 * never sent it again.
 */
static bool append(struct sock *s, const uint8_t *data, int len)
{
    if (!s->rx || len > RX_BUF - s->rx_len)
        return false;
    memcpy(s->rx + s->rx_len, data, len);
    s->rx_len += len;
    return true;
}

/*
 * handles a segment that just arrived, on the card's interrupt. it finds the
 * socket it belongs to, then deals with whatever it says: a reset kills the
 * connection, a syn-ack finishes connecting, an ack moves along how much of
 * our data got through, new data in the right order goes in the buffer, and a
 * fin means the server's done. anything carrying data or a fin gets an ack
 * back.
 */
void tcp_receive(uint32_t src, const uint8_t *seg, int len)
{
    if (len < 20)
        return;
    uint16_t sport = get16(seg), dport = get16(seg + 2);
    uint32_t seq = get32(seg + 4), ack = get32(seg + 8);
    int header = (seg[12] >> 4) * 4;
    uint8_t flags = seg[13];
    if (header < 20 || header > len)
        return;

    struct sock *s = 0;
    for (int i = 0; i < SOCKETS; i++)
        if (socks[i].used && socks[i].local_port == dport && socks[i].remote_port == sport && socks[i].remote_ip == src)
            s = &socks[i];
    if (!s || s->state == CLOSED)
        return;

    if (flags & F_RST) {
        s->reset = true;
        s->state = CLOSED;
        return;
    }

    if (s->state == SYN_SENT) {
        if ((flags & (F_SYN | F_ACK)) == (F_SYN | F_ACK) && ack == s->snd_nxt) {
            s->rcv_nxt = seq + 1;
            s->snd_una = ack;
            s->state = ESTABLISHED;
            send_ack(s);
        }
        return;
    }

    if (flags & F_ACK) {
        if (seq_after(ack, s->snd_una) && !seq_after(ack, s->snd_nxt))
            s->snd_una = ack;
        if (s->state == FIN_WAIT_1 && ack == s->snd_nxt)
            s->state = FIN_WAIT_2;
        else if (s->state == LAST_ACK && ack == s->snd_nxt)
            s->state = CLOSED;
    }

    /*
     * take whatever part of the data we haven't had yet, as long as it starts
     * right where we're up to
     */
    const uint8_t *data = seg + header;
    int dlen = len - header;
    bool need_ack = false;
    if (dlen > 0) {
        uint32_t skip = s->rcv_nxt - seq;
        /*
         * AI Contribution (Bug Fix): only move along when the bytes were kept.
         * if they weren't, the ack still goes back, but for where we really
         * are, so the other end sends them again once there's room.
         */
        if ((int32_t)skip >= 0 && (int)skip < dlen && append(s, data + skip, dlen - skip))
            s->rcv_nxt = seq + dlen;
        need_ack = true;
    }

    if ((flags & F_FIN) && seq + dlen == s->rcv_nxt && !s->peer_fin) {
        s->rcv_nxt++;
        s->peer_fin = true;
        need_ack = true;
        if (s->state == ESTABLISHED)
            s->state = CLOSE_WAIT;
        else if (s->state == FIN_WAIT_1 || s->state == FIN_WAIT_2)
            s->state = CLOSED;
    }
    if (need_ack)
        send_ack(s);
}

static struct sock *get(int n)
{
    return n >= 0 && n < SOCKETS && socks[n].used ? &socks[n] : 0;
}

/*
 * opens a connection: grabs a free socket, picks a local port and a random
 * starting sequence number, then sends a syn every second until the server's
 * syn-ack comes back (which finishes the handshake) or the time runs out.
 */
int tcp_connect(uint32_t ip, uint16_t port, uint32_t timeout_ticks)
{
    if (!net.card)
        return NET_ERR_NO_CARD;
    if (!net.configured)
        return NET_ERR_NO_ADDRESS;

    /* AI Contribution (Bug Fix): the whole receive buffer, up front */
    uint8_t *rx = kmalloc(RX_BUF);
    if (!rx)
        return NET_ERR_FULL;

    int n = -1;
    uint32_t flags = irq_save();
    for (int i = 0; i < SOCKETS && n < 0; i++) {
        if (!socks[i].used) {
            memset(&socks[i], 0, sizeof(socks[i]));
            socks[i].used = true;
            socks[i].rx = rx;
            socks[i].owner = task_current()->pid;
            n = i;
        }
    }
    irq_restore(flags);
    if (n < 0) {
        kfree(rx);
        return NET_ERR_FULL;
    }

    struct sock *s = &socks[n];
    s->remote_ip = ip;
    s->remote_port = port;
    s->local_port = next_port++;
    if (next_port > 60000)
        next_port = 49200;
    uint32_t iss = timer_ticks() * 2654435761u + (uint32_t)n * 7919;
    s->snd_una = iss;
    s->snd_nxt = iss + 1;
    s->state = SYN_SENT;

    uint32_t start = timer_ticks(), last = 0;
    bool first = true;
    while (s->state == SYN_SENT && !s->reset) {
        uint32_t now = timer_ticks();
        if (now - start >= timeout_ticks)
            break;
        if (first || now - last >= timer_hz()) {
            send_segment(s, iss, F_SYN, 0, 0);
            last = now;
            first = false;
        }
        task_sleep(1);
    }
    if (s->state == ESTABLISHED)
        return n;

    int err = s->reset ? NET_ERR_REFUSED : NET_ERR_TIMEOUT;
    flags = irq_save();
    s->used = false;
    s->rx = 0;
    irq_restore(flags);
    kfree(rx);
    return err;
}

/*
 * sends data, a segment at a time, waiting for each one to be acknowledged
 * before sending the next, and sending it again every second (up to eight
 * times) if it isn't.
 */
int tcp_write(int n, const void *data, int len)
{
    struct sock *s = get(n);
    if (!s)
        return NET_ERR_CLOSED;
    const uint8_t *p = data;
    while (len > 0) {
        if (s->state != ESTABLISHED && s->state != CLOSE_WAIT)
            return s->reset ? NET_ERR_RESET : NET_ERR_CLOSED;
        int chunk = len < MSS ? len : MSS;
        uint32_t seq = s->snd_nxt;
        s->snd_nxt += chunk;
        bool acked = false;
        for (int tries = 0; tries < 8 && !acked; tries++) {
            send_segment(s, seq, F_ACK | F_PSH, p, chunk);
            uint32_t until = timer_ticks() + timer_hz();
            while (timer_ticks() < until) {
                if (!seq_after(s->snd_nxt, s->snd_una)) {
                    acked = true;
                    break;
                }
                if (s->reset)
                    return NET_ERR_RESET;
                task_sleep(1);
            }
        }
        if (!acked)
            return NET_ERR_TIMEOUT;
        p += chunk;
        len -= chunk;
    }
    return 0;
}

/*
 * copies out whatever data has arrived, without waiting. once the server has
 * finished and everything has been read, it says the connection is closed (or
 * reset, if it ended badly).
 */
int tcp_read(int n, void *buf, int max)
{
    struct sock *s = get(n);
    if (!s)
        return NET_ERR_CLOSED;
    uint32_t flags = irq_save();
    int was_free = RX_BUF - s->rx_len;
    int avail = s->rx_len - s->rx_read;
    if (avail > max)
        avail = max;
    if (avail > 0) {
        memcpy(buf, s->rx + s->rx_read, avail);
        s->rx_read += avail;
        /*
         * AI Contribution (Bug Fix): whatever's left unread slides down to
         * the front, so the free space at the end (the window) grows back.
         * before, the buffer only started over once every byte was read.
         */
        memmove(s->rx, s->rx + s->rx_read, s->rx_len - s->rx_read);
        s->rx_len -= s->rx_read;
        s->rx_read = 0;
    }
    bool done = s->peer_fin || s->reset || s->state == CLOSED;
    bool reopened = was_free < 2 * MSS && RX_BUF - s->rx_len >= 2 * MSS;
    irq_restore(flags);
    /*
     * AI Contribution (Bug Fix): if the window had all but closed, say so as
     * soon as there's room again, rather than waiting for the other end to
     * come and ask
     */
    if (reopened && !done)
        send_ack(s);
    if (avail > 0)
        return avail;
    return done ? (s->reset ? NET_ERR_RESET : NET_ERR_CLOSED) : 0;
}

/*
 * sends our fin, waits a moment for the goodbye to finish, then lets the
 * socket go.
 */
void tcp_close(int n)
{
    struct sock *s = get(n);
    if (!s)
        return;
    if (s->state == ESTABLISHED || s->state == CLOSE_WAIT) {
        s->state = s->state == ESTABLISHED ? FIN_WAIT_1 : LAST_ACK;
        send_segment(s, s->snd_nxt, F_FIN | F_ACK, 0, 0);
        s->snd_nxt++;
        uint32_t until = timer_ticks() + timer_hz() / 2;
        while (timer_ticks() < until && s->state != CLOSED && s->state != FIN_WAIT_2)
            task_sleep(1);
    }
    uint32_t flags = irq_save();
    s->state = CLOSED;
    s->used = false;
    uint8_t *rx = s->rx;
    s->rx = 0;
    irq_restore(flags);
    kfree(rx);
}

/*
 * AI Contribution (Bug Fix): a task that gets killed never reaches tcp_close,
 * so its sockets stayed taken forever, and after eight of them nothing could
 * connect until a reboot. task_kill calls this to hand them back, with a
 * reset so the other end knows the connection's gone.
 */
void tcp_drop_task(int pid)
{
    for (int i = 0; i < SOCKETS; i++) {
        struct sock *s = &socks[i];
        if (!s->used || s->owner != pid)
            continue;
        if (s->state != CLOSED && s->state != SYN_SENT)
            send_segment(s, s->snd_nxt, F_RST | F_ACK, 0, 0);
        uint32_t flags = irq_save();
        s->state = CLOSED;
        s->used = false;
        uint8_t *rx = s->rx;
        s->rx = 0;
        irq_restore(flags);
        kfree(rx);
    }
}
