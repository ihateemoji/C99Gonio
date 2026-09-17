#ifndef GONIOMETER_DOT_H
#define GONIOMETER_DOT_H

/*
 * C99Gonio — minimal C99 CLAP stereo goniometer
 * ==============================================
 *
 * A pure-C99 CLAP audio effect that visualises the stereo field as a
 * Mid/Side Lissajous figure (classic audio goniometer).  Stereo audio
 * is passed through unchanged; the only job of the DSP side is to feed
 * (L, R) sample pairs into a ring buffer that the X11 GUI reads.
 *
 * Coordinate system (standard mastering goniometer):
 *   X  = Side  = (L - R) / sqrt(2)
 *   Y  = Mid   = (L + R) / sqrt(2)
 * so mono sits on the vertical axis and anti-phase on the horizontal.
 *
 * Display scale is fully automatic: the GUI tracks the recent peak of
 * the Mid/Side magnitude and zooms so the trace always fits inside the
 * scope circle.  There are no user parameters.
 *
 * GUI is driven by timerfd at ~60 Hz via CLAP posix-fd-support, drawn
 * into an offscreen Pixmap and blitted in one shot to avoid flicker.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/timerfd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <clap/clap.h>
#include <clap/host.h>
#include <clap/ext/gui.h>
#include <clap/ext/log.h>
#include <clap/ext/params.h>
#include <clap/ext/posix-fd-support.h>
#include <clap/ext/state.h>
#include <clap/ext/audio-ports.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846264338327950288
#endif

/* ---- identity / sizes -------------------------------------------------- */

#define GO_MAGIC        0x474F4E31u  /* "GON1" — state blob magic          */
#define GO_VERSION      1u           /* state format version               */

#define GO_SCOPE_LEN    1024         /* raw (L,R) samples kept for the GUI */
#define GO_DISP_LEN     256          /* smoothed Mid/Side points on screen */

#define GO_METER_H      90           /* fixed bottom band for CORR / BAL   */
#define GO_GUI_W        480          /* default window width               */
#define GO_GUI_H        560          /* default window height (scope+meters)*/

/* ---- persistent state (saved/restored via CLAP state extension) -------- */

typedef struct {
    uint32_t magic;     /* must be GO_MAGIC                                */
    uint32_t version;   /* must be GO_VERSION                              */
    /* no parameters — auto-scale only; struct kept for future expansion   */
    uint32_t reserved[4];
} go_state_t;

/* A single stereo sample pair captured from the process() callback. */
typedef struct {
    float L;
    float R;
} go_point_t;

/* ---- full plugin instance (shared by plugin.c and gui_x11.c) ----------- */

typedef struct {
    /* CLAP boilerplate */
    clap_plugin_t plugin;
    const clap_host_t *host;
    const clap_host_log_t *host_log;
    const clap_host_params_t *host_params;
    const clap_host_state_t *host_state;
    const clap_host_gui_t *host_gui;
    const clap_host_posix_fd_support_t *host_fd;

    go_state_t st;
    double     sr;              /* sample rate set in activate()           */
    int        active;
    int        processing;

    /*
     * Scope ring — written from the audio thread in process(),
     * read from the main/GUI thread in paint().  Single-producer /
     * single-consumer; indices are only ever advanced, never rewound,
     * so a torn read at most shows a one-frame glitch.
     */
    go_point_t scope[GO_SCOPE_LEN];
    uint32_t   scope_write;     /* next write index                        */
    uint32_t   scope_count;     /* how many valid points (<= GO_SCOPE_LEN) */

    /*
     * Smoothed display trail.  Updated on every paint frame toward the
     * latest ring contents so the curve moves calmly at 60 fps instead
     * of scribbling with every new sample.
     */
    float      disp_s[GO_DISP_LEN];  /* Side coordinate                    */
    float      disp_m[GO_DISP_LEN];  /* Mid  coordinate                    */
    uint32_t   disp_count;

    /*
     * Auto-scale: peak of hypot(Side, Mid) over recent samples, smoothed
     * so the zoom does not jump.  The paint path divides by (auto_peak
     * * margin) so the trace always sits inside the unit circle.
     */
    float      auto_peak;       /* smoothed peak magnitude, >= small eps   */

    /* Running meters (also smoothed, hold-on-silence). */
    float      corr;            /* correlation  -1 ... +1                  */
    float      bal;             /* balance      -1 (L) ... +1 (R)          */
    float      peak_l, peak_r;

    /* X11 GUI */
    Display   *dpy;
    Window     win;
    GC         gc;
    Pixmap     back;            /* offscreen buffer — single blit, no flicker */
    int        back_w, back_h;
    int        gui_w, gui_h;
    int        gui_created;
    int        gui_visible;
    int        xfd;             /* X connection fd registered with the host */
    int        timer_fd;        /* timerfd for ~60 Hz redraw                */
} go_plug_t;

void go_state_default(go_state_t *st);
void go_clamp(go_state_t *st);

/* Implemented in gui_x11.c */
extern const clap_plugin_gui_t go_gui_ext;
extern const clap_plugin_posix_fd_support_t go_posix_fd_ext;
void go_gui_redraw(go_plug_t *plug);

#endif /* GONIOMETER_DOT_H */
