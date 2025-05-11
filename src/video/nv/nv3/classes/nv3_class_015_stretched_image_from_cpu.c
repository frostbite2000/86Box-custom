/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          NV3: Methods for class 0x15 (stretched image from cpu to memory)
 *
 *
 *
 * Authors: Connor Hyde, <mario64crashed@gmail.com> I need a better email address ;^)
 *
 *          Copyright 2024-2025 Connor Hyde
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/mem.h>
#include <86box/pci.h>
#include <86box/rom.h>
#include <86box/video.h>
#include <86box/nv/vid_nv.h>
#include <86box/nv/vid_nv3.h>

void nv3_class_015_method(uint32_t param, uint32_t method_id, nv3_ramin_context_t context, nv3_grobj_t grobj)
{
    switch (method_id)
    {
        case 0x0400:
            nv3->pgraph.pattern.shape = NV3_PATTERN_SHAPE_8X8;
            nv3->pgraph.pattern_bitmap = param;
            break;
        case 0x0404:
            nv3->pgraph.pattern_color_0_rgb.r = (param >> 20) & 0x3FF;
            nv3->pgraph.pattern_color_0_rgb.g = (param >> 10) & 0x3FF;
            nv3->pgraph.pattern_color_0_rgb.b = param & 0x3FF;
            break;
        default:
            warning("%s: Invalid or unimplemented method 0x%04x\n", nv3_class_names[context.class_id & 0x1F], method_id);
            nv3_pgraph_interrupt_invalid(NV3_PGRAPH_INTR_1_SOFTWARE_METHOD_PENDING);
            return;
    }
}