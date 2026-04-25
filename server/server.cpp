/*
 * server.cpp
 * Part of Web Fortress
 *
 * Copyright (c) 2014 mifki, ISC license.
 */

#include "server.hpp"
#include "webfort.hpp"

#define WF_VERSION "WebFortress-v2.0"
#define WF_INVALID "WebFortress-invalid"

#include <cassert>
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

namespace lib = websocketpp::lib;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;
using websocketpp::lib::bind;

typedef ws::server<ws::config::asio> server;

typedef server::message_ptr message_ptr;

static conn_hdl null_conn = std::weak_ptr<void>();
static Client* null_client;

static conn_hdl active_conn = null_conn;

static std::ostream* out;
static std::ostream* logstream;
static DFHack::color_ostream* raw_out;

conn_map clients;

#include "config.hpp"
#include "webfort.hpp"
#include "input.hpp"

#include "MemAccess.h"
#include "Console.h"
#include "Core.h"
#include "DFHackVersion.h"
#include "modules/World.h"
#include "modules/Gui.h"
#include "modules/Maps.h"
#include "df/global_objects.h"
#include "df/graphic.h"
#include "df/viewscreen_dwarfmodest.h"
#include <atomic>
#include <filesystem>
#include <fstream>
using df::global::gps;
using df::global::window_x;
using df::global::window_y;
using df::global::window_z;


class logbuf : public std::stringbuf {
public:
    logbuf(DFHack::color_ostream* i_out) : std::stringbuf()
    {
        dfout = i_out;
    }
    int sync()
    {
        std::string o = this->str();
        size_t i = -1;
        // remove empty lines.
        while ((i = o.find("\n\n")) != std::string::npos) {
            o.replace(i, 2, "\n");
        }
        // Remove uninformative [application]
        while ((i = o.find("[application]")) != std::string::npos) {
            o.replace(i, 13, "[WEBFORT]");
        }

        std::cout << o;
        *dfout << o;

        dfout->flush();
        dfout->color(DFHack::COLOR_RESET);
        std::cout.flush();
        str("");
        return 0;
    }
    DFHack::color_ostream* dfout;
};

class appbuf : public std::stringbuf {
public:
    appbuf(server* i_srv) : std::stringbuf()
    {
        srv = i_srv;
    }
    int sync()
    {
        srv->get_alog().write(ws::log::alevel::app, this->str());
        str("");
        return 0;
    }
private:
    server* srv;
};

class logstream_t : public std::ostream {
public:
    logstream_t(DFHack::color_ostream* i_out)
        : std::ostream(&m_lb), m_lb(i_out)
    {}
private:
    logbuf m_lb;
};

class appstream : public std::ostream {
public:
    appstream(server* i_srv)
        : std::ostream(&m_ab),  m_ab(i_srv)
    {}
private:
    appbuf m_ab;
};


Client* get_client(conn_hdl hdl)
{
    auto it = clients.find(hdl);
    if (it == clients.end()) {
        return null_client;
    }
    return it->second;
}

Client* get_active_client()
{
    if (active_conn == null_conn) return nullptr;
    Client* cl = get_client(active_conn);
    return (cl == null_client) ? nullptr : cl;
}

int32_t round_timer()
{
    if (INGAME_TIME) {
        // FIXME: check if we are actually in-game
        return DFHack::World::ReadCurrentTick(); // uint32_t
    } else {
        return time(NULL); // time_t, usually int32_t
    }
}

#define IDLE_TIMEOUT 10
time_t itime = 0;
bool timed_out = true;
void reset_idle_timer()
{
    itime = time(NULL);
    timed_out = false;
}

void idle_timer()
{
    if (!timed_out && active_conn == null_conn) {
        time_t now = time(NULL);
        time_t diff = now - itime;
        if (diff > IDLE_TIMEOUT) {
            *out << "Quicksave triggered." << std::endl;
            quicksave(raw_out);
            timed_out = true;
        }
    }
}

