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
 * A CLAP timer (CLAP_EXT_TIMER_SUPPORT) fires every ~16–17 ms (~60 Hz).
 * When the host calls on_timer we paint.  X events (Expose,
 * ConfigureNotify) are delivered via the X connection fd registered
 * with CLAP_EXT_POSIX_FD_SUPPORT and also trigger a paint when needed.
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

#include "goniometer.h"

static unsigned long go_col(int r, int g, int b) {
    /* Convert 8-bit RGB components into a 24-bit X11 pixel value.
    Inputs:
      <r,g,b> - red, green and blue components in the range 0..255
    Returns:
      <unsigned long> - packed colour suitable for
                                 XSetForeground / XFillRectangle */
    return ((unsigned long)r << 16) |
            ((unsigned long)g << 8) | (unsigned long)b;
}

static Drawable go_dst(go_plug_t *p) {
    /* Simple fallback to the window if the offscreen pixmap is missing.
        Inputs:
            <*go_plug_t> - pointer to the instance of the plug in 
        Outputs:
            <Drawable> - X11 Drawable object */
    /* if no offscreen pixmap we return the window itself */
    return (p->back != None) ? (Drawable)p->back : (Drawable)p->win;
}

static void go_ensure_back(go_plug_t *p) {
    /* Routine that ensures that the offscreen pixmap is created.
        Drawing directly on a window at 60 fps creates nauseating flicker
        Inputs:
            <*go_plug_t> - pointer to the instance of the plug-in */
    /* if we have no X display or no window for some reason then move on */
    if (!p->dpy || !p->win) 
        return;
    /* if we already have the pixmap and it matches the dimensions of the 
                                               gui, there is nothing to do */
    if (p->back != None && p->back_w == p->gui_w && p->back_h == p->gui_h)
        return;
    /* catch the size change */
    if (p->back != None) {
        /* free old pixmap */
        XFreePixmap(p->dpy, p->back);
        /* mark that we have no pixmap */
        p->back = None;
    }
    /* catch possible invalid size error */
    if (p->gui_w < 1 || p->gui_h < 1)
        return;
    /* get the screen's bit depth */
    int depth = DefaultDepth(p->dpy, DefaultScreen(p->dpy));
    /* allocate a new offscreen pixmap */
    p->back = XCreatePixmap(p->dpy, p->win, (unsigned)p->gui_w,
                            (unsigned)p->gui_h, (unsigned)depth);
    /* save the dimensionality */
    p->back_w = p->gui_w;
    p->back_h = p->gui_h;
}

static void go_fill(go_plug_t *p, int x, int y, int w, int h,
                                                unsigned long c) {
    /* Draw a solid rectangle with specified position, dimensions, and colour.
        Inputs:
            <*go_plug_t> - pointer to the instance of the plug in
            <int>        - x coordinate of the top-left corner 
            <int>        - y coordinate of the top-left corner
            <int>        - width
            <int>        - height
            <long>       - rgb colour packed into unsigned long by go_col */
    /* set the colour for the next draw call */
    XSetForeground(p->dpy, p->gc, c);
    /* draw the rectangle */
    XFillRectangle(p->dpy, go_dst(p), p->gc, x, y, (unsigned)w, (unsigned)h);
}

static void go_rect(go_plug_t *p, int x, int y, int w, int h,
                                                    unsigned long c) {
    /* Draw a one-pixel rectangle outline with specified position,
                                                dimensions, and colour.
        Inputs:
            <*go_plug_t> - pointer to the instance of the plug in
            <int>        - x coordinate of the top-left corner 
            <int>        - y coordinate of the top-left corner
            <int>        - width
            <int>        - height
            <long>       - rgb colour packed into unsigned long by go_col */
    /* set the colour for the next draw call */
    XSetForeground(p->dpy, p->gc, c);
    /* draw the rectangle */
    XDrawRectangle(p->dpy, go_dst(p), p->gc, x, y, (unsigned)w, (unsigned)h);
}

static void go_line(go_plug_t *p, int x0, int y0, int x1, int y1,
                                                        unsigned long c) {
    /* Draw a line with specified starting and ending point.
        Inputs:
            <*go_plug_t> - pointer to the instance of the plug in
            <int>        - x coordinate of the starting point
            <int>        - y coordinate of the starting point
            <int>        - x coordinate of the end point
            <int>        - y coordinate of the end point 
            <int>        - height
            <long>       - rgb colour packed into unsigned long by go_col */
    /* set the colour for the next draw call */
    XSetForeground(p->dpy, p->gc, c);
    /* draw the line */
    XDrawLine(p->dpy, go_dst(p), p->gc, x0, y0, x1, y1);
}

