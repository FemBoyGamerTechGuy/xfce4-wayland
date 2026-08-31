/* xw-render.c — pixman software compositor: layers, windows, popups,
 * snap preview and software cursor. Renders into the output's logical
 * buffer; integer upscaling to native happens in xw-output.c.
 */
#include "xw-internal.h"

#include <stdlib.h>
#include <string.h>

static const char *cursor_art[] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.....XXXXX ",
    "X..X..X     ",
    "X.X X..X    ",
    "XX  X..X    ",
    "X    X..X   ",
    "     X..X   ",
    "      XX    ",
};
#define CURSOR_W 12
#define CURSOR_H 17

/* pixman 0.44 has no fill_rects; fill via fill_boxes with a straight-alpha
 * pixman_color_t (channels widened to 16 bit). `color` is straight (not
 * premultiplied) ARGB8888. */
void xw_render_fill_rect(pixman_image_t *dst, pixman_op_t op, uint32_t color,
                         int x, int y, int w, int h) {
    if (w <= 0 || h <= 0)
        return;
    pixman_color_t c = {
        .alpha = (uint16_t)(((color >> 24) & 0xff) * 0x101),
        .red = (uint16_t)(((color >> 16) & 0xff) * 0x101),
        .green = (uint16_t)(((color >> 8) & 0xff) * 0x101),
        .blue = (uint16_t)((color & 0xff) * 0x101),
    };
    pixman_box32_t box = { x, y, x + w, y + h };
    pixman_image_fill_boxes(op, dst, &c, 1, &box);
}

static void draw_cursor(struct xw_output *o, struct xw_seat *seat) {
    if (!seat)
        return;
    int cx = seat->cursor_x - o->x, cy = seat->cursor_y - o->y;
    if (cx < -CURSOR_W || cy < -CURSOR_H || cx > o->width || cy > o->height)
        return;
    uint32_t black = 0xff000000, white = 0xffffffff;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t color = pass == 0 ? black : white;
        int dx = pass == 0 ? 1 : 0, dy = pass == 0 ? 1 : 0;
        for (int r = 0; r < CURSOR_H; r++) {
            const char *row = cursor_art[r];
            for (int cc = 0; cc < CURSOR_W; cc++) {
                if (row[cc] != 'X')
                    continue;
                int px = cx + cc + dx, py = cy + r + dy;
                if (px < 0 || py < 0 || px >= o->width || py >= o->height)
                    continue;
                xw_render_fill_rect(o->logical, PIXMAN_OP_SRC, color, px, py, 1,
                                    1);
            }
        }
    }
}

static void blit_surface(struct xw_output *o, struct xw_surface *s, int gx,
                         int gy, int gw, int gh) {
    pixman_image_t *src = xw_surface_get_image(s);
    if (!src)
        return;
    int sx = gx - o->x, sy = gy - o->y;
    int bw = s->buf_w, bh = s->buf_h;
    int sc = s->scale > 0 ? s->scale : 1;
    if (bw == gw && bh == gh && sc == 1) {
        pixman_image_composite(PIXMAN_OP_OVER, src, NULL, o->logical, 0, 0, 0, 0,
                               sx, sy, gw, gh);
    } else {
        /* scale the buffer to the target geometry (buffer scale / CSD
         * mismatch tolerance) */
        pixman_transform_t t;
        pixman_fixed_t fx = pixman_int_to_fixed(bw) / gw;
        pixman_fixed_t fy = pixman_int_to_fixed(bh) / gh;
        pixman_transform_init_scale(&t, fx, fy);
        pixman_image_set_transform(src, &t);
        pixman_image_composite(PIXMAN_OP_OVER, src, NULL, o->logical, 0, 0, 0, 0,
                               sx, sy, gw, gh);
        pixman_image_set_transform(src, NULL);
    }
    pixman_image_unref(src);
}

static void render_window(struct xw_output *o, struct xw_window *w) {
    if (!w->surface || (!w->surface->shm && !w->surface->has_single_pixel))
        return;
    blit_surface(o, w->surface, w->x, w->y, w->w, w->h);
}


static void render_layer(struct xw_output *o, struct xw_layer_surface *ls) {
    if (!ls->mapped || !ls->surface)
        return;
    if (!ls->surface->shm && !ls->surface->has_single_pixel)
        return;
    blit_surface(o, ls->surface, ls->x, ls->y, ls->w, ls->h);
}

static void render_snap_preview(struct xw_output *o, struct xw_wm *wm,
                                struct xw_window *w) {
    if (w->inter.mode != 1 || !w->inter.snap)
        return;
    uint32_t color = 0x602a82da; /* translucent blue */
    int x = o->usable.x, y = o->usable.y, wdt = o->usable.w, hgt = o->usable.h;
    int snap = w->inter.snap;

    if (snap == XW_EDGE_T) {
        /* dragging to the top edge previews maximize (xfwm4 behavior) */
    } else {
        if (snap & XW_EDGE_L) {
            wdt /= 2;
        }
        if (snap & XW_EDGE_R) {
            x += o->usable.w / 2;
            wdt = o->usable.w - o->usable.w / 2;
        }
        if (snap & XW_EDGE_T) {
            hgt /= 2;
        }
        if (snap & XW_EDGE_B) {
            y += o->usable.h / 2;
            hgt = o->usable.h - o->usable.h / 2;
        }
    }
    (void)wm;
    xw_render_fill_rect(o->logical, PIXMAN_OP_OVER, color, x - o->x, y - o->y,
                        wdt, hgt);
}

void xw_render_output(struct xw_output *o) {
    struct xw_compositor *c = o->comp;
    struct xw_wm *wm = c->wm;

    /* 1. layer-shell background + bottom (rendered list tail → head) */
    for (int layer = 0; layer <= 1; layer++) {
        struct xw_layer_surface *ls;
        wl_list_for_each_reverse(ls, &wm->layers[layer], link)
            render_layer(o, ls);
    }

    /* 2. toplevels, bottom of stack first */
    struct xw_window *w;
    wl_list_for_each_reverse(w, &wm->stack, stack_link) {
        if (!xw_wm_window_visible(wm, w))
            continue;
        render_window(o, w);
    }

    /* 3. layer-shell top + overlay */
    for (int layer = 2; layer <= 3; layer++) {
        struct xw_layer_surface *ls;
        wl_list_for_each_reverse(ls, &wm->layers[layer], link)
            render_layer(o, ls);
    }

    /* 4. popups (rendered last among surfaces) */
    struct xw_popup *p;
    wl_list_for_each_reverse(p, &c->popups, link) {
        if (!p->mapped || !p->surface)
            continue;
        blit_surface(o, p->surface, p->anchor_x, p->anchor_y, p->w, p->h);
    }

    /* 5. snap preview of the window being moved */
    wl_list_for_each(w, &wm->stack, stack_link) {
        render_snap_preview(o, wm, w);
        if (w->inter.mode == 1)
            break; /* only the grabbed window can have a preview */
    }

    /* 6. software cursor */
    struct xw_seat *seat;
    wl_list_for_each(seat, &c->seats, link) {
        draw_cursor(o, seat);
        break; /* render cursor of the first seat only (v0) */
    }
}
