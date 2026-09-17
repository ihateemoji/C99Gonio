# C99Gonio

Minimal C99 CLAP stereo goniometer with a lightweight X11 GUI

## Build

### Requirements

- C99 compiler (`gcc` or `clang`)
- X11 development headers (`libx11-dev` on Debian/Ubuntu)
- CLAP headers (vendored under `third_party/clap/include`)
CLAP is included as a git submodule:

```bash
git clone --recurse-submodules https://github.com/ihateemoji/C99Gonio
# or after cloning:
git submodule update --init --recursive
```
```bash
make            # → C99Gonio.clap
make install    # copies to ~/.clap/C99Gonio.clap
make clean
```

Then rescan plugins in any CLAP host (Bitwig, Reaper, Ardour, Carla, …).
