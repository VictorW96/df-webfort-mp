#ifndef __WF_SERVER_HPP_
#define __WF_SERVER_HPP__

/*
 * server.hpp
 * Part of Web Fortress
 *
 * Copyright (c) 2014 mifki, ISC license.
 */

#include <ctime>
#include <map>
#include <string>
#include <thread>
#include <websocketpp/server.hpp>

#include "Console.h"

typedef struct {
    std::string addr;
    std::string nick;
    unsigned char mod[256*256];
    time_t atime;
    bool is_admin;
} Client;

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
