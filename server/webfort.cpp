/*
 * webfort.cpp — Phase 4 port for DFHack 53.12
 *
 * Changes from Phase 2/3:
 *   - Post-render capture: capture_screen_buffer() is now called from
 *     DEFINE_VMETHOD_INTERPOSE hooks on viewscreen::render() so the screen
 *     snapshot always reflects the CURRENT frame (cursor position, hover
 *     tooltips, designation overlays) rather than the previous one.
 *   - Persistent hover: plugin_onupdate feeds an empty keyset every tick
 *     while a virtual cursor is active, keeping button highlights/tooltips
 *     alive between explicit mouse-move events.
 *   - Correct precise_mouse coords: uses init->display.grid_x/y (tile pixel
 *     size) to compute the pixel-center of the hovered tile instead of the
 *     hardcoded value of 8.
 *
 * Earlier changes (Phase 2/3):
 *   - df::global::ui → df::global::plotinfo (renamed upstream)
 *   - Removed references to viewscreens that no longer exist in 53.12
 *     (viewscreen_layer_export_play_mapst, viewscreen_overallstatusst,
 *     viewscreen_movieplayerst).
 *   - Dropped the df::renderer subclass entirely. The current df::renderer
 *     has a very different field list and a 25-method vtable; subclassing
 *     it in-tree is fragile. Instead we copy gps->screen into the shared
 *     `sc` buffer on each plugin_onupdate tick. gps itself is unchanged
 *     from the old API in the respects webfort cares about.
 *   - Server start/stop lives in plugin_enable() (matches modern plugin
 *     conventions where plugin_init registers but does not activate).
 */

#include <cassert>
#include <climits>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "Core.h"
#include "Console.h"
#include "Export.h"
#include "PluginManager.h"
#include "VTableInterpose.h"

#include "modules/DFSDL.h"
#include "modules/Gui.h"
#include "modules/Screen.h"
#include "modules/World.h"

#include <SDL_events.h>
#include <SDL_keyboard.h>
#include <SDL_keycode.h>

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

using namespace DFHack;
using std::string;
using std::vector;

using df::global::cursor;
using df::global::enabler;
using df::global::gps;
using df::global::init;
using df::global::plotinfo;
using df::global::world;

DFHACK_PLUGIN("webfort");
DFHACK_PLUGIN_IS_ENABLED(enabled);

REQUIRE_GLOBAL(gps);
REQUIRE_GLOBAL(init);
REQUIRE_GLOBAL(plotinfo);
REQUIRE_GLOBAL(world);

// -----------------------------------------------------------------------------
// Post-render capture hooks (Phase 4)
// Capture gps->screen AFTER viewscreen::render() has written the latest
// cursor/hover/overlay state. One hook per concrete viewscreen class covers
// all gameplay screens; the fallback in plugin_onupdate covers the rest.
// -----------------------------------------------------------------------------

static void capture_screen_buffer(); // forward-declare for the hooks below

struct wf_dwarfmode_render_hook : df::viewscreen_dwarfmodest {
    typedef df::viewscreen_dwarfmodest interpose_base;
    DEFINE_VMETHOD_INTERPOSE(void, render, (uint32_t flags))
    {
        INTERPOSE_NEXT(render)(flags);
        capture_screen_buffer();
    }
};
IMPLEMENT_VMETHOD_INTERPOSE(wf_dwarfmode_render_hook, render);

struct wf_dungeonmode_render_hook : df::viewscreen_dungeonmodest {
    typedef df::viewscreen_dungeonmodest interpose_base;
    DEFINE_VMETHOD_INTERPOSE(void, render, (uint32_t flags))
    {
        INTERPOSE_NEXT(render)(flags);
        capture_screen_buffer();
    }
};
IMPLEMENT_VMETHOD_INTERPOSE(wf_dungeonmode_render_hook, render);

// Generic hook for all other viewscreens (setup, choose_start_site, etc.).
struct wf_viewscreen_render_hook : df::viewscreen {
    typedef df::viewscreen interpose_base;
    DEFINE_VMETHOD_INTERPOSE(void, render, (uint32_t flags))
    {
        INTERPOSE_NEXT(render)(flags);
        capture_screen_buffer();
    }
};
IMPLEMENT_VMETHOD_INTERPOSE(wf_viewscreen_render_hook, render);

static void wf_apply_render_hooks(bool enable)
{
    INTERPOSE_HOOK(wf_dwarfmode_render_hook, render).apply(enable);
    INTERPOSE_HOOK(wf_dungeonmode_render_hook, render).apply(enable);
    INTERPOSE_HOOK(wf_viewscreen_render_hook, render).apply(enable);
}