void set_active(conn_hdl newc)
{
    if (active_conn == newc) { return; }
    Client* newcl = get_client(newc); // fail early

    // Clear the outgoing active player's ghost cursor so it doesn't linger
    // as a frozen spectator cursor after they lose focus.
    Client* oldcl = get_client(active_conn);
    if (oldcl && oldcl != null_client) {
        oldcl->cursor_active = false;
    }

    active_conn = newc;

    // Also clear the incoming player's cursor_active. They may have set it
    // while spectating (opcode 114) and we don't want that stale world coord
    // position to show up anywhere now that gps->mouse_x/y is the authority.
    if (newcl && newcl != null_client) {
        newcl->cursor_active = false;
    }

    if (active_conn != null_conn) {
        newcl->atime = round_timer();
        memset(newcl->mod, 0, sizeof(newcl->mod));

        std::stringstream ss;
        if (newcl->nick == "") {
            ss << "A wandering spirit";
        } else {
        ss << "The spirit " << newcl->nick;
        }
        ss << " has seized control.";
        show_announcement(ss.str());
    } else if (AUTOSAVE_WHILE_IDLE) {
        reset_idle_timer();
    }

    if (!(*df::global::pause_state)) {
        simkey(1, 0, SDL::K_SPACE, ' ');
        simkey(0, 0, SDL::K_SPACE, ' ');
    }

    if (newcl->nick == "") {
        *out << newcl->addr;
    } else {
        *out << newcl->nick;
    }
    *out << " is now active." << std::endl;
}

int32_t get_time_left(bool* time_up = nullptr)
{
    int32_t time_left = -1;
    Client* active_cl = get_client(active_conn);

    if (TURNTIME != 0 && (active_conn != null_conn) && clients.size() > 1) {
        time_t now = round_timer();
        int played = now - active_cl->atime;
        if (played < TURNTIME) {
            time_left = TURNTIME - played;
        } else if (time_up != nullptr) {
            *time_up = true;
        }
    }
    return time_left;
}

std::string str(std::string s)
{
    return "\"" + s + "\"";
}

#define STATUS_ROUTE "/api/status.json"
std::string status_json()
{
    std::stringstream json;
    int active_players = clients.size();
    Client* active_cl = get_client(active_conn);
    std::string current_player = active_cl->nick;
    int32_t time_left = get_time_left();
    bool is_somebody_playing = active_conn != null_conn;

    json << std::boolalpha << "{"
        <<  " \"active_players\": " << active_players
        << ", \"current_player\": " << str(current_player)
        << ", \"time_left\": " << time_left
        << ", \"is_somebody_playing\": " << is_somebody_playing
        << ", \"using_ingame_time\": " << INGAME_TIME
        << ", \"dfhack_version\": " << str(DFHACK_VERSION)
        << ", \"webfort_version\": " << str(WF_VERSION)
        << " }\n";

    return json.str();
}

