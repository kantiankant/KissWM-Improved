#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/XF86keysym.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"
#include "ipc.h"
#include "parser.h"

#define PROTO_DELETE_WINDOW (1 << 0)
#define PROTO_TAKE_FOCUS    (1 << 1)

#define CLEANMASK(mask) ((mask) & ~(LockMask|Mod2Mask|Mod3Mask) & (ShiftMask|ControlMask|Mod1Mask|Mod4Mask|Mod5Mask))

typedef struct { int x, y, w, h; } Rect;

typedef struct Node {
    int          is_leaf;
    Window       win;
    int          global;
    int          fullscreen;
    int          floating;
    int          fx, fy;
    int          fw, fh;
    int          split_h;
    double       split_ratio;
    Rect         rect;
    struct Node *left;
    struct Node *right;
    struct Node *parent;
    int          protocols;
} Node;

#define MAX_SLOTS  256
#define MAX_TAGS   32

static Display  *dpy;
static Window    root;
static int       screen;
static int       sw, sh;
static int       running = 1;

static Node    **tag_tree   = NULL;
static int       ntags      = 9;
static int       cur_tag    = 0;

static Window    focused    = None;
static Time      last_event_time = CurrentTime;
static int       focus_lock = 0;
static int       ignore_unmap = 0;

static int       drag_active   = 0;
static Window    drag_win      = None;
static int       drag_start_x, drag_start_y;
static int       drag_win_x,   drag_win_y;

static int       resize_active = 0;
static Window    resize_win    = None;
static int       resize_start_x, resize_start_y;
static int       resize_orig_w,  resize_orig_h;

static Config   *cfg        = NULL;
static char      cfg_path[512];

static int ipc_fd   = -1;
static char ipc_path[IPC_SOCK_MAX];

static Atom net_atoms[16];
enum {
    NET_SUPPORTED,
    NET_WM_NAME,
    NET_WM_STATE,
    NET_WM_STATE_FULLSCREEN,
    NET_WM_STATE_DEMANDS_ATTENTION,
    NET_ACTIVE_WINDOW,
    NET_CURRENT_DESKTOP,
    NET_NUMBER_OF_DESKTOPS,
    NET_DESKTOP_NAMES,
    NET_DESKTOP_VIEWPORT,
    NET_WM_DESKTOP,
    NET_CLIENT_LIST,
    NET_CLOSE_WINDOW,
    NET_ATOM_COUNT
};

static Atom wm_protocols;
static Atom wm_take_focus;
static Atom wm_delete_window;

static int on_dir_side(Rect r1, Rect r2, int dir)
{
    int r1_rx = r1.x + r1.w - 1, r1_by = r1.y + r1.h - 1;
    int r2_rx = r2.x + r2.w - 1, r2_by = r2.y + r2.h - 1;

    switch (dir) {
    case 0: if (r2_rx >= r1.x)  return 0; break;
    case 1: if (r2.x  <= r1_rx) return 0; break;
    case 2: if (r2_by >= r1.y)  return 0; break;
    case 3: if (r2.y  <= r1_by) return 0; break;
    default: return 0;
    }

    switch (dir) {
    case 0: case 1:
        return (r2.y  >= r1.y && r2.y  <= r1_by) ||
               (r2_by >= r1.y && r2_by <= r1_by) ||
               (r1.y  >  r2.y && r1.y  <  r2_by);
    case 2: case 3:
        return (r2.x  >= r1.x && r2.x  <= r1_rx) ||
               (r2_rx >= r1.x && r2_rx <= r1_rx) ||
               (r1.x  >  r2.x && r1_rx <  r2_rx);
    }
    return 0;
}

static uint32_t boundary_dist(Rect r1, Rect r2, int dir)
{
    int r1_rx = r1.x + r1.w - 1, r1_by = r1.y + r1.h - 1;
    int r2_rx = r2.x + r2.w - 1, r2_by = r2.y + r2.h - 1;
    int d;
    switch (dir) {
    case 0: d = r1.x  - r2_rx; break;
    case 1: d = r2.x  - r1_rx; break;
    case 2: d = r1.y  - r2_by; break;
    case 3: d = r2.y  - r1_by; break;
    default: return UINT32_MAX;
    }
    return d < 0 ? 0 : (uint32_t)d;
}

static void spawn(const char *cmd)
{
    if (fork() == 0) {
        setsid();
        close(ConnectionNumber(dpy));
        execl("/bin/sh", "sh", "-c", cmd, NULL);
        exit(0);
    }
}