static void go_text(go_plug_t *p, int x, int y, const char *s,
                                                unsigned long c) {
    /* Draw a null-terminated string at the given window coordinates.
       Inputs:
         <*go_plug_t> - pointer to the instance of the plug in
         <int>        - x coordinate of the start of the string
         <int>        - y coordinate of the start of the string
         <*char>      - null-terminated string to draw
         <long>       - rgb colour packed into unsigned long by go_col */
    /* set the colour for the next draw call */
    XSetForeground(p->dpy, p->gc, c);
    /* print out the string */
    XDrawString(p->dpy, go_dst(p), p->gc, x, y, s, (int)strlen(s));
}

static void go_gui_paint(go_plug_t *plug) {
    /* Procedure that draws one full frame of the UI.
        Inputs:
          <*go_plug_t> - pointer to the instance of the plug in */
    /* if no X or window, there is nothing to do */
    if (!plug->dpy || !plug->win) return;
    /* ensure we got an off-screen pixmap to alleviate the flickering */
    go_ensure_back(plug);
    /* grab the dimensions of the window */
    int W = plug->gui_w;
    int H = plug->gui_h;
    /* define the list of colours */
    unsigned long bg     = go_col(GO_BG_R, GO_BG_G, GO_BG_B);
    unsigned long fg     = go_col(GO_FG_R, GO_FG_G, GO_FG_B);
    unsigned long cyan   = go_col(GO_CYA_R, GO_CYA_G, GO_CYA_B);
    unsigned long green  = go_col(GO_GRN_R, GO_GRN_G, GO_GRN_B);
    unsigned long yellow = go_col(GO_YEL_R, GO_YEL_G, GO_YEL_B);
    unsigned long red    = go_col(GO_RED_R, GO_RED_G, GO_RED_B);
    unsigned long grid   = go_col(GO_GRID_R, GO_GRID_G, GO_GRID_B);
    unsigned long axis   = go_col(GO_AXIS_R, GO_AXIS_G, GO_AXIS_B);
    unsigned long diag = go_col(GO_DIAG_R, GO_DIAG_G, GO_DIAG_B);
    unsigned long scope_bg = go_col(GO_SCOPE_BG_R,
                                GO_SCOPE_BG_G, GO_SCOPE_BG_B);
    unsigned long bar_bg = go_col(GO_BAR_BG_R, GO_BAR_BG_G, GO_BAR_BG_B);
    /* fill the background colour */
    go_fill(plug, 0, 0, W, H, bg);
    /* drop a little title (if anyone cares) */
    go_text(plug, 12, 18, "C99Gonio", fg);
    /* add a top offset for the title */
    int top = GO_TOP;
    /* add a bottom offset for the meters */
    int meter_h = GO_METER_H;
    /* in case bottom offset is above a third, we shrink it */
    if (meter_h > H / 3) meter_h = H / 3;
    /* work out the space left for the scope */
    int avail_h = H - top - meter_h;
    /* we can only make the scope so small, so take care of that */
    if (avail_h < 80) avail_h = 80;
    /* add a small margin */
    int margin = GO_MARGIN;
    /* we can now work out the size of the scope */
    int scope_size = W - 2 * margin;
    /* again, the scope can only be so small/large so take care of that */
    if (scope_size > avail_h - 8) scope_size = avail_h - 8;
    if (scope_size < 64) scope_size = 64;
    /* find the coordinates of the top left corner of the scope */
    int sx = (W - scope_size) / 2;
    int sy = top + (avail_h - scope_size) / 2;
    /* draw a background for the scope */
    go_fill(plug, sx, sy, scope_size, scope_size, scope_bg);
    go_rect(plug, sx, sy, scope_size, scope_size, axis);
    /* work out the coordinates of the centre of the scope */ 
    int cx = sx + scope_size / 2;
    int cy = sy + scope_size / 2;
    /* get the radius of the scope (minus a few pixels for safety */
    int rad = scope_size / 2 - 4;
    /* Cross-hairs: vertical = Mid, horizontal = Side */
    go_line(plug, cx, sy + 2, cx, sy + scope_size - 2, grid);
    go_line(plug, sx + 2, cy, sx + scope_size - 2, cy, grid);
    /* Diagonals mark pure-Left and pure-Right */
    go_line(plug, sx + 4, sy + 4, sx + scope_size - 4,
                                           sy + scope_size - 4, diag);
    go_line(plug, sx + 4, sy + scope_size - 4, sx + scope_size - 4,
                                                        sy + 4, diag);
    /* constants for smoothing and Mid/Side projection */
    const float inv_sqrt2 = GO_INV_SQRT2;
    const float smooth = GO_SMOOTH;
    /* get the number of life samples loaded by the plug-in */
    uint32_t nsrc = plug->scope_count;
    /* ensure we do not load more samples than provided upper limit */
    if (nsrc > GO_SCOPE_LEN) nsrc = GO_SCOPE_LEN;
    /* if we have at least two samples, we can draw a line */
    if (nsrc > 1) {
        /* get the oldest sample in the system */
        uint32_t start =
                (plug->scope_write + GO_SCOPE_LEN - nsrc) % GO_SCOPE_LEN;
        /* set the number of display points to our predefined value */
        uint32_t nd = GO_DISP_LEN;
        /* account for the case if we have fewer samples available */
        if (nd > nsrc) nd = nsrc;
        /* loop over the samples in the system */
        float frame_peak = 0.f;
        for (uint32_t i = 0; i < nd; i++) {
            /* extract the point corresponding to the sample */
            uint32_t src_i = nsrc - nd + i;
            go_point_t *pt = &plug->scope[(start + src_i) % GO_SCOPE_LEN];
            /* project out mid/side parts from the sample pointer */
            float tgt_s = (pt->L - pt->R) * inv_sqrt2;
            float tgt_m = (pt->L + pt->R) * inv_sqrt2;
            /* if there is no previous value to blend we use the point as it */
            if (plug->disp_count <= i) {
                plug->disp_s[i] = tgt_s;
                plug->disp_m[i] = tgt_m;
            /* otherwise we take a step */
            } else {
                /* for smoothness, we are chasing the value here, so we 
                                    are not travelling the whole distance */
                plug->disp_s[i] += (tgt_s - plug->disp_s[i]) * smooth;
                plug->disp_m[i] += (tgt_m - plug->disp_m[i]) * smooth;
            }
            /* compute the largest magnitude in this frame's trail */
            float mag = sqrtf(plug->disp_s[i] * plug->disp_s[i] +
                              plug->disp_m[i] * plug->disp_m[i]);
            if (mag > frame_peak) frame_peak = mag;
        }
        /* store the number of valid points */
        plug->disp_count = nd;
        /* Fit the box: peak maps to ~92% of radius */
        if (frame_peak > plug->auto_peak)
            /* if signal got louder we blend towards new peak */
            plug->auto_peak = plug->auto_peak * GO_PEAK_UP_A +
                              frame_peak     * GO_PEAK_UP_B;
        else
            /* if a signal got quieter we move slowly towards new peak */
            plug->auto_peak = plug->auto_peak * GO_PEAK_DN_A +
                              frame_peak     * GO_PEAK_DN_B;
        /* floor to avoid division by zero */
        if (plug->auto_peak < 1e-4f)
            plug->auto_peak = 1e-4f;
        /* set the scale for auto-scaling */
        float inv_scale = GO_INV_SCALE / plug->auto_peak;
        /* registers for the previous pixel */
        int prev_x = -1, prev_y = -1;
        /* walk through every point and draw a line between */
        for (uint32_t i = 0; i < nd; i++) {
            /* apply auto scale by the peak */
            float sxv = plug->disp_s[i] * inv_scale;
            float syv = plug->disp_m[i] * inv_scale;
            /* sudden loud sample can overshoot, so we clamp */
            if (sxv > GO_CLAMP) {
                sxv = GO_CLAMP;
            } else if (sxv < -GO_CLAMP) {
                sxv = -GO_CLAMP;
            }
            if (syv > GO_CLAMP) {
                syv = GO_CLAMP; 
            } else if (syv < -GO_CLAMP) {
                syv = -GO_CLAMP;
            }
            /* get the coordinates of each pixel */
            int px = cx + (int)(sxv * rad);
            int py = cy - (int)(syv * rad);
            /* if the previous point exists, we draw a line */
            if (prev_x >= 0)
                go_line(plug, prev_x, prev_y, px, py, cyan);
            /* store the coordinates of current pixel for future use */
            prev_x = px;
            prev_y = py;
        }
    }
    /* work out the top edge of the metre area */
    int band_y = H - meter_h;
    /* offset by a margin */
    int bx = margin;
    /* compute bar width, note that it can only be so small */
    int bar_w = W - 2 * margin;
    if (bar_w < 40) {
        bar_w = 40;
    }
    /* set the thickness of the bar */
    int bar_h = GO_BAR_H;
    /* work out the centre of the bar */
    int mid = bar_w / 2;
    /* add buffer for the string labels */
    char buf[64];
    /* Correlation (−1 anti-phase … +1 mono-compatible) */
    float c = plug->corr;
    /* clamp if needed */
    if (c > 1.f) {
        c = 1.f; 
    } else if (c < -1.f) {
        c = -1.f;
    }
    /* draw the bar */
    int my = band_y + 8;
    go_fill(plug, bx, my, bar_w, bar_h, bar_bg);
    int cw = (int)(c * (float)mid);
    unsigned long cc = (c >= 0.f) ? green : red;
    if (cw > 0)
        go_fill(plug, bx + mid, my, cw, bar_h, cc);
    else if (cw < 0)
        go_fill(plug, bx + mid + cw, my, -cw, bar_h, cc);
    go_rect(plug, bx, my, bar_w, bar_h, axis);
    go_line(plug, bx + mid, my, bx + mid, my + bar_h, axis);
    /* add a text label */
    snprintf(buf, sizeof(buf), "CORR  %+.2f", (double)c);
    go_text(plug, bx, my + bar_h + 14, buf, fg);
    /* Balance (−1 left-heavy … +1 right-heavy) */
    float b = plug->bal;
    /* clamp if needed */
    if (b > 1.f) {
        b = 1.f;
    } else if (b < -1.f) {
        b = -1.f;
    }
    /* draw the bar */
    int by = my + 32;
    go_fill(plug, bx, by, bar_w, bar_h, bar_bg);
    int bw = (int)(b * (float)mid);
    if (bw > 0)
        go_fill(plug, bx + mid, by, bw, bar_h, yellow);
    else if (bw < 0)
        go_fill(plug, bx + mid + bw, by, -bw, bar_h, yellow);
    go_rect(plug, bx, by, bar_w, bar_h, axis);
    go_line(plug, bx + mid, by, bx + mid, by + bar_h, axis);
    /* add a label */
    snprintf(buf, sizeof(buf), "BAL   %+.2f", (double)b);
    go_text(plug, bx, by + bar_h + 14, buf, fg);
    /* if we have been drawing offscreen, we are ready to transfer what
                                         we have done to the main window */    
    if (plug->back != None) {
        XCopyArea(plug->dpy, plug->back, plug->win, plug->gc,
                  0, 0, (unsigned)plug->gui_w, (unsigned)plug->gui_h, 0, 0);
    }
    /* flush the buffer */
    XFlush(plug->dpy);
}

