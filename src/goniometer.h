/*
 * C99Gonio — minimal C99 CLAP goniometer
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
 * GUI is driven by CLAP timer-support at ~60 Hz, drawn into an
 * offscreen Pixmap and blitted in one shot to avoid flicker.
 * X11 events are delivered via CLAP posix-fd-support on the
 * connection file descriptor.
 */
#ifndef GONIOMETER_DOT_H
#define GONIOMETER_DOT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <clap/clap.h>
/* clap.h already pulls in host + all official extensions
 * (gui, log, params, posix-fd-support, timer-support, state, audio-ports, …) */

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

/* ---- GUI drawing constants -------------------------------------------- */

#define GO_TOP          26           /* title bar height / top margin      */
#define GO_MARGIN       16           /* scope outer margin                 */
#define GO_BAR_H        12           /* meter bar thickness                */
#define GO_CLAMP        0.97f        /* max overshoot before clamping      */
#define GO_INV_SCALE    0.92f        /* peak maps to this fraction of rad  */
#define GO_SMOOTH       0.22f        /* display trail smoothing factor     */
#define GO_INV_SQRT2    0.70710678f  /* 1/sqrt(2) for Mid/Side projection  */

/* auto-scale peak smoothing (blend factors) */
#define GO_PEAK_UP_A    0.3f
#define GO_PEAK_UP_B    0.7f
#define GO_PEAK_DN_A    0.8f
#define GO_PEAK_DN_B    0.2f

/* colour helpers (RGB 0..255) — values computed via go_col() in .c */
#define GO_BG_R   18
#define GO_BG_G   20
#define GO_BG_B   24
#define GO_GRID_R 40
#define GO_GRID_G 44
#define GO_GRID_B 52
#define GO_AXIS_R 70
#define GO_AXIS_G 78
#define GO_AXIS_B 92
#define GO_FG_R   230
#define GO_FG_G   234
#define GO_FG_B   240
#define GO_CYA_R  70
#define GO_CYA_G  200
#define GO_CYA_B  220
#define GO_GRN_R  90
#define GO_GRN_G  210
#define GO_GRN_B  140
#define GO_YEL_R  220
#define GO_YEL_G  190
#define GO_YEL_B  70
#define GO_RED_R  220
#define GO_RED_G  90
#define GO_RED_B  90
#define GO_SCOPE_BG_R 12
#define GO_SCOPE_BG_G 14
#define GO_SCOPE_BG_B 18
#define GO_DIAG_R 32
#define GO_DIAG_G 36
#define GO_DIAG_B 44

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
    const clap_host_timer_support_t *host_timer;

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
    Pixmap     back;            /* offscreen buffer single blit, no flicker */
    int        back_w, back_h;
    int        gui_w, gui_h;
    int        gui_created;
    int        gui_visible;
    int        xfd;             /* X connection fd registered with the host */
    clap_id    timer_id;        /* CLAP timer for ~60 Hz redraw             */
} go_plug_t;

void go_state_default(go_state_t *st);
void go_clamp(go_state_t *st);

/* Implemented in gui_x11.c */
extern const clap_plugin_gui_t go_gui_ext;
extern const clap_plugin_posix_fd_support_t go_posix_fd_ext;
extern const clap_plugin_timer_support_t go_timer_ext;
void go_gui_redraw(go_plug_t *plug);

#endif /* GONIOMETER_DOT_H */