static void sigchld_handler(int sig)
{
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

static void setup_sigchld(void)
{
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
}

static void *safe_calloc(size_t n, size_t size)
{
    void *p = calloc(n, size);
    if (!p) { fprintf(stderr, "kisswm: fatal: out of memory\n"); exit(1); }
    return p;
}

static Node *node_new_leaf(Window w, Node *parent)
{
    Node *n    = safe_calloc(1, sizeof(Node));
    n->is_leaf = 1;
    n->win     = w;
    n->parent  = parent;
    return n;
}

static Node *node_find(Node *n, Window w)
{
    if (!n) return NULL;
    if (n->is_leaf && n->win == w) return n;
    Node *r = node_find(n->left, w);
    return r ? r : node_find(n->right, w);
}

static Node *node_find_any_tag(Window w)
{
    for (int i = 0; i < ntags; i++) {
        Node *nd = node_find(tag_tree[i], w);
        if (nd) return nd;
    }
    return NULL;
}

static Node *node_first_leaf(Node *n)
{
    if (!n) return NULL;
    if (n->is_leaf) return n;
    return node_first_leaf(n->left);
}

static Node *node_first_tiled_leaf(Node *n)
{
    if (!n) return NULL;
    if (n->is_leaf) return (!n->floating) ? n : NULL;
    Node *r = node_first_tiled_leaf(n->left);
    return r ? r : node_first_tiled_leaf(n->right);
}

static void node_free_tree(Node *n)
{
    if (!n) return;
    node_free_tree(n->left);
    node_free_tree(n->right);
    free(n);
}

static int node_depth(Node *n)
{
    int d = 0;
    while (n->parent) { n = n->parent; d++; }
    return d;
}

static int cache_protocols(Window w)
{
    int   flags = 0;
    Atom *protocols;
    int   n;
    if (XGetWMProtocols(dpy, w, &protocols, &n)) {
        for (int i = 0; i < n; i++) {
            if (protocols[i] == wm_delete_window) flags |= PROTO_DELETE_WINDOW;
            if (protocols[i] == wm_take_focus)    flags |= PROTO_TAKE_FOCUS;
        }
        XFree(protocols);
    }
    return flags;
}

static int count_tiled(Node *n)
{
    if (!n) return 0;
    if (n->is_leaf) return n->floating ? 0 : 1;
    return count_tiled(n->left) + count_tiled(n->right);
}

static void apply_layout(Node *n, int x, int y, int w, int h)
{
    if (!n) return;
    n->rect = (Rect){ x, y, w, h };

    if (n->is_leaf) return;

    int lt = count_tiled(n->left), rt = count_tiled(n->right);

    if (lt == 0) { apply_layout(n->right, x, y, w, h); return; }
    if (rt == 0) { apply_layout(n->left,  x, y, w, h); return; }

    double ratio = (n->split_ratio > 0.05 && n->split_ratio < 0.95)
                   ? n->split_ratio : 0.5;

    if (n->split_h) {
        int th = (int)((h - GAP) * ratio);
        int bh = h - th - GAP;
        if (th < 1) th = 1;
        if (bh < 1) bh = 1;
        apply_layout(n->left,  x, y,            w, th);
        apply_layout(n->right, x, y + th + GAP, w, bh);
    } else {
        int lw = (int)((w - GAP) * ratio);
        int rw = w - lw - GAP;
        if (lw < 1) lw = 1;
        if (rw < 1) rw = 1;
        apply_layout(n->left,  x,            y, lw, h);
        apply_layout(n->right, x + lw + GAP, y, rw, h);
    }
}

typedef struct { Node *node; int x, y, w, h; } Slot;
static Slot slots[MAX_SLOTS];
static int  nslots;

static void collect_slots(Node *n)
{
    if (!n) return;
    if (n->is_leaf) {
        if (!n->floating && nslots < MAX_SLOTS)
            slots[nslots++] = (Slot){ n, n->rect.x, n->rect.y,
                                         n->rect.w, n->rect.h };
        return;
    }
    collect_slots(n->left);
    collect_slots(n->right);
}

static void collect_slots_all_tags(void)
{
    nslots = 0;
    for (int i = 0; i < ntags; i++) {
        apply_layout(tag_tree[i], GAP, BAR_HEIGHT + GAP,
                     sw - GAP * 2, sh - BAR_HEIGHT - GAP * 2);
        collect_slots(tag_tree[i]);
    }
}

static void render_from_rects(Node *n)
{
    if (!n) return;
    if (n->is_leaf) {
        if (n->fullscreen) {
            XMoveResizeWindow(dpy, n->win, 0, 0, (unsigned)sw, (unsigned)sh);
            XSetWindowBorderWidth(dpy, n->win, 0);
            XRaiseWindow(dpy, n->win);
            return;
        }
        if (n->floating) return;
        if (n->rect.w < 1 || n->rect.h < 1) return;
        XMoveResizeWindow(dpy, n->win,
                          n->rect.x, n->rect.y,
                          (unsigned)n->rect.w, (unsigned)n->rect.h);
        XSetWindowBorderWidth(dpy, n->win, BORDER_WIDTH);
        return;
    }
    render_from_rects(n->left);
    render_from_rects(n->right);
}

static void render_floats(Node *n)
{
    if (!n) return;
    if (n->is_leaf) {
        if (n->floating && !n->fullscreen) {
            if (n->fw < 1 || n->fh < 1) return;
            XMoveResizeWindow(dpy, n->win, n->fx, n->fy,
                              (unsigned)n->fw, (unsigned)n->fh);
            XSetWindowBorderWidth(dpy, n->win, BORDER_WIDTH);
            XRaiseWindow(dpy, n->win);
        }
        if (n->global || n->fullscreen)
            XRaiseWindow(dpy, n->win);
        return;
    }
    render_floats(n->left);
    render_floats(n->right);
}

static void notify_leaves(Node *n)
{
    if (!n) return;
    if (n->is_leaf) {
        XWindowAttributes xa;
        if (!XGetWindowAttributes(dpy, n->win, &xa)) return;
        XEvent ce;
        memset(&ce, 0, sizeof(ce));
        ce.type                    = ConfigureNotify;
        ce.xconfigure.display      = dpy;
        ce.xconfigure.event        = n->win;
        ce.xconfigure.window       = n->win;
        ce.xconfigure.x            = xa.x;
        ce.xconfigure.y            = xa.y;
        ce.xconfigure.width        = xa.width;
        ce.xconfigure.height       = xa.height;
        ce.xconfigure.border_width = n->fullscreen ? 0 : BORDER_WIDTH;
        ce.xconfigure.above        = None;
        ce.xconfigure.override_redirect = False;
        XSendEvent(dpy, n->win, False, StructureNotifyMask, &ce);
        return;
    }
    notify_leaves(n->left);
    notify_leaves(n->right);
}

static void render_all(void)
{
    apply_layout(tag_tree[cur_tag], GAP, BAR_HEIGHT + GAP,
                 sw - GAP * 2, sh - BAR_HEIGHT - GAP * 2);

    render_from_rects(tag_tree[cur_tag]);

    render_floats(tag_tree[cur_tag]);

    XSync(dpy, False);

    notify_leaves(tag_tree[cur_tag]);
}

static void ewmh_init(void)
{
    wm_protocols     = XInternAtom(dpy, "WM_PROTOCOLS",     False);
    wm_take_focus    = XInternAtom(dpy, "WM_TAKE_FOCUS",    False);
    wm_delete_window = XInternAtom(dpy, "WM_DELETE_WINDOW", False);

    net_atoms[NET_SUPPORTED]           = XInternAtom(dpy, "_NET_SUPPORTED",                  False);
    net_atoms[NET_WM_NAME]             = XInternAtom(dpy, "_NET_WM_NAME",                    False);
    net_atoms[NET_WM_STATE]            = XInternAtom(dpy, "_NET_WM_STATE",                   False);
    net_atoms[NET_WM_STATE_FULLSCREEN] = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN",        False);
    net_atoms[NET_WM_STATE_DEMANDS_ATTENTION]
                                       = XInternAtom(dpy, "_NET_WM_STATE_DEMANDS_ATTENTION", False);
    net_atoms[NET_ACTIVE_WINDOW]       = XInternAtom(dpy, "_NET_ACTIVE_WINDOW",              False);
    net_atoms[NET_CURRENT_DESKTOP]     = XInternAtom(dpy, "_NET_CURRENT_DESKTOP",            False);
    net_atoms[NET_NUMBER_OF_DESKTOPS]  = XInternAtom(dpy, "_NET_NUMBER_OF_DESKTOPS",         False);
    net_atoms[NET_DESKTOP_NAMES]       = XInternAtom(dpy, "_NET_DESKTOP_NAMES",              False);
    net_atoms[NET_DESKTOP_VIEWPORT]    = XInternAtom(dpy, "_NET_DESKTOP_VIEWPORT",           False);
    net_atoms[NET_WM_DESKTOP]          = XInternAtom(dpy, "_NET_WM_DESKTOP",                 False);
    net_atoms[NET_CLIENT_LIST]         = XInternAtom(dpy, "_NET_CLIENT_LIST",                False);
    net_atoms[NET_CLOSE_WINDOW]        = XInternAtom(dpy, "_NET_CLOSE_WINDOW",               False);

    XChangeProperty(dpy, root, net_atoms[NET_SUPPORTED], XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)net_atoms, NET_ATOM_COUNT);

    Atom check = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
    XChangeProperty(dpy, root, check, XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)&root, 1);
    XChangeProperty(dpy, root, net_atoms[NET_WM_NAME],
                    XInternAtom(dpy, "UTF8_STRING", False), 8,
                    PropModeReplace, (unsigned char *)"kisswm", 6);
}

