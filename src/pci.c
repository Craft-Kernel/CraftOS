#include <io.h>
#include <pci.h>
#include <string.h>

/*
 * a configuration address picks out one 32-bit register on one function. bit
 * 31 switches it on, then come the bus, the slot (device), the function and
 * the register's offset. then the value gets read or written through the data
 * port.
 */

#define CONFIG_ADDRESS 0xCF8
#define CONFIG_DATA    0xCFC

static uint32_t address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    return 0x80000000u | (uint32_t)bus << 16 | (uint32_t)slot << 11 | (uint32_t)func << 8 | (offset & 0xFC);
}

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    outl(CONFIG_ADDRESS, address(bus, slot, func, offset));
    return inl(CONFIG_DATA);
}

void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value)
{
    outl(CONFIG_ADDRESS, address(bus, slot, func, offset));
    outl(CONFIG_DATA, value);
}

static void fill(struct pci_device *d, uint8_t bus, uint8_t slot, uint8_t func, uint32_t id)
{
    d->bus = bus;
    d->slot = slot;
    d->func = func;
    d->vendor = id & 0xFFFF;
    d->device = id >> 16;
    for (int i = 0; i < 6; i++)
        d->bar[i] = pci_read32(bus, slot, func, 0x10 + i * 4);
    d->irq = pci_read32(bus, slot, func, 0x3C) & 0xFF;
}

/*
 * calls visit for every function that answers. a vendor id of 0xffff means
 * there's nothing there. functions 1 to 7 only exist if function 0 says it's
 * a multi-function device (bit 7 of its header type).
 */
static bool scan(bool (*visit)(const struct pci_device *d, void *ctx), void *ctx)
{
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            for (int func = 0; func < 8; func++) {
                uint32_t id = pci_read32(bus, slot, func, 0);
                if ((id & 0xFFFF) == 0xFFFF) {
                    if (func == 0)
                        break;
                    continue;
                }
                struct pci_device d;
                fill(&d, bus, slot, func, id);
                if (visit(&d, ctx))
                    return true;
                if (func == 0 && !(pci_read32(bus, slot, 0, 0x0C) >> 16 & 0x80))
                    break;
            }
        }
    }
    return false;
}

struct find_ctx {
    uint16_t vendor, device;
    struct pci_device *out;
};

static bool match(const struct pci_device *d, void *ctx)
{
    struct find_ctx *f = ctx;
    if (d->vendor != f->vendor || d->device != f->device)
        return false;
    *f->out = *d;
    return true;
}

bool pci_find(uint16_t vendor, uint16_t device, struct pci_device *out)
{
    struct find_ctx f = { vendor, device, out };
    return scan(match, &f);
}

void pci_enable(const struct pci_device *dev)
{
    uint32_t cmd = pci_read32(dev->bus, dev->slot, dev->func, 0x04);
    pci_write32(dev->bus, dev->slot, dev->func, 0x04, cmd | 0x7);
}

struct describe_ctx {
    char *buf;
    int size, len;
};

/*
 * the class code says what kind of device it is. these are the ones qemu
 * shows.
 */
static const char *class_name(uint32_t class_reg)
{
    switch (class_reg >> 16) {
    case 0x0101: return "ide controller";
    case 0x0200: return "ethernet controller";
    case 0x0300: return "vga display";
    case 0x0600: return "host bridge";
    case 0x0601: return "isa bridge";
    case 0x0680: return "bridge";
    }
    return "device";
}

static bool describe(const struct pci_device *d, void *ctx)
{
    struct describe_ctx *c = ctx;
    uint32_t class_reg = pci_read32(d->bus, d->slot, d->func, 0x08);
    if (c->len < c->size)
        c->len += ksnprintf(c->buf + c->len, c->size - c->len, "%02x:%02x.%x  %04x:%04x  irq %-3u %s\n",
                            d->bus, d->slot, d->func, d->vendor, d->device, d->irq, class_name(class_reg));
    return false;
}

int pci_describe(char *buf, int size)
{
    struct describe_ctx c = { buf, size, 0 };
    buf[0] = '\0';
    scan(describe, &c);
    return c.len < size ? c.len : size - 1;
}