void go_gui_redraw(go_plug_t *plug) {
    /* Request a full repaint if the GUI is currently visible.
       Inputs:
         <*go_plug_t> - plugin instance
       Called after any state change that should be reflected on screen. */
    if (plug && plug->gui_visible) go_gui_paint(plug);
}

static bool go_gui_is_api_supported(const clap_plugin_t *p,
                                        const char *api, bool f) {
    /* CLAP GUI extension – report whether a given windowing API is supported.
       Inputs:
         <*clap_plugin_t> - the plugin instance (unused)
         <*api>           - windowing API name string
         <is_floating>    - true when the host wants a floating window
       Returns:
         <bool> - true only for CLAP_WINDOW_API_X11 */
    (void)p;
    (void)f;
    return api && strcmp(api, CLAP_WINDOW_API_X11) == 0;
}

static bool go_gui_get_preferred_api(const clap_plugin_t *p,
                                    const char **api, bool *f) {
    /* CLAP GUI extension – return the preferred windowing API.
       Inputs:
         <*clap_plugin_t> - the plugin instance (unused)
         <**api>          - out parameter that receives the API name
         <*is_floating>   - out parameter that receives the floating preference
       Returns:
         <bool> - always true;
            sets *api to CLAP_WINDOW_API_X11 and *is_floating to false */
    (void)p;
    *api = CLAP_WINDOW_API_X11;
    *f = false;   /* embedded, not floating */
    return true;
}

