/*
 * webfort.cpp — DFHack 53.12 plugin entry point.
 *
 * Reads gps->screen (and screen_top / screentexpos overlays) each tick,
 * composes the three DF rendering paths into the shared `sc` buffer, and
 * serves it to connected browser clients via the WebSocket server in
 * server.cpp.  Input events received on the WebSocket thread are queued
 * here and drained on the DF main thread to avoid data races.
 *
 * Screen capture: capture_screen_buffer() is called from plugin_onupdate
 * after both viewscreen::render() and DFHack overlay widgets have run,
 * so the snapshot always includes the fully composited frame.
 *
 * Mouse: virtual cursor coords are stamped into gps->mouse_x/y via
 * DEFINE_VMETHOD_INTERPOSE hooks before each render, and re-stamped
 * every tick in plugin_onupdate to keep hover state alive.
 *
 * Server lifecycle: plugin_enable() starts/stops the WebSocket server;
 * plugin_init() registers globals but does not start the server.
 */

#include <cassert>
#include <climits>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <atomic>

#include "Core.h"
#include "Console.h"
#include "Export.h"
#include "PluginManager.h"
#include "VTableInterpose.h"

#include "modules/DFSDL.h"
#include "modules/Gui.h"
#include "modules/Maps.h"
#include "modules/Screen.h"

#include <SDL_events.h>
#include <SDL_keyboard.h>
#include <SDL_keycode.h>
#include <SDL_surface.h>

#include "df/graphic.h"
#include "df/enabler.h"
#include "df/global_objects.h"
#include "df/init.h"
#include "df/interface_key.h"
#include "df/plotinfost.h"
#include "df/ui_sidebar_mode.h"
#include "df/viewscreen.h"
#include "df/viewscreen_choose_start_sitest.h"
#include "df/viewscreen_dungeonmodest.h"
#include "df/viewscreen_dwarfmodest.h"
#include "df/viewscreen_new_regionst.h"
#include "df/viewscreen_setupadventurest.h"

#include "webfort.hpp"
#include "server.hpp"
#include "input.hpp"

#include "lodepng.h"

using namespace DFHack;
using std::string;
using std::vector;

using df::global::cursor;
using df::global::enabler;
using df::global::gps;
using df::global::init;
using df::global::plotinfo;
using df::global::ui_menu_width;
using df::global::window_x;
using df::global::window_y;
using df::global::window_z;
using df::global::world;

DFHACK_PLUGIN("webfort");
DFHACK_PLUGIN_IS_ENABLED(enabled);

REQUIRE_GLOBAL(enabler);
REQUIRE_GLOBAL(gps);
REQUIRE_GLOBAL(init);
REQUIRE_GLOBAL(plotinfo);
REQUIRE_GLOBAL(ui_menu_width);
REQUIRE_GLOBAL(window_x);
REQUIRE_GLOBAL(window_y);
REQUIRE_GLOBAL(window_z);
REQUIRE_GLOBAL(world);

// -----------------------------------------------------------------------------
// Render hooks — virtual mouse stamping
//
// Priority 200 > DFHack overlay plugin's priority 100:
//   webfort(200) → overlay(100) → native render()
//
// These hooks do NOT capture the screen. DF's graphics.cpp calls:
//   1. currentscreen->render(curtick)      ← vtable chain, hooks fire here
//   2. currentscreen->widgets.render()     ← DFHack overlay widgets draw HERE
// Any vtable-hook capture would miss the widgets.render() content.
//
// Instead, plugin_onupdate captures after both calls have completed. In DF's
// async_loop, the render command (render_things) is always processed BEFORE
// mainloop() runs for the same frame, so plugin_onupdate sees the fully
// composited gps->screen including all overlay widgets.
//
// The hooks' only job is to stamp the virtual mouse coordinates into gps
// BEFORE INTERPOSE_NEXT, so DF's render code reads our coords (not the
// physical mouse position from SDL_GetMouseState which ran on the main thread
// just before the render command was dispatched).
// -----------------------------------------------------------------------------

static void capture_screen_buffer(); // forward-declare

// Last known virtual mouse tile, persisted across ticks. -1 = no cursor yet.
static int g_wf_mouse_tile_x = -1;
static int g_wf_mouse_tile_y = -1;

// Stamp virtual mouse coords into gps at the start of a render hook.
static inline void wf_stamp_mouse()
{
    if (!enabled || g_wf_mouse_tile_x < 0 || !gps || !init || !enabler)
        return;
    const int32_t tile_px_w = init->display.grid_x ? init->display.grid_x : 16;
    const int32_t tile_px_h = init->display.grid_y ? init->display.grid_y : 16;
    gps->mouse_x = g_wf_mouse_tile_x;
    gps->mouse_y = g_wf_mouse_tile_y;
    gps->precise_mouse_x = g_wf_mouse_tile_x * tile_px_w + tile_px_w / 2;
    gps->precise_mouse_y = g_wf_mouse_tile_y * tile_px_h + tile_px_h / 2;
    enabler->tracking_on = 1;
}

struct wf_dwarfmode_render_hook : df::viewscreen_dwarfmodest {
    typedef df::viewscreen_dwarfmodest interpose_base;
    DEFINE_VMETHOD_INTERPOSE(void, render, (uint32_t flags))
    {
        if (!g_wf_in_extra_render)
            wf_stamp_mouse();
        INTERPOSE_NEXT(render)(flags);
    }
};
IMPLEMENT_VMETHOD_INTERPOSE_PRIO(wf_dwarfmode_render_hook, render, 200);

struct wf_dungeonmode_render_hook : df::viewscreen_dungeonmodest {
    typedef df::viewscreen_dungeonmodest interpose_base;
    DEFINE_VMETHOD_INTERPOSE(void, render, (uint32_t flags))
    {
        if (!g_wf_in_extra_render)
            wf_stamp_mouse();
        INTERPOSE_NEXT(render)(flags);
    }
};
IMPLEMENT_VMETHOD_INTERPOSE_PRIO(wf_dungeonmode_render_hook, render, 200);

static void wf_apply_render_hooks(bool enable)
{
    INTERPOSE_HOOK(wf_dwarfmode_render_hook, render).apply(enable);
    INTERPOSE_HOOK(wf_dungeonmode_render_hook, render).apply(enable);
}

// Shared with server.cpp via server.hpp externs.
unsigned char sc[256*256*8];
// Previous-frame snapshot so we can compute per-tile deltas.
static unsigned char sc_prev[256*256*8];

