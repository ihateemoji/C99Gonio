/*
 * C99Gonio — X11 GUI (scope + meters)
 *
 * Drawing model
 * -------------
 * Everything is rendered into an offscreen Pixmap (`plug->back`) and
 * copied to the window with a single XCopyArea.  That eliminates the
 * classic full-window-clear flicker at 60 fps.
 *
 * Redraw cadence
 * --------------
 * A timerfd fires every ~16.7 ms and is registered with the host via
 * CLAP_EXT_POSIX_FD_SUPPORT.  When the host calls on_fd for that fd we
 * paint.  X events (Expose, ConfigureNotify, ButtonPress) also trigger
 * a paint when needed.
 *
 * Auto-scale
 * ----------
 * process() maintains plug->auto_peak = smoothed max |Mid/Side|.
 * paint() divides by (auto_peak * 1.15) so the loudest recent sample
 * sits just inside the unit circle.  No user scale control.
 *
 * Layout
 * ------
 *   [ title bar ]
 *   [ square Mid/Side scope — fills remaining width/height ]
 *   [ fixed bottom band: CORR bar, BAL bar, version hint ]
 */

#define _GNU_SOURCE
#include "goniometer.h"

/* ---- colour helpers (24-bit RGB packed for X11 TrueColor) -------------- */

static unsigned long go_col(int r, int g, int b)
{
    return ((unsigned long)r << 16) | ((unsigned long)g << 8) | (unsigned long)b;
}

/* Prefer the offscreen pixmap; fall back to the window if it is missing. */
static Drawable go_dst(go_plug_t *p)
{
    return (p->back != None) ? (Drawable)p->back : (Drawable)p->win;
}

/*
 * Create or recreate the back-buffer when size changes.
 * Called from paint, set_size, ConfigureNotify, and create.
 */
static void go_ensure_back(go_plug_t *p)
{
    if (!p->dpy || !p->win) return;
    if (p->back != None && p->back_w == p->gui_w && p->back_h == p->gui_h)
        return;
    if (p->back != None) {
        XFreePixmap(p->dpy, p->back);
        p->back = None;
    }
    if (p->gui_w < 1 || p->gui_h < 1) return;
    int depth = DefaultDepth(p->dpy, DefaultScreen(p->dpy));
    p->back = XCreatePixmap(p->dpy, p->win, (unsigned)p->gui_w,
                            (unsigned)p->gui_h, (unsigned)depth);
    p->back_w = p->gui_w;
    p->back_h = p->gui_h;
}

static void go_fill(go_plug_t *p, int x, int y, int w, int h, unsigned long c)
{
    XSetForeground(p->dpy, p->gc, c);
    XFillRectangle(p->dpy, go_dst(p), p->gc, x, y, (unsigned)w, (unsigned)h);
}

static void go_rect(go_plug_t *p, int x, int y, int w, int h, unsigned long c)
{
    XSetForeground(p->dpy, p->gc, c);
    XDrawRectangle(p->dpy, go_dst(p), p->gc, x, y, (unsigned)w, (unsigned)h);
}

static void go_line(go_plug_t *p, int x0, int y0, int x1, int y1, unsigned long c)
{
    XSetForeground(p->dpy, p->gc, c);
    XDrawLine(p->dpy, go_dst(p), p->gc, x0, y0, x1, y1);
}

static void go_text(go_plug_t *p, int x, int y, const char *s, unsigned long c)
{
    XSetForeground(p->dpy, p->gc, c);
    XDrawString(p->dpy, go_dst(p), p->gc, x, y, s, (int)strlen(s));
}

/* ========================================================================
 * Full-frame paint
 * ======================================================================== */

