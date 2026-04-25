# Graphics Mode Feasibility Analysis
## webfort plugin — DF 53.12 + multiplayer

---

## Current Implementation Overview

The existing architecture has four tiers:

**1. Screen capture** (`capture_screen_buffer()`)  
Three-path compositing handles all DF 53.12 rendering paths:
- Path 1 (`addchar`): `screen[0]` = char code, directly used
- Path 2 (`add_tile`): `screentexpos[tile]` = texpos, reverse-mapped via `init->font.large_font_texpos[256]`
- Path 3 (`screen_top`): dialog paragraph overlay, composited only for tiles with `ch > 32`

**2. Wire protocol** (`tock()` in server.cpp)  
9 bytes per changed tile: `x, y, char, bg_flags, fg, texpos_lo, texpos_hi, texpos_lower_lo, texpos_lower_hi`.  
A 4-bit atlas version counter in the tock header triggers client re-fetches after world loads or graphics pack changes.

**3. Atlas builder** (`wf_build_atlas()` in webfort.cpp)  
Iterates `enabler->textures.raws[]`, blits valid `SDL_Surface` pixels into a single PNG atlas, serializes `texpos → [atlas_x, atlas_y]` as JSON. Rebuilt every 60 ticks and on mode switches.

**4. Browser renderer** (`renderUpdate()` in webfort.js)  
If `graphicsMode && texpos > 0 && atlasImg`: draw lower + upper sprites from atlas. Falls back to CP437 tile rendering otherwise.

The **ASCII multiplayer skeleton** (turn-taking opcode 116, cursor broadcast opcode 112, ghost cursors, per-client delta mod-mask) is solid and does not need major revision.

---

## Critical Blocker: GPU Upload Frees Surface Pixels

This is the fundamental problem with the current graphics approach.

The `enabler_textures` struct (confirmed in `library/xml/df.g_src.enabler.xml`) has an `uploaded` flag:

```xml
<struct-type type-name='enabler_textures' original-name='textures'>
    <stl-vector type-name='pointer' name='raws' comment='SDL_Surface'/>
    <stl-vector type-name='int32_t' name='free_spaces'/>
    <int32_t name='init_texture_size'/>
    <bool name='uploaded'/>
</struct-type>
```

When DF runs with `USE_GRAPHICS=true` (OpenGL), it uploads every surface via `glTexImage2D` and then **frees the CPU-side pixel data**. After upload, `enabler->textures.raws[i]->pixels == nullptr` for all game sprites.

The disk fallback in `wf_build_atlas()` only helps CP437 glyph sprites (reverse-mapped from `init->font.large_font_texpos[256]`). It cannot recover:
- Creature sprites
- Tile sprites  
- Item sprites
- Runtime-generated sprites (recolored variants using the `DO_RECOLOR` flag)

None of these have a recoverable art-file representation via the `data/art/` scan.

**Result**: After a world loads and GPU upload completes, the atlas is empty for all game-map sprites. The browser falls through to CP437, rendering garbage characters on the map.

The brief window where surfaces are in RAM (between texture load and GPU upload) is not reliably exploitable — by the time a user connects a browser, a world has typically already been loaded and uploaded.

---

## Secondary Problems

### Atlas build cost on the main thread
`lodepng::encode()` runs synchronously on the DF main thread every 60 ticks. A 2000-sprite pack at 16×16 = ~2 MB RGBA encodes in 50–100 ms. With a 32×32 HD pack this doubles. This causes a visible stutter cadence during gameplay.

### Atlas instability across world loads
`texpos` values are runtime-assigned and re-ordered on each world load. The 4-bit version counter handles invalidation, but every world load forces all connected clients to re-fetch the atlas (~500 KB HTTP per client). For a multiplayer session with 5 spectators this means 5 simultaneous large fetches on each world reload.

### uint16 texpos ceiling
The wire protocol stores `screentexpos` as `uint16_t`, capping at 65 535. A heavily-modded install with multiple graphics packs can exceed this. Low risk for vanilla, correctness issue for modded play.

### Disk fallback directory scan is fragile
The current code iterates `data/art/*.png` looking for 256-glyph sheets. The vanilla DF install has 27 art files including 1920×1080 title backgrounds and small UI elements — none are 256-glyph character sheets. The scan succeeds for pure ASCII character sheets (`curses_640x300.png` at 128×192) but finds nothing for game sprites. It would also break with modded art directories that use non-standard layouts.

---

## Comparison with dfplex

dfplex (the previous multiplayer mod, DF 0.34/0.40 era) worked by intercepting the **OpenGL framebuffer** via `glReadPixels()`, compressing each frame as a JPEG, and streaming pixel buffers over WebSocket.

| | webfort tile-differential | dfplex framebuffer |
|---|---|---|
| GPU-agnostic | ✗ (blocked by GPU upload) | ✓ |
| Graphics-mode agnostic | ✗ (requires atlas) | ✓ |
| Text quality | ✓ crisp CP437 | ✗ lossy JPEG blur |
| Per-client bandwidth | ✓ very low (delta tiles) | ✗ high (full frames) |
| Spectator scaling | ✓ excellent | ✗ linear with viewers |
| Latency | ✓ low (tile deltas) | ✓ acceptable |
| Implementation complexity | Medium | Low (one `glReadPixels` hook) |

The webfort tile-differential approach is strictly better for ASCII mode. For graphics mode, it is more complex with diminishing returns until the GPU-surface problem is solved.

