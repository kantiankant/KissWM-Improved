# kisswm

X11 window manager. BSP tiling. Tags. Floating. IPC. Hot-reload. 2,222 lines.
Compiles in under a second. Uses 3 megabytes of RAM. Does its job. Shuts up.

If you need more than that to decide whether to use it, this is not the window
manager for you and frankly the feeling is mutual.

## the architecture

One binary space partition tree per tag. Each node is either a split or a leaf.
Split nodes divide their rectangle and recurse. Leaf nodes call
`XMoveResizeWindow`. The layout engine is two integer identities:

```
lw + rw + GAP = w      /* vertical split */
th + bh + GAP = h      /* horizontal split */
```

These hold at every node in the tree, always, by construction. Each render pass
derives positions top-down from the root rectangle, stores them on every node,
then pushes them to X in a second pass. No accumulated floating point error
because we're using integers like people who understand what a pixel is. The
tree *is* the layout. Derive positions on every render, same result every time,
done.

New windows split the focused leaf. Split direction alternates by depth — even
depth vertical, odd depth horizontal. Coherent spawn order without manual
direction management. Window closes: sibling collapses up to fill the parent's
rectangle. The invariant holds before and after every operation. It is not
complicated. Most things aren't, if you think about them properly instead of
reaching for an abstraction layer.

The original implementation had one flaw: `ConfigureRequest` granted every
client request unconditionally. Clients requesting their own geometry caused
feedback loops. The symptom was fractal window replication. The fix is
rejecting geometry requests from managed windows and sending a synthetic
`ConfigureNotify` with their actual position instead. This is in the ICCCM.
It has been in the ICCCM since 1989. We are not doing anything novel here.

## dependencies

- libx11
- a C compiler

That is the complete list. It is not going to get longer.

## build

```sh
make
doas make install
```

Standard autoconf is not used because this project does not have seventeen
optional subsystems that need feature-detection across forty platforms. It has
a C file, a linker flag, and an opinion about where the binary goes. The
Makefile expresses this in eleven lines. If your build system cannot manage
eleven lines without a configure script, your build system has problems that
are not this project's responsibility.

## configuration

`~/.config/kisswm/kisswmrc`. Runtime parsed. Hot-reloaded.

```sh
mkdir -p ~/.config/kisswm
cp KissWM-Improved/kisswmrc.example ~/.config/kisswm/kisswmrc
```

### grammar

A kisswmrc file is a sequence of lines. Each line is one of:

- a blank line (ignored)
- a comment: any line whose first non-whitespace character is `#` (ignored)
- a directive: `identifier = value`
- a bind: `lhs = rhs`

The distinction between a directive and a bind is determined by the left-hand
side. `tag` is a directive. Everything else is a bind. There are no other
directives. There are no blocks, no sections, no includes, no conditionals, no
types beyond strings and integers. If you require those features, you are
configuring the wrong piece of software and you should take a moment to reflect
on how you arrived here.

Whitespace around `=` is optional. Lines are processed in order. Later binds
for the same key silently override earlier ones.

---

### directives

#### `tag`

```
tag = N
```

Sets the number of tags. `N` is an integer in the range `[1, 32]`. If omitted,
the default is 9. This directive must appear before any `tag-N` or `move-N`
binds that reference indices beyond the default, or those binds will be dropped
at parse time with a warning.

**Type:** integer  
**Range:** 1–32  
**Default:** 9

---

### binds

A bind maps a key combination to an action. The left-hand side specifies the
key combination. The right-hand side specifies the action. There are three
distinct LHS forms and two distinct RHS forms, defined in full below.

---

### left-hand side forms

#### form 1: modifier bind

```
MODIFIERS-KEY = rhs
```

One or more modifier tokens joined by hyphens, followed by a final hyphen,
followed by a key name. The key name is always the last token. Everything
before it is a modifier. Order of modifier tokens is not significant. Duplicate
modifier tokens are collapsed silently.

**Modifier tokens** (case-sensitive):

| token | mask | physical key |
|-------|------|--------------|
| `M` | `Mod4Mask` | Super / Windows |
| `4` | `Mod4Mask` | Super / Windows (identical to `M`) |
| `A` | `Mod1Mask` | Alt |
| `C` | `ControlMask` | Control |
| `S` | `ShiftMask` | Shift |

