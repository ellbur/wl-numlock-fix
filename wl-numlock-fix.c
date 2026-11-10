/*
 * wl-numlock-fix: LD_PRELOAD shim for Wayland clients on ChromeOS (Crostini).
 *
 * ChromeOS' compositor (Exo, via sommelier) keeps NumLock permanently on, and
 * reports it (and CapsLock) in wl_keyboard.modifiers as *depressed*, as if the
 * keys were held down, instead of *locked*. Clients that ignore locked
 * modifiers when matching shortcuts then see NumLock in every key combination,
 * and shortcuts stop working.
 *
 * This shim interposes wl_proxy_add_listener(). For wl_keyboard objects it
 * installs a copy of the client's listener whose modifiers handler moves the
 * affected modifiers from mods_depressed to mods_locked before calling the
 * client's own handler. Effective modifiers are unchanged.
 *
 * The shim does not link libwayland-client; it looks up what it needs only
 * once a client calls wl_proxy_add_listener(), so preloading it into
 * non-Wayland processes costs nothing.
 *
 * Environment:
 *   WL_NUMLOCK_FIX_MASK   modifier bits to move (default 0x12: Lock|Mod2,
 *                         i.e. CapsLock and NumLock)
 *   WL_NUMLOCK_FIX_DEBUG  if set, log to stderr; if an absolute path, append
 *                         to that file instead (some apps close stderr)
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

typedef void (*fn_t)(void);
typedef void (*modifiers_fn)(void *data, struct wl_keyboard *kbd, uint32_t serial,
                             uint32_t depressed, uint32_t latched,
                             uint32_t locked, uint32_t group);

/* One per wl_keyboard listener we replaced. Never freed: there are only a
 * handful per process, and a stale entry is harmless. */
struct wrap {
    struct wrap *next;
    const void *orig_listener;
    modifiers_fn orig_modifiers;
    fn_t fns[]; /* the listener handed to libwayland */
};

static int (*real_add_listener)(struct wl_proxy *, fn_t *, void *);
static const void *(*real_get_listener)(struct wl_proxy *);
static const char *(*real_get_class)(struct wl_proxy *);
static uint32_t (*real_get_version)(struct wl_proxy *);
static uint32_t (*real_get_id)(struct wl_proxy *);
static const struct wl_interface *keyboard_iface;
static uint32_t fix_mask = 0x12; /* Lock (CapsLock) | Mod2 (NumLock) */
static FILE *debug;
static int modifiers_index = -1;

static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct wrap *wraps;

/*
 * Look up SYM in libwayland-client. RTLD_NEXT finds it when libwayland is in
 * the global scope; when a client loaded it via a dlopen()ed plugin (e.g. Qt's
 * platform plugin, RTLD_LOCAL), fall back to the already-loaded library.
 */
static void *
wl_sym(const char *sym)
{
    void *p = dlsym(RTLD_NEXT, sym);
    if (p != NULL)
        return p;

    void *h = dlopen("libwayland-client.so.0", RTLD_LAZY | RTLD_NOLOAD);
    if (h == NULL)
        return NULL;
    p = dlsym(h, sym);
    dlclose(h); /* NOLOAD handle: just drops the extra reference */
    return p;
}

static void
init(void)
{
    real_add_listener = wl_sym("wl_proxy_add_listener");
    real_get_listener = wl_sym("wl_proxy_get_listener");
    real_get_class = wl_sym("wl_proxy_get_class");
    real_get_version = wl_sym("wl_proxy_get_version");
    real_get_id = wl_sym("wl_proxy_get_id");
    keyboard_iface = wl_sym("wl_keyboard_interface");
    if (real_get_class == NULL || real_get_version == NULL ||
        real_get_id == NULL || real_get_listener == NULL)
        keyboard_iface = NULL; /* passthrough only */

    const char *m = getenv("WL_NUMLOCK_FIX_MASK");
    if (m != NULL && *m != '\0')
        fix_mask = (uint32_t)strtoul(m, NULL, 0);
    const char *d = getenv("WL_NUMLOCK_FIX_DEBUG");
    if (d != NULL) {
        debug = d[0] == '/' ? fopen(d, "ae") : NULL;
        if (debug != NULL)
            setvbuf(debug, NULL, _IOLBF, 0);
        else
            debug = stderr;
    }

    /* Find the event's slot from libwayland's own interface description */
    for (int i = 0; keyboard_iface != NULL && i < keyboard_iface->event_count; i++) {
        if (strcmp(keyboard_iface->events[i].name, "modifiers") == 0) {
            modifiers_index = i;
            break;
        }
    }

    if (debug)
        fprintf(debug, "wl-numlock-fix[%d]: loaded, mask=0x%x\n", (int)getpid(), fix_mask);
}

