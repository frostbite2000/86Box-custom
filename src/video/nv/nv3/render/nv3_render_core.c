/*
* 86Box    A hypervisor and IBM PC system emulator that specializes in
*          running old operating systems and software designed for IBM
*          PC systems and compatibles from 1981 through fairly recent
*          system designs based on the PCI bus.
*
*          This file is part of the 86Box distribution.
*
*          NV3 Core rendering code (Software version)
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
#include <86box/plat.h>
#include <86box/rom.h>
#include <86box/video.h>
#include <86box/nv/vid_nv.h>
#include <86box/nv/vid_nv3.h>
#include <86box/utils/video_stdlib.h>

/* Functions only used in this translation unit */
void nv3_render_8bpp(nv3_coord_16_t position, nv3_coord_16_t size, nv3_grobj_t grobj, bool use_destination_buffer);
void nv3_render_15bpp(nv3_coord_16_t position, nv3_coord_16_t size, nv3_grobj_t grobj, bool use_destination_buffer);
void nv3_render_16bpp(nv3_coord_16_t position, nv3_coord_16_t size, nv3_grobj_t grobj, bool use_destination_buffer);
void nv3_render_32bpp(nv3_coord_16_t position, nv3_coord_16_t size, nv3_grobj_t grobj, bool use_destination_buffer);

/* Expand a colour.
   NOTE: THE GPU INTERNALLY OPERATES ON RGB10!!!!!!!!!!!
*/
nv3_color_expanded_t nv3_render_expand_color(uint32_t color, nv3_grobj_t grobj)
{
    // grobj0 = seems to share the format of PGRAPH_CONTEXT_SWITCH register.

    uint8_t format = (grobj.grobj_0 & 0x07);
    bool alpha_enabled = (grobj.grobj_0 >> NV3_PGRAPH_CONTEXT_SWITCH_ALPHA) & 0x01;

    nv3_color_expanded_t color_final; 
    // set the pixel format
    color_final.pixel_format = format;

    nv_log_verbose_only("Expanding Colour 0x%08x using pgraph_pixel_format 0x%x alpha enabled=%d\n", color, format, alpha_enabled);

    // default to fully opaque in case alpha is disabled
    color_final.a = 0xFF;

    switch (format)
    {
        case 0x01: // 8bpp palettized
            color_final.i8 = color & 0xFF;
            break;
            
        case 0x02: // 15bpp RGB
            color_final.r = ((color >> 10) & 0x1F) << 3;
            color_final.g = ((color >> 5) & 0x1F) << 3;
            color_final.b = ((color) & 0x1F) << 3;
            if (alpha_enabled)
                color_final.a = ((color >> 15) & 0x01) ? 0xFF : 0x00;
            break;
            
        case 0x03: // 16bpp RGB
            color_final.r = ((color >> 11) & 0x1F) << 3;
            color_final.g = ((color >> 5) & 0x3F) << 2;
            color_final.b = ((color) & 0x1F) << 3;
            break;
            
        case 0x04: // 32bpp RGBA
            color_final.r = (color >> 16) & 0xFF;
            color_final.g = (color >> 8) & 0xFF;
            color_final.b = color & 0xFF;
            if (alpha_enabled)
                color_final.a = (color >> 24) & 0xFF;
            break;
            
        default:
            // i8 is a union under i16
            color_final.i16 = (color & 0xFFFF);
            break;
    }

    return color_final;
}

/* Used for chroma test */
uint32_t nv3_render_downconvert_color(nv3_grobj_t grobj, nv3_color_expanded_t color)
{
    uint8_t format = (grobj.grobj_0 & 0x07);
    bool alpha_enabled = (grobj.grobj_0 >> NV3_PGRAPH_CONTEXT_SWITCH_ALPHA) & 0x01;

    nv_log_verbose_only("Downconverting Colour 0x%08x using pgraph_pixel_format 0x%x alpha enabled=%d\n", color, format, alpha_enabled);

    uint32_t packed_color = 0x00;

    switch (format)
    {
        case 0x01: // 8bpp palettized
            packed_color = color.i8;
            break;
            
        case 0x02: // 15bpp RGB
            packed_color = ((color.r >> 3) << 10) |
                         ((color.g >> 3) << 5) |
                         (color.b >> 3);
            if (alpha_enabled)
                packed_color |= ((color.a > 0x7F) ? 1 : 0) << 15;
            break;
            
        case 0x03: // 16bpp RGB
            packed_color = ((color.r >> 3) << 11) |
                         ((color.g >> 2) << 5) |
                         (color.b >> 3);
            break;
            
        case 0x04: // 32bpp RGBA
            packed_color = (color.r << 16) |
                         (color.g << 8) |
                         color.b;
            if (alpha_enabled)
                packed_color |= color.a << 24;
            break;
            
        default:
            packed_color = color.i16;
            break;
    }

    return packed_color;
}