// Shared with server.cpp via server.hpp externs.
unsigned char sc[256*256*5];
// Previous-frame snapshot so we can compute per-tile deltas.
static unsigned char sc_prev[256*256*5];
static int32_t sc_prev_dimx = -1;
static int32_t sc_prev_dimy = -1;
int newwidth = 0;
int newheight = 0;
volatile bool needsresize = false;

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

static bool is_dwarf_mode()
{
    t_gamemodes gm;
    World::ReadGameMode(gm);
    return gm.g_mode == df::game_mode::DWARF;
}

void deify(DFHack::color_ostream* raw_out, std::string nick)
{
    if (is_dwarf_mode()) {
        Core::getInstance().runCommand(*raw_out, "webfort/deify " + nick);
    }
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
        // Gui::getMenuWidth was removed in DFHack 53.x. For the Phase 2
        // port we conservatively assume the default menu layout; Phase 3+
        // can reintroduce a precise layout calculation (likely via the new
        // overlay / interface layer widths).
        uint8_t menu_width = 2, area_map_width = 3;
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

static void capture_screen_buffer()
{
    if (!gps || !gps->screen)
        return;

    const int32_t dimx = gps->dimx;
    const int32_t dimy = gps->dimy;
    if (dimx <= 0 || dimy <= 0 || dimx > 256 || dimy > 256)
        return;

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
    const bool dims_changed =
        (dimx != sc_prev_dimx) || (dimy != sc_prev_dimy);

    for (int32_t x = 0; x < dimx; x++) {
        for (int32_t y = 0; y < dimy; y++) {
            const int tile = x * dimy + y;
            unsigned char* dst = sc + tile * 4;
            const unsigned char* s = src + tile * 8;

            uint8_t fg_idx = match_color(s[1], s[2], s[3]);
            uint8_t bg_idx = match_color(s[4], s[5], s[6]);

            dst[0] = s[0];                // character
            dst[1] = fg_idx & 0x7;        // fg 0..7
            dst[2] = bg_idx & 0x7;        // bg 0..7
            dst[3] = (fg_idx & 0x8) ? 1 : 0; // bold flag

            bool is_map;
            if (is_text_tile(x, y, is_map))
                dst[2] |= 64;
        }
    }

    // Invalidate only tiles that actually changed, so tock()'s 64 KB
    // delta packet stays small even on large screens.
    const int total = dimx * dimy;
    for (auto& kv : clients) {
        if (!kv.second) continue;
        if (dims_changed) {
            std::memset(kv.second->mod, 0, sizeof(kv.second->mod));
        } else {
            for (int t = 0; t < total; t++) {
                const unsigned char* a = sc + t * 4;
                const unsigned char* b = sc_prev + t * 4;
                if (a[0] != b[0] || a[1] != b[1] ||
                    a[2] != b[2] || a[3] != b[3]) {
                    kv.second->mod[t] = 0;
                }
            }
        }
    }

    std::memcpy(sc_prev, sc, total * 4);
    sc_prev_dimx = dimx;
    sc_prev_dimy = dimy;
}

// -----------------------------------------------------------------------------
// Phase 3: input injection. The websocket thread pushes events via simkey()
// (in input.hpp); we drain them here, on the DF main thread, translating
// each event to a df::interface_key and feeding it into the current
// viewscreen.
// -----------------------------------------------------------------------------

std::mutex               g_wf_input_mutex;
std::vector<WFKeyEvent>  g_wf_input_queue;
std::mutex               g_wf_mouse_mutex;
std::vector<WFMouseEvent> g_wf_mouse_queue;

// Last known virtual mouse tile, persisted across ticks so DF's SDL event
// loop can't clobber it. -1 means "no virtual cursor yet".
static int g_wf_mouse_tile_x = -1;
static int g_wf_mouse_tile_y = -1;

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

    { FILE* f = fopen("/tmp/webfort.log", "a"); if (f) { fprintf(f, "[webfort] flush: %zu events (SDL push)\n", local.size()); fclose(f); } }

    for (const auto& ev : local) {
        SDL_Keycode sk = wf_to_sdl_keycode(ev.sym, ev.unicode);
        { FILE* f = fopen("/tmp/webfort.log", "a"); if (f) { fprintf(f, "[webfort]   ev sym=%d uni=%d mods=%d -> SDL sym=0x%x\n", ev.sym, ev.unicode, ev.sdlmods, (unsigned)sk); fclose(f); } }
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
    out.print("webfort: Phase 2 loaded (server off; run `enable webfort`).\n");
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
        if (gps) {
            out << "webfort: enabled. DF screen is "
                << gps->dimx << "x" << gps->dimy << " tiles.\n";
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