void on_http(server* s, conn_hdl hdl)
{
    server::connection_ptr con = s->get_con_from_hdl(hdl);
    std::string route = con->get_resource();
    // Strip query string from the route before any comparisons.
    {
        auto qp = route.find('?');
        if (qp != std::string::npos) route.resize(qp);
    }
    con->replace_header("Access-Control-Allow-Origin", "*");

    if (route == STATUS_ROUTE) {
        con->set_status(websocketpp::http::status_code::ok);
        con->replace_header("Content-Type", "application/json");
        con->set_body(status_json());
        return;
    }

    // Sprite atlas — built in webfort.cpp, served from memory.
    if (route == "/atlas.png" || route == "/atlas.json") {
        std::lock_guard<std::mutex> lk(g_atlas_mutex);
        if (route == "/atlas.png") {
            if (g_atlas_png.empty()) {
                con->set_status(websocketpp::http::status_code::not_found);
                con->set_body("Atlas not yet built");
                return;
            }
            con->set_status(websocketpp::http::status_code::ok);
            con->replace_header("Content-Type", "image/png");
            con->replace_header("Cache-Control", "no-cache");
            con->set_body(std::string(g_atlas_png.begin(), g_atlas_png.end()));
        } else {
            if (g_atlas_json.empty() || g_atlas_json == "{}") {
                con->set_status(websocketpp::http::status_code::not_found);
                con->set_body("Atlas not yet built");
                return;
            }
            con->set_status(websocketpp::http::status_code::ok);
            con->replace_header("Content-Type", "application/json");
            con->replace_header("Cache-Control", "no-cache");
            con->set_body(g_atlas_json);
        }
        return;
    }

    // Serve raw art PNGs directly from <df_root>/data/art/.
    // Route: /df-art/<filename> — browser can use these as sprite sheets.
    if (route.substr(0, 8) == "/df-art/") {
        std::string fname = route.substr(8);
        // query string already stripped above; no further stripping needed.
        // Reject path traversal.
        if (fname.find('/') != std::string::npos ||
            fname.find('\\') != std::string::npos ||
            fname.find("..") != std::string::npos) {
            con->set_status(websocketpp::http::status_code::bad_request);
            con->set_body("Bad request");
            return;
        }
        std::filesystem::path art_file =
            DFHack::Core::getInstance().getHackPath().parent_path()
            / "data" / "art" / fname;
        std::error_code fec;
        if (!std::filesystem::is_regular_file(art_file, fec)) {
            con->set_status(websocketpp::http::status_code::not_found);
            con->set_body("Art file not found: " + fname);
            return;
        }
        std::ifstream afs(art_file, std::ios::binary);
        std::stringstream abody;
        abody << afs.rdbuf();
        con->set_status(websocketpp::http::status_code::ok);
        con->replace_header("Content-Type", "image/png");
        con->replace_header("Access-Control-Allow-Origin", "*");
        con->set_body(abody.str());
        return;
    }

    // Serve static files from <hack>/webfort/static/.
    std::filesystem::path static_root =
        DFHack::Core::getInstance().getHackPath() / "webfort" / "static";
    std::string path = route; // query string already stripped above
    if (path == "/" || path.empty()) path = "/webfort.html";

    // Prevent path traversal.
    if (path.find("..") != std::string::npos) {
        con->set_status(websocketpp::http::status_code::bad_request);
        con->set_body("Bad request");
        return;
    }

    std::filesystem::path file = static_root;
    file += path; // path starts with '/'

    std::error_code fec;
    if (!std::filesystem::is_regular_file(file, fec)) {
        con->set_status(websocketpp::http::status_code::not_found);
        con->set_body("Not found: " + path + "\n(served from " + static_root.string() + ")");
        return;
    }

    std::ifstream ifs(file, std::ios::binary);
    std::stringstream body;
    body << ifs.rdbuf();

    // Minimal content-type guessing.
    std::string ext = file.extension().string();
    std::string ctype = "application/octet-stream";
    if (ext == ".html")      ctype = "text/html; charset=utf-8";
    else if (ext == ".js")   ctype = "application/javascript";
    else if (ext == ".css")  ctype = "text/css";
    else if (ext == ".json") ctype = "application/json";
    else if (ext == ".png")  ctype = "image/png";
    else if (ext == ".ico")  ctype = "image/x-icon";
    else if (ext == ".svg")  ctype = "image/svg+xml";
    else if (ext == ".ttf")  ctype = "font/ttf";
    else if (ext == ".woff") ctype = "font/woff";

    con->set_status(websocketpp::http::status_code::ok);
    con->replace_header("Content-Type", ctype);
    con->set_body(body.str());
}

bool validate_open(server* s, conn_hdl hdl)
{
    auto raw_conn = s->get_con_from_hdl(hdl);

    std::vector<std::string> protos = raw_conn->get_requested_subprotocols();
    if (std::find(protos.begin(), protos.end(), WF_VERSION) != protos.end()) {
        raw_conn->select_subprotocol(WF_VERSION);
    } else if (std::find(protos.begin(), protos.end(), WF_INVALID) != protos.end()) {
        raw_conn->select_subprotocol(WF_INVALID);
    }

    return true;
}

static void send_map_info(server* s, conn_hdl hdl); // defined below tock()

