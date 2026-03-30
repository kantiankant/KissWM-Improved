# kisswm

![screenshot](/assets/logo.svg)

A minimal X11 BSP tiling window manager modelled directly on [bspwm](https://github.com/baskerville/bspwm). It implements the same binary space partition tree layout, the same directional focus model, and the same tag-based window organisation, with a reduced feature set and a single-binary architecture. If you need full bspwm functionality, multi-monitor support, node rules, `bspc` scripting and whatnot, just use bspwm instead.

It's essentially a really light clone of bspwm and that's about it. Its codebase is also a lot smaller, which I guess is either an advantage or disadvantage depending on who you ask.

---

## differences from bspwm

| feature | bspwm | kisswm |
|---|---|---|
| architecture | WM daemon + `bspc` client | single binary |
| configuration | shell script of `bspc` calls | declarative rc file |
| multi-monitor | yes | no |
| node/window rules | yes | no |
| split ratio adjustment | yes | yes (IPC `ratio` command) |
| floating windows | yes | yes |
| fullscreen | yes | yes |
| sticky/global windows | yes | yes (`_NET_WM_DESKTOP = 0xFFFFFFFF`) |
| mouse drag/resize | yes | yes |
| tags/desktops | yes | yes (up to 32) |
| hot config reload | yes | yes |
| scripting via IPC | extensive (`bspc`) | basic (`kisswmctl`) |
| EWMH support | full | partial (see below) |

---

## dependencies

- libx11

---

## build

```sh
make
doas make clean install
```

---

## configuration

`~/.config/kisswm/kisswmrc`

```sh
mkdir -p ~/.config/kisswm
cp kisswmrc.example ~/.config/kisswm/kisswmrc
```

The rc file is parsed at startup and re-parsed on `reload`. Each line is either blank, a comment (`#`), a directive, or a keybind. There are no blocks, includes, or conditionals.

### directives

#### `tag = N`

Number of tags. Integer, range 1–32. Default: 9.

### keybind syntax

```
MODIFIERS-KEY = action
```

**Modifiers:**

| token | mask |
|---|---|
| `M` or `4` | `Mod4Mask` (Super) |
| `A` | `Mod1Mask` (Alt) |
| `C` | `ControlMask` |
| `S` | `ShiftMask` |

Key names follow `XStringToKeysym(3)` conventions. Use `xev(1)` to find the correct name for any key. Key names are case-sensitive (`Return`, not `return`; `F1`, not `f1`).

Lock key state (NumLock, CapsLock, ScrollLock) is stripped at dispatch. All eight combinations are registered at startup.

**XF86 keys:** `XF86-AudioRaiseVolume`, `XF86-MonBrightnessUp`, etc. No modifier support.

**Bare keys:** `KEY = action` grabs the key globally with no modifier. The key will not reach any application.

### internal actions

| action | description |
|---|---|
| `kill` | Close focused window (`WM_DELETE_WINDOW` or `XDestroyWindow`) |
| `quit` | Shut down kisswm |
| `global` | Toggle window visibility across all tags |
| `focus-left/right/up/down` | Directional focus |
| `swap-left/right/up/down` | Swap window position with directional neighbour |
| `tag-N` | Switch to tag N (1-based) |
| `move-N` | Move focused window to tag N |

Any value not matching the above is executed as a shell command via `/bin/sh -c`.

### example config

```
tag = 9

M-Return             = kitty
M-S-q                = kisswmctl kill
M-f                  = kisswmctl fullscreen
M-v                  = kisswmctl float
M-S-r                = kisswmctl reload
M-S-e                = quit

M-h                  = focus-left
M-l                  = focus-right
M-k                  = focus-up
M-j                  = focus-down

M-S-h                = swap-left
M-S-l                = swap-right
M-S-k                = swap-up
M-S-j                = swap-down

M-1                  = tag-1
M-2                  = tag-2
M-3                  = tag-3

M-S-1                = move-1
M-S-2                = move-2
M-S-3                = move-3

XF86-AudioRaiseVolume  = pamixer --increase 5
XF86-AudioLowerVolume  = pamixer --decrease 5
XF86-AudioMute         = pamixer --toggle-mute
XF86-MonBrightnessUp   = brightnessctl set +5%
XF86-MonBrightnessDown = brightnessctl set 5%-
```

---

## IPC

Unix socket at `/tmp/kisswm-$DISPLAY.sock`. Plain text, one command per line, one response per command. `kisswmctl` wraps the socket for shell use.

```
fullscreen
float
kill
global
focus  left|right|up|down
swap   left|right|up|down
tag    N
move   N
move   X Y
resize W H
ratio  left|right|up|down [delta]
reload
quit
status
```

Responses: `ok`, `ok <data>`, or `err <reason>`. `kisswmctl` exits 0 on ok, 1 on error.

---

## floating

Floating windows remain in the BSP tree but are excluded from tiling geometry. They render above tiled windows at their stored position. Toggle via `kisswmctl float`.

Mouse controls (floating windows only): `Mod4+Button1` to drag, `Mod4+Button3` to resize.

IPC: `move X Y` and `resize W H` set position and dimensions. Minimum size 40×40.

---

## appearance

Edit `src/config.h` and recompile:

```c
#define GAP             8        /* px gap between windows     */
#define BAR_HEIGHT      24       /* px reserved at top         */
#define BORDER_WIDTH    1        /* px border thickness        */
#define BORDER_FOCUS    0xfcfbf9 /* focused border colour      */
#define BORDER_NORMAL   0x000000 /* unfocused border colour    */
```

---

## EWMH

Root window atoms set: `_NET_SUPPORTED`, `_NET_SUPPORTING_WM_CHECK`,
`_NET_WM_NAME`, `_NET_NUMBER_OF_DESKTOPS`, `_NET_DESKTOP_NAMES`,
`_NET_DESKTOP_VIEWPORT`, `_NET_CURRENT_DESKTOP`, `_NET_ACTIVE_WINDOW`,
`_NET_CLIENT_LIST`.

Per-window: `_NET_WM_DESKTOP`. Global windows receive `0xFFFFFFFF`.

Single monitor only. `_NET_DESKTOP_VIEWPORT` is all zeros.

---

## source layout

```
src/config.h       compile-time appearance constants
src/ipc.h          socket path construction
src/parser.h       Config, Keybind, TagBind types
src/parser.c       rc file parser
src/kisswm.c       window manager (1,556 lines)
src/kisswmctl.c    IPC client
```

---

## license

GPL-3.0