static void ewmh_update_tags(void)
{
    long n = ntags;
    XChangeProperty(dpy, root, net_atoms[NET_NUMBER_OF_DESKTOPS], XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&n, 1);

    char names[MAX_TAGS * 4];
    int  off = 0;
    for (int i = 0; i < ntags && off < (int)sizeof(names) - 2; i++)
        off += snprintf(names + off, sizeof(names) - (size_t)off, "%d%c", i + 1, '\0');
    XChangeProperty(dpy, root, net_atoms[NET_DESKTOP_NAMES],
                    XInternAtom(dpy, "UTF8_STRING", False), 8,
                    PropModeReplace, (unsigned char *)names, off);

    long viewport[MAX_TAGS * 2];
    memset(viewport, 0, sizeof(long) * (size_t)(ntags * 2));
    XChangeProperty(dpy, root, net_atoms[NET_DESKTOP_VIEWPORT], XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)viewport, ntags * 2);
}

static void ewmh_update_current_tag(void)
{
    long d = cur_tag;
    XChangeProperty(dpy, root, net_atoms[NET_CURRENT_DESKTOP], XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&d, 1);
}

static void ewmh_update_active_window(void)
{
    XChangeProperty(dpy, root, net_atoms[NET_ACTIVE_WINDOW], XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)&focused, 1);
}

static void ewmh_update_client_list(void)
{
    Window wins[MAX_SLOTS * MAX_TAGS];
    int    nwins = 0;
    collect_slots_all_tags();
    for (int i = 0; i < nslots && nwins < (int)(sizeof(wins)/sizeof(wins[0])); i++)
        wins[nwins++] = slots[i].node->win;
    XChangeProperty(dpy, root, net_atoms[NET_CLIENT_LIST], XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)wins, nwins);
}

static void ewmh_set_window_tag(Window w, int tag, int is_global)
{
    long desk = is_global ? (long)0xFFFFFFFF : (long)tag;
    XChangeProperty(dpy, w, net_atoms[NET_WM_DESKTOP], XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&desk, 1);
}

static Slot *slot_for_window(Window w);

static void focus_window(Window w, int warp)
{
    if (focused != None && focused != w) {
        XEvent fe;
        memset(&fe, 0, sizeof(fe));
        fe.type          = FocusOut;
        fe.xfocus.window = focused;
        fe.xfocus.mode   = NotifyNormal;
        fe.xfocus.detail = NotifyNonlinear;
        XSendEvent(dpy, focused, False, FocusChangeMask, &fe);
    }
    focused = w;
    if (w != None) {
        XRaiseWindow(dpy, w);
        XSetInputFocus(dpy, w, RevertToPointerRoot, last_event_time);
        XEvent fe;
        memset(&fe, 0, sizeof(fe));
        fe.type          = FocusIn;
        fe.xfocus.window = w;
        fe.xfocus.mode   = NotifyNormal;
        fe.xfocus.detail = NotifyNonlinear;
        XSendEvent(dpy, w, False, FocusChangeMask, &fe);
        Node *nd = node_find_any_tag(w);
        if (nd && (nd->protocols & PROTO_TAKE_FOCUS)) {
            XEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type                 = ClientMessage;
            ev.xclient.window       = w;
            ev.xclient.message_type = wm_protocols;
            ev.xclient.format       = 32;
            ev.xclient.data.l[0]    = wm_take_focus;
            ev.xclient.data.l[1]    = last_event_time;
            XSendEvent(dpy, w, False, NoEventMask, &ev);
        }
        if (warp)
            focus_lock = 1;

        if (warp) {
            Slot *s = slot_for_window(w);
            if (s)
                XWarpPointer(dpy, None, root, 0, 0, 0, 0,
                             s->x + s->w / 2,
                             s->y + s->h / 2);
        }
    }
    ewmh_update_active_window();
}

