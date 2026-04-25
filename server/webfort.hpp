#ifndef __WF_WEBFORT_HPP__
#define __WF_WEBFORT_HPP__

/*
 * webfort.hpp
 * Part of Web Fortress
 *
 * Copyright (c) 2014 mifki, ISC license.
 */

#include "Console.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

void deify(DFHack::color_ostream* raw_out, std::string nick);
void quicksave(DFHack::color_ostream* out);

// sc[] layout: 8 bytes per tile
//   [0]   char_code  (CP437; 0 if pure graphic tile)
//   [1]   fg_idx lo3 bits (color index 0-7)
//   [2]   bg_flags   (bits 0-2 = bg_idx 0-7; bit 6 = text-mode tile)
//   [3]   bold       (1 if fg_idx bit 3 is set)
//   [4]   screentexpos     low byte  (uint16 LE)
//   [5]   screentexpos     high byte
//   [6]   screentexpos_lower low byte
//   [7]   screentexpos_lower high byte
extern unsigned char sc[256*256*8];

extern int newwidth, newheight;
extern volatile bool needsresize;

// Sprite atlas — built from enabler->textures.raws in wf_build_atlas().
// g_atlas_mutex must be held when reading these from the HTTP thread.
extern std::mutex      g_atlas_mutex;
extern std::vector<uint8_t> g_atlas_png;
extern std::string     g_atlas_json;
// Monotonic counter incremented on every atlas rebuild. Sent to browser
// in tock() bits[4..7] (4-bit, wraps 0-15) so the browser knows to
// re-fetch atlas.png/atlas.json when the counter changes.
extern uint8_t g_atlas_version;

// True when DF is currently running in graphical (USE_GRAPHICS) mode.
// Updated every tick. The browser reads this from the tock() mode byte.
extern bool g_graphics_mode;

// Set true during per-client extra render passes (Phase 5 multiplayer).
// Render hooks check this to skip mouse stamping during extra passes.
extern bool g_wf_in_extra_render;

// Camera deltas queued by the active player from opcode 117 (CamMove).
// These are accumulated on the WS thread and drained on the DF main thread
// (in plugin_onupdate) to avoid data races on *window_x/y/z.
extern std::atomic<int32_t> g_active_cam_dx;
extern std::atomic<int32_t> g_active_cam_dy;
extern std::atomic<int32_t> g_active_cam_dz;

bool is_safe_to_escape();
void show_announcement(std::string announcement);

#endif