static bool go_gui_create(const clap_plugin_t *plugin,
                                    const char *api, bool f) {
    /* CLAP GUI extension – create the X11 window and associated resources.
       Inputs:
         <*clap_plugin_t> - the plugin instance
         <*api>           - requested windowing API (must be X11)
         <is_floating>    - ignored (always embedded)
       Returns:
         <bool> - true on success, false if the display cannot be opened or
                  the API is not X11
       Opens the default X display, creates a simple window of the default
       size, obtains a graphics context, selects the required event masks
       and registers the X11 file descriptor with the host so that events
       can be processed from the main thread. */
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
    unsigned long bg = go_col(GO_BG_R, GO_BG_G, GO_BG_B);
    plug->win = XCreateSimpleWindow(plug->dpy, root, 0, 0,
                                (unsigned)plug->gui_w, (unsigned)plug->gui_h,
                                                                   0, bg, bg);
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
     * ~60 Hz CLAP timer.  The host calls our on_timer callback, which
     * is the primary place we continuous-redraw from.  period_ms = 16
     * is accepted by hosts that allow ≥30 Hz; many will round to 16–17.
     */
    plug->timer_id = CLAP_INVALID_ID;
    if (plug->host_timer && plug->host_timer->register_timer) {
        clap_id id = CLAP_INVALID_ID;
        if (plug->host_timer->register_timer(plug->host, 16, &id)) {
            plug->timer_id = id;
        }
    }
    go_ensure_back(plug);
    plug->gui_created = 1;
    plug->gui_visible = 0;
    return true;
}

