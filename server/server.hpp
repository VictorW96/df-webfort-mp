#ifndef __WF_SERVER_HPP_
#define __WF_SERVER_HPP__

/*
 * server.hpp
 * Part of Web Fortress
 *
 * Copyright (c) 2014 mifki, ISC license.
 */

#include <cstdint>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <thread>
#include <websocketpp/server.hpp>

#include "Console.h"

struct Client {
    std::string addr;
    std::string nick;
    unsigned char mod[256*256];
    time_t atime;
    bool is_admin;
    // Ghost cursor position. The focus client's cursor is written to
    // gps->mouse_x/y; non-focus cursors are stored here and broadcast.
    int cursor_x;
    int cursor_y;
    bool cursor_active;

    // Per-client camera for independent viewports.
    // cam_x/y/z hold this client's viewport tile offset; initialized
    // from the global window_x/y/z when the client connects.
    // has_own_cam is set true on the first CamMove opcode (opcode 117)
    // and causes an extra per-client render pass each tick.
    int32_t cam_x, cam_y, cam_z;
    bool has_own_cam;

    // Per-client screen buffers (256*256*8 bytes each).
    // own_sc     — current rendered frame for this client's viewport.
    // own_sc_prev — previous frame for tile-delta computation.
    // tock() always reads from own_sc; for clients without has_own_cam
    // own_sc is a copy of the global sc[] buffer.
    uint8_t* own_sc;
    uint8_t* own_sc_prev;

    Client()
        : atime(0), is_admin(false),
          cursor_x(-1), cursor_y(-1), cursor_active(false),
          cam_x(0), cam_y(0), cam_z(0), has_own_cam(false)
    {
        memset(mod, 0, sizeof(mod));
        own_sc      = new uint8_t[256*256*8]();
        own_sc_prev = new uint8_t[256*256*8]();
    }
    ~Client() {
        delete[] own_sc;
        delete[] own_sc_prev;
    }
    // Non-copyable: owns heap buffers.
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
};

// FIXME: webfort.cpp should not know about this
namespace ws = websocketpp;
typedef ws::connection_hdl conn_hdl;

static std::owner_less<conn_hdl> conn_lt;
inline bool operator==(const conn_hdl& p, const conn_hdl& q)
{
    return (!conn_lt(p, q) && !conn_lt(q, p));
}
inline bool operator!=(const conn_hdl& p, const conn_hdl& q)
{
    return conn_lt(p, q) || conn_lt(q, p);
}

typedef std::map<conn_hdl, Client*, std::owner_less<conn_hdl>> conn_map;
extern conn_map clients;

// Returns the currently active (focus) client, or nullptr if nobody is active.
Client* get_active_client();

struct WFServerImpl;
struct WFServer {
    WFServer(DFHack::color_ostream&);
    ~WFServer();
    void start();
    void stop();
private:
    WFServerImpl* impl;
};

#endif