static void add_window(Window w)
{
    if (w == root) return;
    XWindowAttributes xa;
    if (!XGetWindowAttributes(dpy, w, &xa)) return;
    if (xa.override_redirect) return;
    if (xa.class == InputOnly) return;
    for (int i = 0; i < ntags; i++)
        if (node_find(tag_tree[i], w)) return;

    XMapWindow(dpy, w);
    XSelectInput(dpy, w, EnterWindowMask | FocusChangeMask | PropertyChangeMask);
    XSetWindowBorderWidth(dpy, w, BORDER_WIDTH);

    int protos = cache_protocols(w);

    if (!tag_tree[cur_tag]) {
        tag_tree[cur_tag] = node_new_leaf(w, NULL);
        tag_tree[cur_tag]->protocols = protos;
    } else {
        Node *focused_node = focused != None
            ? node_find(tag_tree[cur_tag], focused)
            : NULL;
        Node *target = (focused_node && !focused_node->floating)
            ? focused_node
            : node_first_tiled_leaf(tag_tree[cur_tag]);
        if (!target)
            target = node_first_tiled_leaf(tag_tree[cur_tag]);

        if (!target) {
            Node *new_win      = node_new_leaf(w, NULL);
            new_win->protocols = protos;
            Node *old_root     = tag_tree[cur_tag];
            Node *split        = safe_calloc(1, sizeof(Node));
            split->split_h     = 0;
            split->split_ratio = 0.5;
            split->left        = old_root;
            split->right       = new_win;
            old_root->parent   = split;
            new_win->parent    = split;
            tag_tree[cur_tag]  = split;
        } else {
            int depth = node_depth(target);

            Node *old_leaf      = node_new_leaf(target->win, target->parent);
            old_leaf->protocols = target->protocols;
            old_leaf->global    = target->global;

            Node *new_win       = node_new_leaf(w, target);
            new_win->protocols  = protos;

            apply_layout(tag_tree[cur_tag], GAP, BAR_HEIGHT + GAP,
                         sw - GAP * 2, sh - BAR_HEIGHT - GAP * 2);

            int split_h;
            if (target->rect.w > 0 && target->rect.h > 0) {
                if      (target->rect.h > target->rect.w * 2) split_h = 1;
                else if (target->rect.w > target->rect.h * 2) split_h = 0;
                else    split_h = (depth % 2 == 1) ? 1 : 0;
            } else {
                split_h = (depth % 2 == 1) ? 1 : 0;
            }

            target->is_leaf     = 0;
            target->win         = None;
            target->split_h     = split_h;
            target->split_ratio = 0.5;
            target->left        = old_leaf;
            target->right       = new_win;
            old_leaf->parent    = target;
            new_win->parent     = target;
        }
    }

    ewmh_set_window_tag(w, cur_tag, 0);
    render_all();
    focus_window(w, 1);
    ewmh_update_client_list();
}

static void remove_window(Window w)
{
    Node *leaf = NULL;
    int   ws   = cur_tag;
    for (int i = 0; i < ntags; i++) {
        leaf = node_find(tag_tree[i], w);
        if (leaf) { ws = i; break; }
    }
    if (!leaf) return;

    Node *par = leaf->parent;
    if (!par) {
        free(leaf);
        tag_tree[ws] = NULL;
        if (ws == cur_tag) { focused = None; ewmh_update_active_window(); }
        ewmh_update_client_list();
        return;
    }

    Node *sib = (par->left == leaf) ? par->right : par->left;
    if (!sib) {
        fprintf(stderr, "kisswm: tree corruption in remove_window\n");
        free(leaf); free(par); tag_tree[ws] = NULL;
        ewmh_update_client_list();
        return;
    }

    Node *gp    = par->parent;
    sib->parent = gp;
    if (!gp) tag_tree[ws] = sib;
    else { if (gp->left == par) gp->left = sib; else gp->right = sib; }
    free(leaf); free(par);

    if (ws == cur_tag) {
        Node *next = node_first_leaf(tag_tree[cur_tag]);
        focus_window(next ? next->win : None, 1);
        render_all();
    }
    ewmh_update_client_list();
}

static void tag_unmap_leaves(Node *n)
{
    if (!n) return;
    if (n->is_leaf) {
        if (!n->global) { ignore_unmap++; XUnmapWindow(dpy, n->win); }
        return;
    }
    tag_unmap_leaves(n->left);
    tag_unmap_leaves(n->right);
}

static void tag_map_leaves(Node *n)
{
    if (!n) return;
    if (n->is_leaf) {
        if (!n->global) XMapWindow(dpy, n->win);
        return;
    }
    tag_map_leaves(n->left);
    tag_map_leaves(n->right);
}

static void tag_show(int tag)
{
    if (tag < 0 || tag >= ntags || tag == cur_tag) return;

    tag_unmap_leaves(tag_tree[cur_tag]);
    cur_tag = tag;
    tag_map_leaves(tag_tree[cur_tag]);

    Node *first = node_first_leaf(tag_tree[cur_tag]);
    focus_window(first ? first->win : None, 1);
    render_all();
    ewmh_update_current_tag();
    ewmh_update_client_list();
}

static void window_to_tag(Window w, int tag)
{
    if (tag < 0 || tag >= ntags || tag == cur_tag) return;
    Node *leaf = node_find(tag_tree[cur_tag], w);
    if (!leaf || leaf->global) return;

    Node *par = leaf->parent;
    if (par) {
        Node *sib = (par->left == leaf) ? par->right : par->left;
        Node *gp  = par->parent;
        sib->parent = gp;
        if (!gp) tag_tree[cur_tag] = sib;
        else { if (gp->left == par) gp->left = sib; else gp->right = sib; }
        free(par);
    } else {
        tag_tree[cur_tag] = NULL;
    }

    ignore_unmap++;
    XUnmapWindow(dpy, w);
    leaf->parent = NULL;

    if (!tag_tree[tag]) {
        tag_tree[tag] = leaf;
    } else {
        Node *first = node_first_leaf(tag_tree[tag]);
        if (first) {
            Node *split        = safe_calloc(1, sizeof(Node));
            split->split_h     = 0;
            split->split_ratio = 0.5;
            split->left        = first;
            split->right       = leaf;
            split->parent      = first->parent;
            if (first->parent) {
                if (first->parent->left == first) first->parent->left = split;
                else first->parent->right = split;
            } else tag_tree[tag] = split;
            first->parent = split;
            leaf->parent  = split;
        }
    }

    ewmh_set_window_tag(w, tag, 0);
    Node *next = node_first_leaf(tag_tree[cur_tag]);
    focus_window(next ? next->win : None, 1);
    render_all();
    ewmh_update_client_list();
}

static void window_toggle_global(Window w)
{
    Node *leaf = node_find_any_tag(w);
    if (!leaf) return;
    leaf->global = !leaf->global;
    ewmh_set_window_tag(w, cur_tag, leaf->global);
    render_all();
}

static Slot *slot_for_window(Window w)
{
    apply_layout(tag_tree[cur_tag], GAP, BAR_HEIGHT + GAP,
                 sw - GAP * 2, sh - BAR_HEIGHT - GAP * 2);
    nslots = 0;
    collect_slots(tag_tree[cur_tag]);
    for (int i = 0; i < nslots; i++)
        if (slots[i].node->win == w) return &slots[i];
    return NULL;
}