// Sprite atlas globals (built on main thread, read by HTTP thread).
std::mutex           g_atlas_mutex;
std::vector<uint8_t> g_atlas_png;
std::string          g_atlas_json;
// Number of valid (non-null, has pixels) surfaces the last atlas was built from.
static size_t g_atlas_last_valid_count = 0;
// Last raws.size() for which the atlas was built.
static size_t g_atlas_last_raws_size = 0;
// Last graphics-mode flag for which the atlas was built.
static bool   g_atlas_last_gfx_mode  = false;

// Current graphics mode (USE_GRAPHICS flag). Updated in plugin_onupdate().
bool g_graphics_mode = false;
// Set true during per-client extra render passes so the render hooks
// skip wf_stamp_mouse() (which would overwrite the virtual cursor).
bool g_wf_in_extra_render = false;
// Camera deltas queued by the active player on the WS thread.
// Drained on the DF main thread in plugin_onupdate().
std::atomic<int32_t> g_active_cam_dx{0};
std::atomic<int32_t> g_active_cam_dy{0};
std::atomic<int32_t> g_active_cam_dz{0};
// Atlas version counter — incremented each time a new atlas is successfully
// built. Sent in tock() header bits[4..7] (4-bit, wraps 0-15).
uint8_t g_atlas_version = 0;
static int32_t sc_prev_dimx = -1;
static int32_t sc_prev_dimy = -1;

static WFServer* s_hdl = nullptr;

// -----------------------------------------------------------------------------
// Helpers used by server.cpp
// -----------------------------------------------------------------------------

#define IS_SCREEN(_sc) (id == &df::_sc::_identity)

/*
 * Detects if it is safe for a non-privileged user to trigger an ESC keybind.
 * It should not be safe if it would lead to the menu normally accessible by
 * hitting ESC in dwarf mode, as this would give access to keybind changes,
 * fort abandonment etc.
 */
bool is_safe_to_escape()
{
    df::viewscreen* ws = Gui::getCurViewscreen();
    const virtual_identity* id = virtual_identity::get(ws);
    if (IS_SCREEN(viewscreen_dwarfmodest) &&
        plotinfo->main.mode == df::ui_sidebar_mode::Default) {
        return false;
    }
    // TODO: adventurer mode
    if (IS_SCREEN(viewscreen_dungeonmodest)) {
    }
    return true;
}

void show_announcement(std::string announcement)
{
    DFHack::Gui::showAnnouncement(announcement);
}

void quicksave(DFHack::color_ostream* out)
{
    Core::getInstance().runCommand(*out, "quicksave");
}

/*
 * Text-tile detection. Ported from the original with `ui->` replaced by
 * `plotinfo->` and dead viewscreens removed. Only kept for the screens we
 * could verify still exist in 53.12.
 */
static bool is_text_tile(int x, int y, bool &is_map)
{
    df::viewscreen* ws = Gui::getCurViewscreen();
    const virtual_identity* id = virtual_identity::get(ws);
    assert(ws != NULL);

    int32_t w = gps->dimx, h = gps->dimy;

    is_map = false;

    if (!x || !y || x == w - 1 || y == h - 1)
        return true;

    if (IS_SCREEN(viewscreen_dwarfmodest))
    {
        // Read the live menu-width values from DF's own global (menuposition[0/1]).
        // Falls back to conservative defaults if the global is not available.
        uint8_t menu_width     = ui_menu_width ? (*ui_menu_width)[0] : 2;
        uint8_t area_map_width = ui_menu_width ? (*ui_menu_width)[1] : 3;
        int32_t menu_left = w - 1, menu_right = w - 1;

        bool menuforced = (plotinfo->main.mode != df::ui_sidebar_mode::Default ||
                           (cursor && cursor->x != -30000));

        if ((menuforced || menu_width == 1) && area_map_width == 2) {
            menu_left = w - 56;
            menu_right = w - 25;
        }
        else if (menu_width == 2 && area_map_width == 2) {
            menu_left = menu_right = w - 25;
        }
        else if (menu_width == 1) {
            menu_left = w - 56;
        }
        else if (menuforced || (menu_width == 2 && area_map_width == 3)) {
            menu_left = w - 32;
        }

        if (x >= menu_left && x <= menu_right)
        {
            if (menuforced &&
                plotinfo->main.mode == df::ui_sidebar_mode::Burrows &&
                plotinfo->burrows.in_define_mode)
            {
                if ((y != 12 && y != 13 && !(x == menu_left + 2 && y == 2)) ||
                    x == menu_left || x == menu_right)
                    return true;
            }
            else {
                return true;
            }
        }

        is_map = (x > 0 && x < menu_left);
        return false;
    }

    if (IS_SCREEN(viewscreen_dungeonmodest))
    {
        if (y >= h - 2)
            return true;
        return false;
    }

    if (IS_SCREEN(viewscreen_setupadventurest))
    {
        // Subscreen enum layout has shifted; be conservative and render as text.
        return true;
    }

    if (IS_SCREEN(viewscreen_choose_start_sitest))
    {
        if (y <= 1 || y >= h - 6 || x == 0 || x >= 57)
            return true;
        return false;
    }

    if (IS_SCREEN(viewscreen_new_regionst))
    {
        if (y <= 1 || y >= h - 2 || x <= 37 || x == w - 1)
            return true;
        return false;
    }

    return true;
}

// -----------------------------------------------------------------------------
// Screen capture (replaces the old renderer subclass)
// -----------------------------------------------------------------------------

