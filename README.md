# hyprgrid-native

An experimental Hyprland plugin that makes the existing `(x,y)` workspace grid
respond to three-finger swipes with native 1:1 motion and mid-swipe
cancellation. It builds against **Hyprland 0.56.2 and 0.57.0** from the same
source.

Physical gestures map directly to coordinates:

- right → `(x+1,y)`
- left → `(x-1,y)`
- up → `(x,y+1)`
- down → `(x,y-1)`

To flip an axis (for example so a right swipe goes to `(x-1,y)`), set:

```ini
# hyprland.conf (0.56)
plugin {
    hyprgrid {
        invert_x = true
        invert_y = false
    }
}
```

```lua
-- hyprland.lua (0.57+)
hl.config({ plugin = { hyprgrid = { invert_x = true, invert_y = false } } })
```

Save the config and the next swipe uses it. The slide animation always
follows your fingers.

It reads and extends the map created by `hypr-grid.py` at
`$XDG_STATE_HOME/hypr-grid/map.json` or `~/.local/state/hypr-grid/map.json`.
Only the `"cells"` object of that file is rewritten; other keys are kept.

## Install on Arch

The `hyprland` package already ships the headers a plugin needs, so all you
need are the build tools:

```bash
sudo pacman -S --needed base-devel pkgconf
```

Then, from a terminal inside your Hyprland session, in this folder:

```bash
./install-arch.sh
```

It builds against your installed Hyprland, copies the plugin to
`~/.local/share/hyprgrid-native/hyprgrid-native.so`, and loads it. You should
see a green "loaded" notification. A red one says what to fix.

To load it at every login, add the line the script prints to your config:

- Hyprland 0.56 (`hyprland.conf`):
  `plugin = /home/YOU/.local/share/hyprgrid-native/hyprgrid-native.so`
- Hyprland 0.57+ (`hyprland.lua`):
  `hl.plugin.load("/home/YOU/.local/share/hyprgrid-native/hyprgrid-native.so")`

### After every Hyprland update

Plugins only work with the exact Hyprland version they were built against.
After `pacman -Syu` updates Hyprland: log out, log back in, then run
`./install-arch.sh` again. (The script refuses to build while the old Hyprland
is still running.)

### Updating from 0.56 to 0.57: read this first

**Hyprland 0.57.0 no longer reads `hyprland.conf` at all.** It only reads
`~/.config/hypr/hyprland.lua`; without one, it starts with a default config.
Convert your config to Lua before or right after updating. In Lua, plugins are
loaded with `hl.plugin.load(...)` and gestures are declared with
`hl.gesture({...})`.

## Grid and numbered workspaces

The grid is where you normally work. Ordinary numbered workspaces (1, 2, 3…)
stay **outside** it as quick-access spots: reach them with your usual
`SUPER+1`… keybinds. Swiping only moves around the grid; on a numbered
workspace a swipe just reminds you how to get back in.

Jump into the grid with two numbers:

```bash
hyprctl hyprgrid goto 2,1          # go to cell (2,1); a new cell is created if needed
hyprctl hyprgrid goto 0,0          # home
hyprctl hyprgrid goto "(-1,0)"     # negative x/y: wrap in parentheses (hyprctl treats -1 as a flag)
```

`install-arch.sh` also installs `~/.local/bin/hyprgrid-goto`: a small prompt
where you type the two numbers (`2 1`, `2,1` or `-1 0`) and press Enter. It
uses fuzzel, wofi, rofi, tofi or bemenu, whichever you have
(`sudo pacman -S fuzzel` if none).

Suggested keys (the installer prints them with your paths):

```ini
# hyprland.conf (0.56)
bind = SUPER, G, exec, ~/.local/bin/hyprgrid-goto
bind = SUPER, X, exec, hyprctl hyprgrid goto 0,0
```

```lua
-- hyprland.lua (0.57+)
hl.bind("SUPER + G", hl.dsp.exec_cmd("~/.local/bin/hyprgrid-goto"))
hl.bind("SUPER + X", hl.dsp.exec_cmd("hyprctl hyprgrid goto 0,0"))
```

Check that SUPER+G and SUPER+X aren't already bound to something you use.

To take a cell back out of the grid (its workspace and windows stay, as an
ordinary numbered workspace):