static Slot *best_neighbour(Slot *src, int dir)
{
    Rect     rsrc = { src->x, src->y, src->w, src->h };
    Slot    *best = NULL;
    uint32_t best_d = UINT32_MAX;

    for (int i = 0; i < nslots; i++) {
        Slot *s = &slots[i];
        if (s->node->win == src->node->win) continue;
        Rect r = { s->x, s->y, s->w, s->h };
        if (!on_dir_side(rsrc, r, dir)) continue;
        uint32_t d = boundary_dist(rsrc, r, dir);
        if (d < best_d) { best_d = d; best = s; }
    }
    return best;
}

static void focus_direction(int dir)
{
    if (focused == None) return;
    Slot *src = slot_for_window(focused);
    if (!src) return;
    Slot *dst = best_neighbour(src, dir);
    if (dst) focus_window(dst->node->win, 1);
}

static void swap_direction(int dir)
{
    if (focused == None) return;
    Slot *src = slot_for_window(focused);
    if (!src) return;
    Slot *dst = best_neighbour(src, dir);
    if (!dst) return;

    Window tmp     = src->node->win;
    src->node->win = dst->node->win;
    dst->node->win = tmp;

    focused = dst->node->win;
    render_all();
    focus_window(focused, 1);
}

static void resize_ratio(Window w, int dir, double delta)
{
    Node *leaf = node_find(tag_tree[cur_tag], w);
    if (!leaf) return;

    Node *n = leaf->parent;
    while (n) {
        int want_h = (dir == 2 || dir == 3);
        if (n->split_h == want_h) {
            double d = (n->left == leaf || node_find(n->left, w))
                       ? delta : -delta;
            if (dir == 1 || dir == 3) d = -d;
            n->split_ratio += d;
            if (n->split_ratio < 0.05) n->split_ratio = 0.05;
            if (n->split_ratio > 0.95) n->split_ratio = 0.95;
            render_all();
            return;
        }
        n = n->parent;
    }
}

void do_kill(void)
{
    if (focused == None) return;
    Node *nd = node_find_any_tag(focused);
    if (nd && (nd->protocols & PROTO_DELETE_WINDOW)) {
        XEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type                 = ClientMessage;
        ev.xclient.window       = focused;
        ev.xclient.message_type = wm_protocols;
        ev.xclient.format       = 32;
        ev.xclient.data.l[0]    = wm_delete_window;
        ev.xclient.data.l[1]    = last_event_time;
        XSendEvent(dpy, focused, False, NoEventMask, &ev);
    } else {
        XDestroyWindow(dpy, focused);
    }
}

void do_float(void)
{
    if (focused == None) return;
    Node *leaf = node_find(tag_tree[cur_tag], focused);
    if (!leaf) return;

    if (!leaf->floating) {
        Slot *s = slot_for_window(focused);
        if (s) {
            leaf->fx = s->x; leaf->fy = s->y;
            leaf->fw = s->w; leaf->fh = s->h;
        } else {
            XWindowAttributes xa;
            if (XGetWindowAttributes(dpy, focused, &xa)) {
                leaf->fx = xa.x; leaf->fy = xa.y;
                leaf->fw = xa.width  ? xa.width  : sw / 2;
                leaf->fh = xa.height ? xa.height : (sh - BAR_HEIGHT) / 2;
            }
        }
        leaf->floating = 1;
        render_all();
        focus_window(focused, 1);
        return;
    }

    int fcx = leaf->fx + leaf->fw / 2;
    int fcy = leaf->fy + leaf->fh / 2;

    leaf->floating = 0;

    Node *old_par = leaf->parent;
    if (old_par) {
        Node *sib = (old_par->left == leaf) ? old_par->right : old_par->left;
        Node *gp  = old_par->parent;
        if (sib) {
            sib->parent = gp;
            if (!gp) tag_tree[cur_tag] = sib;
            else { if (gp->left == old_par) gp->left = sib; else gp->right = sib; }
        } else {
            if (!gp) tag_tree[cur_tag] = NULL;
        }
        free(old_par);
    } else {
        tag_tree[cur_tag] = NULL;
    }
    leaf->parent = NULL;

    apply_layout(tag_tree[cur_tag], GAP, BAR_HEIGHT + GAP,
                 sw - GAP * 2, sh - BAR_HEIGHT - GAP * 2);
    nslots = 0;
    collect_slots(tag_tree[cur_tag]);

    Slot *nearest = NULL;
    int   best    = INT_MAX;
    for (int i = 0; i < nslots; i++) {
        int dx = (slots[i].x + slots[i].w / 2) - fcx;
        int dy = (slots[i].y + slots[i].h / 2) - fcy;
        int d  = dx * dx + dy * dy;
        if (d < best) { best = d; nearest = &slots[i]; }
    }

    if (!nearest) {
        tag_tree[cur_tag] = leaf;
        leaf->fx = leaf->fy = leaf->fw = leaf->fh = 0;
        render_all();
        focus_window(focused, 1);
        return;
    }

    Node *target       = nearest->node;
    int   depth        = node_depth(target);
    Node *old_leaf     = node_new_leaf(target->win, target->parent);
    old_leaf->protocols  = target->protocols;
    old_leaf->global     = target->global;
    old_leaf->fullscreen = target->fullscreen;

    target->is_leaf     = 0;
    target->win         = None;
    target->split_h     = (depth % 2 == 1) ? 1 : 0;
    target->split_ratio = 0.5;
    target->left        = old_leaf;
    target->right       = leaf;
    old_leaf->parent    = target;
    leaf->parent        = target;

    leaf->fx = leaf->fy = leaf->fw = leaf->fh = 0;

    render_all();
    focus_window(focused, 1);
}

void do_fullscreen(void)
{
    if (focused == None) return;
    Node *leaf = node_find(tag_tree[cur_tag], focused);
    if (!leaf) return;
    leaf->fullscreen = !leaf->fullscreen;
    if (leaf->fullscreen) {
        XChangeProperty(dpy, focused, net_atoms[NET_WM_STATE], XA_ATOM, 32,
                        PropModeReplace,
                        (unsigned char *)&net_atoms[NET_WM_STATE_FULLSCREEN], 1);
    } else {
        XDeleteProperty(dpy, focused, net_atoms[NET_WM_STATE]);
    }
    render_all();
    focus_window(focused, 1);
}