// Fill dst[] (8 bytes per tile) from gps->screen. Pure fill — no delta
// tracking and no mod updates. Safe to call during extra render passes.
// Returns false and leaves dst unchanged if gps is not ready.
static bool fill_screen_buffer(uint8_t* dst)
{
    if (!gps || !gps->screen)
        return false;

    const int32_t dimx = gps->dimx;
    const int32_t dimy = gps->dimy;
    if (dimx <= 0 || dimy <= 0 || dimx > 256 || dimy > 256)
        return false;

    // gps->screen layout in DF >= 50 (Steam release) is 8 bytes per tile:
    //   [0]       character
    //   [1..3]    foreground RGB
    //   [4..6]    background RGB
    //   [7]       reserved / padding
    // The webfort wire protocol still uses 0..15 indexed curses colors, so
    // we reverse-map the RGB triples against gps->uccolor (the CGA palette
    // DF uses for non-graphics display modes).
    auto match_color = [](uint8_t r, uint8_t g, uint8_t b) -> uint8_t {
        int best_i = 0;
        int best_d = INT32_MAX;
        for (int i = 0; i < 16; i++) {
            int dr = int(gps->uccolor[i][0]) - int(r);
            int dg = int(gps->uccolor[i][1]) - int(g);
            int db = int(gps->uccolor[i][2]) - int(b);
            int d = dr*dr + dg*dg + db*db;
            if (d < best_d) { best_d = d; best_i = i; }
        }
        return (uint8_t)best_i;
    };

    const unsigned char* src = gps->screen;

    // Build a reverse map: screentexpos value → CP437 character code.
    // See the three-path compositing notes in the SKILL.md for details.
    std::unordered_map<long, uint8_t> texpos_to_char;
    if (init) {
        for (int ch = 1; ch < 256; ch++) {
            long tp = init->font.large_font_texpos[ch];
            if (tp > 0) texpos_to_char.emplace(tp, (uint8_t)ch);
        }
    }

    for (int32_t x = 0; x < dimx; x++) {
        for (int32_t y = 0; y < dimy; y++) {
            const int tile = x * dimy + y;
            unsigned char* td = dst + tile * 8;       // tile destination
            const unsigned char* s = src + tile * 8;

            // Determine the character code for this tile.
            //
            // DF 53.12 uses three distinct paths depending on context:
            //
            // 1. addchar(ch): screen[0]=ch, screentexpos=0. Covers most UI
            //    text and plain game tiles. Handled by td[0]=s[0] below.
            //
            // 2. add_tile(texpos): screentexpos=texpos, screen[0] left as 0
            //    or 32. Used for graphical map sprites AND dialog border/button
            //    glyphs in graphics mode. We reverse-map the texpos back to
            //    the CP437 character code via large_font_texpos[].
            //
            // 3. screen_top overlay (top_in_use): DF renders dialog paragraph
            //    text into screen_top with ch>32 and non-zero RGB. The base
            //    screen at those positions is blank (ch=32/0, RGB=0). We
            //    overlay only tiles where screen_top has ch>32, which skips
            //    the erased tiles (ch=32, RGB=0) that fill the rest of
            //    screen_top and would otherwise wipe out base-screen border
            //    chars resolved by path 2.
            const unsigned char* src_top =
                (gps->top_in_use && gps->screen_top) ? gps->screen_top : nullptr;

            uint8_t char_code = s[0];
            uint8_t fg_r = s[1], fg_g = s[2], fg_b = s[3];
            uint8_t bg_r = s[4], bg_g = s[5], bg_b = s[6];

            // Path 3: screen_top text overlay.
            if (src_top) {
                const unsigned char* st = src_top + tile * 8;
                if (st[0] > 32) {
                    char_code = st[0];
                    fg_r = st[1]; fg_g = st[2]; fg_b = st[3];
                    bg_r = st[4]; bg_g = st[5]; bg_b = st[6];
                }
            }

            // Path 2: screentexpos reverse-map, when char_code is still blank.
            if (char_code <= 32 && gps->screentexpos) {
                long tp = gps->screentexpos[tile];
                if (tp > 0) {
                    auto it = texpos_to_char.find(tp);
                    if (it != texpos_to_char.end())
                        char_code = it->second;
                }
            }

            uint8_t fg_idx = match_color(fg_r, fg_g, fg_b);
            uint8_t bg_idx = match_color(bg_r, bg_g, bg_b);

            td[0] = char_code;
            td[1] = fg_idx & 0x7;
            td[2] = bg_idx & 0x7;
            td[3] = (fg_idx & 0x8) ? 1 : 0;

            bool is_map;
            if (is_text_tile(x, y, is_map))
                td[2] |= 64;

            // Store raw screentexpos values (uint16 LE) for sprite rendering.
            long raw_tp  = gps->screentexpos       ? gps->screentexpos[tile]       : 0L;
            long raw_tpl = gps->screentexpos_lower  ? gps->screentexpos_lower[tile] : 0L;
            uint16_t tp  = (raw_tp  > 0 && raw_tp  < 65536) ? (uint16_t)raw_tp  : 0u;
            uint16_t tpl = (raw_tpl > 0 && raw_tpl < 65536) ? (uint16_t)raw_tpl : 0u;
            td[4] = (uint8_t)(tp  & 0xFF);
            td[5] = (uint8_t)(tp  >> 8);
            td[6] = (uint8_t)(tpl & 0xFF);
            td[7] = (uint8_t)(tpl >> 8);
        }
    }
    return true;
}

// Capture the current gps->screen into the global sc[] buffer, compute
// per-client tile deltas, and synchronise own_sc for clients that share
// the global viewport (has_own_cam == false).
static void capture_screen_buffer()
{
    if (!gps || !gps->screen)
        return;

    const int32_t dimx = gps->dimx;
    const int32_t dimy = gps->dimy;
    if (dimx <= 0 || dimy <= 0 || dimx > 256 || dimy > 256)
        return;

    const bool dims_changed =
        (dimx != sc_prev_dimx) || (dimy != sc_prev_dimy);

    fill_screen_buffer(sc);

    // For each client that shares the global viewport, copy sc to their
    // own_sc (tock always reads from own_sc) and mark changed tiles dirty.
    const int total = dimx * dimy;
    for (auto& kv : clients) {
        Client* cl = kv.second;
        if (!cl || cl->has_own_cam) continue;

        // Keep own_sc in sync so tock() can always read from it.
        if (cl->own_sc) std::memcpy(cl->own_sc, sc, (size_t)total * 8);

        // Invalidate only tiles that actually changed.
        if (dims_changed) {
            std::memset(cl->mod, 0, sizeof(cl->mod));
        } else {
            for (int t = 0; t < total; t++) {
                if (std::memcmp(sc + t * 8, sc_prev + t * 8, 8) != 0)
                    cl->mod[t] = 0;
            }
        }
    }

    std::memcpy(sc_prev, sc, (size_t)total * 8);
    sc_prev_dimx = dimx;
    sc_prev_dimy = dimy;
}

// -----------------------------------------------------------------------------
// Sprite atlas builder
//
// In graphical mode (USE_GRAPHICS=true): exports ALL sprites from
// enabler->textures.raws[] into a single PNG atlas so the browser can draw
// any texpos it encounters without waiting for those tiles to appear on screen.
//
// In ASCII/curses mode: the atlas is not needed — the browser uses the CP437
// tileset path. wf_build_atlas() is a no-op in that mode.
//
// Rebuild is triggered by wf_trigger_atlas_rebuild() (called on plugin_enable
// and whenever the graphics mode or raws.size() changes).
//
// The JSON format:
//   { "tw": W, "th": H, "map": { "texpos_str": [atlas_x, atlas_y], ... } }
// -----------------------------------------------------------------------------