```bash
hyprctl hyprgrid release           # the current cell
hyprctl hyprgrid release 0,0       # a given cell
```

## Moving windows

Numbered workspaces: use Hyprland's normal binds (`SUPER+SHIFT+1`… in the
default config).

Grid cells, by coordinates:

```bash
hyprctl hyprgrid send 2,1            # send the focused window to (2,1); you stay
hyprctl hyprgrid send 2,1 follow     # ...and go with it
hyprctl hyprgrid send right          # to the neighbouring cell (right = x+1, up = y+1)
hyprctl hyprgrid send right follow
hyprctl hyprgrid send "(-1,0)"       # negative coordinates need parentheses
```

With the prompt: `hyprgrid-goto take` asks for two numbers and takes the
window there with you; `hyprgrid-goto send` leaves you where you are.

```ini
# hyprland.conf (0.56)
bind = SUPER SHIFT, G, exec, ~/.local/bin/hyprgrid-goto take
bind = SUPER SHIFT CTRL, left,  exec, hyprctl hyprgrid send left follow
bind = SUPER SHIFT CTRL, right, exec, hyprctl hyprgrid send right follow
bind = SUPER SHIFT CTRL, up,    exec, hyprctl hyprgrid send up follow
bind = SUPER SHIFT CTRL, down,  exec, hyprctl hyprgrid send down follow
```

```lua
-- hyprland.lua (0.57+)
hl.bind("SUPER + SHIFT + G", hl.dsp.exec_cmd("~/.local/bin/hyprgrid-goto take"))
for _, dir in ipairs({ "left", "right", "up", "down" }) do
    hl.bind("SUPER + SHIFT + CTRL + " .. dir, hl.dsp.exec_cmd("hyprctl hyprgrid send " .. dir .. " follow"))
end
```

If you still use `hypr-grid.py move-left` etc., those binds keep working;
pick keys that don't clash with them.

## Adding ordinary workspaces (1, 2, 3…) to the grid

Optional. Apps on normal numbered workspaces can join the grid without moving: the
workspace itself becomes a cell, windows and all.

```bash
hyprctl hyprgrid list        # grid cells, plus numbered workspaces outside the grid
hyprctl hyprgrid adopt-all   # every numbered workspace with windows -> free cells
hyprctl hyprgrid adopt       # just the current workspace -> next free cell
hyprctl hyprgrid adopt 2,1   # the current workspace -> cell (2,1)
```

New cells go on row 0, in the first free spots to the right of `(0,0)`, in
workspace-number order. Rearrange them afterwards with `move` / `swap` below.
Adopted workspaces are renamed to their `(x,y)` label but keep their number,
so a `SUPER+1`-style keybind still reaches them.

## Moving cells around the grid

A cell (its workspace and all its windows) can be moved to another grid
position. If the destination is taken, the two cells swap places. Nothing
moves on screen; only the cell's coordinates change, so the next swipes take
you along the new layout.

```bash
hyprctl hyprgrid list                 # show the grid (highest row first)
hyprctl hyprgrid shift right          # move the current workspace one cell right
hyprctl hyprgrid move 2,1             # move the current workspace to (2,1)
hyprctl hyprgrid swap 0,0 1,0         # swap two cells (or move one into an empty cell)
```

`shift` uses the same directions as the swipes: right is x+1, up is y+1.
Workspaces with grid names are renamed to their new `(x,y)`; names you gave
yourself are left alone.