Modifiers may be combined in any order. `M-S-h` and `S-M-h` are identical.
`M-A-C-x` binds Super+Alt+Control+X. There is no upper limit on the number of
modifiers beyond the physical constraints of your keyboard and the tolerance
of your wrists.

**Key name:** any string accepted by `XStringToKeysym(3)`. The complete set
includes but is not limited to:

- single printable ASCII characters: `a`–`z`, `0`–`9`, `comma`, `period`,
  `minus`, `equal`, `bracketleft`, `bracketright`, `backslash`, `semicolon`,
  `apostrophe`, `grave`, `slash`
- named keys: `space`, `Return`, `Tab`, `BackSpace`, `Escape`, `Delete`,
  `Insert`, `Home`, `End`, `Prior` (Page Up), `Next` (Page Down)
- function keys: `F1` through `F24`
- `Print`, `Pause`, `Scroll_Lock`, `Caps_Lock`, `Num_Lock`
- arrow keys: `Left`, `Right`, `Up`, `Down`

Key names are case-sensitive as defined by `XStringToKeysym(3)`. `space` is
correct. `Space` resolves to `NoSymbol` and the bind is dropped. `Return` is
correct. `return` is not. `F1` is correct. `f1` is not. The canonical name for
any key is obtained by running `xev(1)`, pressing the key, and reading the
`keysym` field from the output. This takes fifteen seconds. Do it before
opening an issue.

Keybinds are registered with all lock key modifier combinations at startup and
on reload. The eight combinations of NumLock (`Mod2Mask`), CapsLock
(`LockMask`), and ScrollLock (`Mod3Mask`) are registered separately for each
bind. At dispatch time, lock masks are stripped from the event state before
comparison. A bind registered as `M-f` fires when Super+F is pressed
regardless of NumLock or CapsLock state. This is not a configurable behaviour.
It is the only correct behaviour.

**Examples:**
```
M-q      = kitty
M-S-h    = swap-left
M-A-Tab  = cycle-all
M-1      = tag-1
M-S-1    = move-1
```

---

#### form 2: bare key bind

```
KEY = rhs
```

A key name with no modifier tokens. Fires when the key is pressed with no
modifier held. The key name follows the same rules and the same
`XStringToKeysym(3)` namespace as the key name component of a modifier bind.

A bare key bind grabs the key globally at the root window. The key will not be
delivered to any application for the lifetime of the WM. Use this exclusively
for keys that have no meaningful application semantics: `Print`, `Pause`,
navigation keys used outside applications. Binding `a` with this form will make
it impossible to type the letter a. This is not a defect. You asked for it
explicitly in the configuration file that you wrote and loaded.

**Examples:**
```
Print  = maim -s ~/Screenshots/$(date +%Y-%m-%d_%H-%M-%S).png
Pause  = playerctl play-pause
```

---

#### form 3: XF86 key bind

```
XF86-NAME = rhs
```

A dedicated form for XF86 extended keysyms produced by multimedia keys. The
literal prefix `XF86-` is followed by the keysym suffix — the keysym name with
its own `XF86` prefix removed.

The resolution is mechanical: `XF86-AudioMute` → `XStringToKeysym("XF86AudioMute")`. If the result is `NoSymbol`, the bind is dropped and a warning
is printed to stderr. XF86 binds take no modifier tokens. XF86 keys do not
carry a modifier.

The correct suffix for any XF86 key is obtained by running `xev(1)`, pressing
the key, reading the keysym name from the output, and removing the leading
`XF86`. The result is the suffix. This procedure is not difficult. It is
documented here because asking "what do I put after XF86-" is the single most
common question asked about this class of software, and the answer has not
changed since 1999.

**Examples:**
```
XF86-AudioMute         = pamixer -t
XF86-AudioLowerVolume  = pamixer -d 5
XF86-AudioRaiseVolume  = pamixer -i 5
XF86-MonBrightnessDown = brightnessctl set 5%-
XF86-MonBrightnessUp   = brightnessctl set +5%
XF86-AudioPrev         = playerctl previous
XF86-AudioPlay         = playerctl play-pause
XF86-AudioNext         = playerctl next
```