static void go_gui_destroy(const clap_plugin_t *plugin) {
    /* CLAP GUI extension – destroy the X11 window and free all resources.
       Inputs:
         <*clap_plugin_t> - the plugin instance
       Unregisters the CLAP timer and X11 fd, frees the GC / pixmap,
       destroys the window and closes the display.  All fields reset. */
    go_plug_t *plug = (go_plug_t *)plugin->plugin_data;
    if (plug->host_timer && plug->host_timer->unregister_timer &&
        plug->timer_id != CLAP_INVALID_ID) {
        plug->host_timer->unregister_timer(plug->host, plug->timer_id);
        plug->timer_id = CLAP_INVALID_ID;
    }
    if (plug->host_fd && plug->host_fd->unregister_fd) {
        if (plug->xfd >= 0) {
            plug->host_fd->unregister_fd(plug->host, plug->xfd);
        }
    }
    if (plug->back != None && plug->dpy) {
        XFreePixmap(plug->dpy, plug->back);
        plug->back = None;
    }
    if (plug->gc && plug->dpy) {
        XFreeGC(plug->dpy, plug->gc);
        plug->gc = NULL;
    }
    if (plug->win && plug->dpy) {
        XDestroyWindow(plug->dpy, plug->win);
        plug->win = 0;
    }
    if (plug->dpy) {
        XCloseDisplay(plug->dpy);
        plug->dpy = NULL;
    }
    plug->gui_created = 0;
    plug->gui_visible = 0;
    plug->xfd = -1;
}

static bool go_gui_set_scale(const clap_plugin_t *p, double s) {
    /* CLAP GUI extension – set a global UI scale factor.
       Inputs:
         <*clap_plugin_t> - the plugin instance (unused)
         <scale>          - requested scale factor
       Returns:
         <bool> - always true */
    (void)p;
    (void)s;
    return true;
}

static bool go_gui_get_size(const clap_plugin_t *plugin,
                                    uint32_t *w, uint32_t *h) {
    /* CLAP GUI extension – report the current window size.
       Inputs:
         <*clap_plugin_t> - the plugin instance
         <*w,*h>          - out parameters that receive width and height
       Returns:
         <bool> - always true */
    go_plug_t *plug = (go_plug_t *)plugin->plugin_data;
    *w = (uint32_t)plug->gui_w;
    *h = (uint32_t)plug->gui_h;
    return true;
}

static bool go_gui_can_resize(const clap_plugin_t *p) {
    /* CLAP GUI extension – indicate whether the window may be resized.
       Inputs:
         <*clap_plugin_t> - the plugin instance (unused)
       Returns:
         <bool> - always true */
    (void)p;
    return true;
}