// Extract RGBA bytes from a 32-bit SDL pixel value using the surface's format masks.
static inline void sdl_get_rgba(uint32_t pixel, const SDL_PixelFormat* fmt,
                                 uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a)
{
    r = (fmt->Rmask) ? (uint8_t)((pixel & fmt->Rmask) >> fmt->Rshift) : 0;
    g = (fmt->Gmask) ? (uint8_t)((pixel & fmt->Gmask) >> fmt->Gshift) : 0;
    b = (fmt->Bmask) ? (uint8_t)((pixel & fmt->Bmask) >> fmt->Bshift) : 0;
    a = (fmt->Amask) ? (uint8_t)((pixel & fmt->Amask) >> fmt->Ashift) : 0xFF;
}

// Copy one SDL_Surface tile into RGBA pixel buffer at (dest_x, dest_y).
static void blit_surface_tile(SDL_Surface* surf, int tw, int th,
                               std::vector<uint8_t>& pixels,
                               int atlas_w, int dest_x, int dest_y)
{
    const SDL_PixelFormat* fmt = surf->format;
    const int copy_w = std::min(surf->w, tw);
    const int copy_h = std::min(surf->h, th);
    for (int py = 0; py < copy_h; py++) {
        const uint8_t* row_ptr =
            static_cast<const uint8_t*>(surf->pixels) + py * surf->pitch;
        for (int px = 0; px < copy_w; px++) {
            const uint8_t* p = row_ptr + px * fmt->BytesPerPixel;
            uint32_t pixel = 0;
            switch (fmt->BytesPerPixel) {
                case 4: pixel = *reinterpret_cast<const uint32_t*>(p); break;
                case 3: pixel = (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16); break;
                case 2: pixel = *reinterpret_cast<const uint16_t*>(p); break;
                case 1: pixel = *p; break;
            }
            uint8_t r, g, b, a;
            sdl_get_rgba(pixel, fmt, r, g, b, a);
            const size_t oi = ((size_t)(dest_y+py)*atlas_w + (dest_x+px))*4;
            pixels[oi+0]=r; pixels[oi+1]=g; pixels[oi+2]=b; pixels[oi+3]=a;
        }
    }
}

static void wf_build_atlas()
{
    if (!enabler || !init || !gps)
        return;

    // Only build an atlas in graphical mode. In ASCII mode the browser uses
    // the CP437 tileset and needs no sprite atlas.
    if (!g_graphics_mode) {
        std::lock_guard<std::mutex> lk(g_atlas_mutex);
        g_atlas_png.clear();
        g_atlas_json = "";
        g_atlas_last_valid_count = 0;
        g_atlas_last_raws_size   = 0;
        g_atlas_last_gfx_mode    = false;
        return;
    }

    const size_t raws_size = enabler->textures.raws.size();
    if (raws_size == 0)
        return;

    // Count valid (non-null with pixels) surfaces for change-detection.
    // We must count — not just compare raws.size() — because after a world
    // load DF calls delete_all_post_init_textures() and then reloads sprites
    // into the SAME slots, keeping raws.size() identical but filling in
    // previously-null entries.
    size_t valid_count = 0;
    for (size_t i = 0; i < raws_size; i++) {
        SDL_Surface* s = static_cast<SDL_Surface*>(enabler->textures.raws[i]);
        if (s && s->pixels) ++valid_count;
    }

    // Skip rebuild if nothing has changed.
    if (valid_count == g_atlas_last_valid_count &&
        raws_size   == g_atlas_last_raws_size   &&
        g_graphics_mode == g_atlas_last_gfx_mode)
        return;

    const int32_t tw = gps->tile_pixel_x > 0 ? gps->tile_pixel_x : 16;
    const int32_t th = gps->tile_pixel_y > 0 ? gps->tile_pixel_y : 16;

    // Collect all valid texpos values from raws[].
    // We export every non-null surface — this means the atlas covers all sprites
    // currently loaded, not just those visible on the current screen.
    std::vector<long> all_texpos;
    all_texpos.reserve(raws_size);
    for (size_t i = 0; i < raws_size; i++) {
        SDL_Surface* s = static_cast<SDL_Surface*>(enabler->textures.raws[i]);
        if (s && s->pixels && s->format && s->format->BytesPerPixel >= 1)
            all_texpos.push_back((long)i);
    }

    // Disk-based fallback for surfaces that are null (freed after GPU upload).
    // Build reverse-map: texpos → CP437 char, plus a PNG cache for art files.
    std::unordered_map<long, uint8_t> texpos_to_char;
    for (int ch = 1; ch < 256; ch++) {
        long tp = init->font.large_font_texpos[ch];
        if (tp > 0) texpos_to_char.emplace(tp, (uint8_t)ch);
    }

    namespace fs = std::filesystem;
    fs::path art_dir = DFHack::Core::getInstance().getHackPath().parent_path()
                       / "data" / "art";
    struct PngCache { std::vector<uint8_t> px; unsigned w=0, h=0; };
    std::unordered_map<std::string, PngCache> png_cache;

    auto load_art_png = [&](const std::string& fname) -> const PngCache* {
        auto it = png_cache.find(fname);
        if (it != png_cache.end()) return &it->second;
        fs::path p = art_dir / fname;
        std::error_code ec;
        if (!fs::is_regular_file(p, ec)) return nullptr;
        PngCache c;
        if (lodepng::decode(c.px, c.w, c.h, p.string()) || c.px.empty()) return nullptr;
        png_cache.emplace(fname, std::move(c));
        return &png_cache.at(fname);
    };

    // Also try null-surface slots if they map to font chars (disk fallback).
    for (size_t i = 0; i < raws_size; i++) {
        if (enabler->textures.raws[i]) continue; // already have the surface
        if (texpos_to_char.count((long)i))
            all_texpos.push_back((long)i);
    }
    std::sort(all_texpos.begin(), all_texpos.end());
    all_texpos.erase(std::unique(all_texpos.begin(), all_texpos.end()), all_texpos.end());

    if (all_texpos.empty())
        return;

    // Atlas layout: 64 tiles per row.
    const int ATLAS_COLS = 64;
    const int num        = (int)all_texpos.size();
    const int atlas_cols = std::min(num, ATLAS_COLS);
    const int atlas_rows = (num + atlas_cols - 1) / atlas_cols;
    const int atlas_w    = atlas_cols * tw;
    const int atlas_h    = atlas_rows * th;

    std::vector<uint8_t> pixels((size_t)atlas_w * atlas_h * 4, 0);

    std::string json;
    json.reserve(64 + (size_t)num * 32);
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"tw\":%d,\"th\":%d,\"map\":{", tw, th);
        json += buf;
    }

    bool first_entry = true;
    for (int idx = 0; idx < num; idx++) {
        const long tp    = all_texpos[idx];
        const int dest_x = (idx % ATLAS_COLS) * tw;
        const int dest_y = (idx / ATLAS_COLS) * th;
        bool copied = false;

        // Path 1: surface is in memory.
        if (tp < (long)raws_size) {
            SDL_Surface* surf = static_cast<SDL_Surface*>(enabler->textures.raws[tp]);
            if (surf && surf->pixels && surf->format) {
                blit_surface_tile(surf, tw, th, pixels, atlas_w, dest_x, dest_y);
                copied = true;
            }
        }

        // Path 2: disk fallback via font texpos reverse-map.
        if (!copied) {
            auto cit = texpos_to_char.find(tp);
            if (cit != texpos_to_char.end()) {
                const uint8_t char_idx = cit->second;
                std::error_code ec;
                for (const auto& entry : fs::directory_iterator(art_dir, ec)) {
                    if (entry.path().extension() != ".png") continue;
                    const PngCache* pc = load_art_png(entry.path().filename().string());
                    if (!pc) continue;
                    unsigned tpr = pc->w / (unsigned)tw;
                    unsigned tpc2 = pc->h / (unsigned)th;
                    if (tpr == 0 || tpc2 == 0) continue;
                    if (tpr * tpc2 < 256) continue; // not a 256-glyph sheet
                    if ((unsigned)char_idx >= tpr * tpc2) continue;
                    unsigned src_col = char_idx % tpr;
                    unsigned src_row = char_idx / tpr;
                    for (int py = 0; py < th; py++) {
                        for (int px2 = 0; px2 < tw; px2++) {
                            unsigned sx = src_col*(unsigned)tw + (unsigned)px2;
                            unsigned sy = src_row*(unsigned)th + (unsigned)py;
                            if (sx >= pc->w || sy >= pc->h) continue;
                            const size_t si = ((size_t)sy*pc->w + sx)*4;
                            const size_t oi = ((size_t)(dest_y+py)*atlas_w + (dest_x+px2))*4;
                            pixels[oi+0]=pc->px[si+0]; pixels[oi+1]=pc->px[si+1];
                            pixels[oi+2]=pc->px[si+2]; pixels[oi+3]=pc->px[si+3];
                        }
                    }
                    copied = true;
                    break;
                }
            }
        }

        if (!copied) continue;

        char entry[64];
        int len = snprintf(entry, sizeof(entry), "%s\"%ld\":[%d,%d]",
                           first_entry ? "" : ",", tp, dest_x, dest_y);
        json.append(entry, (size_t)len);
        first_entry = false;
    }
    json += "}}";

    std::vector<uint8_t> png_out;
    if (lodepng::encode(png_out, pixels.data(), (unsigned)atlas_w, (unsigned)atlas_h))
        return;

    {
        std::lock_guard<std::mutex> lk(g_atlas_mutex);
        g_atlas_png  = std::move(png_out);
        g_atlas_json = std::move(json);
        g_atlas_version = (g_atlas_version + 1) & 0x0F; // 4-bit wrap
    }
    g_atlas_last_valid_count = valid_count;
    g_atlas_last_raws_size   = raws_size;
    g_atlas_last_gfx_mode    = g_graphics_mode;
}