static void go_gui_paint(go_plug_t *plug)
{
    if (!plug->dpy || !plug->win) return;
    go_ensure_back(plug);

    int W = plug->gui_w;
    int H = plug->gui_h;

    unsigned long bg     = go_col(18, 20, 24);
    unsigned long grid   = go_col(40, 44, 52);
    unsigned long axis   = go_col(70, 78, 92);
    unsigned long fg     = go_col(230, 234, 240);
    unsigned long mut    = go_col(100, 108, 120);
    unsigned long cyan   = go_col(70, 200, 220);
    unsigned long green  = go_col(90, 210, 140);
    unsigned long yellow = go_col(220, 190, 70);
    unsigned long red    = go_col(220, 90, 90);

    go_fill(plug, 0, 0, W, H, bg);

    /* Title */
    go_text(plug, 12, 18, "C99Gonio", fg);
    go_text(plug, W - 42, 18, "0.0.1", mut);

    /*
     * Fixed bottom band for meters — always visible regardless of how
     * the host resizes the window.  Scope is a square that fits in the
     * remaining area above that band.
     */
    int top = 26;
    int meter_h = GO_METER_H;
    if (meter_h > H / 3) meter_h = H / 3;
    int avail_h = H - top - meter_h;
    if (avail_h < 80) avail_h = 80;

    int margin = 16;
    int scope_size = W - 2 * margin;
    if (scope_size > avail_h - 8) scope_size = avail_h - 8;
    if (scope_size < 64) scope_size = 64;

    int sx = (W - scope_size) / 2;
    int sy = top + (avail_h - scope_size) / 2;

    go_fill(plug, sx, sy, scope_size, scope_size, go_col(12, 14, 18));
    go_rect(plug, sx, sy, scope_size, scope_size, axis);

    int cx = sx + scope_size / 2;
    int cy = sy + scope_size / 2;
    int rad = scope_size / 2 - 4;

    /* Cross-hairs: vertical = Mid, horizontal = Side */
    go_line(plug, cx, sy + 2, cx, sy + scope_size - 2, grid);
    go_line(plug, sx + 2, cy, sx + scope_size - 2, cy, grid);

    /* Diagonals mark pure-Left and pure-Right */
    go_line(plug, sx + 4, sy + 4, sx + scope_size - 4, sy + scope_size - 4, go_col(32, 36, 44));
    go_line(plug, sx + 4, sy + scope_size - 4, sx + scope_size - 4, sy + 4, go_col(32, 36, 44));

    go_text(plug, sx + 4, cy - 4, "S", mut);
    go_text(plug, sx + scope_size - 14, cy - 4, "S", mut);
    go_text(plug, cx + 4, sy + 14, "M+", mut);
    go_text(plug, cx + 4, sy + scope_size - 6, "M-", mut);

    /* ---- smoothed trail + auto-scale to fill the box -------------------- */

    /*
     * Strategy:
     *  1. Lerp unscaled Mid/Side into disp_* (calm motion).
     *  2. Measure peak magnitude of that trail this frame.
     *  3. Smooth auto_peak (fast attack, moderate release).
     *  4. Scale at draw time so peak ≈ 0.92 of the radius.
     *
     * Scaling at draw time (not into disp_*) means a change in zoom does
     * not fight the position smoother — the line always fills the box.
     */
    const float inv_sqrt2 = 0.70710678f;
    const float smooth = 0.22f;

    uint32_t nsrc = plug->scope_count;
    if (nsrc > GO_SCOPE_LEN) nsrc = GO_SCOPE_LEN;

    if (nsrc > 1) {
        uint32_t start = (plug->scope_write + GO_SCOPE_LEN - nsrc) % GO_SCOPE_LEN;
        uint32_t nd = GO_DISP_LEN;
        if (nd > nsrc) nd = nsrc;

        float frame_peak = 0.f;

        for (uint32_t i = 0; i < nd; i++) {
            uint32_t src_i = nsrc - nd + i;
            go_point_t *pt = &plug->scope[(start + src_i) % GO_SCOPE_LEN];

            /* Unscaled Mid/Side */
            float tgt_s = (pt->L - pt->R) * inv_sqrt2;
            float tgt_m = (pt->L + pt->R) * inv_sqrt2;

            if (plug->disp_count <= i) {
                plug->disp_s[i] = tgt_s;
                plug->disp_m[i] = tgt_m;
            } else {
                plug->disp_s[i] += (tgt_s - plug->disp_s[i]) * smooth;
                plug->disp_m[i] += (tgt_m - plug->disp_m[i]) * smooth;
            }

            float mag = sqrtf(plug->disp_s[i] * plug->disp_s[i] +
                              plug->disp_m[i] * plug->disp_m[i]);
            if (mag > frame_peak) frame_peak = mag;
        }
        plug->disp_count = nd;

        /* Fit the box: peak maps to ~92% of radius */
        if (frame_peak > plug->auto_peak)
            plug->auto_peak = plug->auto_peak * 0.5f + frame_peak * 0.5f;  /* fast attack */
        else
            plug->auto_peak = plug->auto_peak * 0.97f + frame_peak * 0.03f; /* moderate release */
        if (plug->auto_peak < 1e-4f)
            plug->auto_peak = 1e-4f;

        float inv_scale = 0.92f / plug->auto_peak;

        int prev_x = -1, prev_y = -1;
        for (uint32_t i = 0; i < nd; i++) {
            float sxv = plug->disp_s[i] * inv_scale;
            float syv = plug->disp_m[i] * inv_scale;
            if (sxv > 1.05f) sxv = 1.05f; else if (sxv < -1.05f) sxv = -1.05f;
            if (syv > 1.05f) syv = 1.05f; else if (syv < -1.05f) syv = -1.05f;

            int px = cx + (int)(sxv * rad);
            int py = cy - (int)(syv * rad);

            if (prev_x >= 0)
                go_line(plug, prev_x, prev_y, px, py, cyan);
            prev_x = px;
            prev_y = py;
        }
    }


    int band_y = H - meter_h;
    int bx = margin;
    int bar_w = W - 2 * margin;
    if (bar_w < 40) bar_w = 40;
    int bar_h = 12;
    int mid = bar_w / 2;
    char buf[64];

    /* Correlation (−1 anti-phase … +1 mono-compatible) */
    float c = plug->corr;
    if (c > 1.f) c = 1.f; else if (c < -1.f) c = -1.f;
    int my = band_y + 8;
    go_fill(plug, bx, my, bar_w, bar_h, go_col(28, 30, 36));
    int cw = (int)(c * (float)mid);
    unsigned long cc = (c >= 0.f) ? green : red;
    if (cw > 0)
        go_fill(plug, bx + mid, my, cw, bar_h, cc);
    else if (cw < 0)
        go_fill(plug, bx + mid + cw, my, -cw, bar_h, cc);
    go_rect(plug, bx, my, bar_w, bar_h, axis);
    go_line(plug, bx + mid, my, bx + mid, my + bar_h, axis);
    snprintf(buf, sizeof(buf), "CORR  %+.2f", (double)c);
    go_text(plug, bx, my + bar_h + 14, buf, fg);

    /* Balance (−1 left-heavy … +1 right-heavy) */
    float b = plug->bal;
    if (b > 1.f) b = 1.f; else if (b < -1.f) b = -1.f;
    int by = my + 32;
    go_fill(plug, bx, by, bar_w, bar_h, go_col(28, 30, 36));
    int bw = (int)(b * (float)mid);
    if (bw > 0)
        go_fill(plug, bx + mid, by, bw, bar_h, yellow);
    else if (bw < 0)
        go_fill(plug, bx + mid + bw, by, -bw, bar_h, yellow);
    go_rect(plug, bx, by, bar_w, bar_h, axis);
    go_line(plug, bx + mid, by, bx + mid, by + bar_h, axis);
    snprintf(buf, sizeof(buf), "BAL   %+.2f", (double)b);
    go_text(plug, bx, by + bar_h + 14, buf, fg);

    go_text(plug, bx, H - 8, "ihateemoji  ·  auto-scale", mut);

    /* Single blit of the finished frame → no flicker */
    if (plug->back != None) {
        XCopyArea(plug->dpy, plug->back, plug->win, plug->gc,
                  0, 0, (unsigned)plug->gui_w, (unsigned)plug->gui_h, 0, 0);
    }
    XFlush(plug->dpy);
}