static bool go_gui_get_resize_hints(const clap_plugin_t *p,
                                    clap_gui_resize_hints_t *h) {
    /* CLAP GUI extension – supply resize constraints to the host.
       Inputs:
         <*clap_plugin_t>          - the plugin instance (unused)
         <*clap_gui_resize_hints_t> - structure filled with the hints
       Returns:
         <bool> - always true
       Horizontal and vertical resizing are both allowed; aspect ratio is
       not enforced. */
    (void)p;
    h->can_resize_horizontally = true;
    h->can_resize_vertically = true;
    /* No aspect lock -
                hosts that force square were clipping the meter band. */
    h->preserve_aspect_ratio = false;
    h->aspect_ratio_width = 0;
    h->aspect_ratio_height = 0;
    return true;
}

static bool go_gui_adjust_size(const clap_plugin_t *p,
                                    uint32_t *w, uint32_t *h) {
    /* CLAP GUI extension – clamp a proposed size to the minimums.
       Inputs:
         <*clap_plugin_t> - the plugin instance (unused)
         <*w,*h>          - in/out size that may be adjusted
       Returns:
         <bool> - always true
       Ensures the window never becomes smaller than 320×420. */
    (void)p;
    if (*w < 320) *w = 320;
    if (*h < 420) *h = 420;   /* room for scope + fixed meter band */
    return true;
}