void on_open(server* s, conn_hdl hdl)
{
    if (s->get_con_from_hdl(hdl)->get_subprotocol() == WF_INVALID) {
        s->close(hdl, 4000, "Invalid version, expected '" WF_VERSION "'.");
        return;
    }

    if (clients.size() >= MAX_CLIENTS) {
        s->close(hdl, 4001, "Server is full.");
        return;
    }

    auto raw_conn = s->get_con_from_hdl(hdl);
    auto path = split(raw_conn->get_resource().substr(1).c_str(), '/');
    std::string nick = path[0];
    std::string user_secret = (path.size() > 1) ? path[1] : "";

    if (nick == "__NOBODY") {
        s->close(hdl, 4002, "Invalid nickname.");
        return;
    }

    Client* cl = new Client;
    cl->is_admin = (user_secret == SECRET);

    cl->addr = raw_conn->get_remote_endpoint();
    cl->nick = nick;
    cl->atime = round_timer();
    // mod, cursor_*, cam_*, has_own_cam, own_sc, own_sc_prev are all
    // initialised by the Client constructor.

    // Seed the per-client camera from the current global viewport so
    // the first CamMove delta is applied relative to where the game
    // is actually looking right now.
    if (window_x) cl->cam_x = *window_x;
    if (window_y) cl->cam_y = *window_y;
    if (window_z) cl->cam_z = *window_z;

    assert(cl->addr != "");
    assert(cl->nick != "__NOBODY");
    clients[hdl] = cl;

    // Tell the new client the map dimensions for camera clamping.
    send_map_info(s, hdl);
}

void on_close(server* s, conn_hdl c)
{
    Client* cl = get_client(c);
    if (cl != null_client) {
        if (c == active_conn) {
            set_active(null_conn);
        }
        delete cl;
    }
    clients.erase(c);
}

// Send opcode 118 (MapInfo) to a single client.
// Reports map dimensions in tiles so the browser can clamp camera moves.
// Safe to call before a world is loaded (returns immediately if no map).
static void send_map_info(server* s, conn_hdl hdl)
{
    if (!DFHack::Maps::IsValid()) return;
    uint32_t map_sx = 0, map_sy = 0, map_sz = 0;
    DFHack::Maps::getSize(map_sx, map_sy, map_sz);
    // Convert DF map blocks (16×16 tiles each) to tile counts.
    map_sx *= 16;
    map_sy *= 16;
    // Clamp to uint16_t wire range.
    uint16_t wx = (uint16_t)std::min(map_sx, (uint32_t)65535u);
    uint16_t wy = (uint16_t)std::min(map_sy, (uint32_t)65535u);
    uint16_t wz = (uint16_t)std::min(map_sz, (uint32_t)65535u);
    unsigned char mbuf[7];
    mbuf[0] = 118; // MapInfo
    mbuf[1] = (uint8_t)(wx & 0xFF); mbuf[2] = (uint8_t)(wx >> 8);
    mbuf[3] = (uint8_t)(wy & 0xFF); mbuf[4] = (uint8_t)(wy >> 8);
    mbuf[5] = (uint8_t)(wz & 0xFF); mbuf[6] = (uint8_t)(wz >> 8);
    s->send(hdl, (const void*)mbuf, 7, ws::frame::opcode::binary);
}