// -----------------------------------------------------------------------------
// Input injection.
// The WebSocket thread pushes events via simkey() / simmouse() (input.hpp);
// we drain them here on the DF main thread and translate each event into
// SDL key/mouse events pushed into DF's own event queue.
// -----------------------------------------------------------------------------

std::mutex               g_wf_input_mutex;
std::vector<WFKeyEvent>  g_wf_input_queue;
std::mutex               g_wf_mouse_mutex;
std::vector<WFMouseEvent> g_wf_mouse_queue;

// Translate a webfort key event to an SDL_Keycode. Returns 0 if unknown.
// The webfort protocol sends either a JS DOM keyCode (mdata[1], non-printable
// keys) or a charCode (mdata[2], printable characters). Those are passed to us
// as `sym` (= either) and `unicode` (= charCode when present).
static SDL_Keycode wf_to_sdl_keycode(int sym, int unicode)
{
    // Printable ASCII chars: SDLK for letters equals the lowercase ASCII
    // value. Upper-case letters get normalized to lowercase + shift modifier
    // is expected to be set separately by the caller.
    if (unicode >= 0x20 && unicode <= 0x7E) {
        int c = unicode;
        if (c >= 'A' && c <= 'Z') c += ('a' - 'A');
        return (SDL_Keycode)c;
    }

    // Non-printable: sym is a JS DOM keyCode.
    switch (sym) {
    case 8:   return SDLK_BACKSPACE;
    case 9:   return SDLK_TAB;
    case 13:  return SDLK_RETURN;
    case 27:  return SDLK_ESCAPE;
    case 32:  return SDLK_SPACE;
    case 33:  return SDLK_PAGEUP;
    case 34:  return SDLK_PAGEDOWN;
    case 35:  return SDLK_END;
    case 36:  return SDLK_HOME;
    case 37:  return SDLK_LEFT;
    case 38:  return SDLK_UP;
    case 39:  return SDLK_RIGHT;
    case 40:  return SDLK_DOWN;
    case 45:  return SDLK_INSERT;
    case 46:  return SDLK_DELETE;
    case 112: return SDLK_F1;
    case 113: return SDLK_F2;
    case 114: return SDLK_F3;
    case 115: return SDLK_F4;
    case 116: return SDLK_F5;
    case 117: return SDLK_F6;
    case 118: return SDLK_F7;
    case 119: return SDLK_F8;
    case 120: return SDLK_F9;
    case 121: return SDLK_F10;
    case 122: return SDLK_F11;
    case 123: return SDLK_F12;
    default:
        if (sym >= 0x20 && sym <= 0x7E) {
            int c = sym;
            if (c >= 'A' && c <= 'Z') c += ('a' - 'A');
            return (SDL_Keycode)c;
        }
        return 0;
    }
}