static bool go_gui_set_size(const clap_plugin_t *plugin,
                                        uint32_t w, uint32_t h) {
    /* CLAP GUI extension – apply a new window size.
       Inputs:
         <*clap_plugin_t> - the plugin instance
         <w,h>            - new width and height in pixels
       Returns:
         <bool> - always true
       Updates the internal size, resizes the X11 window and forces a
       full repaint. */
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

static bool go_gui_set_parent(const clap_plugin_t *plugin,
                                    const clap_window_t *window) {
    /* CLAP GUI extension – embed the plugin window into a host parent.
       Inputs:
         <*clap_plugin_t>   - the plugin instance
         <*clap_window_t>   - host-provided window handle
       Returns:
         <bool> - true on success, false if the API is not X11 or resources
                  are missing
       Reparents the plugin window under the given X11 parent and maps it. */
    go_plug_t *plug = (go_plug_t *)plugin->plugin_data;
    if (!window || !plug->dpy ||
            strcmp(window->api, CLAP_WINDOW_API_X11) != 0) {
        return false;
    }
    XReparentWindow(plug->dpy, plug->win, (Window)window->x11, 0, 0);
    XMapWindow(plug->dpy, plug->win);
    XFlush(plug->dpy);
    return true;
}

static bool go_gui_set_transient(const clap_plugin_t *p,
                                        const clap_window_t *w) {
    /* CLAP GUI extension – set a transient parent (unused).
       Inputs:
         <*clap_plugin_t> - the plugin instance (unused)
         <*clap_window_t> - transient parent (unused)
       Returns:
         <bool> - always true */
    (void)p;
    (void)w;
    return true;
}

static void go_gui_suggest_title(const clap_plugin_t *plugin,
                                                const char *title) {
    /* CLAP GUI extension – suggest a window title to the X server.
       Inputs:
         <*clap_plugin_t> - the plugin instance
         <*title>         - null-terminated title string
       Stores the title with XStoreName when a display and window exist. */
    go_plug_t *plug = (go_plug_t *)plugin->plugin_data;
    if (plug->dpy && plug->win && title)
        XStoreName(plug->dpy, plug->win, title);
}

static bool go_gui_show(const clap_plugin_t *plugin) {
    /* CLAP GUI extension – make the plugin window visible.
       Inputs:
         <*clap_plugin_t> - the plugin instance
       Returns:
         <bool> - true on success, false if the window does not exist
       Maps the window, records that it is visible and performs an initial
       paint. */
    go_plug_t *plug = (go_plug_t *)plugin->plugin_data;
    if (!plug->dpy || !plug->win) {
        return false;
    }
    XMapWindow(plug->dpy, plug->win);
    plug->gui_visible = 1;
    go_gui_paint(plug);
    return true;
}

static bool go_gui_hide(const clap_plugin_t *plugin) {
    /* CLAP GUI extension – hide the plugin window.
       Inputs:
         <*clap_plugin_t> - the plugin instance
       Returns:
         <bool> - always true
       Unmaps the window and clears the visible flag. */
    go_plug_t *plug = (go_plug_t *)plugin->plugin_data;
    if (plug->dpy && plug->win) XUnmapWindow(plug->dpy, plug->win);
    plug->gui_visible = 0;
    return true;
}

static void go_sync_size_from_parent(go_plug_t *plug) {
    /* Take care of one specific edge case:
        If the host grows its container without resizing our child window
        (common when dragging the bottom-right corner), match our window to
        the parent's client size so the UI fills the frame instead of leaving
        a black margin.
        Inputs:
            <*go_plug_t> - instance of our goniometer plug-in  */
    /* if we have no window open, our job is easy */
    if (!plug->dpy || !plug->win) return;
    /* query X tree */
    Window root = 0, parent = 0, *kids = NULL;
    unsigned int nkids = 0;
    if (!XQueryTree(plug->dpy, plug->win, &root, &parent, &kids, &nkids)) {
        return;
    }
    /* clear the list of window IDs as we are not using it*/
    if (kids) {
        XFree(kids);
    }
    /* not reparented (still under root) — nothing to sync */
    if (!parent || parent == root) {
        return;
    }
    /* read the parent's width/height in pixels (host GUI frame) */
    Window r;
    int x, y;
    unsigned int pw, ph, bw, depth;
    if (!XGetGeometry(plug->dpy, parent, &r, &x, &y, &pw, &ph, &bw, &depth))
        return;
    int nw = (int)pw;
    int nh = (int)ph;
    if (nw < 320) nw = 320;
    if (nh < 420) nh = 420;
    /* already matching — avoid a redundant XResizeWindow every frame */
    if (nw == plug->gui_w && nh == plug->gui_h) {
        return;
    }
    /* if not, we update the layout */
    plug->gui_w = nw;
    plug->gui_h = nh;
    XResizeWindow(plug->dpy, plug->win, (unsigned)nw, (unsigned)nh);
}

static void go_gui_on_fd(const clap_plugin_t *plugin, int fd,
                                    clap_posix_fd_flags_t flags) {
    /* CLAP POSIX FD support – process pending X11 events on the
       connection fd.  Timer-driven redraw is handled separately by
       go_gui_on_timer (CLAP_EXT_TIMER_SUPPORT).
       Inputs:
         <*clap_plugin_t>     - the plugin instance
         <fd>                 - file descriptor that became readable
         <clap_posix_fd_flags_t> - event flags
       Drains the X event queue.  Expose and ConfigureNotify trigger
       a full repaint.  ButtonPress is ignored (no interactive controls). */
    (void)flags;
    (void)fd;
    go_plug_t *plug = (go_plug_t *)plugin->plugin_data;
    if (!plug->dpy) {
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
            }
            need_redraw = true;
        }
        /* ButtonPress intentionally ignored — no interactive controls. */
    }
    if (need_redraw) {
        go_gui_paint(plug);
    }
}

static void go_gui_on_timer(const clap_plugin_t *plugin, clap_id timer_id) {
    /* CLAP timer-support – periodic redraw callback (~60 Hz).
       Inputs:
         <*clap_plugin_t> - the plugin instance
         <timer_id>       - id of the timer that fired
       Syncs window size from the host parent (if embedded) and paints. */
    go_plug_t *plug = (go_plug_t *)plugin->plugin_data;
    if (timer_id != plug->timer_id)
        return;
    if (plug->gui_visible && plug->dpy) {
        go_sync_size_from_parent(plug);
        go_gui_paint(plug);
    }
}

/* Static table of CLAP GUI extension entry points.
   Provides the host with all required callbacks for creating, sizing,
   showing and destroying the X11-based user interface. */
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

/* Static table of CLAP POSIX file-descriptor support.
   Allows the host to notify the plugin when the X11 connection becomes
   readable so that events can be processed on the main thread. */
const clap_plugin_posix_fd_support_t go_posix_fd_ext = {
    .on_fd = go_gui_on_fd
};

/* Static table of CLAP timer-support.
   Host-driven periodic timer used for continuous ~60 Hz GUI refresh. */
const clap_plugin_timer_support_t go_timer_ext = {
    .on_timer = go_gui_on_timer
};