static unsigned char buf[1024*1024];
void tock(server* s, conn_hdl hdl)
{
    Client* cl = get_client(hdl);
    Client* active_cl = get_client(active_conn);
    bool time_up = false;

    idle_timer();
    int32_t time_left = get_time_left(&time_up);

    if (time_up) {
        *out << active_cl->nick << " has run out of time." << std::endl;
        set_active(null_conn);
    }

    unsigned char *b = buf;
    unsigned char * const buf_end = buf + sizeof(buf);
    // [0] msgtype
    *(b++) = 110;

    uint8_t client_count = clients.size();
    // [1] # of connected clients. 128 bit set if client is active player.
    *(b++) = client_count;

    // [2] Bitfield (unchanged from before).
    uint8_t bits = 0;
    bits |= hdl == active_conn?       1 : 0; // are you the active player?
    bits |= null_conn == active_conn? 2 : 0; // is nobody playing?
    bits |= INGAME_TIME?              4 : 0; // are we using in-game time?
    bits |= g_graphics_mode?          8 : 0; // USE_GRAPHICS (sprite mode)
    bits |= (g_atlas_version & 0x0F) << 4;   // atlas rebuild counter [bits 4-7]
    *(b++) = bits;

    // [3] flags2: bit 0 = a non-dwarf-mode screen (menu/dialog) is active.
    // Used by the browser to route WASD/scroll to camera vs. game input.
    {
        uint8_t flags2 = 0;
        df::viewscreen* vs = DFHack::Gui::getCurViewscreen(true);
        bool is_menu = !vs || !virtual_cast<df::viewscreen_dwarfmodest>(vs);
        flags2 |= is_menu ? 1 : 0;
        *(b++) = flags2;
    }

    // [4-7] time left, in seconds. -1 if no timer.
    memcpy(b, &time_left, sizeof(time_left));
    b += sizeof(time_left);

    // [8-9] game dimensions
    *(b++) = gps->dimx;
    *(b++) = gps->dimy;

    // [10-15] per-client camera position (int16_t LE each).
    // Reflects the client's own viewport if has_own_cam, else the current
    // global viewport so the browser always knows where its view is.
    {
        int32_t cx = cl->has_own_cam ? cl->cam_x : (window_x ? *window_x : 0);
        int32_t cy = cl->has_own_cam ? cl->cam_y : (window_y ? *window_y : 0);
        int32_t cz = cl->has_own_cam ? cl->cam_z : (window_z ? *window_z : 0);
        auto clamp16 = [](int32_t v) -> int16_t {
            if (v < -32768) return (int16_t)-32768;
            if (v >  32767) return (int16_t) 32767;
            return (int16_t)v;
        };
        int16_t wcx = clamp16(cx), wcy = clamp16(cy), wcz = clamp16(cz);
        memcpy(b, &wcx, 2); b += 2;
        memcpy(b, &wcy, 2); b += 2;
        memcpy(b, &wcz, 2); b += 2;
    }

    // [16] Length of current active player's nick, including '\0'.
    uint8_t nick_len = active_cl->nick.length() + 1;
    *(b++) = nick_len;

    unsigned char *mod = cl->mod;

    // [17-M] null-terminated string: active player's nick
    memcpy(b, active_cl->nick.c_str(), nick_len);
    b += nick_len;

    // [M-N] Changed tiles. 9 bytes per tile:
    //   x, y, char, bg_flags, fg, texpos_lo, texpos_hi, texpos_lower_lo, texpos_lower_hi
    // Read from cl->own_sc (per-client buffer, always up to date for this
    // client's viewport — same as global sc[] when has_own_cam is false).
    const uint8_t* tile_src = cl->own_sc ? cl->own_sc : sc;
    for (int y = 0; y < gps->dimy; y++) {
        for (int x = 0; x < gps->dimx; x++) {
            const int tile = x * gps->dimy + y;
            const unsigned char *s = tile_src + tile*8;
            if (mod[tile])
                continue;
            if (b + 9 > buf_end)
                break; // packet full; remaining tiles sent next tock
            *(b++) = x;
            *(b++) = y;
            *(b++) = s[0]; // char
            *(b++) = s[2]; // bg_flags

            int bold = (s[3] != 0) * 8;
            int fg   = (s[1] + bold) % 16;
            *(b++) = (uint8_t)fg;  // fg 0-15

            *(b++) = s[4]; // screentexpos     lo
            *(b++) = s[5]; // screentexpos     hi
            *(b++) = s[6]; // screentexpos_lower lo
            *(b++) = s[7]; // screentexpos_lower hi
            mod[tile] = 1;
        }
    }
    s->send(hdl, (const void*) buf, (size_t)(b-buf), ws::frame::opcode::binary);

    // -- Cursor broadcast (opcode 112): one separate packet per tick --
    // Format: [112, count, (nick_len, nick..., tile_x, tile_y, color_idx)...]
    // color_idx: 0=driver (white), 1..N-1 = spectators (cycling colors)
    {
        unsigned char cbuf[4096];
        unsigned char *cb = cbuf;
        *(cb++) = 112; // CURSORS_UPDATE
        unsigned char *count_byte = cb++; // fill in count below
        uint8_t count = 0;
        uint8_t color_idx = 0;
        for (auto& kv : clients) {
            if (!kv.second) continue;
            // Skip the requesting client — they don't see their own cursor.
            if (kv.first == hdl) { color_idx++; continue; }
            Client* c = kv.second;
            bool is_driver = (kv.first == active_conn);

            // Convert cursor to world tile coordinates.
            // Driver cursor: gps->mouse_x/y is relative to the global viewport
            // (window_x/y), which was just synced to the active player's cam.
            // Non-driver cursor: already stored in world coords (see opcode 114).
            int world_cx, world_cy;
            bool active_cursor;
            if (is_driver) {
                world_cx = (window_x ? *window_x : 0) + gps->mouse_x;
                world_cy = (window_y ? *window_y : 0) + gps->mouse_y;
                active_cursor = (gps->mouse_x >= 0);
            } else {
                world_cx = c->cursor_x;
                world_cy = c->cursor_y;
                active_cursor = c->cursor_active;
            }
            if (!active_cursor) { color_idx++; continue; }

            // Translate world coords to the receiving client's viewport.
            int32_t recv_base_x = cl->has_own_cam ? cl->cam_x : (window_x ? *window_x : 0);
            int32_t recv_base_y = cl->has_own_cam ? cl->cam_y : (window_y ? *window_y : 0);
            int cx = world_cx - recv_base_x;
            int cy = world_cy - recv_base_y;
            // Skip cursors outside the receiving client's visible area.
            if (cx < 0 || cy < 0 || cx >= gps->dimx || cy >= gps->dimy) {
                color_idx++; continue;
            }
            uint8_t nick_len = (uint8_t)(c->nick.size() + 1);
            if (cb + nick_len + 4 > cbuf + sizeof(cbuf)) break;
            *(cb++) = nick_len;
            memcpy(cb, c->nick.c_str(), nick_len);
            cb += nick_len;
            *(cb++) = (uint8_t)cx;
            *(cb++) = (uint8_t)cy;
            *(cb++) = color_idx; // 0=driver(white), 1+=spectators
            count++;
            color_idx++;
        }
        *count_byte = count;
        // Always send the cursor packet even when count=0 so the browser
        // clears its overlay canvas every tick. Without this, a stale ghost
        // cursor from a previous tick (e.g. a spectator who scrolled out of
        // view) persists in the overlay and looks like a second cursor.
        s->send(hdl, (const void*)cbuf, (size_t)(cb-cbuf), ws::frame::opcode::binary);
    }
}