// Push a synthesized SDL key event into DF's event queue. This drives the
// *real* game loop — map panning, keybindings, text entry, everything a
// physical keyboard would — instead of just the top viewscreen's feed().
static void wf_push_sdl_key(SDL_Keycode sym, int sdlmods, bool down, int unicode)
{
    int sdl2_mod = 0;
    if (sdlmods & SDL::KMOD_SHIFT) sdl2_mod |= KMOD_SHIFT;
    if (sdlmods & SDL::KMOD_CTRL)  sdl2_mod |= KMOD_CTRL;
    if (sdlmods & SDL::KMOD_ALT)   sdl2_mod |= KMOD_ALT;

    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    ev.key.type = ev.type;
    ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    ev.key.repeat = 0;
    ev.key.keysym.sym = sym;
    ev.key.keysym.scancode = SDL_SCANCODE_UNKNOWN;
    ev.key.keysym.mod = sdl2_mod;
    DFHack::DFSDL::DFSDL_PushEvent(&ev);

    // For printable ASCII on key-down, also push a TEXTINPUT event so that
    // text fields (rename, search, etc.) receive the character.
    if (down && unicode >= 0x20 && unicode <= 0x7E &&
        !(sdlmods & (SDL::KMOD_CTRL | SDL::KMOD_ALT)))
    {
        SDL_Event te;
        memset(&te, 0, sizeof(te));
        te.type = SDL_TEXTINPUT;
        te.text.type = SDL_TEXTINPUT;
        te.text.text[0] = (char)unicode;
        te.text.text[1] = '\0';
        DFHack::DFSDL::DFSDL_PushEvent(&te);
    }
}

void wf_flush_input_queue()
{
    std::vector<WFKeyEvent> local;
    {
        std::lock_guard<std::mutex> lock(g_wf_input_mutex);
        if (g_wf_input_queue.empty())
            return;
        local.swap(g_wf_input_queue);
    }

    for (const auto& ev : local) {
        SDL_Keycode sk = wf_to_sdl_keycode(ev.sym, ev.unicode);
        if (!sk)
            continue;

        // Synthesize modifier key-downs first so DF sees them as held.
        if (ev.sdlmods & SDL::KMOD_SHIFT) wf_push_sdl_key(SDLK_LSHIFT, 0, true, 0);
        if (ev.sdlmods & SDL::KMOD_CTRL)  wf_push_sdl_key(SDLK_LCTRL,  0, true, 0);
        if (ev.sdlmods & SDL::KMOD_ALT)   wf_push_sdl_key(SDLK_LALT,   0, true, 0);

        wf_push_sdl_key(sk, ev.sdlmods, true,  ev.unicode);
        wf_push_sdl_key(sk, ev.sdlmods, false, ev.unicode);

        if (ev.sdlmods & SDL::KMOD_ALT)   wf_push_sdl_key(SDLK_LALT,   0, false, 0);
        if (ev.sdlmods & SDL::KMOD_CTRL)  wf_push_sdl_key(SDLK_LCTRL,  0, false, 0);
        if (ev.sdlmods & SDL::KMOD_SHIFT) wf_push_sdl_key(SDLK_LSHIFT, 0, false, 0);
    }
}

// Apply a mouse event on the DF main thread by directly manipulating enabler
// flags — the same technique Lua's gui.simulateInput() uses. This avoids any
// SDL window-coordinate mismatch; gps->mouse_x/y (tile coords) is all DF
// needs to know which widget is under the cursor.
static void wf_apply_mouse(int tile_x, int tile_y, uint8_t button,
                            WFMouseType type)
{
    // Always update persisted position so plugin_onupdate can re-stamp it
    // each tick (prevents DF's SDL handler from overwriting with physical
    // mouse coords between our updates).
    if (type == WF_MOUSE_MOVE || type == WF_MOUSE_DOWN) {
        g_wf_mouse_tile_x = tile_x;
        g_wf_mouse_tile_y = tile_y;
    }

    // Stamp tile coords into gps right now (will be re-stamped in onupdate).
    gps->mouse_x = tile_x;
    gps->mouse_y = tile_y;
    const int32_t tile_px_w = init ? init->display.grid_x : 16;
    const int32_t tile_px_h = init ? init->display.grid_y : 16;
    gps->precise_mouse_x = tile_x * tile_px_w + tile_px_w / 2;
    gps->precise_mouse_y = tile_y * tile_px_h + tile_px_h / 2;

    if (!enabler) return;
    df::viewscreen* ws = Gui::getCurViewscreen(true);
    if (!ws) return;

    enabler->tracking_on = 1;

    if (type == WF_MOUSE_MOVE) {
        // Feed an empty keyset with tracking_on=1 so the viewscreen updates
        // its hover state and queues any tooltip render.
        std::set<df::interface_key> keys;
        ws->feed(&keys);
        return;
    }

    if (type == WF_MOUSE_WHEEL) {
        // Push a real SDL_MOUSEWHEEL event so DF's SDL handler processes it
        // (this is what changes z-levels on the map). Also feed the scroll
        // interface key so list/menu widgets scroll correctly.
        SDL_Event wev;
        memset(&wev, 0, sizeof(wev));
        wev.type = SDL_MOUSEWHEEL;
        wev.wheel.type = SDL_MOUSEWHEEL;
        wev.wheel.x = 0;
        wev.wheel.y = (button == 4) ? 1 : -1; // 4=scroll up, 5=scroll down
        DFHack::DFSDL::DFSDL_PushEvent(&wev);

        std::set<df::interface_key> keys;
        keys.insert(button == 4
            ? df::interface_key::STANDARDSCROLL_UP
            : df::interface_key::STANDARDSCROLL_DOWN);
        ws->feed(&keys);
        return;
    }

    // Button click: set the relevant enabler flags, feed(), then restore.
    int8_t prev_lbut      = enabler->mouse_lbut;
    int8_t prev_lbut_down = enabler->mouse_lbut_down;
    int8_t prev_lbut_lift = enabler->mouse_lbut_lift;
    int8_t prev_rbut      = enabler->mouse_rbut;
    int8_t prev_rbut_down = enabler->mouse_rbut_down;
    int8_t prev_rbut_lift = enabler->mouse_rbut_lift;
    int8_t prev_mbut      = enabler->mouse_mbut;
    int8_t prev_mbut_down = enabler->mouse_mbut_down;
    int8_t prev_mbut_lift = enabler->mouse_mbut_lift;

    bool is_left   = (button == 1);
    bool is_right  = (button == 2);
    bool is_middle = (button == 3);

    if (type == WF_MOUSE_DOWN) {
        if (is_left)   { enabler->mouse_lbut = 1; enabler->mouse_lbut_down = 1; }
        if (is_right)  { enabler->mouse_rbut = 1; enabler->mouse_rbut_down = 1; }
        if (is_middle) { enabler->mouse_mbut = 1; enabler->mouse_mbut_down = 1; }
    } else { // WF_MOUSE_UP
        if (is_left)   { enabler->mouse_lbut = 0; enabler->mouse_lbut_lift = 1; }
        if (is_right)  { enabler->mouse_rbut = 0; enabler->mouse_rbut_lift = 1; }
        if (is_middle) { enabler->mouse_mbut = 0; enabler->mouse_mbut_lift = 1; }
    }

    std::set<df::interface_key> keys;
    ws->feed(&keys);

    enabler->mouse_lbut      = prev_lbut;
    enabler->mouse_lbut_down = prev_lbut_down;
    enabler->mouse_lbut_lift = prev_lbut_lift;
    enabler->mouse_rbut      = prev_rbut;
    enabler->mouse_rbut_down = prev_rbut_down;
    enabler->mouse_rbut_lift = prev_rbut_lift;
    enabler->mouse_mbut      = prev_mbut;
    enabler->mouse_mbut_down = prev_mbut_down;
    enabler->mouse_mbut_lift = prev_mbut_lift;
}

