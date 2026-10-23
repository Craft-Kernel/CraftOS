#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * the pci bus, which is where all the plug-in hardware lives. we talk to it
 * through the old configuration ports (0xcf8 and 0xcfc). pci_find goes
 * through every bus, slot and function looking for a device with a certain
 * vendor and device id, which is all the network driver needs.
 */

struct pci_device {
    uint8_t bus, slot, func;
    uint16_t vendor, device;
    uint32_t bar[6];
    uint8_t irq;
};

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);

bool pci_find(uint16_t vendor, uint16_t device, struct pci_device *out);

/*
 * switches on the device's i/o space, memory space and bus mastering (which
 * lets it read and write memory by itself, called dma).
 */
void pci_enable(const struct pci_device *dev);

/* writes a line for every device found, for /proc/pci. */
int pci_describe(char *buf, int size);