void on_message(server* s, conn_hdl hdl, message_ptr msg)
{
    auto str = msg->get_payload();
    const unsigned char *mdata = (const unsigned char*) str.c_str();
    int msz = str.size();
    if (mdata[0] == 111 && msz == 4) { // KeyEvent
        // Only the focus (active) player can send game input.
        // Non-focus clients use opcode 117 (CamMove) to pan their viewport.
        if (hdl != active_conn)
            return;
        Client* cl = get_client(hdl);
        SDL::Key k = mdata[2] ? (SDL::Key)mdata[2] : mapInputCodeToSDL(mdata[1]);
        bool is_safe_key = (cl && cl->is_admin) ||
            k != SDL::K_ESCAPE ||
            is_safe_to_escape();
        if (k != SDL::K_UNKNOWN && is_safe_key) {
            int jsmods = mdata[3];
            int sdlmods = 0;

            if (jsmods & 1) {
                simkey(1, 0, SDL::K_LALT, 0);
                sdlmods |= SDL::KMOD_ALT;
            }
            if (jsmods & 2) {
                simkey(1, 0, SDL::K_LSHIFT, 0);
                sdlmods |= SDL::KMOD_SHIFT;
            }
            if (jsmods & 4) {
                simkey(1, 0, SDL::K_LCTRL, 0);
                sdlmods |= SDL::KMOD_CTRL;
            }

            simkey(1, sdlmods, k, mdata[2]);
            simkey(0, sdlmods, k, mdata[2]);

            if (jsmods & 1) {
                simkey(0, 0, SDL::K_LALT, 0);
            }
            if (jsmods & 2) {
                simkey(0, 0, SDL::K_LSHIFT, 0);
            }
            if (jsmods & 4) {
                simkey(0, 0, SDL::K_LCTRL, 0);
            }
        }
    } else if (mdata[0] == 113 && msz == 5) { // MouseEvent
        // Only the focus player's mouse input reaches the game.
        if (hdl != active_conn)
            return;
        // [113, tile_x, tile_y, button, type]
        simmouse(mdata[1], mdata[2], mdata[3], (WFMouseType)mdata[4]);
    } else if (mdata[0] == 114 && msz == 3) { // cursorMove (non-driver ghost cursor)
        // [114, tile_x, tile_y] — tile coords in the sender's viewport.
        // Gate to non-active clients: the active player's cursor is tracked
        // via gps->mouse_x/y (opcode 113 path). If we let the active player
        // set cursor_active here it can linger and appear as a ghost cursor
        // for other clients if the driver path briefly returns gps->mouse_x<0.
        if (hdl == active_conn) return;
        Client* cl = get_client(hdl);
        if (cl && cl != null_client) {
            // World position = client's camera origin + viewport tile offset.
            // Reading window_x/y on the WS thread is a benign race on x86_64
            // (32-bit reads are naturally atomic); worst case is a one-tick lag.
            int32_t base_x = cl->has_own_cam ? cl->cam_x : (window_x ? *window_x : 0);
            int32_t base_y = cl->has_own_cam ? cl->cam_y : (window_y ? *window_y : 0);
            cl->cursor_x = base_x + (int32_t)mdata[1];
            cl->cursor_y = base_y + (int32_t)mdata[2];
            cl->cursor_active = true;
        }
    } else if (mdata[0] == 115) { // refreshScreen
        Client* cl = get_client(hdl);
        memset(cl->mod, 0, sizeof(cl->mod));
    } else if (mdata[0] == 117 && msz == 4) { // CamMove [117, dx, dy, dz]
        // Accepted from any client (not focus-gated).
        // dx/dy/dz are signed int8 values packed in uint8.
        Client* cl = get_client(hdl);
        if (cl && cl != null_client) {
            int8_t dx = (int8_t)mdata[1];
            int8_t dy = (int8_t)mdata[2];
            int8_t dz = (int8_t)mdata[3];

            if (hdl == active_conn) {
                // Active player: queue deltas for the DF main thread to apply
                // to *window_x/y/z directly. Do NOT touch cl->cam_x/y/z here —
                // plugin_onupdate() syncs it FROM *window_x/y/z after applying.
                g_active_cam_dx.fetch_add(dx, std::memory_order_relaxed);
                g_active_cam_dy.fetch_add(dy, std::memory_order_relaxed);
                g_active_cam_dz.fetch_add(dz, std::memory_order_relaxed);
                // mod will be cleared by plugin_onupdate after the delta is applied.
            } else {
                // Spectator: update their own cam_x/y/z directly.
                if (!cl->has_own_cam) {
                    // First camera move — snap to the current game camera so
                    // the delta is applied from the player's actual position,
                    // not from the potentially stale value saved at connect time.
                    if (window_x) cl->cam_x = *window_x;
                    if (window_y) cl->cam_y = *window_y;
                    if (window_z) cl->cam_z = *window_z;
                    cl->has_own_cam = true;
                }
                cl->cam_x += dx;
                cl->cam_y += dy;
                cl->cam_z += dz;
                if (cl->cam_x < 0) cl->cam_x = 0;
                if (cl->cam_y < 0) cl->cam_y = 0;
                if (cl->cam_z < 0) cl->cam_z = 0;
                if (DFHack::Maps::IsValid()) {
                    uint32_t map_sx = 0, map_sy = 0, map_sz = 0;
                    DFHack::Maps::getSize(map_sx, map_sy, map_sz);
                    if (map_sz > 0 && cl->cam_z >= (int32_t)map_sz)
                        cl->cam_z = (int32_t)map_sz - 1;
                }
                // Force full re-send: viewport changed.
                memset(cl->mod, 0, sizeof(cl->mod));
            }
        }
    } else if (mdata[0] == 116) { // requestTurn
        assert(hdl != null_conn);
        if (hdl == active_conn) {
            set_active(null_conn);
        } else if (active_conn == null_conn) {
            set_active(hdl);
        }
    } else {
        tock(s, hdl);
    }

    return;
}