---

## Options to Fix Graphics Mode

### Option A — Hook before GPU upload
Intercept the texture manager's upload path to copy pixel data to a plugin-owned buffer before the surface is freed. Requires a `DEFINE_VMETHOD_INTERPOSE` on `enabler_textures::upload()` or its SDL/OpenGL wrapper.

**Feasibility: Medium difficulty, fragile.**  
The hook point is not in the current DFHack XML definitions, so it requires a hard-coded vtable offset. Breaks on DF updates.

---

### Option B — Force software renderer
Setting `[PRINT_MODE:SOFTWARE]` in `data/init/init.txt` keeps all `SDL_Surfaces` alive. The current `wf_build_atlas()` would work correctly with zero code changes.

**Feasibility: Trivially easy, but limits user experience.**  
DF runs noticeably slower in software mode and is incompatible with the "Premium" graphical experience. Viable as a temporary development scaffold to validate the end-to-end browser pipeline before solving the GPU problem.

---

### Option C — Map texpos → (art_file, col, row) at load time ✓ *Recommended*

**The key insight:** DF loads all graphics from `data/art/*.png` and mod PNG files at startup. Those files never change during a session. The `texpos → (file, tile_index)` mapping is established when `add_texture()` is called during texture loading — *before* GPU upload. If we intercept that call, we can build a permanent reverse map that survives GPU upload.

**Approach:**
1. Hook `df::enabler_textures::add_texture()` (or the SDL `SDL_CreateTextureFromSurface` equivalent) to record `texpos → (filename_index, tile_col, tile_row)` at load time.
2. Serve art PNG files at `/df-art/<fname>` (already implemented in server.cpp).
3. Send a compact `(file_index uint8, tile_index uint16)` per tile instead of raw texpos — same or smaller than the current 4-byte screentexpos field.
4. Browser fetches art files once on connect, caches them via the browser cache, renders using `(file, col, row)` offsets.

**Advantages over current approach:**
- No GPU-surface dependency.
- No main-thread PNG encode cost.
- Art files are HTTP-cacheable across sessions and world loads — no re-fetch storm on world load.
- Per-tile wire data is 3 bytes vs current 4 bytes.
- Correct for all sprite types including recolored variants (the source PNG is available even if the recolored surface was freed).

**Open research task:**  
Identify the correct hook point for `add_texture()` in DF 53.12. It is likely a member of `enabler_textures` or a free function in `texture_fullid`-related code. The struct is partially defined in `library/xml/df.g_src.enabler.xml` but the method is not yet mapped. Reverse-engineering `dwarfort` with `readelf -sW` or `objdump -d` to find the call site is the required next step.

**Feasibility: Medium–high difficulty, correct and robust once implemented.**

---

### Option D — Keep current code, document limitations
The current atlas approach works during the pre-GPU-upload window (i.e., software renderer mode) and does not break ASCII mode. For near-term multiplayer work this is acceptable.

**Feasibility: Zero additional work.** Viable as a hold position.

---

## Recommended Roadmap

### Near-term (multiplayer focus)
- **ASCII mode is fully functional.** Ship the multiplayer turn-taking and cursor-broadcast features against ASCII mode. The infrastucture is already correct.
- The graphics code is cleanly guarded by `g_graphics_mode` and does not regress ASCII behavior.
- Consider `wf_build_atlas()` a proof-of-concept that validates the browser-side atlas rendering path only.

### Medium-term (graphics support)
1. **Validate end-to-end with Option B** (software renderer). This confirms the browser atlas path, JS sprite rendering, and texpos wire encoding all work before touching the GPU upload problem.
2. **Research the `add_texture()` hook point** in `dwarfort`. This is a one-time reverse-engineering task.
3. **Implement Option C** once the hook point is confirmed. Replace `wf_build_atlas()` with a load-time registration table. Simplify the browser renderer to use `(file_index, tile_index)` offsets.

### Long-term
- Move `lodepng::encode` (if still used) off the main thread to a dedicated encoder thread.
- Extend the wire protocol `texpos` field from `uint16_t` to `uint32_t` to support heavily-modded installs.
- Consider streaming individual changed art tiles on demand rather than a monolithic atlas, to reduce world-load latency for new clients.

---

## Key File Reference

| File | Role |
|------|------|
| `plugins/external/webfort/server/webfort.cpp` | Screen capture, atlas builder, input injection, plugin lifecycle |
| `plugins/external/webfort/server/server.cpp` | WebSocket server, `tock()` wire encoding, HTTP static file serving |
| `plugins/external/webfort/server/webfort.hpp` | Shared globals (`sc[]`, atlas, `g_graphics_mode`) |
| `plugins/external/webfort/server/server.hpp` | `Client` struct, `conn_map`, `WFServer` |
| `plugins/external/webfort/static/js/webfort.js` | Browser renderer, atlas fetching, CP437 + sprite rendering |
| `library/xml/df.g_src.enabler.xml` | `enabler_textures` struct definition (confirms `uploaded` flag) |
| `library/xml/df.g_src.init.xml` | `large_font_texpos[256]`, `USE_GRAPHICS` flag |
| `library/xml/df.g_src.graphics.xml` | `tile_pixel_x/y`, `screentexpos`, `screen_top` fields |
| `.github/skills/df53-rendering/SKILL.md` | DF 53.12 rendering pipeline reference (3-path compositing) |