---

### right-hand side forms

#### form 1: internal action

A reserved string that maps directly to a WM function. No process is spawned.
The action executes synchronously in the event loop. Matching is exact and
case-sensitive. The complete set of valid internal action strings:

| string | semantics |
|--------|-----------|
| `kill` | Send `WM_DELETE_WINDOW` to the focused window if it advertises the protocol; otherwise call `XDestroyWindow`. The window is removed from the BSP tree and sibling geometry is updated. |
| `quit` | Terminate kisswm. All managed windows are unmapped, reparented to root, and the display connection is closed cleanly. |
| `global` | Toggle `_NET_WM_DESKTOP` between the current tag index and `0xFFFFFFFF` on the focused window. A window with `_NET_WM_DESKTOP = 0xFFFFFFFF` appears on all tags simultaneously. Toggle again to restore to current tag. |
| `focus-left` | Move focus to the nearest tiled window to the left of the focused window. Nearest is defined as minimum edge-to-edge boundary distance, constrained to candidates whose extent lies left of the focused window's left edge with vertical overlap. |
| `focus-right` | As `focus-left`, rightward. |
| `focus-up` | As `focus-left`, upward. |
| `focus-down` | As `focus-left`, downward. |
| `swap-left` | Exchange the X11 window ID stored in the focused BSP leaf with that of the nearest left neighbour leaf. The focused window moves to the neighbour's slot. The neighbour moves to the focused window's previous slot. Focus tracks the moved window. The cursor warps to the new window centre. |
| `swap-right` | As `swap-left`, rightward. |
| `swap-up` | As `swap-left`, upward. |
| `swap-down` | As `swap-left`, downward. |
| `cycle-all` | Not yet implemented. |
| `cycle-tag` | Not yet implemented. |
| `tag-N` | Switch the visible tag to N. N is a decimal integer literal, 1-based. Valid values are `tag-1` through `tag-32`, constrained by the configured tag count. A `tag-N` bind for N greater than the configured tag count is dropped at parse time. |
| `move-N` | Move the focused window to tag N and unmap it from the current tag. N follows the same constraints as `tag-N`. |

Any right-hand side string not in the above table is treated as a shell command.
There is no escape mechanism for internal action names. If you want to spawn a
process called `kill`, name it something else.

---

#### form 2: shell command

Any right-hand side string that does not exactly match an internal action name
is executed as a shell command via `execl("/bin/sh", "sh", "-c", cmd, NULL)`.
The full string is passed verbatim to the shell. Shell metacharacters, variable
expansion, command substitution, pipes, and redirections are all valid and
processed by the shell, not by kisswm. Output is not captured. Return value is
not checked.

Process lifecycle: `fork(2)` is called. In the child: `setsid(2)` is called to
detach from the process group, the X11 display connection is closed, then
`execl(3)` is called. The WM does not `wait(2)` on the child directly; `SIGCHLD`
is handled with `waitpid(2)` and `WNOHANG` to reap zombies. The WM continues
processing events immediately after fork. There is no timeout, no process
management, no output capture, and no way to determine whether the spawned
process succeeded. It is fork-and-forget. If you need the result, pipe it
somewhere yourself.

**Examples:**
```
M-q    = kitty
M-e    = kitty -e yazi
M-f    = kisswmctl fullscreen
M-v    = kisswmctl float
M-S-r  = kisswmctl reload
Print  = maim -s ~/Screenshots/$(date +%Y-%m-%d_%H-%M-%S).png
```


---

### IPC protocol

Unix socket at `/tmp/kisswm-$DISPLAY.sock`. Plain text. One command per line.
One response per command. Connection-per-command model.

```
fullscreen          toggle _NET_WM_STATE_FULLSCREEN on focused window
float               toggle floating state on focused window
kill                as above
global              as above
focus left          directional focus
focus right
focus up
focus down
swap left           swap with warp
swap right
swap up
swap down
tag N               switch tag (1-based)
move N              move window to tag (1-based)
move X Y            set floating window position in screen pixels
resize W H          set floating window dimensions, minimum 40x40
ratio left|right|up|down [delta]
                    nudge the split ratio of the nearest ancestor splitting
                    on the relevant axis by delta (default 0.05, clamped
                    to [0.05, 0.95])
reload              re-parse config, re-grab keys
quit                clean shutdown
status              returns: ok tag=N focused=0x<window-id>
```

