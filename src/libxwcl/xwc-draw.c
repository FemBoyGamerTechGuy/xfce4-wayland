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
/* UTF-8 text rendering against the codepoint-indexed bitmap tables.
 *
 * The tables cover printable ASCII, Latin-1 Supplement, Latin
 * Extended-A/B (U+00A0..U+024F) and general punctuation (U+2000..U+203F,
 * dashes/quotes/bullets/ellipsis). Codepoints outside the coverage
 * render a visible hollow "tofu" box so a missing glyph can never look
 * like silently dropped text (the old ASCII-only raster rendered every
 * non-ASCII letter as an invisible blank gap — application names
 * appeared to be missing letters). Malformed UTF-8 (mid-sequence
 * truncation) advances without drawing; the callers clamp strings at
 * codepoint boundaries (panel_text_fit) so it should not occur.
 *
 * Glyphs sit on a baseline at pen y + ascent; per-glyph offsets shift
 * the bitmap relative to the pen. */

/* decode one UTF-8 sequence; returns the codepoint or 0xFFFFFFFF for a
 * malformed sequence. Always advances *pp by at least one byte. */
static uint32_t utf8_next(const unsigned char **pp) {
    const unsigned char *s = *pp;
    unsigned char c = *s;
    if (c < 0x80) {
        *pp = s + 1;
        return c;
    }
    int len;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0) {
        len = 2;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4;
        cp = c & 0x07;
    } else {
        *pp = s + 1;
        return 0xFFFFFFFF;
    }
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *pp = s + 1;
            return 0xFFFFFFFF;
        }
        cp = (cp << 6) | (uint32_t)(s[i] & 0x3F);
    }
    *pp = s + len;
    return cp;
}

struct xw_font_tables {
    const struct xw_glyph *main; /* XW_FONT_FIRST..XW_FONT_LAST */
    const struct xw_glyph *punct; /* XW_FONT_P_FIRST..XW_FONT_P_LAST */
    int ascent;
};

static const struct xw_glyph *glyph_for(uint32_t cp,
                                        const struct xw_font_tables *ft) {
    if (cp >= XW_FONT_FIRST && cp <= XW_FONT_LAST)
        return &ft->main[cp - XW_FONT_FIRST];
    if (cp >= XW_FONT_P_FIRST && cp <= XW_FONT_P_LAST)
        return &ft->punct[cp - XW_FONT_P_FIRST];
    return NULL;
}

/* advance width + glyph fetch in one pass: *g_out = NULL when the
 * codepoint has no drawable glyph (caller draws the fallback) */
static int glyph_adv(uint32_t cp, const struct xw_font_tables *ft,
                     const struct xw_glyph **g_out) {
    const struct xw_glyph *g = glyph_for(cp, ft);
    if (g && g->bits) {
        *g_out = g;
        return g->adv;
    }
    if (g && !g->bits) {
        /* in range but the font lacks the glyph: space-width advance */
        *g_out = NULL;
        return ft->ascent / 2;
    }
    /* outside the rasterized ranges (CJK, emoji, ...): fallback width */
    *g_out = NULL;
    return ft->ascent / 2;
}

/* the visible missing-glyph box ("tofu"): a thin hollow rectangle the
 * height of an x-height, so uncovered codepoints are unmistakably
 * present instead of silently vanishing */
static void draw_tofu(uint32_t *pix, int stride, int w, int h, int x, int y,
                      int adv, int ascent, uint32_t color) {
    int bw = adv > 2 ? adv - 1 : 2;
    int bh = ascent > 6 ? ascent * 2 / 3 : 4;
    int top = y + ascent - bh;
    uint32_t c = (color & 0x00ffffffu) | 0x80000000u;
    for (int i = 0; i < bw && x + i < w; i++) {
        if (x + i < 0 || top < 0 || top >= h || top + bh >= h)
            continue;
        pix[top * stride + x + i] = blend(pix[top * stride + x + i], c);
        pix[(top + bh) * stride + x + i] =
            blend(pix[(top + bh) * stride + x + i], c);
    }
    for (int j = 0; j <= bh && top + j < h; j++) {
        if (top + j < 0 || x < 0 || x >= w || x + bw >= w)
            continue;
        pix[(top + j) * stride + x] = blend(pix[(top + j) * stride + x], c);
        pix[(top + j) * stride + x + bw] =
            blend(pix[(top + j) * stride + x + bw], c);
    }
}

static int draw_text_tbl(uint32_t *pix, int stride, int w, int h, int x,
                         int y, const char *text, uint32_t color,
                         const struct xw_font_tables *ft) {
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        const struct xw_glyph *g = NULL;
        int adv = glyph_adv(cp, ft, &g);
        if (cp == 0xFFFFFFFF) {
            x += 2; /* malformed byte: skip, never draw garbage */
            continue;
        }
        if (!g) {
            if (pix)
                draw_tofu(pix, stride, w, h, x, y, adv, ft->ascent, color);
            x += adv;
            continue;
        }
        for (int j = 0; j < g->h; j++) {
            int py = y + ft->ascent + g->yoff + j;
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
        x += adv;
    }
    return x;
}

static int text_width_tbl(const char *text, const struct xw_font_tables *ft) {
    int x = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        const struct xw_glyph *g = NULL;
        x += glyph_adv(cp, ft, &g);
    }
    return x;
}

static const struct xw_font_tables g_font1 = {xw_glyph_table, xw_glyph_ptable,
                                              XW_FONT_ASCENT};
static const struct xw_font_tables g_font2 = {xw_glyph_table2,
                                              xw_glyph_ptable2,
                                              XW_FONT2_ASCENT};

int xwc_text_width(const char *text) {
    return text_width_tbl(text, &g_font1);
}

int xwc_draw_text(uint32_t *pix, int stride, int w, int h, int x, int y,
                  const char *text, uint32_t color) {
    /* y = top of the line box; glyphs sit on the baseline */
    return draw_text_tbl(pix, stride, w, h, x, y, text, color, &g_font1);
}

int xwc_text_width2(const char *text) {
    return text_width_tbl(text, &g_font2);
}

int xwc_draw_text2(uint32_t *pix, int stride, int w, int h, int x, int y,
                   const char *text, uint32_t color) {
    return draw_text_tbl(pix, stride, w, h, x, y, text, color, &g_font2);
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