/* Runs the chroma key/color key test */
bool nv3_render_chroma_test(uint32_t color, nv3_grobj_t grobj)
{
    bool chroma_enabled = ((grobj.grobj_0 >> NV3_PGRAPH_CONTEXT_SWITCH_CHROMA_KEY) & 0x01);

    if (!chroma_enabled)
        return true;
    
    bool alpha = ((nv3->pgraph.chroma_key >> 31) & 0x01);

    if (!alpha)
        return true;

    /* this is dumb but i'm lazy, if it kills perf, fix it later - we need to do some format shuffling */
    nv3_grobj_t grobj_fake = {0};
    grobj_fake.grobj_0 = 0x02; /* we don't care about any other bits */

    nv3_color_expanded_t chroma_expanded = nv3_render_expand_color(nv3->pgraph.chroma_key, grobj_fake);
   
    uint32_t chroma_downconverted = nv3_render_downconvert_color(grobj, chroma_expanded);

    return !(chroma_downconverted == color);
}

/* Convert expanded colour format to chroma key format */
uint32_t nv3_render_to_chroma(nv3_color_expanded_t expanded)
{
    // convert the alpha to 1 bit. then return packed rgb10
    return !!expanded.a | (expanded.r << 30) | (expanded.b << 20) | (expanded.a << 10);
}

/* Get a colour for a palette index. (The colours are 24 bit RGB888 with a 0xFF alpha added for some purposes.) */
uint32_t nv3_render_get_palette_index(uint8_t index)
{
    uint32_t red_index = index * 3;
    uint32_t green_index = red_index + 1; 
    uint32_t blue_index = red_index + 2; 

    uint8_t red_colour = nv3->pramdac.palette[red_index];
    uint8_t green_colour = nv3->pramdac.palette[green_index];
    uint8_t blue_colour = nv3->pramdac.palette[blue_index];
    
    /* Alpha is always 0xFF */
    return (0xFF << 24) | ((red_colour) << 16) | ((green_colour) << 8) | blue_colour; 
}

/* Convert a rgb10 colour to a pattern colour */
void nv3_render_set_pattern_color(nv3_color_expanded_t pattern_colour, bool use_color1)
{
    /* select the right pattern colour, _rgb is already in RGB10 format, so we don't need to do any conversion */

    if (!use_color1)
    {
        nv3->pgraph.pattern_color_0_alpha = (pattern_colour.a) & 0xFF;
        nv3->pgraph.pattern_color_0_rgb.r = pattern_colour.r;
        nv3->pgraph.pattern_color_0_rgb.g = pattern_colour.g;
        nv3->pgraph.pattern_color_0_rgb.b = pattern_colour.b;

    }
    else 
    {
        nv3->pgraph.pattern_color_1_alpha = (pattern_colour.a) & 0xFF;
        nv3->pgraph.pattern_color_1_rgb.r = pattern_colour.r;
        nv3->pgraph.pattern_color_1_rgb.g = pattern_colour.g;
        nv3->pgraph.pattern_color_1_rgb.b = pattern_colour.b;
    }
    
}

/* Combine the current buffer with the pitch to get the address in the framebuffer to draw from for a given position. */
uint32_t nv3_render_get_vram_address(nv3_coord_16_t position, nv3_grobj_t grobj)
{
    uint32_t vram_x = position.x;
    uint32_t vram_y = position.y;
    uint32_t current_buffer = (grobj.grobj_0 >> NV3_PGRAPH_CONTEXT_SWITCH_SRC_BUFFER) & 0x03; 
    
    // Get buffer pixel format from bpixel register
    uint32_t buffer_fmt = (nv3->pgraph.bpixel[current_buffer] & 0x03);

    // Convert bpixel format to bytes per pixel for address calculation 
    switch (buffer_fmt) {
        case 0x01: // 8bpp
            break;
        case 0x02: // 15/16bpp
            vram_x <<= 1;
            break;
        case 0x04: // 32bpp
            vram_x <<= 2;
            break;
        default:
            break;
    }

    uint32_t pixel_addr_vram = vram_x + (nv3->pgraph.bpitch[current_buffer] * vram_y) + nv3->pgraph.boffset[current_buffer];

    pixel_addr_vram &= nv3->nvbase.svga.vram_mask;

    return pixel_addr_vram;
}