void wf_flush_mouse_queue()
{
    std::vector<WFMouseEvent> local;
    {
        std::lock_guard<std::mutex> lock(g_wf_mouse_mutex);
        if (g_wf_mouse_queue.empty())
            return;
        local.swap(g_wf_mouse_queue);
    }
    for (const auto& ev : local)
        wf_apply_mouse(ev.tile_x, ev.tile_y, ev.button, ev.type);
}

// -----------------------------------------------------------------------------
// Plugin lifecycle
// -----------------------------------------------------------------------------

DFhackCExport command_result plugin_init(color_ostream &out,
                                         vector<PluginCommand> &commands)
{
    if (!gps || !init || !plotinfo) {
        out.printerr("webfort: missing required globals; plugin disabled.\n");
        return CR_OK;
    }

    s_hdl = new WFServer(out);
    out.print("webfort: loaded (server off; run `enable webfort`).\n");
    return CR_OK;
}

DFhackCExport command_result plugin_enable(color_ostream &out, bool enable)
{
    if (enable == enabled)
        return CR_OK;

    if (enable) {
        if (!s_hdl) {
            out.printerr("webfort: server not initialized.\n");
            return CR_FAILURE;
        }
        enabled = true;
        wf_apply_render_hooks(true);
        s_hdl->start();
        // Update mode and pre-build atlas immediately so the first client
        // connection can already receive a valid atlas.png / atlas.json.
        if (init)
            g_graphics_mode = init->display.flag.is_set(
                df::init_display_flags::USE_GRAPHICS);
        // Reset change-detection so wf_build_atlas() runs unconditionally.
        g_atlas_last_valid_count = 0;
        g_atlas_last_raws_size   = 0;
        g_atlas_last_gfx_mode    = !g_graphics_mode; // force mismatch
        wf_build_atlas();
        if (gps) {
            out << "webfort: enabled. DF screen is "
                << gps->dimx << "x" << gps->dimy << " tiles. Mode: "
                << (g_graphics_mode ? "graphical" : "ASCII") << ".\n";
        } else {
            out.print("webfort: enabled.\n");
        }
    } else {
        enabled = false;
        wf_apply_render_hooks(false);
        if (s_hdl)
            s_hdl->stop();
        out.print("webfort: disabled.\n");
    }
    return CR_OK;
}

