/*
 * input.hpp — key and mouse injection bridge.
 *
 * The WebSocket thread calls simkey() / simmouse() from server.cpp's
 * on_message(). Calling into DF directly from that thread is unsafe
 * (viewscreen state is only valid from the DF main thread), so we push a
 * compact event onto a mutex-protected queue. The DF main thread drains it
 * in plugin_onupdate via wf_flush_input_queue() / wf_flush_mouse_queue(),
 * which translate each event into SDL key/mouse events pushed into DF's
 * own event queue via DFHack::DFSDL::DFSDL_PushEvent().
 *
 * The SDL::Key vocabulary is preserved so server.cpp does not need changes.
 */
#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

namespace SDL {

enum Key : int32_t {
    K_UNKNOWN = 0,
    K_RETURN  = 13,
    K_ESCAPE  = 27,
    K_SPACE   = 32,

    K_LESS    = 60,
    K_GREATER = 62,

    K_DELETE  = 127,

    K_KP0 = 256, K_KP1, K_KP2, K_KP3, K_KP4,
    K_KP5, K_KP6, K_KP7, K_KP8, K_KP9,
    K_KP_PERIOD, K_KP_DIVIDE, K_KP_MULTIPLY,
    K_KP_MINUS, K_KP_PLUS, K_KP_ENTER, K_KP_EQUALS,

    K_UP, K_DOWN, K_RIGHT, K_LEFT,
    K_INSERT, K_HOME, K_END, K_PAGEUP, K_PAGEDOWN,

    K_F1, K_F2, K_F3, K_F4, K_F5, K_F6,
    K_F7, K_F8, K_F9, K_F10, K_F11, K_F12,

    K_NUMLOCK,

    K_LSHIFT = 304,
    K_RSHIFT = 303,
    K_LCTRL  = 306,
    K_RCTRL  = 305,
    K_LALT   = 308,
    K_RALT   = 307,
};

enum Mod : int32_t {
    KMOD_NONE  = 0x0000,
    KMOD_SHIFT = 0x0003,
    KMOD_CTRL  = 0x00C0,
    KMOD_ALT   = 0x0300,
};

} // namespace SDL

// One queued key-press from a websocket client.
struct WFKeyEvent {
    int       down;
    int       sdlmods; // SDL::Mod bitmask
    SDL::Key  sym;
    int       unicode; // 0 if none
};

extern std::mutex                g_wf_input_mutex;
extern std::vector<WFKeyEvent>   g_wf_input_queue;

static inline SDL::Key mapInputCodeToSDL(const uint32_t code)
{
    switch (code) {
    case 96:  return SDL::K_KP0;
    case 97:  return SDL::K_KP1;
    case 98:  return SDL::K_KP2;
    case 99:  return SDL::K_KP3;
    case 100: return SDL::K_KP4;
    case 101: return SDL::K_KP5;
    case 102: return SDL::K_KP6;
    case 103: return SDL::K_KP7;
    case 104: return SDL::K_KP8;
    case 105: return SDL::K_KP9;
    case 144: return SDL::K_NUMLOCK;
    case 111: return SDL::K_KP_DIVIDE;
    case 106: return SDL::K_KP_MULTIPLY;
    case 109: return SDL::K_KP_MINUS;
    case 107: return SDL::K_KP_PLUS;
    case 33:  return SDL::K_PAGEUP;
    case 34:  return SDL::K_PAGEDOWN;
    case 35:  return SDL::K_END;
    case 36:  return SDL::K_HOME;
    case 46:  return SDL::K_DELETE;
    case 112: return SDL::K_F1;
    case 113: return SDL::K_F2;
    case 114: return SDL::K_F3;
    case 115: return SDL::K_F4;
    case 116: return SDL::K_F5;
    case 117: return SDL::K_F6;
    case 118: return SDL::K_F7;
    case 119: return SDL::K_F8;
    case 120: return SDL::K_F9;
    case 121: return SDL::K_F10;
    case 122: return SDL::K_F11;
    case 123: return SDL::K_F12;
    case 37:  return SDL::K_LEFT;
    case 39:  return SDL::K_RIGHT;
    case 38:  return SDL::K_UP;
    case 40:  return SDL::K_DOWN;
    case 188: return SDL::K_LESS;
    case 190: return SDL::K_GREATER;
    case 13:  return SDL::K_RETURN;
    case 27:  return SDL::K_ESCAPE;
    default:
        if (code <= 177)
            return (SDL::Key)code;
        return SDL::K_UNKNOWN;
    }
}

// Called on the websocket thread. Only press events are retained.
static inline void simkey(int down, int mod, SDL::Key sym, int unicode)
{
    if (!down)
        return;
    if (sym == SDL::K_UNKNOWN && unicode == 0)
        return;
    // Skip pure-modifier presses; mods are folded into `mod`.
    if (sym == SDL::K_LSHIFT || sym == SDL::K_RSHIFT ||
        sym == SDL::K_LCTRL  || sym == SDL::K_RCTRL  ||
        sym == SDL::K_LALT   || sym == SDL::K_RALT)
        return;

    WFKeyEvent ev{ down, mod, sym, unicode };
    std::lock_guard<std::mutex> lock(g_wf_input_mutex);
    g_wf_input_queue.push_back(ev);
}

// Drain the input queue on the DF main thread. Implemented in webfort.cpp.
void wf_flush_input_queue();

// ── Mouse ─────────────────────────────────────────────────────────────────

// Mouse event types (must match JS MOUSE_* constants).
enum WFMouseType : uint8_t {
    WF_MOUSE_MOVE  = 0,
    WF_MOUSE_DOWN  = 1,
    WF_MOUSE_UP    = 2,
    WF_MOUSE_WHEEL = 3,
};

// button: 1=left 2=right 3=middle 4=wheel-up 5=wheel-down
struct WFMouseEvent {
    uint8_t tile_x;
    uint8_t tile_y;
    uint8_t button;  // 0 for MOVE
    WFMouseType type;
};

extern std::mutex                  g_wf_mouse_mutex;
extern std::vector<WFMouseEvent>   g_wf_mouse_queue;

static inline void simmouse(uint8_t tile_x, uint8_t tile_y,
                             uint8_t button, WFMouseType type)
{
    WFMouseEvent ev{ tile_x, tile_y, button, type };
    std::lock_guard<std::mutex> lock(g_wf_mouse_mutex);
    g_wf_mouse_queue.push_back(ev);
}

void wf_flush_mouse_queue();
