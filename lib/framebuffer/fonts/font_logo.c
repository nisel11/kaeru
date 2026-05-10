//
// SPDX-FileCopyrightText: 2026 Nisel <nisel11good@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//

#include <lib/framebuffer.h>
#include <lib/string.h>

#define FONT_ADV(font, idx) ((font)->data[(idx) * 4 + 2])
#define FONT_METRICS_IDX    95
#define FONT_LINE_HEIGHT(f) ((f)->data[FONT_METRICS_IDX * 4 + 0])
#define FONT_CELL_SIZE(f)   ((f)->data[FONT_METRICS_IDX * 4 + 1])

static fb_font_t *g_active_font = NULL;
static uint32_t g_last_width = 8;

void fb_font_select(fb_font_t *font) {
    g_active_font = font;
    if (font) g_last_width = (uint32_t)FONT_ADV(font, 0);
}

static uint32_t font_draw_glyph(uint32_t x, uint32_t y, char c, uint32_t color, const fb_font_t *font) {
    if (!font || !font->data || !font->sheet_stride) return 0;

    uint32_t cell = (uint32_t)FONT_CELL_SIZE(font);

    int ch = (int)(unsigned char)c;
    if (ch < 32 || ch >= 32 + 95) ch = 32;
    int idx = ch - 32;

    uint8_t fg_r = (color >> 16) & 0xFF;
    uint8_t fg_g = (color >> 8) & 0xFF;
    uint8_t fg_b = color & 0xFF;

    fb_config_t *config = fb_get_config();
    uint32_t *fb = config->buffer;
    uint32_t stride = config->stride / 4;
    uint16_t ss = font->sheet_stride;

    int ss_int = (int)ss;
    uint8_t *gb = font->data
                  + (1 + (idx / 12) * cell) * ss_int
                  + (idx % 12) * cell * 4;

    for (uint32_t gy = 0; gy < cell; gy++) {
        uint8_t *row = gb + gy * ss_int;
        for (uint32_t gx = 0; gx < cell; gx++) {
            uint8_t a = row[gx * 4 + 3];
            if (!a) continue;

            uint32_t px = x + gx, py = y + gy;
            if (px >= config->width || py >= config->height) continue;

            uint32_t out;
            if (a == 0xFF) {
                out = 0xFF000000u | color;
            } else {
                out = 0xFF000000u
                    | ((uint32_t)((fg_r * a) / 255u) << 16)
                    | ((uint32_t)((fg_g * a) / 255u) <<  8)
                    |  (uint32_t)((fg_b * a) / 255u);
            }
            fb[py * stride + px] = out;
        }
    }

    return (uint32_t)FONT_ADV(font, idx);
}

void fb_char(uint32_t x, uint32_t y, char c, uint32_t color) {
    g_last_width = font_draw_glyph(x, y, c, color, g_active_font);
}

uint32_t fb_get_char_width(void) {
    return g_last_width;
}

uint32_t fb_get_char_height(void) {
    if (!g_active_font) return 8;
    return (uint32_t)FONT_LINE_HEIGHT(g_active_font) + 4;
}

void fb_font_draw_str(uint32_t x, uint32_t y, const char *str, uint32_t color, const fb_font_t *font) {
    if (!font || !font->data) {
        fb_text(x, y, str, color);
        return;
    }

    uint32_t ox = x;

    while (*str) {
        if (*str == '\n') {
            x  = ox;
            y += (uint32_t)FONT_LINE_HEIGHT(font) + 4;
            str++;
            continue;
        }

        x += font_draw_glyph(x, y, *str, color, font);
        str++;
    }
}

uint32_t fb_font_str_width(const char *str, const fb_font_t *font) {
    if (!font || !font->data)
        return (uint32_t)(strlen(str) * fb_get_char_width());

    uint32_t w = 0;
    while (*str) {
        int ch = (int)(unsigned char)*str;
        if (ch == '\n') break;
        if (ch >= 32 && ch < 32 + 95)
            w += FONT_ADV(font, ch - 32);
        str++;
    }
    return w;
}

int fb_font_logo_load(fb_font_t *font, uint32_t logo_index, void *buf, uint32_t buf_size) {
    void *comp_data;
    uint32_t comp_len;

    if (fb_logo_parse(logo_index, &comp_data, &comp_len) != 0)
        return -1;

    int (*decompress)(void *, void *, int, int) = (int (*)(void *, void *, int, int))(CONFIG_LOGO_DECOMPRESS_ADDR | 1);
    int raw_size = decompress(comp_data, buf, (int)comp_len, (int)buf_size);
    if (raw_size <= 0 || raw_size > (int)buf_size)
        return -1;

    font->data = (uint8_t *)buf;
    font->sheet_stride = (uint16_t)(12 * (uint32_t)FONT_CELL_SIZE(font) * 4);

    if (!font->sheet_stride) {
        font->data = NULL; // metadata row is corrupt, don't use this font
        return -1;
    }

    if (!g_active_font) {
        g_active_font = font;
    }

    return 0;
}
