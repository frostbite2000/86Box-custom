/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Implementation of the AMD-761 North Bridge.
 *
 *
 * Authors: frostbite2000
 *
 *          Copyright 2025 frostbite2000.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <86box/86box.h>
#include "cpu.h"
#include <86box/mem.h>
#include <86box/smram.h>
#include <86box/io.h>
#include <86box/device.h>
#include <86box/pci.h>
#include <86box/chipset.h>
#include <86box/spd.h>
#include <86box/agpgart.h>
#include <86box/plat_unused.h>

typedef struct amd761_t {
    uint8_t pci_conf[256];
    uint8_t pci_slot;
    
    agpgart_t *agpgart;
} amd761_t;

static void
amd761_agp_map(amd761_t* dev)
{
    agpgart_set_aperture(dev->agpgart,
                         (dev->pci_conf[0x13] << 24),
                         0x2000000 << (((uint32_t)dev->pci_conf[0xac] & 0x0e) >> 1),
                         !!(dev->pci_conf[0xac] & 0x01));
    agpgart_set_gart(dev->agpgart, (dev->pci_conf[0x15] << 8) | (dev->pci_conf[0x16] << 16) | (dev->pci_conf[0x17] << 24));
}

static uint8_t
amd761_read(int func, int addr, void *priv)
{
    amd761_t *dev = (amd761_t *) priv;
    uint8_t ret = 0xff;

    if (func > 0)
        return ret;

    switch (addr) {
        case 0x00: return 0x22; /* AMD */
        case 0x01: return 0x10;
        case 0x02: return 0x0e; /* AMD-761 */
        case 0x03: return 0x70;
        case 0x04: return (dev->pci_conf[0x04] & 0x02) | 0x06; /* Command - based on scanpci */
        case 0x05: return 0x00;
        case 0x06: return 0x10; /* Status - based on scanpci */
        case 0x07: return 0x22;
        case 0x08: return 0x13; /* Revision ID from scanpci */
        case 0x09: return 0x00; /* Programming interface */
        case 0x0a: return 0x00; /* Subclass */
        case 0x0b: return 0x06; /* Base class - Host bridge */
        case 0x0c: return 0x00; /* Cache line size */
        case 0x0d: return dev->pci_conf[0x0d];
        case 0x0e: return 0x00; /* Header type */
        case 0x0f: return 0x00; /* BIST */
        
        /* BAR0 - Prefetchable memory */
        case 0x10:
        case 0x11:
        case 0x12:
        case 0x13:
            return dev->pci_conf[addr];
        
        /* BAR1 - Prefetchable memory */
        case 0x14:
        case 0x15:
        case 0x16:
        case 0x17:
            return dev->pci_conf[addr];
        
        /* BAR2 - I/O */
        case 0x18:
        case 0x19:
        case 0x1a:
        case 0x1b:
            return dev->pci_conf[addr];

        default:
            return dev->pci_conf[addr];
    }

    return ret;
}

static void
amd761_write(int func, int addr, uint8_t val, void *priv)
{
    amd761_t *dev = (amd761_t *) priv;

    if (func > 0)
        return;

    switch (addr) {
        case 0x04: /* Command */
            dev->pci_conf[addr] = val & 0x02;
            break;

        /* BAR0 - Prefetchable memory */
        case 0x10:
        case 0x11:
        case 0x12:
        case 0x13:
            dev->pci_conf[addr] = val;
            break;

        /* BAR1 - Prefetchable memory */
        case 0x14:
        case 0x15:
        case 0x16:
        case 0x17:
            dev->pci_conf[addr] = val;
            break;

        /* BAR2 - I/O */
        case 0x18:
        case 0x19:
        case 0x1a:
        case 0x1b:
            dev->pci_conf[addr] = val;
            break;

        default:
            dev->pci_conf[addr] = val;
            break;
    }
}

static void
amd761_reset(void *priv)
{
    amd761_t *dev = (amd761_t *) priv;

    memset(dev->pci_conf, 0, sizeof(dev->pci_conf));

    /* Host bridge initial values */
    dev->pci_conf[0x00] = 0x22; /* AMD */
    dev->pci_conf[0x01] = 0x10;
    dev->pci_conf[0x02] = 0x0e; /* AMD-761 */
    dev->pci_conf[0x03] = 0x70;
    dev->pci_conf[0x04] = 0x06;
    dev->pci_conf[0x06] = 0x10;
    dev->pci_conf[0x07] = 0x22;
    dev->pci_conf[0x08] = 0x13;
    dev->pci_conf[0x0b] = 0x06;
}

static void *
amd761_init(UNUSED(const device_t *info))
{
    amd761_t *dev = (amd761_t *) malloc(sizeof(amd761_t));
    memset(dev, 0, sizeof(amd761_t));

    /* Add host bridge (function 0) */
    pci_add_card(PCI_ADD_NORTHBRIDGE, amd761_read, amd761_write, dev, &dev->pci_slot);

    /* Add AGP bridge */
    device_add(&amd761_agp_device);

    dev->agpgart = device_add(&agpgart_device);

    cpu_cache_int_enabled = 1;
    cpu_cache_ext_enabled = 1;
    cpu_update_waitstates();

    amd761_reset(dev);

    return dev;
}

static void
amd761_close(void *priv)
{
    amd761_t *dev = (amd761_t *) priv;

    free(dev);
}

const device_t amd761_device = {
    .name          = "AMD 761 System Controller",
    .internal_name = "amd761",
    .flags         = DEVICE_PCI,
    .local         = 0,
    .init          = amd761_init,
    .close         = amd761_close,
    .reset         = amd761_reset,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = NULL
};
