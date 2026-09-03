/* xwc-draw.c — libxwcl pixel drawing with the build-time bitmap font. */
#include "xwc.h"

#include "xw-font-data.h"

/* straight-alpha blend of `color` (0xAARRGGBB) over dst pixel */
static inline uint32_t blend(uint32_t dst, uint32_t color) {
    uint32_t a = color >> 24;
    if (a == 255)
        return color;
    uint32_t inv = 255 - a;
    uint32_t dr = (dst >> 16) & 0xff, dg = (dst >> 8) & 0xff, db = dst & 0xff;
    uint32_t sr = (color >> 16) & 0xff, sg = (color >> 8) & 0xff,
             sb = color & 0xff;
    uint32_t r = (a * sr + inv * dr + 127) / 255;
    uint32_t g = (a * sg + inv * dg + 127) / 255;
    uint32_t b = (a * sb + inv * db + 127) / 255;
    return 0xff000000u | (r << 16) | (g << 8) | b;
}

void xwc_fill_rect(uint32_t *pix, int stride, int w, int h, int x, int y,
                   int rw, int rh, uint32_t color) {
    if (x < 0) {
        rw += x;
        x = 0;
    }
    if (y < 0) {
        rh += y;
        y = 0;
    }
    if (x + rw > w)
        rw = w - x;
    if (y + rh > h)
        rh = h - y;
    if (rw <= 0 || rh <= 0)
        return;
    if ((color >> 24) == 255) {
        for (int j = 0; j < rh; j++)
            for (int i = 0; i < rw; i++)
                pix[(y + j) * stride + x + i] = color;
    } else {
        for (int j = 0; j < rh; j++)
            for (int i = 0; i < rw; i++)
                pix[(y + j) * stride + x + i] =
                    blend(pix[(y + j) * stride + x + i], color);
    }
}

void xwc_draw_hline(uint32_t *pix, int stride, int w, int h, int x, int y,
                    int len, uint32_t color) {
    xwc_fill_rect(pix, stride, w, h, x, y, len, 1, color);
}

void xwc_draw_vline(uint32_t *pix, int stride, int w, int h, int x, int y,
                    int len, uint32_t color) {
    xwc_fill_rect(pix, stride, w, h, x, y, 1, len, color);
}

/* ---------------------------------------------------------------- text */
/* One rasterization of the glyph blitter, parameterized by the table:
 * glyphs sit on a baseline at pen y + ascent; per-glyph offsets shift
 * the bitmap relative to the pen. */
static int draw_text_tbl(uint32_t *pix, int stride, int w, int h, int x,
                         int y, const char *text, uint32_t color,
                         const struct xw_glyph *table, int ascent) {
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p < XW_FONT_FIRST || *p > XW_FONT_LAST) {
            x += ascent / 2;
            continue;
        }
        const struct xw_glyph *g = &table[*p - XW_FONT_FIRST];
        if (!g->bits) {
            x += g->adv;
            continue;
        }
        for (int j = 0; j < g->h; j++) {
            int py = y + ascent + g->yoff + j;
            if (py < 0 || py >= h)
                continue;
            for (int i = 0; i < g->w; i++) {
                int px = x + g->xoff + i;
                if (px < 0 || px >= w)
                    continue;
                uint8_t alpha = g->bits[j * g->w + i];
                if (!alpha)
                    continue;
                uint32_t c =
                    (color & 0x00ffffffu) | ((uint32_t)alpha << 24);
                pix[py * stride + px] = blend(pix[py * stride + px], c);
            }
        }
        x += g->adv;
    }
    return x;
}

static int text_width_tbl(const char *text, const struct xw_glyph *table,
                          int ascent) {
    int x = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p < XW_FONT_FIRST || *p > XW_FONT_LAST)
            x += ascent / 2;
        else
            x += table[*p - XW_FONT_FIRST].adv;
    }
    return x;
}

int xwc_text_width(const char *text) {
    return text_width_tbl(text, xw_glyph_table, XW_FONT_ASCENT);
}

int xwc_draw_text(uint32_t *pix, int stride, int w, int h, int x, int y,
                  const char *text, uint32_t color) {
    /* y = top of the line box; glyphs sit on the baseline */
    return draw_text_tbl(pix, stride, w, h, x, y, text, color, xw_glyph_table,
                         XW_FONT_ASCENT);
}

int xwc_text_width2(const char *text) {
    return text_width_tbl(text, xw_glyph_table2, XW_FONT2_ASCENT);
}

int xwc_draw_text2(uint32_t *pix, int stride, int w, int h, int x, int y,
                   const char *text, uint32_t color) {
    return draw_text_tbl(pix, stride, w, h, x, y, text, color,
                         xw_glyph_table2, XW_FONT2_ASCENT);
}

/* ---------------------------------------------------------------- icon */
void xwc_draw_icon(uint32_t *pix, int stride, int w, int h, int x, int y,
                   const struct xwc_icon *ic, int size) {
    if (!ic || !ic->pix || size < 1)
        return;
    /* center the (already cell-sized) surface in a size x size cell */
    int ox = x + (size - ic->w) / 2;
    int oy = y + (size - ic->h) / 2;
    for (int j = 0; j < ic->h; j++) {
        int py = oy + j;
        if (py < 0 || py >= h)
            continue;
        for (int i = 0; i < ic->w; i++) {
            int px = ox + i;
            if (px < 0 || px >= w)
                continue;
            pix[py * stride + px] = blend(pix[py * stride + px],
                                          ic->pix[j * ic->w + i]);
        }
    }
}

void xwc_draw_box(uint32_t *pix, int stride, int w, int h, int x, int y,
                  int bw, int bh, uint32_t fill, uint32_t border) {
    /* 2px rounded border + flat fill; corners cut by 2 pixels */
    xwc_fill_rect(pix, stride, w, h, x + 1, y, bw - 2, bh, border);
    xwc_fill_rect(pix, stride, w, h, x, y + 1, bw, bh - 2, border);
    xwc_fill_rect(pix, stride, w, h, x + 3, y + 3, bw - 6, bh - 6, fill);
    xwc_fill_rect(pix, stride, w, h, x + 2, y + 2, bw - 4, bh - 4, fill);
    xwc_fill_rect(pix, stride, w, h, x + 2, y + 2, 2, 2, fill);
    xwc_fill_rect(pix, stride, w, h, x + bw - 4, y + 2, 2, 2, fill);
    xwc_fill_rect(pix, stride, w, h, x + 2, y + bh - 4, 2, 2, fill);
    xwc_fill_rect(pix, stride, w, h, x + bw - 4, y + bh - 4, 2, 2, fill);
}