/* Combine the current buffer with the pitch to get the address in the video ram for a specific position relative to a specific framebuffer */
uint32_t nv3_render_get_vram_address_for_buffer(nv3_coord_16_t position, uint32_t buffer)
{
    uint32_t vram_x = position.x;
    uint32_t vram_y = position.y;

    uint32_t framebuffer_bpp = nv3->nvbase.svga.bpp;

    // we have to multiply the x position by the number of bytes per pixel
    switch (framebuffer_bpp)
    {
        case 8:
            break;
        case 15:
        case 16:
            vram_x = position.x << 1;
            break;
        case 32:
            vram_x = position.x << 2;
            break;
    }

    uint32_t pixel_addr_vram = vram_x + (nv3->pgraph.bpitch[buffer] * vram_y) + nv3->pgraph.boffset[buffer];

    pixel_addr_vram &= nv3->nvbase.svga.vram_mask;

    return pixel_addr_vram;
}

/* Convert a dumb framebuffer address to a position. No buffer setup or anything, but just start at 0,0 for address 0. */
nv3_coord_16_t nv3_render_get_dfb_position(uint32_t vram_address)
{
    nv3_coord_16_t pos = {0};

    uint32_t pitch = nv3->nvbase.svga.hdisp;

    if (nv3->nvbase.svga.bpp == 15
    || nv3->nvbase.svga.bpp == 16)
        pitch <<= 1;
    else if (nv3->nvbase.svga.bpp == 32)
        pitch <<= 2;

    pos.y = (vram_address / pitch);
    pos.x = (vram_address % pitch);

    /* Fixup our x position */
    if (nv3->nvbase.svga.bpp == 15
    || nv3->nvbase.svga.bpp == 16)
        pos.x >>= 1; 
    else if (nv3->nvbase.svga.bpp == 32)
        pos.x >>= 2; 
    

    /* there is some strange behaviour where it writes long past the end of the fb */
    if (pos.y >= nv3->nvbase.svga.monitor->target_buffer->h) pos.y = nv3->nvbase.svga.monitor->target_buffer->h - 1;

    return pos; 
}

/* Read an 8bpp pixel from the framebuffer. */
uint8_t nv3_render_read_pixel_8(nv3_coord_16_t position, nv3_grobj_t grobj)
{ 
    // hope you call it with the right bit
    uint32_t vram_address = nv3_render_get_vram_address(position, grobj);

    return nv3->nvbase.svga.vram[vram_address];
}

/* Read an 16bpp pixel from the framebuffer. */
uint16_t nv3_render_read_pixel_16(nv3_coord_16_t position, nv3_grobj_t grobj)
{ 
    // hope you call it with the right bit
    uint32_t vram_address = nv3_render_get_vram_address(position, grobj);

    uint16_t* vram_16 = (uint16_t*)(nv3->nvbase.svga.vram);
    vram_address >>= 1; //convert to 16bit pointer

    return vram_16[vram_address];
}

/* Read an 16bpp pixel from the framebuffer. */
uint32_t nv3_render_read_pixel_32(nv3_coord_16_t position, nv3_grobj_t grobj)
{ 
    // hope you call it with the right bit
    uint32_t vram_address = nv3_render_get_vram_address(position, grobj);

    uint32_t* vram_32 = (uint32_t*)(nv3->nvbase.svga.vram);
    vram_address >>= 2; //convert to 32bit pointer

    return vram_32[vram_address];
}