void go_gui_redraw(go_plug_t *plug)
{
    if (plug && plug->gui_visible) go_gui_paint(plug);
}

/* ========================================================================
 * CLAP GUI + POSIX FD extension implementation
 * ======================================================================== */

static bool go_gui_is_api_supported(const clap_plugin_t *p, const char *api, bool f)
{
    (void)p;
    (void)f;
    return api && strcmp(api, CLAP_WINDOW_API_X11) == 0;
}

static bool go_gui_get_preferred_api(const clap_plugin_t *p, const char **api, bool *f)
{
    (void)p;
    *api = CLAP_WINDOW_API_X11;
    *f = false;   /* embedded, not floating */
    return true;
}

static bool go_gui_create(const clap_plugin_t *plugin, const char *api, bool f)
{
    go_plug_t *plug = (go_plug_t *)plugin->plugin_data;
    (void)f;
    if (api && strcmp(api, CLAP_WINDOW_API_X11) != 0) return false;

    plug->dpy = XOpenDisplay(NULL);
    if (!plug->dpy) return false;

    int screen = DefaultScreen(plug->dpy);
    Window root = RootWindow(plug->dpy, screen);
    plug->gui_w = GO_GUI_W;
    plug->gui_h = GO_GUI_H;
    plug->back = None;
    plug->back_w = plug->back_h = 0;

    plug->win = XCreateSimpleWindow(plug->dpy, root, 0, 0,
                                    (unsigned)plug->gui_w, (unsigned)plug->gui_h,
                                    0, go_col(18, 20, 24), go_col(18, 20, 24));

    /* Ask the X server to keep a backing store when the window is mapped. */
    {
        XSetWindowAttributes wa;
        wa.backing_store = WhenMapped;
        XChangeWindowAttributes(plug->dpy, plug->win, CWBackingStore, &wa);
    }

    plug->gc = XCreateGC(plug->dpy, plug->win, 0, NULL);
    XSelectInput(plug->dpy, plug->win,
                 ExposureMask | ButtonPressMask | StructureNotifyMask);
    plug->xfd = ConnectionNumber(plug->dpy);
    if (plug->host_fd && plug->host_fd->register_fd)
        plug->host_fd->register_fd(plug->host, plug->xfd, CLAP_POSIX_FD_READ);

    /*
     * ~60 Hz timerfd.  The host polls this fd and calls our on_fd
     * callback, which is the only place we continuous-redraw from.
     */
    plug->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (plug->timer_fd >= 0) {
        struct itimerspec ts;
        ts.it_interval.tv_sec = 0;
        ts.it_interval.tv_nsec = 16666667; /* ≈ 60 Hz */
        ts.it_value = ts.it_interval;
        timerfd_settime(plug->timer_fd, 0, &ts, NULL);
        if (plug->host_fd && plug->host_fd->register_fd)
            plug->host_fd->register_fd(plug->host, plug->timer_fd, CLAP_POSIX_FD_READ);
    }

    go_ensure_back(plug);
    plug->gui_created = 1;
    plug->gui_visible = 0;
    return true;
}

