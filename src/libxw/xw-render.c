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

    /* client-provided cursor image: composited at (cursor - hotspot),
     * honoring the surface's buffer scale. Falls through to the default
     * arrow when the client set no cursor or its surface has no buffer
     * yet (the usual first-frame window before the cursor commit). */
    if (seat->cursor_surface && seat->cursor_surface->buf_w > 0) {
        struct xw_surface *cs = seat->cursor_surface;
        pixman_image_t *img = xw_surface_get_image(cs);
        if (img) {
            int sc = cs->scale > 0 ? cs->scale : 1;
            int px = cx - seat->cursor_hot_x;
            int py = cy - seat->cursor_hot_y;
            int w = cs->buf_w / sc, h = cs->buf_h / sc;
            if (sc != 1) {
                pixman_transform_t t;
                pixman_transform_init_scale(
                    &t, pixman_double_to_fixed(1.0 / sc),
                    pixman_double_to_fixed(1.0 / sc));
                pixman_image_set_transform(img, &t);
                pixman_image_composite(PIXMAN_OP_OVER, img, NULL, o->logical,
                                       0, 0, 0, 0, px, py, w, h);
                pixman_image_set_transform(img, NULL);
            } else {
                pixman_image_composite(PIXMAN_OP_OVER, img, NULL, o->logical,
                                       0, 0, 0, 0, px, py, w, h);
            }
            pixman_image_unref(img);
            return;
        }
    }

    /* default arrow (software cursor) */
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

/* gx/gy/gw/gh: destination rect (layout coords). src_x/src_y: origin
 * of the source rect in SURFACE-LOCAL (logical, pre-scale) coordinates
 * -- the xdg set_window_geometry offset for CSD toplevels: the content
 * rect starts there inside the buffer, and only the content is
 * composited (the spec makes rendering outside the geometry optional;
 * we drop it, which keeps damage and input rectangles exact). */
static void blit_surface(struct xw_output *o, struct xw_surface *s, int gx,
                         int gy, int gw, int gh, int src_x, int src_y) {
    pixman_image_t *src = xw_surface_get_image(s);
    if (!src)
        return;
    int dx = gx - o->x, dy = gy - o->y;
    int bw = s->buf_w, bh = s->buf_h;
    int sc = s->scale > 0 ? s->scale : 1;
    if (bw >= (src_x + gw) * sc && bh >= (src_y + gh) * sc) {
        /* the buffer contains the declared source rect: 1:1 sub-rect
         * (exact match, CSD shadows, oversized buffers) */
        pixman_image_composite(PIXMAN_OP_OVER, src, NULL, o->logical,
                               src_x * sc, src_y * sc, 0, 0, dx, dy, gw, gh);
    } else {
        /* scale the buffer to the target geometry (buffer scale /
         * mismatched-size tolerance; geometry offsets do not apply
         * here by definition -- the source is the whole buffer) */
        pixman_transform_t t;
        pixman_fixed_t fx = pixman_int_to_fixed(bw) / gw;
        pixman_fixed_t fy = pixman_int_to_fixed(bh) / gh;
        pixman_transform_init_scale(&t, fx, fy);
        pixman_image_set_transform(src, &t);
        pixman_image_composite(PIXMAN_OP_OVER, src, NULL, o->logical, 0, 0, 0, 0,
                               dx, dy, gw, gh);
        pixman_image_set_transform(src, NULL);
    }
    pixman_image_unref(src);
}

/* render a surface tree: children stacked below the parent's own
 * buffer first, then the parent blit, then children above it. Within
 * each group the list order is bottom → top (tail = topmost). Each
 * child is itself rendered as a full tree (nesting). */
static void render_subsurface_tree(struct xw_output *o,
                                    struct xw_subsurface *sub, int pgx,
                                    int pgy);

static void render_surface_tree(struct xw_output *o, struct xw_surface *s,
                                 int gx, int gy, int gw, int gh, int src_x,
                                 int src_y) {
    /* children below the parent's own buffer */
    struct xw_subsurface *sub;
    wl_list_for_each(sub, &s->subsurfaces, parent_link) {
        if (sub->below_parent)
            render_subsurface_tree(o, sub, gx, gy);
    }
    /* the parent's own image */
    blit_surface(o, s, gx, gy, gw, gh, src_x, src_y);
    /* children above the parent's buffer, bottom → top */
    wl_list_for_each(sub, &s->subsurfaces, parent_link) {
        if (!sub->below_parent)
            render_subsurface_tree(o, sub, gx, gy);
    }
}

static void render_subsurface_tree(struct xw_output *o,
                                    struct xw_subsurface *sub, int pgx,
                                    int pgy) {
    struct xw_surface *s = sub->surface;
    if (!s || !s->mapped || (!s->shm && !s->has_single_pixel))
        return;
    int sc = s->scale > 0 ? s->scale : 1;
    int w = s->buf_w / sc, h = s->buf_h / sc;
    render_surface_tree(o, s, pgx + sub->x, pgy + sub->y, w, h, 0, 0);
}

static void render_window(struct xw_output *o, struct xw_window *w) {
    if (!w->surface || (!w->surface->shm && !w->surface->has_single_pixel))
        return;
    int ox = 0, oy = 0;
    if (w->geometry_set) {
        ox = w->geo_x;
        oy = w->geo_y;
    }
    render_surface_tree(o, w->surface, w->x, w->y, w->w, w->h, ox, oy);
}


static void render_layer(struct xw_output *o, struct xw_layer_surface *ls) {
    if (!ls->mapped || !ls->surface)
        return;
    if (!ls->surface->shm && !ls->surface->has_single_pixel)
        return;
    render_surface_tree(o, ls->surface, ls->x, ls->y, ls->w, ls->h, 0, 0);
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

    /* 0. session lock: the ONLY thing rendered is the lock layer
     * (opaque blank + lock surfaces + cursor). No window, layer,
     * popup or snap-preview pixel may leak while locked — this is a
     * security invariant of ext-session-lock, enforced here. */
    if (xw_session_lock_active(c)) {
        xw_session_lock_render(o);

        /* software cursor still renders (lock surfaces take input and
         * need to see the pointer) */
        struct xw_seat *seat;
        wl_list_for_each(seat, &c->seats, link) {
            draw_cursor(o, seat);
            break; /* render cursor of the first seat only (v0) */
        }
        return;
    }

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

    /* 2b. override-redirect X11 windows (menus, tooltips): above all
     * managed toplevels, below layer-shell top/overlay — same order in
     * surface_at (hit-testing) so input lands where pixels are */
    wl_list_for_each_reverse(w, &wm->or_windows, link) {
        if (!w->mapped)
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
        render_surface_tree(o, p->surface, p->anchor_x, p->anchor_y, p->w,
                            p->h, 0, 0);
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