/* Plots a pixel. */
void nv3_render_write_pixel(nv3_coord_16_t position, uint32_t color, nv3_grobj_t grobj)
{
    bool alpha_enabled = (grobj.grobj_0 >> NV3_PGRAPH_CONTEXT_SWITCH_ALPHA) & 0x01;
    uint32_t current_buffer = (grobj.grobj_0 >> NV3_PGRAPH_CONTEXT_SWITCH_SRC_BUFFER) & 0x03;
    uint32_t buffer_fmt = (nv3->pgraph.bpixel[current_buffer] & 0x03);
    
    int32_t clip_end_x = nv3->pgraph.clip_start.x + nv3->pgraph.clip_size.x;
    int32_t clip_end_y = nv3->pgraph.clip_start.y + nv3->pgraph.clip_size.y;

    // Basic clipping 
    if (position.x < nv3->pgraph.clip_start.x || position.x >= clip_end_x ||
        position.y < nv3->pgraph.clip_start.y || position.y >= clip_end_y)
        return;

    // Get the address to write to
    uint32_t vram_address = nv3_render_get_vram_address(position, grobj);

    // Handle chroma key if enabled
    if (!nv3_render_chroma_test(color, grobj))
        return;

    // Write the pixel based on the buffer format
    switch (buffer_fmt) {
        case 0x01: // 8bpp
            nv3->nvbase.svga.vram[vram_address] = color & 0xFF;
            nv3_render_current_bpp_dfb_8(vram_address);
            break;
            
        case 0x02: // 15/16bpp
            {
                uint16_t* vram_16 = (uint16_t*)(nv3->nvbase.svga.vram);
                vram_16[vram_address >> 1] = color & 0xFFFF;
                nv3_render_current_bpp_dfb_16(vram_address);
            }
            break;
            
        case 0x04: // 32bpp
            {
                uint32_t* vram_32 = (uint32_t*)(nv3->nvbase.svga.vram);
                vram_32[vram_address >> 2] = color;
                nv3_render_current_bpp_dfb_32(vram_address);
            }
            break;
    }
}

/* Current renderer, called for real-time buffer updates */
void nv3_render_current_bpp(svga_t *svga, nv3_coord_16_t position, nv3_coord_16_t size, nv3_grobj_t grobj, bool run_render_check, bool use_destination_buffer)
{
    if (!nv3) return;

    uint32_t buffer_id = 0;
    if (use_destination_buffer) {
        /* Determine destination buffer from enabled bits */
        if ((grobj.grobj_0 >> NV3_PGRAPH_CONTEXT_SWITCH_DST_BUFFER1_ENABLED) & 0x01) buffer_id = 1;  
        if ((grobj.grobj_0 >> NV3_PGRAPH_CONTEXT_SWITCH_DST_BUFFER2_ENABLED) & 0x01) buffer_id = 2;  
        if ((grobj.grobj_0 >> NV3_PGRAPH_CONTEXT_SWITCH_DST_BUFFER3_ENABLED) & 0x01) buffer_id = 3;
    }

    /* Get pixel format for this buffer */
    uint32_t fmt = nv3->pgraph.bpixel[buffer_id];
    if (!(fmt & (1 << NV3_BPIXEL_FORMAT_IS_VALID))) return;

    fmt &= 0x03; /* Get just the format bits */
    uint32_t addr = nv3_render_get_vram_address_for_buffer(position, buffer_id);

    /* Update dirty region */
    switch (fmt) {
        case bpixel_fmt_8bit:
            /* 8bpp uses full bytes */
            svga->changedvram[addr >> 12] = changeframecount;
            break;
        case bpixel_fmt_16bit:
            /* 16bpp spans two bytes */
            svga->changedvram[addr >> 11] = changeframecount;
            break;
        case bpixel_fmt_32bit:
            /* 32bpp spans four bytes */
            svga->changedvram[addr >> 10] = changeframecount; 
            break;
        default:
            nv_log("Unknown bpixel format %d", fmt);
            break;
    }
}

/* DFB (Dumb Frame Buffer) update handlers */
void nv3_render_current_bpp_dfb_8(uint32_t address) 
{
    if (!nv3) return;
    nv3->nvbase.svga.changedvram[address >> 12] = changeframecount;
}

void nv3_render_current_bpp_dfb_16(uint32_t address)
{
    if (!nv3) return;
    nv3->nvbase.svga.changedvram[address >> 11] = changeframecount;
}

void nv3_render_current_bpp_dfb_32(uint32_t address)
{
    if (!nv3) return;
    nv3->nvbase.svga.changedvram[address >> 10] = changeframecount;
}
