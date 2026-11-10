# wl-numlock-fix

An `LD_PRELOAD` shim that fixes NumLock and CapsLock for Linux apps on
ChromeOS (Crostini).

## The problem

ChromeOS' compositor keeps NumLock permanently on, and reports NumLock and
CapsLock in `wl_keyboard.modifiers` as *depressed* (as if the keys were held
down) rather than *locked*. Apps that ignore locked modifiers when matching
shortcuts, but not depressed ones, then see NumLock in every key combination.
In [foot](https://codeberg.org/dnkl/foot), for example, `Control+Shift+n` is
seen as `Control+Shift+Mod2+n`, and the mouse wheel as `Mod2+wheel`, so
shortcuts and scrollback scrolling stop working. Tapping a real NumLock key
does not turn it off.

## The fix

The shim interposes `wl_proxy_add_listener()` from libwayland-client. For
`wl_keyboard` objects, it wraps the listener's `modifiers` handler and moves
the lock modifiers from `mods_depressed` to `mods_locked`. Effective modifiers
(and so typed text and the keypad) are unchanged.

It is meant to be preloaded into **sommelier**, the Wayland proxy Crostini
uses between ChromeOS and the container's apps. Sommelier receives the
modifiers from ChromeOS through libwayland-client and forwards them to every
app, so fixing them there fixes every Wayland and X11 (Xwayland) app at once.
It also works preloaded into individual apps.

The shim does not link libwayland-client; it looks it up only when a client
first calls `wl_proxy_add_listener()`, so it does nothing in non-Wayland
processes.

## Install

    make
    sudo make install   # /usr/local/lib/libwl-numlock-fix.so

Then add a systemd user drop-in for both sommelier templates, e.g.
`~/.config/systemd/user/sommelier@.service.d/numlock-fix.conf` and
`~/.config/systemd/user/sommelier-x@.service.d/numlock-fix.conf`:

    [Service]
    Environment=LD_PRELOAD=/usr/local/lib/libwl-numlock-fix.so

and restart the container (or `systemctl --user daemon-reload` and restart
the sommelier services, which closes all Linux GUI apps).

## Environment

* `WL_NUMLOCK_FIX_MASK`: modifier bits to move (default `0x12`: Lock and
  Mod2, i.e. CapsLock and NumLock).
* `WL_NUMLOCK_FIX_DEBUG`: if set, log to stderr; if an absolute path, append
  to that file instead.

## Recovery

If Linux apps stop opening after enabling it, remove the drop-ins from a
terminal that still works (or via `vsh termina` → `lxc exec penguin -- su -l
<user>` from crosh), then restart the container.
