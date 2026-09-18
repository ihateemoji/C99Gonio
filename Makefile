# C99Gonio — minimal C99 CLAP stereo goniometer
#
#   make          build C99Gonio.clap
#   make install  copy into ~/.clap
#   make clean    remove C99Gonio.clap from current directory 

CC      ?= gcc
CFLAGS  ?= -O3 -fPIC -Wall -Wextra -std=c99
CFLAGS  += -Ithird_party/clap/include -Isrc
LDFLAGS ?= -shared -Wl,--version-script=export.map -lm -lX11

# BSD systems install packages under /usr/local
UNAME_S := $(shell uname -s)
ifneq (,$(filter FreeBSD OpenBSD NetBSD DragonFly,$(UNAME_S)))
CFLAGS  += -I/usr/local/include
LDFLAGS += -L/usr/local/lib
endif

SRC = src/goniometer.c src/gui_x11.c
OUT = C99Gonio.clap

.PHONY: all clean install

all: $(OUT)

$(OUT): $(SRC) src/goniometer.h
	$(CC) $(CFLAGS) -o $(OUT) $(SRC) $(LDFLAGS)

install: $(OUT)
	mkdir -p "$(HOME)/.clap"
	cp -f $(OUT) "$(HOME)/.clap/C99Gonio.clap"

clean:
	rm -f $(OUT)