void do_quit(void)        { running = 0; }
void do_focus_left(void)  { focus_direction(0); }
void do_focus_right(void) { focus_direction(1); }
void do_focus_up(void)    { focus_direction(2); }
void do_focus_down(void)  { focus_direction(3); }
void do_swap_left(void)   { swap_direction(0); }
void do_swap_right(void)  { swap_direction(1); }
void do_swap_up(void)     { swap_direction(2); }
void do_swap_down(void)   { swap_direction(3); }
void do_cycle_all(void)   { }
void do_cycle_tag(void)   { }
void do_global(void)      { if (focused != None) window_toggle_global(focused); }

static void grab_key_with_locks(KeyCode kc, unsigned int mod)
{
    static const unsigned int locks[] = { 0, LockMask, Mod2Mask, Mod3Mask,
                                          LockMask|Mod2Mask, LockMask|Mod3Mask,
                                          Mod2Mask|Mod3Mask, LockMask|Mod2Mask|Mod3Mask };
    for (int i = 0; i < 8; i++)
        XGrabKey(dpy, kc, mod | locks[i], root, True, GrabModeAsync, GrabModeAsync);
}

static void grab_keys(void)
{
    if (!cfg) return;
    XUngrabKey(dpy, AnyKey, AnyModifier, root);

    for (int i = 0; i < cfg->nbinds; i++) {
        KeyCode kc = XKeysymToKeycode(dpy, cfg->binds[i].key);
        if (kc) grab_key_with_locks(kc, cfg->binds[i].mod);
    }
    for (int i = 0; i < cfg->ntagbinds; i++) {
        KeyCode kc = XKeysymToKeycode(dpy, cfg->tagbinds[i].key);
        if (kc) grab_key_with_locks(kc, cfg->tagbinds[i].mod);
    }
}

static void grab_buttons(Window w)
{
    XGrabButton(dpy, Button1, Mod4Mask, w, False,
                ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                GrabModeAsync, GrabModeAsync, None, None);
    XGrabButton(dpy, Button3, Mod4Mask, w, False,
                ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                GrabModeAsync, GrabModeAsync, None, None);
}

static void handle_keypress(XKeyEvent *e)
{
    KeySym ks = XLookupKeysym(e, 0);

    if (cfg) {
        for (int i = 0; i < cfg->ntagbinds; i++) {
            TagBind *tb = &cfg->tagbinds[i];
            if (ks == tb->key && CLEANMASK(e->state) == CLEANMASK(tb->mod)) {
                if (tb->tag >= ntags) return;
                if (tb->move) window_to_tag(focused, tb->tag);
                else          tag_show(tb->tag);
                return;
            }
        }
        for (int i = 0; i < cfg->nbinds; i++) {
            Keybind *b = &cfg->binds[i];
            if (ks == b->key && CLEANMASK(e->state) == CLEANMASK(b->mod)) {
                if (b->fn)       b->fn();
                else if (b->cmd) spawn(b->cmd);
                return;
            }
        }
    }
}

static void handle_button_press(XButtonEvent *e)
{
    if (e->window == root) return;
    focus_window(e->window, 0);

    Node *leaf = node_find(tag_tree[cur_tag], e->window);
    if (!leaf || !leaf->floating) return;

    if (e->button == Button1 && (e->state & Mod4Mask)) {
        drag_active  = 1;
        drag_win     = e->window;
        drag_start_x = e->x_root;
        drag_start_y = e->y_root;
        drag_win_x   = leaf->fx;
        drag_win_y   = leaf->fy;
        XRaiseWindow(dpy, e->window);
    } else if (e->button == Button3 && (e->state & Mod4Mask)) {
        resize_active  = 1;
        resize_win     = e->window;
        resize_start_x = e->x_root;
        resize_start_y = e->y_root;
        resize_orig_w  = leaf->fw;
        resize_orig_h  = leaf->fh;
        XRaiseWindow(dpy, e->window);
    }
}

static void handle_button_release(XButtonEvent *e)
{
    (void)e;
    drag_active   = 0; drag_win   = None;
    resize_active = 0; resize_win = None;
}

static void reload_config(void);