static void go_gui_destroy(const clap_plugin_t *plugin)
{
    go_plug_t *plug = (go_plug_t *)plugin->plugin_data;

    if (plug->host_fd && plug->host_fd->unregister_fd) {
        if (plug->xfd >= 0) plug->host_fd->unregister_fd(plug->host, plug->xfd);
        if (plug->timer_fd >= 0) plug->host_fd->unregister_fd(plug->host, plug->timer_fd);
    }
    if (plug->timer_fd >= 0) { close(plug->timer_fd); plug->timer_fd = -1; }
    if (plug->back != None && plug->dpy) {
        XFreePixmap(plug->dpy, plug->back);
        plug->back = None;
    }
    if (plug->gc && plug->dpy) { XFreeGC(plug->dpy, plug->gc); plug->gc = NULL; }
    if (plug->win && plug->dpy) { XDestroyWindow(plug->dpy, plug->win); plug->win = 0; }
    if (plug->dpy) { XCloseDisplay(plug->dpy); plug->dpy = NULL; }

    plug->gui_created = 0;
    plug->gui_visible = 0;
    plug->xfd = -1;
}

static bool go_gui_set_scale(const clap_plugin_t *p, double s)
{
    (void)p;
    (void)s;
    return true;
}

static bool go_gui_get_size(const clap_plugin_t *plugin, uint32_t *w, uint32_t *h)
{
    go_plug_t *plug = (go_plug_t *)plugin->plugin_data;
    *w = (uint32_t)plug->gui_w;
    *h = (uint32_t)plug->gui_h;
    return true;
}

static bool go_gui_can_resize(const clap_plugin_t *p)
{
    (void)p;
    return true;
}

static bool go_gui_get_resize_hints(const clap_plugin_t *p, clap_gui_resize_hints_t *h)
{
    (void)p;
    h->can_resize_horizontally = true;
    h->can_resize_vertically = true;
    /* No aspect lock — hosts that force square were clipping the meter band. */
    h->preserve_aspect_ratio = false;
    h->aspect_ratio_width = 0;
    h->aspect_ratio_height = 0;
    return true;
}

static bool go_gui_adjust_size(const clap_plugin_t *p, uint32_t *w, uint32_t *h)
{
    (void)p;
    if (*w < 320) *w = 320;
    if (*h < 420) *h = 420;   /* room for scope + fixed meter band */
    return true;
}

