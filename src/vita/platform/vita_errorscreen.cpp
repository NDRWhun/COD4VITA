#include "vita_errorscreen.h"

#include "vita_system.h"

#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <string.h>

#define SCREEN_WIDTH    960
#define SCREEN_HEIGHT   544
#define SCREEN_PITCH    960
#define SCREEN_BYTES    (SCREEN_PITCH * SCREEN_HEIGHT * 4)

#define GLYPH_WIDTH     5
#define GLYPH_HEIGHT    7
#define CELL_WIDTH      6
#define CELL_HEIGHT     8

#define MARGIN          16
#define TITLE_SCALE     3
#define BODY_SCALE      2

#define DISMISS_BUTTONS (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE | SCE_CTRL_START)
#define DISMISS_SECONDS 120

// 5x7 glyphs for 0x20..0x7E, one byte a row, bit 7 leftmost
static const unsigned char s_font[95][7] =
{
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x20 },
    { 0x50, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x50, 0x50, 0xF8, 0x50, 0xF8, 0x50, 0x50 },
    { 0x20, 0x78, 0xA0, 0x70, 0x28, 0xF0, 0x20 },
    { 0xC0, 0xC8, 0x10, 0x20, 0x40, 0x98, 0x18 },
    { 0x60, 0x90, 0xA0, 0x40, 0xA8, 0x90, 0x68 },
    { 0x20, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x10, 0x20, 0x40, 0x40, 0x40, 0x20, 0x10 },
    { 0x40, 0x20, 0x10, 0x10, 0x10, 0x20, 0x40 },
    { 0x00, 0xA8, 0x70, 0xF8, 0x70, 0xA8, 0x00 },
    { 0x00, 0x20, 0x20, 0xF8, 0x20, 0x20, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x60, 0x20, 0x40 },
    { 0x00, 0x00, 0x00, 0xF8, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x60, 0x60 },
    { 0x08, 0x10, 0x20, 0x20, 0x40, 0x80, 0x80 },
    { 0x70, 0x88, 0x98, 0xA8, 0xC8, 0x88, 0x70 },
    { 0x20, 0x60, 0x20, 0x20, 0x20, 0x20, 0x70 },
    { 0x70, 0x88, 0x08, 0x10, 0x20, 0x40, 0xF8 },
    { 0xF8, 0x10, 0x20, 0x10, 0x08, 0x88, 0x70 },
    { 0x10, 0x30, 0x50, 0x90, 0xF8, 0x10, 0x10 },
    { 0xF8, 0x80, 0xF0, 0x08, 0x08, 0x88, 0x70 },
    { 0x30, 0x40, 0x80, 0xF0, 0x88, 0x88, 0x70 },
    { 0xF8, 0x08, 0x10, 0x20, 0x40, 0x40, 0x40 },
    { 0x70, 0x88, 0x88, 0x70, 0x88, 0x88, 0x70 },
    { 0x70, 0x88, 0x88, 0x78, 0x08, 0x10, 0x60 },
    { 0x00, 0x60, 0x60, 0x00, 0x60, 0x60, 0x00 },
    { 0x00, 0x60, 0x60, 0x00, 0x60, 0x20, 0x40 },
    { 0x10, 0x20, 0x40, 0x80, 0x40, 0x20, 0x10 },
    { 0x00, 0x00, 0xF8, 0x00, 0xF8, 0x00, 0x00 },
    { 0x40, 0x20, 0x10, 0x08, 0x10, 0x20, 0x40 },
    { 0x70, 0x88, 0x08, 0x10, 0x20, 0x00, 0x20 },
    { 0x70, 0x88, 0xB8, 0xA8, 0xB8, 0x80, 0x70 },
    { 0x20, 0x50, 0x88, 0x88, 0xF8, 0x88, 0x88 },
    { 0xF0, 0x88, 0x88, 0xF0, 0x88, 0x88, 0xF0 },
    { 0x70, 0x88, 0x80, 0x80, 0x80, 0x88, 0x70 },
    { 0xE0, 0x90, 0x88, 0x88, 0x88, 0x90, 0xE0 },
    { 0xF8, 0x80, 0x80, 0xF0, 0x80, 0x80, 0xF8 },
    { 0xF8, 0x80, 0x80, 0xF0, 0x80, 0x80, 0x80 },
    { 0x70, 0x88, 0x80, 0x98, 0x88, 0x88, 0x78 },
    { 0x88, 0x88, 0x88, 0xF8, 0x88, 0x88, 0x88 },
    { 0x70, 0x20, 0x20, 0x20, 0x20, 0x20, 0x70 },
    { 0x38, 0x10, 0x10, 0x10, 0x10, 0x90, 0x60 },
    { 0x88, 0x90, 0xA0, 0xC0, 0xA0, 0x90, 0x88 },
    { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0xF8 },
    { 0x88, 0xD8, 0xA8, 0xA8, 0x88, 0x88, 0x88 },
    { 0x88, 0x88, 0xC8, 0xA8, 0x98, 0x88, 0x88 },
    { 0x70, 0x88, 0x88, 0x88, 0x88, 0x88, 0x70 },
    { 0xF0, 0x88, 0x88, 0xF0, 0x80, 0x80, 0x80 },
    { 0x70, 0x88, 0x88, 0x88, 0xA8, 0x90, 0x68 },
    { 0xF0, 0x88, 0x88, 0xF0, 0xA0, 0x90, 0x88 },
    { 0x78, 0x80, 0x80, 0x70, 0x08, 0x08, 0xF0 },
    { 0xF8, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20 },
    { 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x70 },
    { 0x88, 0x88, 0x88, 0x88, 0x88, 0x50, 0x20 },
    { 0x88, 0x88, 0x88, 0xA8, 0xA8, 0xD8, 0x88 },
    { 0x88, 0x88, 0x50, 0x20, 0x50, 0x88, 0x88 },
    { 0x88, 0x88, 0x50, 0x20, 0x20, 0x20, 0x20 },
    { 0xF8, 0x08, 0x10, 0x20, 0x40, 0x80, 0xF8 },
    { 0x70, 0x40, 0x40, 0x40, 0x40, 0x40, 0x70 },
    { 0x80, 0x80, 0x40, 0x20, 0x20, 0x10, 0x08 },
    { 0x70, 0x10, 0x10, 0x10, 0x10, 0x10, 0x70 },
    { 0x20, 0x50, 0x88, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8 },
    { 0x40, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x00, 0x00, 0x70, 0x08, 0x78, 0x88, 0x78 },
    { 0x80, 0x80, 0xF0, 0x88, 0x88, 0x88, 0xF0 },
    { 0x00, 0x00, 0x70, 0x88, 0x80, 0x88, 0x70 },
    { 0x08, 0x08, 0x78, 0x88, 0x88, 0x88, 0x78 },
    { 0x00, 0x00, 0x70, 0x88, 0xF8, 0x80, 0x70 },
    { 0x30, 0x48, 0x40, 0xE0, 0x40, 0x40, 0x40 },
    { 0x00, 0x78, 0x88, 0x88, 0x78, 0x08, 0x70 },
    { 0x80, 0x80, 0xF0, 0x88, 0x88, 0x88, 0x88 },
    { 0x20, 0x00, 0x60, 0x20, 0x20, 0x20, 0x70 },
    { 0x10, 0x00, 0x30, 0x10, 0x10, 0x90, 0x60 },
    { 0x80, 0x80, 0x90, 0xA0, 0xC0, 0xA0, 0x90 },
    { 0x60, 0x20, 0x20, 0x20, 0x20, 0x20, 0x70 },
    { 0x00, 0x00, 0xD0, 0xA8, 0xA8, 0x88, 0x88 },
    { 0x00, 0x00, 0xF0, 0x88, 0x88, 0x88, 0x88 },
    { 0x00, 0x00, 0x70, 0x88, 0x88, 0x88, 0x70 },
    { 0x00, 0xF0, 0x88, 0x88, 0xF0, 0x80, 0x80 },
    { 0x00, 0x78, 0x88, 0x88, 0x78, 0x08, 0x08 },
    { 0x00, 0x00, 0xB0, 0xC8, 0x80, 0x80, 0x80 },
    { 0x00, 0x00, 0x78, 0x80, 0x70, 0x08, 0xF0 },
    { 0x40, 0x40, 0xE0, 0x40, 0x40, 0x48, 0x30 },
    { 0x00, 0x00, 0x88, 0x88, 0x88, 0x98, 0x68 },
    { 0x00, 0x00, 0x88, 0x88, 0x88, 0x50, 0x20 },
    { 0x00, 0x00, 0x88, 0x88, 0xA8, 0xA8, 0x50 },
    { 0x00, 0x00, 0x88, 0x50, 0x20, 0x50, 0x88 },
    { 0x00, 0x88, 0x88, 0x88, 0x78, 0x08, 0x70 },
    { 0x00, 0x00, 0xF8, 0x10, 0x20, 0x40, 0xF8 },
    { 0x18, 0x20, 0x20, 0x40, 0x20, 0x20, 0x18 },
    { 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20 },
    { 0xC0, 0x20, 0x20, 0x10, 0x20, 0x20, 0xC0 },
    { 0x00, 0x48, 0xA8, 0x90, 0x00, 0x00, 0x00 },
};