/* The wrap whose fns array is LISTENER, if any. */
static struct wrap *
find_wrap(const void *listener)
{
    struct wrap *found = NULL;
    pthread_mutex_lock(&lock);
    for (struct wrap *w = wraps; w != NULL; w = w->next) {
        if ((const void *)w->fns == listener) {
            found = w;
            break;
        }
    }
    pthread_mutex_unlock(&lock);
    return found;
}

/* An event's "since" version: the signature's leading digits, or 1. */
static uint32_t
event_since(const struct wl_message *msg)
{
    uint32_t since = 0;
    for (const char *s = msg->signature; isdigit((unsigned char)*s); s++)
        since = since * 10 + (uint32_t)(*s - '0');
    return since == 0 ? 1 : since;
}

static void
fixed_modifiers(void *data, struct wl_keyboard *kbd, uint32_t serial,
                uint32_t depressed, uint32_t latched, uint32_t locked,
                uint32_t group)
{
    struct wrap *w = find_wrap(real_get_listener((struct wl_proxy *)kbd));
    if (w == NULL || w->orig_modifiers == NULL)
        return;

    uint32_t move = depressed & fix_mask;
    if (move != 0) {
        if (debug)
            fprintf(debug, "wl-numlock-fix[%d]: depressed 0x%x -> 0x%x, locked 0x%x -> 0x%x\n",
                    (int)getpid(), depressed, depressed & ~move, locked, locked | move);
        depressed &= ~move;
        locked |= move;
    }
    w->orig_modifiers(data, kbd, serial, depressed, latched, locked, group);
}

int
wl_proxy_add_listener(struct wl_proxy *proxy, fn_t *implementation, void *data)
{
    pthread_once(&once, init);
    if (real_add_listener == NULL) {
        fprintf(stderr, "wl-numlock-fix: cannot find libwayland-client\n");
        return -1;
    }

    if (modifiers_index < 0 || implementation == NULL ||
        strcmp(real_get_class(proxy), "wl_keyboard") != 0)
        return real_add_listener(proxy, implementation, data);

    uint32_t version = real_get_version(proxy);
    if (version == 0)
        version = 1; /* wl_display-style proxies report 0 */

    const struct wl_message *ev = &keyboard_iface->events[modifiers_index];
    if (event_since(ev) > version || implementation[modifiers_index] == NULL)
        return real_add_listener(proxy, implementation, data);

    int n = keyboard_iface->event_count;
    struct wrap *w = calloc(1, sizeof(*w) + (size_t)n * sizeof(fn_t));
    if (w == NULL)
        return real_add_listener(proxy, implementation, data);

    /*
     * Copy only the handlers for events of the version the client bound.
     * libwayland never dispatches later ones, and a client built against
     * older headers has no slots for them.
     */
    for (int i = 0; i < n; i++) {
        if (event_since(&keyboard_iface->events[i]) <= version)
            w->fns[i] = implementation[i];
    }
    w->orig_listener = implementation;
    w->orig_modifiers = (modifiers_fn)implementation[modifiers_index];
    w->fns[modifiers_index] = (fn_t)fixed_modifiers;

    int ret = real_add_listener(proxy, w->fns, data);
    if (ret != 0) {
        free(w); /* a listener was already set; libwayland refused ours */
        return ret;
    }

    pthread_mutex_lock(&lock);
    w->next = wraps;
    wraps = w;
    pthread_mutex_unlock(&lock);

    if (debug)
        fprintf(debug, "wl-numlock-fix[%d]: wrapped wl_keyboard@%u (v%u)\n",
                (int)getpid(), real_get_id(proxy), version);
    return 0;
}

/* Clients that compare wl_proxy_get_listener() against their own listener
 * must still see their own pointer. */
const void *
wl_proxy_get_listener(struct wl_proxy *proxy)
{
    pthread_once(&once, init);
    if (real_get_listener == NULL)
        return NULL;
    const void *l = real_get_listener(proxy);
    struct wrap *w = find_wrap(l);
    return w != NULL ? w->orig_listener : l;
}