Keybindings (pick keys you don't already use):

```ini
# hyprland.conf (0.56)
bind = SUPER ALT, left,  exec, hyprctl hyprgrid shift left
bind = SUPER ALT, right, exec, hyprctl hyprgrid shift right
bind = SUPER ALT, up,    exec, hyprctl hyprgrid shift up
bind = SUPER ALT, down,  exec, hyprctl hyprgrid shift down
```

```lua
-- hyprland.lua (0.57+)
for _, dir in ipairs({ "left", "right", "up", "down" }) do
    hl.bind("SUPER + ALT + " .. dir, hl.dsp.exec_cmd("hyprctl hyprgrid shift " .. dir))
end
```

## Before loading: remove conflicting 3-finger gestures

Config gestures are matched before plugin gestures. So a 3-finger workspace
gesture in your config swallows those swipes:

- `hyprland.conf`: `gesture = 3, horizontal, workspace`
- `hyprland.lua` (also in Hyprland's example config):
  `hl.gesture({ fingers = 3, direction = "horizontal", action = "workspace" })`

Remove or comment out every 3-finger gesture. The plugin detects leftovers and
shows a red notification naming the affected directions. Keep the keyboard
bindings that call `hypr-grid.py move-left`, `move-right`, `move-up`, and
`move-down`.

Reloading the config is safe: the gesture is registered again after each
reload.

## Manual build

```bash
make                                      # or: cmake -B build && cmake --build build
hyprctl plugin load "$PWD/hyprgrid-native.so"
hyprctl plugin unload "$PWD/hyprgrid-native.so"
```

Do not unload the plugin while a swipe is physically in progress.

## Tuning

The plugin uses Hyprland's standard workspace swipe settings:
`gestures:workspace_swipe_distance` (finger travel for a full transition),
`gestures:workspace_swipe_cancel_ratio` (commit threshold) and
`gestures:workspace_swipe_min_speed_to_force` (a fast flick always commits).

## Behaviour

- The current cell is found by workspace ID in `map.json`; the `(x,y)` name is
  only a fallback. Numbered workspaces are never added to the grid unless you
  `adopt` them.
- Existing neighbour cells slide in with their windows. A brand-new cell's
  workspace is only created if the swipe commits, so a cancelled swipe leaves
  nothing behind.
- When a swipe does nothing, a notification explains why (not a grid cell,
  cell on another monitor, conflicting gesture, …).
- Ignored while the session is locked.
- One global grid, three fingers, no modifier keys.

## Troubleshooting

Test from a TTY-accessible session; a native plugin bug can crash Hyprland.

- **No notification at all:** the plugin did not load. Check
  `hyprctl plugin list`. If `ecosystem:enforce_permissions` is on, approve the
  plugin-load prompt.
- **"version mismatch":** Hyprland was updated. Log out and back in, then rerun
  `./install-arch.sh`.
- **"…take priority for these swipes…":** remove the 3-finger gestures it
  names and save the config.
- **"…is not a cell in …map.json":** the current workspace is not in the grid.
  Jump in with SUPER+G or `hyprctl hyprgrid goto 0,0`.
- **Three-finger swipes move the pointer:** libinput three-finger drag
  (`input:touchpad:drag_3fg`) is on, so the swipes never reach gestures.

Logs: `hyprctl rollinglog -f` (plugin lines mention `hyprgrid-native`).

## Nix

`flake.nix` / `default.nix` are kept for Nix users (`nix build` →
`result/lib/libhyprgrid-native.so`); the flake is pinned to 0.56.2. Arch users
can ignore them.

## Changes in 0.7.1

- `hyprgrid-goto send|take` remembers the focused window before the menu opens
  (the menu steals focus), and shows any error as a notification.
- `send` accepts `window=0xADDRESS` to move a specific window.

## Changes in 0.7.0

- `hyprctl hyprgrid send X,Y|direction [follow]` moves the focused window to a
  cell; `hyprgrid-goto send|take` prompts for the cell.

## Changes in 0.6.0

- `plugin:hyprgrid:invert_x` / `invert_y` flip the swipe direction per axis.

## Changes in 0.5.0

- `hyprctl hyprgrid goto X,Y` and the `hyprgrid-goto` prompt (SUPER+G); SUPER+X
  for home `(0,0)`.
- `hyprctl hyprgrid release` takes a cell out of the grid.
- Numbered workspaces are no longer turned into `(0,0)` automatically.

## Changes in 0.4.0

- `hyprctl hyprgrid adopt` / `adopt-all` add ordinary numbered workspaces to the
  grid; `list` shows the ones still outside it.

## Changes in 0.3.0

- `hyprctl hyprgrid shift|move|swap|list` to move whole cells around the grid.
- A workspace whose `(x,y)` name is out of date is renamed instead of being
  refused as a swipe target.

## Changes in 0.2.0

- Builds on Hyprland 0.57.0 (new workspace API) as well as 0.56.2.
- Arch build script; quiet, flag-correct Makefile.
- Fixed in 0.1.1: the gesture was deleted by the config reload Hyprland runs
  after loading a plugin; the version check compared pointers and always
  failed; the code did not compile; `nix build` had no CMakeLists.txt.
