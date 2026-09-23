# Theming C99Gonio

All colours used by the X11 GUI live in `src/goniometer.h` as simple
`#define`s.  Change the RGB values, recompile, and you have a new theme.


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

Ready-made themes live in the `themes/` directory as unified diff patches
against the default colour block in `src/goniometer.h`. Apply one before
building:

```bash
patch -p0 < themes/amber-crt.patch
make clean && make
```

To return to the default theme:

```bash
git checkout -- src/goniometer.h
```

Other user-contributed themes are always welcome — open a PR that adds a
new `.patch` file under `themes/` (and a screenshot under `imgs/` with an appropriate change to `THEMING.md`).

### Amber CRT

![screenshot](imgs/C99Gonio_AmberCRT.jpeg?raw=true)

```bash
patch -p0 < themes/amber-crt.patch
```

### Nord

![screenshot](imgs/C99Gonio_Nord.jpeg?raw=true)

```bash
patch -p0 < themes/nord.patch
```

### Vintage Green

![screenshot](imgs/C99Gonio_Vintage%20Green.jpeg?raw=true)

```bash
patch -p0 < themes/vintage-green.patch
```

## How to create a new theme

1. Edit the `#define`s in `src/goniometer.h`.
2. Generate a patch:

   ```bash
   git diff src/goniometer.h > themes/my-theme.patch
   ```

3. Rebuild to test:

   ```bash
   make clean && make
   ```
   
4. Replace your old `C99Gonio.clap` binary with the one you just built and
   rescan the plug-ins in your DAW.
   
6. Add a screenshot under `imgs/` and document the theme here.
