#include <console.h>
#include <cpu.h>
#include <io.h>
#include <net.h>
#include <pci.h>
#include <string.h>

/*
 * the realtek rtl8139, a simple 10/100 ethernet card that qemu can pretend to
 * be (-nic user,model=rtl8139). we drive it through a handful of i/o ports,
 * found in its first pci address slot.
 *
 * receiving: the card writes incoming frames one after another into a ring
 * buffer in memory, each with a 4 byte header in front (the status and the
 * length), and raises an interrupt. the handler walks round the ring from
 * wherever it got to last time, passes each frame up to the network stack,
 * and tells the card how far it's read.
 *
 * sending: the card has four transmit slots, which we use in turn. a frame
 * gets copied into the slot's buffer, and writing its length sets it off.
 *
 * paging is off, so the addresses we give the card are simply the buffers'
 * own addresses.
 */

#define REG_MAC      0x00
#define REG_TSD0     0x10    /* transmit status, one per slot */
#define REG_TSAD0    0x20    /* transmit buffer address, one per slot */
#define REG_RBSTART  0x30
#define REG_CMD      0x37
#define REG_CAPR     0x38    /* how far the driver has read, minus 16 */
#define REG_IMR      0x3C
#define REG_ISR      0x3E
#define REG_TCR      0x40
#define REG_RCR      0x44
#define REG_CONFIG1  0x52

#define CMD_BUFE     0x01    /* the receive buffer is empty */
#define CMD_TE       0x04
#define CMD_RE       0x08
#define CMD_RESET    0x10

#define INT_ROK      0x0001
#define INT_RER      0x0002
#define INT_TOK      0x0004
#define INT_TER      0x0008
#define INT_RXOVW    0x0010

#define TSD_OWN      (1 << 13)

/*
 * a 32 kib ring (the rcr length bits set to 2). with wrap mode on, a frame
 * that runs past the end gets written on past it instead of wrapping round to
 * the start, so the buffer has room for one more full frame after the ring.
 */
#define RX_RING      32768
#define RX_SIZE      (RX_RING + 16 + 1536)
#define TX_SLOTS     4
#define TX_SIZE      1536

static uint8_t rx_buf[RX_SIZE] __attribute__((aligned(16)));
static uint8_t tx_buf[TX_SLOTS][TX_SIZE] __attribute__((aligned(16)));
static uint16_t io;
static int tx_next;
static unsigned rx_offset;
static bool ready;
static uint8_t irq_line;

/*
 * the card's interrupt. it reads and clears what happened, then, if frames
 * arrived, walks the ring handing each one to net_receive, skipping along to
 * the next frame (rounded up to 4 bytes) and telling the card how far it's
 * got. if the ring ever looks confused, it just starts the ring over.
 */
static void handle_irq(struct regs *r)
{
    (void)r;
    uint16_t status = inw(io + REG_ISR);
    outw(io + REG_ISR, status);

    if (status & (INT_ROK | INT_RXOVW)) {
        net_in_irq = true;
        while (!(inb(io + REG_CMD) & CMD_BUFE)) {
            uint8_t *p = rx_buf + rx_offset;
            uint16_t rx_status = p[0] | p[1] << 8;
            uint16_t len = p[2] | p[3] << 8;
            if (!(rx_status & 1) || len < 4 || len > 1522) {
                /*
                 * the ring's confused, and the simplest fix is to start it
                 * over
                 */
                outb(io + REG_CMD, CMD_TE);
                rx_offset = 0;
                outl(io + REG_RBSTART, (uint32_t)rx_buf);
                outb(io + REG_CMD, CMD_TE | CMD_RE);
                break;
            }
            net_receive(p + 4, len - 4);   /* the length includes the card's 4 byte checksum on the end */
            rx_offset = (rx_offset + len + 4 + 3) & ~3u;
            rx_offset %= RX_RING;
            outw(io + REG_CAPR, (uint16_t)(rx_offset - 16));
        }
        net_in_irq = false;
    }
}

bool rtl8139_init(uint8_t mac[6])
{
    struct pci_device dev;
    if (!pci_find(0x10EC, 0x8139, &dev))
        return false;
    pci_enable(&dev);
    io = dev.bar[0] & ~3u;
    irq_line = dev.irq;

    outb(io + REG_CONFIG1, 0);
    outb(io + REG_CMD, CMD_RESET);
    for (int i = 0; i < 100000 && (inb(io + REG_CMD) & CMD_RESET); i++)
        io_wait();

    outl(io + REG_RBSTART, (uint32_t)rx_buf);
    rx_offset = 0;
    outw(io + REG_IMR, INT_ROK | INT_RER | INT_TOK | INT_TER | INT_RXOVW);

    /*
     * take frames sent to our address, broadcasts and multicasts (bits 1 to
     * 3), use a 32 kib ring (bits 11 and 12 set to 2) and wrap mode (bit 7).
     * the dma burst and fifo threshold bits are set to "unlimited".
     */
    outl(io + REG_RCR, 0x0E | (2 << 11) | (1 << 7) | (7 << 8) | (7 << 13));
    outl(io + REG_TCR, 6 << 8);
    outb(io + REG_CMD, CMD_TE | CMD_RE);

    for (int i = 0; i < 6; i++)
        mac[i] = inb(io + REG_MAC + i);

    irq_register(irq_line, handle_irq);
    ready = true;
    return true;
}

uint8_t rtl8139_irq(void)
{
    return irq_line;
}

/*
 * a slot's free again once the card has copied the frame out of it (the own
 * bit). frames shorter than ethernet's 60 byte minimum get padded out.
 */
void rtl8139_send(const void *frame, int len)
{
    if (!ready || len > TX_SIZE)
        return;
    uint32_t flags = irq_save();
    int slot = tx_next;
    uint16_t tsd = io + REG_TSD0 + slot * 4;
    /*
     * AI Contribution (Bug Fix): this used to wait up to 100,000 rounds (about
     * a tenth of a second, all with interrupts off) and then write over the
     * slot whether it was free or not. the card sends a frame in well under a
     * millisecond, so if the slot's still busy after 2,000 rounds the card is
     * stuck, and the frame is dropped instead (tcp sends it again anyway).
     */
    bool free = false;
    for (int i = 0; i < 2000 && !free; i++) {
        uint32_t st = inl(tsd);
        free = st & TSD_OWN || st == 0 || st == 0x2000;
        if (!free)
            io_wait();
    }
    if (!free) {
        irq_restore(flags);
        return;
    }
    memcpy(tx_buf[slot], frame, len);
    if (len < 60) {
        memset(tx_buf[slot] + len, 0, 60 - len);
        len = 60;
    }
    outl(io + REG_TSAD0 + slot * 4, (uint32_t)tx_buf[slot]);
    outl(tsd, len);
    tx_next = (tx_next + 1) % TX_SLOTS;
    irq_restore(flags);
}
