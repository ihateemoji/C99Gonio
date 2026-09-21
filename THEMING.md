# Theming C99Gonio

All colours used by the X11 GUI live in `src/goniometer.h` as simple
`#define`s.  Change the RGB values, recompile, and you have a new theme.

There is no runtime theme switching — this is intentional for a minimal
C99 plugin.

## Colour constants

| Constant          | Default RGB     | Used for                                      |
|-------------------|-----------------|-----------------------------------------------|
| `GO_BG_*`         | 18, 20, 24      | Main window background                        |
| `GO_SCOPE_BG_*`   | 12, 14, 18      | Darker fill inside the scope square           |
| `GO_BAR_BG_*`     | 28, 30, 36      | Background of the CORR / BAL meter bars       |
| `GO_FG_*`         | 230, 234, 240   | Title text and general foreground             |
| `GO_GRID_*`       | 40, 44, 52      | Mid/Side cross-hairs                          |
| `GO_AXIS_*`       | 70, 78, 92      | Scope border rectangle                        |
| `GO_DIAG_*`       | 32, 36, 44      | Pure-L / pure-R diagonal guides               |
| `GO_CYA_*`        | 70, 200, 220    | Lissajous trace (the main scope line)         |
| `GO_GRN_*`        | 90, 210, 140    | Positive correlation / balanced meter         |
| `GO_YEL_*`        | 220, 190, 70    | Warning / mid-range meter values              |
| `GO_RED_*`        | 220, 90, 90     | Negative correlation / out-of-phase warning   |

All values are 8-bit (0–255).  They are packed into X11 pixel values by
the helper `go_col()` in `gui_x11.c`.

## Themes

### Amber CRT

```C
#define GO_BG_R   12
#define GO_BG_G   10
#define GO_BG_B   6
#define GO_SCOPE_BG_R 8
#define GO_SCOPE_BG_G 6
#define GO_SCOPE_BG_B 4
#define GO_BAR_BG_R 22
#define GO_BAR_BG_G 18
#define GO_BAR_BG_B 12
#define GO_FG_R   255
#define GO_FG_G   200
#define GO_FG_B   80
#define GO_GRID_R 60
#define GO_GRID_G 45
#define GO_GRID_B 20
#define GO_AXIS_R 100
#define GO_AXIS_G 75
#define GO_AXIS_B 30
#define GO_DIAG_R 40
#define GO_DIAG_G 30
#define GO_DIAG_B 15
#define GO_CYA_R  255
#define GO_CYA_G  180
#define GO_CYA_B  40
#define GO_GRN_R  200
#define GO_GRN_G  160
#define GO_GRN_B  40
#define GO_YEL_R  255
#define GO_YEL_G  140
#define GO_YEL_B  30
#define GO_RED_R  255
#define GO_RED_G  80
#define GO_RED_B  40
```


## How to change the theme

1. Edit the `#define`s in `src/goniometer.h`.
2. Rebuild:

   ```bash
   make clean && make