static uint32_t *s_pixels;
static SceUID s_block = -1;

// the display reads A8B8G8R8, so red sits in the low byte
static uint32_t VitaErrorScreen_Colour(uint32_t r, uint32_t g, uint32_t b)
{
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

static bool VitaErrorScreen_Acquire(void)
{
    if (s_pixels)
        return true;

    // video memory first, since a report after an out-of-memory has little main memory left
    const uint32_t cdramSize = (SCREEN_BYTES + 0x3FFFF) & ~0x3FFFFu;
    s_block = sceKernelAllocMemBlock("kcod_errorscreen",
                                     SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, cdramSize, NULL);
    if (s_block < 0)
    {
        const uint32_t mainSize = (SCREEN_BYTES + 0xFFF) & ~0xFFFu;
        s_block = sceKernelAllocMemBlock("kcod_errorscreen",
                                         SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, mainSize, NULL);
    }
    if (s_block < 0)
        return false;

    void *base = NULL;
    if (sceKernelGetMemBlockBase(s_block, &base) < 0 || !base)
    {
        sceKernelFreeMemBlock(s_block);
        s_block = -1;
        return false;
    }

    s_pixels = (uint32_t *)base;
    return true;
}

static int VitaErrorScreen_Present(void)
{
    SceDisplayFrameBuf frame;
    memset(&frame, 0, sizeof(frame));
    frame.size = sizeof(frame);
    frame.base = s_pixels;
    frame.pitch = SCREEN_PITCH;
    frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    frame.width = SCREEN_WIDTH;
    frame.height = SCREEN_HEIGHT;
    // IMMEDIATE is the exception-handler mode; the ordinary path is the one the hardware expects
    return sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

static void VitaErrorScreen_FillRows(int y, int height, uint32_t colour)
{
    if (y < 0)
        y = 0;
    if (y + height > SCREEN_HEIGHT)
        height = SCREEN_HEIGHT - y;
    for (int row = 0; row < height; ++row)
    {
        uint32_t *line = &s_pixels[(y + row) * SCREEN_PITCH];
        for (uint32_t x = 0; x < SCREEN_PITCH; ++x)
            line[x] = colour;
    }
}

static void VitaErrorScreen_Fill(uint32_t colour)
{
    for (uint32_t i = 0; i < SCREEN_PITCH * SCREEN_HEIGHT; ++i)
        s_pixels[i] = colour;
}

static void VitaErrorScreen_Glyph(char character, int x, int y, int scale, uint32_t colour)
{
    if ((unsigned char)character < 0x20 || (unsigned char)character > 0x7E)
        character = '?';

    const unsigned char *glyph = s_font[(unsigned char)character - 0x20];
    for (int row = 0; row < GLYPH_HEIGHT; ++row)
    {
        for (int column = 0; column < GLYPH_WIDTH; ++column)
        {
            if (!(glyph[row] & (0x80 >> column)))
                continue;

            for (int sy = 0; sy < scale; ++sy)
            {
                const int py = y + row * scale + sy;
                if (py < 0 || py >= SCREEN_HEIGHT)
                    continue;
                uint32_t *line = s_pixels + py * SCREEN_PITCH;
                for (int sx = 0; sx < scale; ++sx)
                {
                    const int px = x + column * scale + sx;
                    if (px >= 0 && px < SCREEN_WIDTH)
                        line[px] = colour;
                }
            }
        }
    }
}

static void VitaErrorScreen_Text(const char *text, int x, int y, int scale, uint32_t colour)
{
    for (int i = 0; text[i]; ++i)
        VitaErrorScreen_Glyph(text[i], x + i * CELL_WIDTH * scale, y, scale, colour);
}

// wraps on width, breaking at the last space where one is in reach; returns the next y
static int VitaErrorScreen_Paragraph(const char *text, int x, int y, int scale, uint32_t colour)
{
    const int columns = (SCREEN_WIDTH - 2 * MARGIN) / (CELL_WIDTH * scale);
    const int lineHeight = CELL_HEIGHT * scale;

    char line[128];
    const char *cursor = text;

    while (*cursor && y + lineHeight <= SCREEN_HEIGHT - MARGIN)
    {
        int count = 0;
        int lastSpace = -1;
        while (count < columns && count < (int)sizeof(line) - 1 && cursor[count]
               && cursor[count] != '\n' && cursor[count] != '\r')
        {
            if (cursor[count] == ' ')
                lastSpace = count;
            ++count;
        }

        int advance = count;
        if (cursor[count] && cursor[count] != '\n' && cursor[count] != '\r'
            && lastSpace > columns / 2)
        {
            count = lastSpace;
            advance = lastSpace + 1;
        }

        memcpy(line, cursor, count);
        line[count] = 0;
        VitaErrorScreen_Text(line, x, y, scale, colour);
        y += lineHeight;

        cursor += advance;
        while (*cursor == '\n' || *cursor == '\r')
            ++cursor;
    }

    return y;
}

bool VitaErrorScreen_Show(const char *title, const char *body, const char *footer)
{
    if (!VitaErrorScreen_Acquire())
        return false;

    const uint32_t background = VitaErrorScreen_Colour(24, 8, 8);
    const uint32_t titleColour = VitaErrorScreen_Colour(255, 96, 64);
    const uint32_t bodyColour = VitaErrorScreen_Colour(232, 232, 232);
    const uint32_t footerColour = VitaErrorScreen_Colour(144, 144, 144);

    VitaErrorScreen_Fill(background);

    int y = MARGIN;
    if (title)
    {
        VitaErrorScreen_Text(title, MARGIN, y, TITLE_SCALE, titleColour);
        y += CELL_HEIGHT * TITLE_SCALE + CELL_HEIGHT * BODY_SCALE;
    }
    if (body)
        y = VitaErrorScreen_Paragraph(body, MARGIN, y, BODY_SCALE, bodyColour);
    if (footer)
    {
        const int footerY = SCREEN_HEIGHT - MARGIN - CELL_HEIGHT * BODY_SCALE;
        VitaErrorScreen_Paragraph(footer, MARGIN, y < footerY ? footerY : y, BODY_SCALE,
                                  footerColour);
    }

    // whatever is held on entry counts as already seen, so the press that got here does not dismiss
    uint32_t previous = DISMISS_BUTTONS;

    // the buffer is re-asserted every frame, so a flip left queued in GXM cannot hide the report
    for (uint32_t frames = 0; frames < DISMISS_SECONDS * 60; ++frames)
    {
        VitaErrorScreen_Present();
        sceDisplayWaitVblankStart();

        SceCtrlData pad;
        if (sceCtrlPeekBufferPositive(0, &pad, 1) < 0)
            continue;

        const uint32_t pressed = pad.buttons & ~previous;
        previous = pad.buttons;
        if (pressed & DISMISS_BUTTONS)
            break;
    }

    return true;
}

// --- the boot screen, which holds the display until the renderer owns it ---

static bool s_bootScreenDone;

void VitaBootScreen_Disable(void)
{
    s_bootScreenDone = true;
}

void VitaBootScreen_Tick(const char *status)
{
    if (s_bootScreenDone)
        return;

    // boot is long enough that the system gives up on an application that has shown nothing
    static uint64_t last;
    const uint64_t now = sceKernelGetProcessTimeWide();
    if (last && now - last < 250000ull)
        return;
    last = now;

    if (!VitaErrorScreen_Acquire())
    {
        s_bootScreenDone = true;
        VitaSys_LogPrint("boot screen: no framebuffer could be allocated\n");
        return;
    }

    const uint32_t background = VitaErrorScreen_Colour(0, 0, 0);
    const int statusY = MARGIN + CELL_HEIGHT * TITLE_SCALE * 2;
    const int statusRows = CELL_HEIGHT * BODY_SCALE * 3;

    // the buffer is in CDRAM, where a full clear each tick costs more than the boot screen saves
    static bool painted;
    if (!painted)
    {
        VitaErrorScreen_Fill(background);
        VitaErrorScreen_Text("Call of Duty 4", MARGIN, MARGIN, TITLE_SCALE,
                             VitaErrorScreen_Colour(220, 220, 220));
        painted = true;
    }

    VitaErrorScreen_FillRows(statusY, statusRows, background);
    if (status)
        VitaErrorScreen_Paragraph(status, MARGIN, statusY, BODY_SCALE,
                                  VitaErrorScreen_Colour(150, 150, 150));

    const int result = VitaErrorScreen_Present();

    // the recursive log call lands back here and stops at the rate limit above
    static bool reported;
    if (!reported)
    {
        reported = true;
        VitaSys_LogPrintf("boot screen: buffer %p, sceDisplaySetFrameBuf returned 0x%08x\n",
                          (void *)s_pixels, (unsigned)result);
    }
}