DFhackCExport command_result plugin_onupdate(color_ostream& /*out*/)
{
    if (!enabled)
        return CR_OK;

    // Apply any camera deltas queued by the active player (opcode 117 CamMove).
    // These are accumulated atomically on the WS thread and consumed here on
    // the DF main thread so *window_x/y/z is only ever written from one thread.
    {
        int32_t dx = g_active_cam_dx.exchange(0, std::memory_order_relaxed);
        int32_t dy = g_active_cam_dy.exchange(0, std::memory_order_relaxed);
        int32_t dz = g_active_cam_dz.exchange(0, std::memory_order_relaxed);
        if ((dx || dy || dz) && window_x && window_y && window_z) {
            *window_x += dx;
            *window_y += dy;
            *window_z += dz;
            if (*window_x < -30) *window_x = -30;
            if (*window_y < -30) *window_y = -30;
            if (*window_z < 0)   *window_z = 0;
            if (DFHack::Maps::IsValid()) {
                uint32_t msx = 0, msy = 0, msz = 0;
                DFHack::Maps::getSize(msx, msy, msz);
                msx *= 16; msy *= 16;
                int32_t vw = gps ? gps->dimx : 0;
                int32_t vh = gps ? gps->dimy : 0;
                if (*window_x > (int32_t)msx - vw + 30) *window_x = (int32_t)msx - vw + 30;
                if (*window_y > (int32_t)msy - vh + 30) *window_y = (int32_t)msy - vh + 30;
                if (msz > 0 && *window_z >= (int32_t)msz) *window_z = (int32_t)msz - 1;
            }
            // Force full re-send for the active player after a viewport shift.
            Client* acl = get_active_client();
            if (acl) memset(acl->mod, 0, sizeof(acl->mod));
        }
    }

    // Keep the active player's cam_x/y/z in sync with *window_x/y/z.
    // *window_x/y/z is the authority for the active player: DF may have moved it
    // via native camera controls (arrow keys, scroll, etc.) in mainloop().
    // We READ from *window_x/y/z into cam_x/y/z — never write in the other direction.
    if (window_x && window_y && window_z) {
        Client* acl = get_active_client();
        if (acl) {
            acl->cam_x = *window_x;
            acl->cam_y = *window_y;
            acl->cam_z = *window_z;
            acl->has_own_cam = true;
        }
    }

    wf_flush_input_queue();
    wf_flush_mouse_queue();

    // Re-stamp the virtual cursor every tick so DF's SDL event loop can't
    // overwrite gps->mouse_x/y with the physical mouse between our ticks.
    // Also feed an empty keyset each tick so hover state (tooltips, button
    // highlights) stays alive and DF re-renders it on the next render() call.
    if (g_wf_mouse_tile_x >= 0 && gps && enabler) {
        const int32_t tile_px_w = init ? init->display.grid_x : 16;
        const int32_t tile_px_h = init ? init->display.grid_y : 16;
        gps->mouse_x = g_wf_mouse_tile_x;
        gps->mouse_y = g_wf_mouse_tile_y;
        gps->precise_mouse_x = g_wf_mouse_tile_x * tile_px_w + tile_px_w / 2;
        gps->precise_mouse_y = g_wf_mouse_tile_y * tile_px_h + tile_px_h / 2;
        enabler->tracking_on = 1;
        // Keep hover state alive: feed an empty keyset so the current
        // viewscreen re-evaluates what's under the cursor every game tick.
        df::viewscreen* ws = Gui::getCurViewscreen(true);
        if (ws) {
            std::set<df::interface_key> empty_keys;
            ws->feed(&empty_keys);
        }
    }

    // Capture the current frame. In DF's async_loop the render command
    // (render_things → viewscreen->render + widgets->render) always runs
    // BEFORE mainloop() for the same frame, so gps->screen here is the
    // fully composited frame including all DFHack overlay widgets.
    capture_screen_buffer();

    // Per-client extra render passes for independent viewports.
    // Clients that have sent at least one CamMove opcode have has_own_cam=true
    // and get a separate render pass at their cam_x/y/z position each tick.
    // We save and restore the global viewport so the main game loop is unaffected.
    if (gps && window_x && window_y && window_z) {
        const int32_t save_x = *window_x;
        const int32_t save_y = *window_y;
        const int32_t save_z = *window_z;
        const int total = gps->dimx * gps->dimy;

        if (total > 0) {
            df::viewscreen* vs = Gui::getCurViewscreen(true);
            for (auto& kv : clients) {
                Client* cl = kv.second;
                if (!cl || !cl->has_own_cam || !cl->own_sc) continue;

                if (cl->cam_x == save_x && cl->cam_y == save_y && cl->cam_z == save_z) {
                    // Same position as the main viewport: reuse sc directly.
                    std::memcpy(cl->own_sc, sc, (size_t)total * 8);
                } else {
                    // Different viewport: do an extra render pass at cam_x/y/z.
                    // Save gps->screen and screen_top BEFORE rendering so that
                    // DF's local window never shows the spectator's viewport
                    // (which would cause visible flicker for the active player).
                    const size_t screen_bytes = (size_t)total * 8;
                    static std::vector<uint8_t> s_screen_save;
                    static std::vector<uint8_t> s_screen_top_save;

                    s_screen_save.resize(screen_bytes);
                    if (gps->screen)
                        std::memcpy(s_screen_save.data(), gps->screen, screen_bytes);

                    const bool saved_top_in_use = gps->top_in_use;
                    s_screen_top_save.clear();
                    if (gps->screen_top && gps->top_in_use) {
                        s_screen_top_save.resize(screen_bytes);
                        std::memcpy(s_screen_top_save.data(), gps->screen_top, screen_bytes);
                    }

                    // Save gps->mouse coords. DF rendering code and DFHack
                    // overlay hooks may read AND write gps->mouse_x/y during
                    // render(). If we don't restore them the cursor broadcast
                    // in tock() will see corrupted values and show the cursor
                    // at the wrong position (the wrong-overlay bug).
                    const int32_t save_mx  = gps->mouse_x;
                    const int32_t save_my  = gps->mouse_y;
                    const int32_t save_pmx = gps->precise_mouse_x;
                    const int32_t save_pmy = gps->precise_mouse_y;

                    // Translate the active player's cursor into this
                    // spectator's viewport so DFHack overlays (mining
                    // designation preview etc.) appear at the correct
                    // world position rather than the raw tile offset.
                    const int32_t tile_px_w = init ? init->display.grid_x : 16;
                    const int32_t tile_px_h = init ? init->display.grid_y : 16;
                    if (g_wf_mouse_tile_x >= 0) {
                        // Active player world cursor = their camera + tile offset.
                        const int32_t world_mx = save_x + g_wf_mouse_tile_x;
                        const int32_t world_my = save_y + g_wf_mouse_tile_y;
                        // Translate to spectator tile coords.
                        const int32_t spec_mx = world_mx - cl->cam_x;
                        const int32_t spec_my = world_my - cl->cam_y;
                        if (spec_mx >= 0 && spec_mx < gps->dimx &&
                            spec_my >= 0 && spec_my < gps->dimy) {
                            gps->mouse_x = spec_mx;
                            gps->mouse_y = spec_my;
                            gps->precise_mouse_x = spec_mx * tile_px_w + tile_px_w / 2;
                            gps->precise_mouse_y = spec_my * tile_px_h + tile_px_h / 2;
                        } else {
                            // Active player cursor is outside this spectator's
                            // viewport — hide it so no overlay is drawn.
                            gps->mouse_x = -1;
                            gps->mouse_y = -1;
                            gps->precise_mouse_x = -1;
                            gps->precise_mouse_y = -1;
                        }
                    } else {
                        gps->mouse_x = -1;
                        gps->mouse_y = -1;
                        gps->precise_mouse_x = -1;
                        gps->precise_mouse_y = -1;
                    }

                    *window_x = cl->cam_x;
                    *window_y = cl->cam_y;
                    *window_z = cl->cam_z;
                    g_wf_in_extra_render = true;
                    if (vs) vs->render(0);
                    g_wf_in_extra_render = false;
                    fill_screen_buffer(cl->own_sc);

                    // Restore gps->screen and screen_top so DF's display
                    // buffer is never left in the spectator's viewport state.
                    if (gps->screen && !s_screen_save.empty())
                        std::memcpy(gps->screen, s_screen_save.data(), screen_bytes);
                    gps->top_in_use = saved_top_in_use;
                    if (gps->screen_top && !s_screen_top_save.empty())
                        std::memcpy(gps->screen_top, s_screen_top_save.data(), screen_bytes);

                    // Restore mouse coords so the cursor broadcast in tock()
                    // always reads the active player's correct tile position.
                    gps->mouse_x = save_mx;
                    gps->mouse_y = save_my;
                    gps->precise_mouse_x = save_pmx;
                    gps->precise_mouse_y = save_pmy;
                }

                // Delta computation: mark tiles that changed since last tick.
                if (cl->own_sc_prev) {
                    for (int t = 0; t < total; t++) {
                        if (std::memcmp(cl->own_sc + t*8, cl->own_sc_prev + t*8, 8) != 0)
                            cl->mod[t] = 0;
                    }
                    std::memcpy(cl->own_sc_prev, cl->own_sc, (size_t)total * 8);
                }
            }
        }

        // Always restore the global viewport.
        *window_x = save_x;
        *window_y = save_y;
        *window_z = save_z;
    }

    // Update graphics mode flag. If it changed, rebuild the atlas immediately
    // so the browser switches modes without a multi-second delay.
    if (init) {
        bool new_mode = init->display.flag.is_set(
            df::init_display_flags::USE_GRAPHICS);
        if (new_mode != g_graphics_mode) {
            g_graphics_mode = new_mode;
            wf_build_atlas();
        }
    }

    // Periodic atlas rebuild to pick up newly-loaded sprites (e.g. after
    // a world load or graphics pack swap). Rate-limited to every 60 ticks.
    static uint32_t s_atlas_tick = 0;
    if (++s_atlas_tick >= 60) {
        s_atlas_tick = 0;
        wf_build_atlas();
    }
    return CR_OK;
}

DFhackCExport command_result plugin_shutdown(color_ostream &out)
{
    if (enabled && s_hdl)
        s_hdl->stop();
    enabled = false;
    delete s_hdl;
    s_hdl = nullptr;
    return CR_OK;
}

/* vim: set et sw=4 : */