Responses are `ok`, `ok <data>`, or `err <reason>`. All commands are
synchronous within a single event loop iteration. `kisswmctl(1)` wraps the
socket for shell and keybind use. Exit status reflects the response: 0 for ok,
1 for err or connection failure.

### floating

Floating leaves remain in the BSP tree. They are excluded from `count_tiled`
and `collect_slots`, so the tiling geometry closes around them as if they were
absent. They are rendered separately after the tiling pass at their stored
`fx, fy, fw, fh` geometry and raised above tiled windows. This means the
invariant is unaffected by floating state. Adding or removing a floating window
from tiling consideration does not corrupt the tree.

Re-tiling a floating window: the window is detached from its current tree
position, slots are re-collected, the nearest tiled leaf is found by
centre-to-centre distance, and the floating window is inserted adjacent to it
via the standard in-place split mutation. The detach-before-graft ordering is
mandatory — grafting without detaching first creates a DAG, not a tree, and
everything that assumes a tree breaks silently and catastrophically. This is
not a subtle point. It is in the code. Read it.

Tag switches unmap and map all leaves including floating ones via a full tree
walk, not a slot collection. Floating windows are therefore correctly per-tag.
Using `collect_slots` for tag visibility management was the original
implementation. It was wrong. It has been corrected.

### appearance

`src/config.h`. Recompile after changing.

```c
#define GAP             8
#define BAR_HEIGHT      32
#define BORDER_WIDTH    1
#define BORDER_FOCUS    0xfcfbf9
#define BORDER_NORMAL   0x000000
```

## EWMH

Sets on root window: `_NET_SUPPORTED`, `_NET_SUPPORTING_WM_CHECK`,
`_NET_WM_NAME`, `_NET_NUMBER_OF_DESKTOPS`, `_NET_DESKTOP_NAMES`,
`_NET_DESKTOP_VIEWPORT`, `_NET_CURRENT_DESKTOP`, `_NET_ACTIVE_WINDOW`,
`_NET_CLIENT_LIST`.

Sets per window: `_NET_WM_DESKTOP`. Global windows receive `0xFFFFFFFF`.

`_NET_DESKTOP_VIEWPORT` is all zeros. Single monitor. No virtual desktop
panning. If you need virtual desktop panning you need a different window
manager and possibly a larger monitor.

## compositor interaction

A synthetic `ConfigureNotify` is sent to every managed window after each render
pass, after `XSync`. This gives compositors accurate geometry before they
repaint. Without it, compositors using damage tracking repaint from stale pixmap
data during bulk geometry changes. The symptom is transparent artefacts on
window borders during resize. The fix is in the WM, not the compositor config,
regardless of what the compositor documentation implies. We have tried it both
ways.

Some artefacts will persist regardless. X11's COMPOSITE extension does not
provide atomic geometry-update-to-repaint semantics. This is a protocol
limitation. It is not fixable at the application level. Wayland addresses this
at the protocol level. Wayland also requires you to solve seventeen other
problems that X11 solved in 1987. You make your choices.

## source layout

```
src/config.h       compile-time appearance constants
src/ipc.h          socket path construction, wire format documentation
src/parser.h       Config, Keybind, TagBind types; parse API
src/parser.c       kisswmrc lexer and config builder
src/kisswm.c       window manager
src/kisswmctl.c    IPC client
```

`kisswm.c` is one file. The BSP tree, event loop, EWMH, IPC server, floating,
fullscreen, directional focus, tag management — all of it. 1,556 lines. If you
think this should be split into `bsp.c`, `ewmh.c`, `events.c`, and a shared
`types.h`, you are wrong, but more importantly you are free to do it yourself
and I am free to not care.

## credit

kantiankant wrote the BSP math. In an afternoon. It is the best part of this
codebase by a margin that is not close. Everything else in this repository
exists to serve those two invariants without breaking them. We have largely
succeeded at this, modulo several intermediate states that we have collectively
agreed not to document.

## license

GPL-3.0.
