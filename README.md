## Web Fortress ##

Web Fortress is a [DFHack](http://github.com/dfhack/dfhack) plugin that
exposes the rendering and input of a game of
[Dwarf Fortress](http://bay12games.com) over a websocket, and an HTML5
client that allows players to join in a shared game over their browsers.

This fork targets **Dwarf Fortress 53.x (the Steam release, v50+)** and the
corresponding DFHack release. It is based on the original
[df-webfort](https://github.com/mifki/df-webfort) plugin by Vitaly Pronkin.

> **Note:** Only **ASCII graphics mode** is currently supported. Dwarf Fortress
> must be launched with the classic ASCII renderer; sprite graphics mode
> are not yet supported.

### Quick Install (Recommended) ###

Pre-built binaries for **Linux** and **Windows** (64-bit) are available on the
[Releases page](https://github.com/VictorW96/df-webfort-mp/releases).

1. Download the zip for your platform (`-linux.zip` or `-windows.zip`).
2. Extract the zip **directly into your Dwarf Fortress install folder**.
3. Launch Dwarf Fortress, then type in the DFHack console:

		enable webfort

4. Open your browser and go to `http://localhost:1234/webfort.html`.

The page URL accepts query parameters to customise the connection:

| Parameter  | Default                          | Description                              |
|------------|----------------------------------|------------------------------------------|
| `host`     | current hostname                 | Server host to connect to                |
| `port`     | `1234`                           | Server port                              |
| `nick`     | *(empty)*                        | Display name shown to other players      |
| `secret`   | *(empty)*                        | Password if the server requires one      |
| `tiles`    | `df53_curses_square_16x16.png`   | Tileset image for map tiles              |
| `text`     | `df53_curses_square_16x16.png`   | Tileset image for text                   |
| `colors`   | *(server default)*               | Colour scheme name (from `web/colors/`)  |
| `show-fps` | *(off)*                          | Set to show the FPS counter              |

Example: `http://localhost:1234/webfort.html?nick=Alice&colors=VheridDusk&show-fps`

Example using the Phoebus tileset: `http://localhost:1234/webfort.html?tiles=Phoebus.png&text=t_Phoebus.png`

Available tilesets (in `web/art/`): `Curses.png`, `Phoebus.png`, `Mayday.png`, `Spacefox.png`, `Ironhand.png`, `Obsidian.png`, `SimpleMood.png`, `ShizzleClean.png` and more. Paired text variants use a `t_` prefix (e.g. `t_Phoebus.png`).

See [INSTALLING.txt](INSTALLING.txt) for port forwarding and other details.

### Architecture (DF v50+ / Steam release) ###

This fork makes several structural changes relative to the original
[df-webfort](https://github.com/mifki/df-webfort) and the intermediate
[Ankoku fork](https://github.com/Ankoku/df-webfort), all driven by breaking
changes in Dwarf Fortress 50+.

#### Screen capture — `gps->screen` polling instead of renderer subclassing

Older builds subclassed `df::renderer` (via `renderer_wrap.hpp`) and
intercepted rendering calls at the OpenGL/SDL-blit level.  The `df::renderer`
vtable was overhauled in DF v50 and no longer has the same hook points, so
that approach no longer compiles or works.

This fork instead polls `gps->screen` directly in `plugin_onupdate`, *after*
both the active viewscreen's `render()` and DFHack's overlay widgets have
run.  That ordering is guaranteed by DF's own async render loop, so the
snapshot always includes the fully-composited frame.

#### Three-path tile compositing

DF 53.12 writes tile data through three separate buffers that must be merged:

1. **`gps->screen` / `addchar`** — the primary buffer; each tile is 8 bytes
   (`[char, fg_R, fg_G, fg_B, bg_R, bg_G, bg_B, pad]`).
2. **`gps->screentexpos` / `add_tile`** — a parallel array of texture-atlas
   slot indices used for graphical map sprites and dialog border glyphs.
   Tiles that use this path leave `screen[0]` as 0 or space.  The plugin
   reverse-maps each slot back to a CP437 character using
   `init->font.large_font_texpos[]`.
3. **`gps->screen_top`** — a second full-screen overlay for dialog paragraph
   text.  Only tiles where `screen_top[0] > 32` are composited on top of the
   base screen, so border characters resolved by path 2 are not overwritten.

Pre-v50 builds only had to read a single flat screen buffer with one-byte
color indices; the three-path merge is entirely new to this fork.

#### Color representation — RGB to indexed

In DF v50+ the screen array stores raw RGB triples per tile instead of the
16-color CGA indices the wire protocol uses.  The plugin reverse-maps each
RGB triple against `gps->uccolor` (the CGA palette DF derives its colors
from) with a nearest-neighbor search so the existing client-side palette
logic is preserved unchanged.

#### Virtual mouse — vtable interpose hooks + `gps->precise_mouse_*`

The old plugin wrote directly to the SDL mouse state.  DF v50 added a
separate `gps->mouse_x/y` (tile-level) and `gps->precise_mouse_x/y`
(pixel-level) pair that the game reads instead of polling SDL.  This fork
stamps those fields via `DEFINE_VMETHOD_INTERPOSE` hooks on
`viewscreen_dwarfmodest::render` and `viewscreen_dungeonmodest::render`,
ensuring the virtual cursor is in place before each native render call.

#### Input — SDL 2 event queue instead of SDL 1.2 direct calls

The original plugin called SDL 1.2 key-injection APIs directly from the
WebSocket thread.  This fork:

- Translates browser key codes to a stable internal `SDL::Key` vocabulary in
  `input.hpp` (matching the original mapping so `server.cpp` is unchanged).
- Enqueues translated events onto a mutex-protected queue on the WebSocket
  thread.
- Drains the queue on the DF main thread in `plugin_onupdate` and pushes each
  event as a proper SDL 2 event via `DFHack::DFSDL::DFSDL_PushEvent()`.

This removes the data-race that existed when the old code called into DF
state from a background thread.

#### Viewscreens and globals

Many viewscreens used by earlier forks (`viewscreen_layer_export_play_mapst`,
etc.) were removed or renamed in DF v50.  Dead screen checks have been
dropped; only viewscreens that still exist in 53.12 are referenced.
`df::global::ui` was replaced by `df::global::plotinfo` throughout.

#### Per-client independent cameras (new feature)

Each connected client now carries its own `cam_x/y/z` viewport offset and a
`has_own_cam` flag.  On the first camera-move opcode the plugin triggers an
extra render pass for that client using its own viewport coordinates,
producing an independent `own_sc` framebuffer that is delta-compressed and
sent separately from the shared global frame.  This feature has no equivalent
in any earlier fork.

### Compiling from Source ###

Web Fortress is known to compile with 64-bit gcc/clang on Linux, and recent
MSVC on Windows. See [COMPILING.md](COMPILING.md) for more.

### Authors and Links ###

[Original Source](https://github.com/mifki/df-webfort) -- [This Fork](https://github.com/VictorW96/df-webfort-mp) -- [Discussion](http://www.bay12forums.com/smf/index.php?topic=139167.0) -- [Report an Issue](https://github.com/VictorW96/df-webfort-mp/issues)

Copyright (c) 2014, Vitaly Pronkin <pronvit@me.com>
Copyright (c) 2014, Kyle McLamb <alloyed@tfwno.gf>
Copyright (c) 2026, Victor Wolf

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
