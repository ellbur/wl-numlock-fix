PREFIX ?= /usr/local
CFLAGS ?= -O2 -Wall -Wextra
WAYLAND_CFLAGS := $(shell pkg-config --cflags wayland-client)

libwl-numlock-fix.so: wl-numlock-fix.c
	$(CC) $(CFLAGS) $(WAYLAND_CFLAGS) -fPIC -shared -o $@ $< -ldl -lpthread

install: libwl-numlock-fix.so
	install -Dm644 libwl-numlock-fix.so $(DESTDIR)$(PREFIX)/lib/libwl-numlock-fix.so

clean:
	rm -f libwl-numlock-fix.so

.PHONY: install clean
