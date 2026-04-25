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

### Downloading ###

This project manages dependencies through Git submodules. To clone the full
tree:

	git clone --recursive <the git repo>

Windows binaries are provided through [Github](https://github.com/VictorW96/df-webfort-mp/releases).

### Compiling ###

Web Fortress is known to compile with 64-bit gcc/clang on Linux, and recent
MSVC on Windows. See [COMPILING.md](COMPILING.md) for more.

### Installation ###

Installation is documented in [INSTALLING.txt](INSTALLING.txt), which
should come with your release of Web Fortress.

### Authors and Links ###

[Original Source](https://github.com/mifki/df-webfort) -- [Fork](https://github.com/VictorW96/df-webfort-mp) -- [Discussion](http://www.bay12forums.com/smf/index.php?topic=139167.0) -- [Report an Issue](https://github.com/VictorW96/df-webfort-mp/issues)

Copyright (c) 2014, Vitaly Pronkin <pronvit@me.com>
Copyright (c) 2014, Kyle McLamb <alloyed@tfwno.gf>

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
