PREFIX ?= /usr
DESTDIR ?=
CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -Wno-unused-parameter
PKGS = libpipewire-0.3 libyuv

all: rk-pw-camera

rk-pw-camera: src/rk-pw-camera.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(shell pkg-config --cflags $(PKGS) 2>/dev/null || pkg-config --cflags libpipewire-0.3) \
		-o $@ $< $(LDFLAGS) $(shell pkg-config --libs libpipewire-0.3) -lyuv

install: rk-pw-camera
	install -Dm755 rk-pw-camera $(DESTDIR)$(PREFIX)/bin/rk-pw-camera
	install -Dm755 src/rk-camera-select $(DESTDIR)$(PREFIX)/libexec/rk-camera-select
	install -Dm644 data/rk-camera-select.service $(DESTDIR)$(PREFIX)/lib/systemd/system/rk-camera-select.service
	install -Dm644 data/rk-pw-camera.service $(DESTDIR)$(PREFIX)/lib/systemd/user/rk-pw-camera.service
	install -Dm644 data/50-rk-pw-camera.conf $(DESTDIR)$(PREFIX)/share/wireplumber/wireplumber.conf.d/50-rk-pw-camera.conf

clean:
	rm -f rk-pw-camera

.PHONY: all install clean
