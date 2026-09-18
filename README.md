# C99Gonio

Minimal C99 CLAP goniometer with a lightweight X11 GUI

![screenshot](imgs/C99Gonio.jpeg?raw=true)

## Build

All you need is:
- C99 compiler (`gcc` or `clang`)
- X11 development headers (`libx11-dev` on Debian/Ubuntu)
  
CLAP is included as a git submodule, so all you need is to clone this repository:
```bash
git clone --recurse-submodules https://github.com/ihateemoji/C99Gonio
```
and then simply build the code with 
```bash
make            
```
which will produce a `C99Gonio.clap` binary. You could install this binary manually or let make copy it for you into `~/.clap/C99Gonio.clap` via 
```bash
make install
```

Then rescan plugins in any CLAP host (Bitwig, Reaper, Ardour, Carla, …).

PS: If you are like me and forget `--recurse-submodules` 9/10 times, you can just cd into the cloned directory and
```bash
git submodule update --init --recursive
```

## Disclaimer

I have very minimal experience with GUI programming, so I started this project as an opportunity to gain some. Any improvements/contributions from anyone more experienced than me will be greatly appreciated!