static bool go_gui_set_size(const clap_plugin_t *plugin, uint32_t w, uint32_t h)
{
    go_plug_t *plug = (go_plug_t *)plugin->plugin_data;
    if (w < 320) w = 320;
    if (h < 420) h = 420;
    plug->gui_w = (int)w;
    plug->gui_h = (int)h;
    if (plug->dpy && plug->win) {
        XResizeWindow(plug->dpy, plug->win, w, h);
        go_ensure_back(plug);
        go_gui_paint(plug);
    }
    return true;
}

static bool go_gui_set_parent(const clap_plugin_t *plugin, const clap_window_t *window)
{
    go_plug_t *plug = (go_plug_t *)plugin->plugin_data;
    if (!window || !plug->dpy || strcmp(window->api, CLAP_WINDOW_API_X11) != 0)
        return false;
    XReparentWindow(plug->dpy, plug->win, (Window)window->x11, 0, 0);
    XMapWindow(plug->dpy, plug->win);
    XFlush(plug->dpy);
    return true;
}

static bool go_gui_set_transient(const clap_plugin_t *p, const clap_window_t *w)
{
    (void)p;
    (void)w;
    return true;
}

static void go_gui_suggest_title(const clap_plugin_t *plugin, const char *title)
{
    go_plug_t *plug = (go_plug_t *)plugin->plugin_data;
    if (plug->dpy && plug->win && title)
        XStoreName(plug->dpy, plug->win, title);
}

static bool go_gui_show(const clap_plugin_t *plugin)
{
    go_plug_t *plug = (go_plug_t *)plugin->plugin_data;
    if (!plug->dpy || !plug->win) return false;
    XMapWindow(plug->dpy, plug->win);
    plug->gui_visible = 1;
    go_gui_paint(plug);
    return true;
}

static bool go_gui_hide(const clap_plugin_t *plugin)
{
    go_plug_t *plug = (go_plug_t *)plugin->plugin_data;
    if (plug->dpy && plug->win) XUnmapWindow(plug->dpy, plug->win);
    plug->gui_visible = 0;
    return true;
}

/*
 * Host calls this when one of our registered fds is readable.
 *   - timer_fd → continuous 60 Hz paint
 *   - xfd      → drain X events (Expose / Configure / click)
 */
static void go_gui_on_fd(const clap_plugin_t *plugin, int fd, clap_posix_fd_flags_t flags)
{
    (void)flags;
    go_plug_t *plug = (go_plug_t *)plugin->plugin_data;
    if (!plug->dpy) return;

    if (plug->timer_fd >= 0 && fd == plug->timer_fd) {
        uint64_t expirations;
        while (read(plug->timer_fd, &expirations, sizeof(expirations)) > 0) {}
        if (plug->gui_visible) go_gui_paint(plug);
        return;
    }

    XEvent ev;
    bool need_redraw = false;
    while (XPending(plug->dpy)) {
        XNextEvent(plug->dpy, &ev);
        if (ev.type == Expose) {
            need_redraw = true;
        } else if (ev.type == ConfigureNotify) {
            int nw = ev.xconfigure.width;
            int nh = ev.xconfigure.height;
            if (nw < 320) nw = 320;
            if (nh < 420) nh = 420;
            if (nw != plug->gui_w || nh != plug->gui_h) {
                plug->gui_w = nw;
                plug->gui_h = nh;
                go_ensure_back(plug);
            }
            need_redraw = true;
        }
        /* ButtonPress intentionally ignored — no interactive controls. */
    }
    if (need_redraw) go_gui_paint(plug);
}

const clap_plugin_gui_t go_gui_ext = {
    .is_api_supported  = go_gui_is_api_supported,
    .get_preferred_api = go_gui_get_preferred_api,
    .create            = go_gui_create,
    .destroy           = go_gui_destroy,
    .set_scale         = go_gui_set_scale,
    .get_size          = go_gui_get_size,
    .can_resize        = go_gui_can_resize,
    .get_resize_hints  = go_gui_get_resize_hints,
    .adjust_size       = go_gui_adjust_size,
    .set_size          = go_gui_set_size,
    .set_parent        = go_gui_set_parent,
    .set_transient     = go_gui_set_transient,
    .suggest_title     = go_gui_suggest_title,
    .show              = go_gui_show,
    .hide              = go_gui_hide
};

const clap_plugin_posix_fd_support_t go_posix_fd_ext = {
    .on_fd = go_gui_on_fd
};
