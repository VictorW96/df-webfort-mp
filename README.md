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