void wsloop(void *a_srv)
{
    try {
        ((server*)a_srv)->run();
    } catch (const std::exception & e) {
        *out << "ERROR: std::exception caught: " << e.what() << std::endl;
    } catch (lib::error_code e) {
        *out << "ERROR: ws++ exception caught: " << e.message() << std::endl;
    } catch (...) {
        *out << "ERROR: Unknown exception caught:" << std::endl;
    }
    return;
}

struct WFServerImpl {
    std::thread* loop;
    server srv;
    WFServerImpl(DFHack::color_ostream&);
    ~WFServerImpl();
    void start();
    void stop();
};

WFServerImpl::WFServerImpl(DFHack::color_ostream& i_raw_out)
{
    loop = nullptr;
    null_client = new Client;
    null_client->nick = "__NOBODY";

    raw_out = &i_raw_out;
    logstream = new logstream_t(raw_out);
    out = new appstream(&srv);
}

WFServerImpl::~WFServerImpl()
{
    delete null_client;
    delete logstream;
    delete out;
}

void WFServerImpl::start()
{
    load_config();
    const char* stage = "";
    try {
        stage = "clear_access_channels";
        srv.clear_access_channels(ws::log::alevel::all);
        stage = "set_access_channels";
        srv.set_access_channels(
                ws::log::alevel::connect    |
                ws::log::alevel::disconnect |
                ws::log::alevel::app
        );
        stage = "set_error_channels";
        srv.set_error_channels(
                ws::log::elevel::info   |
                ws::log::elevel::warn   |
                ws::log::elevel::rerror |
                ws::log::elevel::fatal
        );
        stage = "init_asio";
        srv.init_asio();

        stage = "set_alog_ostream";
        srv.get_alog().set_ostream(logstream);

        stage = "set_handlers";
        srv.set_http_handler(bind(&on_http, &srv, ::_1));
        srv.set_validate_handler(bind(&validate_open, &srv, ::_1));
        srv.set_open_handler(bind(&on_open, &srv, ::_1));
        srv.set_message_handler(bind(&on_message, &srv, ::_1, ::_2));
        srv.set_close_handler(bind(&on_close, &srv, ::_1));

        stage = "set_reuse_addr";
        srv.set_reuse_addr(true);

        stage = "listen";
        lib::error_code ec;
        srv.listen(PORT, ec);
        if (ec) {
            *out << "ERROR: Unable to listen on port " << PORT
                 << " (v6): " << ec.message() << " - retrying on v4" << std::endl;
            ec.clear();
            srv.listen(asio::ip::tcp::v4(), PORT, ec);
            if (ec) {
                *out << "ERROR: Unable to start Webfort on port " << PORT
                     << " (v4 also failed): " << ec.message() << std::endl;
                return;
            }
        }

        stage = "start_accept";
        srv.start_accept();
        *out << "Web Fortress started on port " << PORT << std::endl;
    } catch (const std::exception & e) {
        *out << "Webfort failed to start during '" << stage << "': " << e.what() << std::endl;
        return;
    } catch (lib::error_code e) {
        *out << "Webfort failed to start during '" << stage << "': " << e.message() << std::endl;
        return;
    } catch (...) {
        *out << "Webfort failed to start during '" << stage << "': other exception" << std::endl;
        return;
    }
    loop = new std::thread(wsloop, &srv);
}

void WFServerImpl::stop()
{
    srv.stop();
    if (loop) {
        if (loop->joinable())
            loop->join();
        delete loop;
        loop = nullptr;
    }
}

WFServer::WFServer(DFHack::color_ostream& o) { impl = new WFServerImpl(o); }
WFServer::~WFServer()  { delete impl; }
void WFServer::start() { impl->start(); }
void WFServer::stop()  { impl->stop(); }