static void ipc_setup(void)
{
    const char *disp = getenv("DISPLAY");
    ipc_socket_path(ipc_path, disp ? disp : ":0");
    unlink(ipc_path);

    ipc_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ipc_fd < 0) { perror("kisswm: ipc socket"); return; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", ipc_path);

    if (bind(ipc_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("kisswm: ipc bind"); close(ipc_fd); ipc_fd = -1; return;
    }
    if (listen(ipc_fd, 8) < 0) {
        perror("kisswm: ipc listen"); close(ipc_fd); ipc_fd = -1; return;
    }
}

static void ipc_teardown(void)
{
    if (ipc_fd >= 0) { close(ipc_fd); ipc_fd = -1; }
    unlink(ipc_path);
}

static const char *dispatch_command(const char *line)
{
    if (strcmp(line, "fullscreen") == 0) { do_fullscreen(); return "ok"; }
    if (strcmp(line, "float")      == 0) { do_float();      return "ok"; }
    if (strcmp(line, "kill")       == 0) { do_kill();       return "ok"; }
    if (strcmp(line, "global")     == 0) { do_global();     return "ok"; }

    if (strcmp(line, "focus left")  == 0) { do_focus_left();  return "ok"; }
    if (strcmp(line, "focus right") == 0) { do_focus_right(); return "ok"; }
    if (strcmp(line, "focus up")    == 0) { do_focus_up();    return "ok"; }
    if (strcmp(line, "focus down")  == 0) { do_focus_down();  return "ok"; }

    if (strcmp(line, "swap left")  == 0) { do_swap_left();  return "ok"; }
    if (strcmp(line, "swap right") == 0) { do_swap_right(); return "ok"; }
    if (strcmp(line, "swap up")    == 0) { do_swap_up();    return "ok"; }
    if (strcmp(line, "swap down")  == 0) { do_swap_down();  return "ok"; }

    if (strncmp(line, "tag ", 4) == 0) {
        int t = atoi(line + 4) - 1;
        if (t < 0 || t >= ntags) return "err bad tag index";
        tag_show(t); return "ok";
    }

    if (strncmp(line, "move ", 5) == 0) {
        int a = 0, b = 0;
        if (sscanf(line + 5, "%d %d", &a, &b) == 2) {
            if (focused == None) return "err no focused window";
            Node *nd = node_find(tag_tree[cur_tag], focused);
            if (!nd || !nd->floating) return "err window not floating";
            nd->fx = a; nd->fy = b;
            render_all();
            return "ok";
        } else {
            int t = a - 1;
            if (t < 0 || t >= ntags) return "err bad tag index";
            if (focused != None) window_to_tag(focused, t);
            return "ok";
        }
    }

    if (strncmp(line, "resize ", 7) == 0) {
        if (focused == None) return "err no focused window";
        Node *nd = node_find(tag_tree[cur_tag], focused);
        if (!nd || !nd->floating) return "err window not floating";
        int w, h;
        if (sscanf(line + 7, "%d %d", &w, &h) != 2) return "err usage: resize W H";
        if (w < 40) w = 40;
        if (h < 40) h = 40;
        nd->fw = w; nd->fh = h;
        render_all();
        return "ok";
    }

    if (strncmp(line, "ratio ", 6) == 0) {
        if (focused == None) return "err no focused window";
        const char *arg = line + 6;
        int   dir   = -1;
        double delta = 0.05;
        char  dname[16];
        double dtmp;
        if (sscanf(arg, "%15s %lf", dname, &dtmp) >= 1) delta = dtmp;
        if      (strcmp(dname, "left")  == 0) dir = 0;
        else if (strcmp(dname, "right") == 0) dir = 1;
        else if (strcmp(dname, "up")    == 0) dir = 2;
        else if (strcmp(dname, "down")  == 0) dir = 3;
        if (dir < 0) return "err usage: ratio left|right|up|down [delta]";
        resize_ratio(focused, dir, delta);
        return "ok";
    }

    if (strcmp(line, "reload") == 0) { reload_config(); return "ok"; }
    if (strcmp(line, "quit")   == 0) { running = 0;     return "ok"; }

    if (strcmp(line, "status") == 0) {
        static char buf[IPC_RESP_MAX];
        if (focused != None)
            snprintf(buf, sizeof(buf), "ok tag=%d focused=0x%lx",
                     cur_tag + 1, focused);
        else
            snprintf(buf, sizeof(buf), "ok tag=%d focused=none", cur_tag + 1);
        return buf;
    }

    return "err unknown command";
}

static void ipc_handle(void)
{
    int client = accept(ipc_fd, NULL, NULL);
    if (client < 0) return;

    char msg[IPC_MSG_MAX];
    int  n = (int)read(client, msg, sizeof(msg) - 1);
    if (n <= 0) { close(client); return; }
    msg[n] = '\0';

    char *end = msg + n - 1;
    while (end >= msg && (*end == '\n' || *end == '\r' || *end == ' '))
        *end-- = '\0';

    const char *resp = dispatch_command(msg);

    char out[IPC_RESP_MAX + 2];
    snprintf(out, sizeof(out), "%s\n", resp);
    ssize_t nw = write(client, out, strlen(out));
    (void)nw;
    close(client);
}

static void reload_config(void)
{
    Config *new_cfg = config_parse(cfg_path);
    if (!new_cfg) {
        fprintf(stderr, "kisswm: reload failed, keeping current config\n");
        return;
    }

    config_free(cfg);
    cfg = new_cfg;

    if (cfg->ntags != ntags) {
        for (int i = cfg->ntags; i < ntags; i++) {
            apply_layout(tag_tree[i], GAP, BAR_HEIGHT + GAP,
                         sw - GAP * 2, sh - BAR_HEIGHT - GAP * 2);
            nslots = 0;
            collect_slots(tag_tree[i]);
            for (int j = 0; j < nslots; j++) {
                ignore_unmap++;
                XUnmapWindow(dpy, slots[j].node->win);
            }
            node_free_tree(tag_tree[i]);
            tag_tree[i] = NULL;
        }

        tag_tree = realloc(tag_tree, (size_t)cfg->ntags * sizeof(Node *));
        if (!tag_tree) { fprintf(stderr, "kisswm: oom on reload\n"); exit(1); }

        for (int i = ntags; i < cfg->ntags; i++)
            tag_tree[i] = NULL;

        ntags = cfg->ntags;
        if (cur_tag >= ntags) cur_tag = ntags - 1;
    }

    grab_keys();
    ewmh_update_tags();
    fprintf(stderr, "kisswm: config reloaded (%d tags, %d binds)\n",
            ntags, cfg->nbinds + cfg->ntagbinds);
}

static int xerror_handler(Display *d, XErrorEvent *e)
{
    (void)d;
    if (e->error_code == BadWindow)   return 0;
    if (e->error_code == BadAccess)   return 0;
    if (e->error_code == BadMatch)    return 0;
    if (e->error_code == BadDrawable) return 0;
    fprintf(stderr, "kisswm: X error %d\n", e->error_code);
    return 0;
}

int main(void)
{
    setup_sigchld();

    const char *home = getenv("HOME");
    if (!home) { fprintf(stderr, "kisswm: $HOME not set\n"); return 1; }
    snprintf(cfg_path, sizeof(cfg_path), "%s%s", home, CONFIG_PATH_SUFFIX);

    cfg = config_parse(cfg_path);
    if (!cfg) {
        fprintf(stderr, "kisswm: could not parse config at %s\n", cfg_path);
        return 1;
    }
    ntags = cfg->ntags;

    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "kisswm: cannot open display\n"); return 1; }

    screen = DefaultScreen(dpy);
    root   = RootWindow(dpy, screen);
    sw     = DisplayWidth(dpy, screen);
    sh     = DisplayHeight(dpy, screen);

    XSetErrorHandler(xerror_handler);
    XSelectInput(dpy, root,
                 SubstructureRedirectMask | SubstructureNotifyMask |
                 ButtonPressMask | KeyPressMask | PropertyChangeMask);
    XSync(dpy, False);

    tag_tree = safe_calloc((size_t)ntags, sizeof(Node *));

    ewmh_init();
    ewmh_update_tags();
    ewmh_update_current_tag();

    grab_keys();

    Window dummy, *children;
    unsigned int nchildren;
    XQueryTree(dpy, root, &dummy, &dummy, &children, &nchildren);
    if (children) {
        for (unsigned int i = 0; i < nchildren; i++) {
            XWindowAttributes xa;
            if (!XGetWindowAttributes(dpy, children[i], &xa)) continue;
            if (xa.override_redirect || xa.map_state != IsViewable) continue;
            add_window(children[i]);
            grab_buttons(children[i]);
        }
        XFree(children);
    }

    ipc_setup();

    int xfd = ConnectionNumber(dpy);
    XEvent ev;
    while (running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        if (ipc_fd >= 0) FD_SET(ipc_fd, &fds);
        int maxfd = (ipc_fd > xfd) ? ipc_fd : xfd;

        if (select(maxfd + 1, &fds, NULL, NULL, NULL) < 0) continue;

        if (ipc_fd >= 0 && FD_ISSET(ipc_fd, &fds))
            ipc_handle();

        if (!FD_ISSET(xfd, &fds) && !XPending(dpy)) continue;

        while (XPending(dpy)) {
        XNextEvent(dpy, &ev);

        switch (ev.type) {
        case KeyPress: case KeyRelease:
            last_event_time = ev.xkey.time; break;
        case ButtonPress: case ButtonRelease:
            last_event_time = ev.xbutton.time; break;
        case MotionNotify:
            last_event_time = ev.xmotion.time; break;
        case EnterNotify: case LeaveNotify:
            last_event_time = ev.xcrossing.time; break;
        case PropertyNotify:
            last_event_time = ev.xproperty.time; break;
        }

        switch (ev.type) {
        case MapRequest:
            add_window(ev.xmaprequest.window);
            grab_buttons(ev.xmaprequest.window);
            break;

        case UnmapNotify:
            if (ignore_unmap > 0) { ignore_unmap--; break; }
            remove_window(ev.xunmap.window);
            break;

        case DestroyNotify:
            remove_window(ev.xdestroywindow.window);
            break;

        case ConfigureRequest: {
            if (node_find_any_tag(ev.xconfigurerequest.window)) {
                XWindowAttributes xa;
                if (XGetWindowAttributes(dpy, ev.xconfigurerequest.window, &xa)) {
                    XEvent ce;
                    memset(&ce, 0, sizeof(ce));
                    ce.type                    = ConfigureNotify;
                    ce.xconfigure.display      = dpy;
                    ce.xconfigure.event        = ev.xconfigurerequest.window;
                    ce.xconfigure.window       = ev.xconfigurerequest.window;
                    ce.xconfigure.x            = xa.x;
                    ce.xconfigure.y            = xa.y;
                    ce.xconfigure.width        = xa.width;
                    ce.xconfigure.height       = xa.height;
                    ce.xconfigure.border_width = BORDER_WIDTH;
                    ce.xconfigure.above        = None;
                    ce.xconfigure.override_redirect = False;
                    XSendEvent(dpy, ev.xconfigurerequest.window, False,
                               StructureNotifyMask, &ce);
                }
            } else {
                XWindowChanges wc;
                wc.x            = ev.xconfigurerequest.x;
                wc.y            = ev.xconfigurerequest.y;
                wc.width        = ev.xconfigurerequest.width;
                wc.height       = ev.xconfigurerequest.height;
                wc.border_width = BORDER_WIDTH;
                wc.sibling      = ev.xconfigurerequest.above;
                wc.stack_mode   = ev.xconfigurerequest.detail;
                XConfigureWindow(dpy, ev.xconfigurerequest.window,
                                 ev.xconfigurerequest.value_mask, &wc);
            }
            break;
        }

        case EnterNotify:
            if (focus_lock) {
                focus_lock = 0;
                break;
            }
            if (ev.xcrossing.mode == NotifyNormal &&
                ev.xcrossing.window != focused)
                focus_window(ev.xcrossing.window, 0);
            break;

        case KeyPress:
            handle_keypress(&ev.xkey);
            break;

        case ButtonPress:
            handle_button_press(&ev.xbutton);
            break;

        case ButtonRelease:
            handle_button_release(&ev.xbutton);
            break;

        case MotionNotify:
            while (XCheckTypedEvent(dpy, MotionNotify, &ev));
            if (drag_active) {
                Node *nd = node_find(tag_tree[cur_tag], drag_win);
                if (!nd) { drag_active = 0; break; }
                int nx = drag_win_x + (ev.xmotion.x_root - drag_start_x);
                int ny = drag_win_y + (ev.xmotion.y_root - drag_start_y);
                if (nx < -(nd->fw - 20)) nx = -(nd->fw - 20);
                if (ny < BAR_HEIGHT)     ny = BAR_HEIGHT;
                if (nx > sw - 20)        nx = sw - 20;
                if (ny > sh - 20)        ny = sh - 20;
                nd->fx = nx; nd->fy = ny;
                XMoveWindow(dpy, drag_win, nd->fx, nd->fy);
            } else if (resize_active) {
                Node *nd = node_find(tag_tree[cur_tag], resize_win);
                if (!nd) { resize_active = 0; break; }
                int nw = resize_orig_w + (ev.xmotion.x_root - resize_start_x);
                int nh = resize_orig_h + (ev.xmotion.y_root - resize_start_y);
                if (nw < 40) nw = 40;
                if (nh < 40) nh = 40;
                if (nw > sw) nw = sw;
                if (nh > sh) nh = sh;
                nd->fw = nw; nd->fh = nh;
                XResizeWindow(dpy, resize_win, (unsigned)nd->fw, (unsigned)nd->fh);
            }
            break;

        case FocusIn:
            if (ev.xfocus.mode == NotifyNormal ||
                ev.xfocus.mode == NotifyWhileGrabbed)
                XSetWindowBorder(dpy, ev.xfocus.window, BORDER_FOCUS);
            break;

        case FocusOut:
            if (ev.xfocus.mode == NotifyNormal ||
                ev.xfocus.mode == NotifyWhileGrabbed)
                XSetWindowBorder(dpy, ev.xfocus.window, BORDER_NORMAL);
            break;

        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == net_atoms[NET_CLOSE_WINDOW])
                remove_window(ev.xclient.window);
            break;
        }
        }

        XSync(dpy, False);
    }

    for (int i = 0; i < ntags; i++) {
        apply_layout(tag_tree[i], GAP, BAR_HEIGHT + GAP,
                     sw - GAP * 2, sh - BAR_HEIGHT - GAP * 2);
        nslots = 0;
        collect_slots(tag_tree[i]);
        for (int j = 0; j < nslots; j++) {
            XMapWindow(dpy, slots[j].node->win);
            XReparentWindow(dpy, slots[j].node->win, root, 0, 0);
        }
        node_free_tree(tag_tree[i]);
    }
    free(tag_tree);
    config_free(cfg);
    ipc_teardown();
    XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
    XCloseDisplay(dpy);
    return 0;
}
